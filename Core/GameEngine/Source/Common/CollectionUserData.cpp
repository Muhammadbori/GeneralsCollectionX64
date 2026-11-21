/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// CollectionUserData.cpp ///////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O118 Our own user data folders (CollectionUserData.h).

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CollectionUserData.h"

#include "Common/CommandLine.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Shell.h"
#include "GameLogic/GameLogic.h"

#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
	const char *const kCollection = "Generals Collection x64";
	const char *const kMarkerName = "UseThisFolder.txt";      // O133: present = this LOCALAPPDATA folder is the one in use
	const char *const kNoticeShownName = "FolderNoticeShown.txt"; // O133: the one-time menu notice was shown

	// O133: test switches (*uitest*.exe only) that stand in for the two Windows folders, so a test can make "Documents"
	// unwritable (a deny-write ACL) without touching the player's real folders or Windows settings.
	const char *testFolderArg(const char *name)
	{
		if (!CommandLine::isTestExe()) {
			return nullptr;
		}
		for (int arg = 1; arg + 1 < __argc; ++arg) {
			if (stricmp(__argv[arg], name) == 0) {
				return __argv[arg + 1];
			}
		}
		return nullptr;
	}

	AsciiString withSlash(const AsciiString &path)
	{
		AsciiString out = path;
		if (!out.isEmpty() && !out.endsWith("\\")) {
			out.concat('\\');
		}
		return out;
	}

	AsciiString knownFolder(const GUID &id)
	{
		AsciiString out;
		typedef HRESULT(WINAPI *KnownFolderFunc)(const GUID &rfid, DWORD flags, HANDLE token, PWSTR *path);
		HMODULE shell = GetModuleHandleA("shell32.dll");
		if (shell == nullptr) {
			shell = LoadLibraryA("shell32.dll");
		}
		KnownFolderFunc func = shell != nullptr ? (KnownFolderFunc)(void *)GetProcAddress(shell, "SHGetKnownFolderPath") : nullptr;
		if (func != nullptr)
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(func(id, 0, nullptr, &path)) && path != nullptr)
			{
				char buffer[MAX_PATH * 2];
				if (WideCharToMultiByte(CP_ACP, 0, path, -1, buffer, sizeof(buffer), nullptr, nullptr) > 0) {
					out = buffer;
				}
				CoTaskMemFree(path);
			}
		}
		return out;
	}

	// O121 (S65): the Documents folder itself, from Windows (OneDrive or Group Policy redirection included), never
	// derived from the original game's folder name (in S65 that gave the profile folder for Generals and nothing for
	// Zero Hour).
	AsciiString documentsFolder()
	{
		if (const char *test = testFolderArg("-testDocumentsDir")) {
			return withSlash(test);
		}
		AsciiString out = knownFolder(FOLDERID_Documents);
		if (out.isEmpty())
		{
			char buffer[MAX_PATH + 1];
			if (SHGetSpecialFolderPathA(nullptr, buffer, CSIDL_PERSONAL, TRUE)) {
				out = buffer;
			}
		}
		return withSlash(out);
	}

	// O133: %LOCALAPPDATA% (Controlled folder access does not protect it)
	AsciiString localAppDataFolder()
	{
		if (const char *test = testFolderArg("-testLocalAppDataDir")) {
			return withSlash(test);
		}
		AsciiString out = knownFolder(FOLDERID_LocalAppData);
		if (out.isEmpty())
		{
			if (const char *env = getenv("LOCALAPPDATA")) {
				out = env;
			}
		}
		return withSlash(out);
	}

	Bool isFolder(const AsciiString &path)
	{
		const DWORD attr = GetFileAttributesA(path.str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	Bool isFile(const AsciiString &path)
	{
		const DWORD attr = GetFileAttributesA(path.str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	/// <base>Generals Collection x64\<gameName>\ ; makes both levels
	AsciiString makeChain(const AsciiString &base, const char *gameName)
	{
		AsciiString collection = base;
		collection.concat(kCollection);
		CreateDirectoryA(collection.str(), nullptr);
		AsciiString ours = collection;
		ours.concat('\\');
		ours.concat(gameName);
		ours.concat('\\');
		CreateDirectoryA(ours.str(), nullptr);
		return ours;
	}

	/// O133: can this process really create a file here? Controlled folder access lets the folder be listed and read but
	/// fails CreateFile/CreateDirectory with ACCESS_DENIED. The probe file deletes itself when closed.
	Bool canWrite(const AsciiString &folder, DWORD *error)
	{
		*error = 0;
		if (!isFolder(folder)) {
			*error = GetLastError() != 0 ? GetLastError() : ERROR_PATH_NOT_FOUND;
			return FALSE;
		}
		AsciiString probe;
		probe.format("%sWriteTest_%lu.tmp", folder.str(), (unsigned long)GetCurrentProcessId());
		HANDLE h = CreateFileA(probe.str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			*error = GetLastError();
			return FALSE;
		}
		DWORD written = 0;
		const Bool ok = WriteFile(h, "x", 1, &written, nullptr) && written == 1;
		if (!ok) {
			*error = GetLastError();
		}
		CloseHandle(h);
		return ok;
	}

	/// O133: copy what the player already had in the Documents folder (read only there), never overwriting.
	void copyTreeOnce(const AsciiString &from, const AsciiString &to, Int *files, Int *failed)
	{
		AsciiString pattern = from;
		pattern.concat('*');
		WIN32_FIND_DATAA found;
		HANDLE h = FindFirstFileA(pattern.str(), &found);
		if (h == INVALID_HANDLE_VALUE) {
			return;
		}
		do
		{
			if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) {
				continue;
			}
			AsciiString src = from;
			src.concat(found.cFileName);
			AsciiString dst = to;
			dst.concat(found.cFileName);
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				CreateDirectoryA(dst.str(), nullptr);
				copyTreeOnce(withSlash(src), withSlash(dst), files, failed);
			}
			else if (strnicmp(found.cFileName, "WriteTest_", 10) != 0 && stricmp(found.cFileName, "UserDataFolders.log") != 0)
			{
				if (CopyFileA(src.str(), dst.str(), TRUE)) {
					++*files;
				} else if (GetLastError() != ERROR_FILE_EXISTS) {
					++*failed;
				}
			}
		} while (FindNextFileA(h, &found));
		FindClose(h);
	}

	UnicodeString toUnicode(const AsciiString &s)
	{
		UnicodeString out;
		WideChar buffer[MAX_PATH * 2];
		if (MultiByteToWideChar(CP_ACP, 0, s.str(), -1, buffer, ARRAY_SIZE(buffer)) > 0) {
			out = buffer;
		} else {
			out.translate(s);
		}
		return out;
	}

	// the decision, made once per run (makeOurs is called more than once per run)
	struct Decision
	{
		Bool made;
		AsciiString ours;
		AsciiString documentsOurs;
		Bool fallback;         // ours is the LOCALAPPDATA folder
		const char *why;
		DWORD documentsError;  // the probe's error in Documents (0 = not probed or writable)
		Int copiedFiles;
		Int copyFailures;
		const char *gameName;  // "Zero Hour" or "Generals"
	};
	Decision s_decision = { FALSE, AsciiString(), AsciiString(), FALSE, "", 0, 0, 0, "" };
	Bool s_noticeDone = FALSE;
}

AsciiString CollectionUserData::makeOurs(const AsciiString &originalDir, const char *originalLeaf, const char *gameName)
{
	const AsciiString documents = documentsFolder();
	if (documents.isEmpty() || gameName == nullptr) {
		return AsciiString::TheEmptyString;
	}

	// O133: Documents, or the LOCALAPPDATA folder when Documents cannot be written (Controlled folder access).
	// Which one wins, decided once per run:
	//  1. a marker file in <LOCALAPPDATA>\Generals Collection x64\<game>\ -> that folder, always. Once the game has moved
	//     there it stays there, even if Documents becomes writable again, so the two folders never mix.
	//  2. else Documents\Generals Collection x64\<game>\ when a probe file can be created there;
	//  3. else the LOCALAPPDATA folder: made, marked, and filled once with a copy of what the Documents folder had;
	//  4. if that cannot be written either, Documents as before (nothing better exists).
	Decision &d = s_decision;
	if (!d.made)
	{
		d.made = TRUE;
		d.gameName = gameName;   // a string literal of the callers
		AsciiString docsOurs = documents;
		docsOurs.concat(kCollection);
		docsOurs.concat('\\');
		docsOurs.concat(gameName);
		docsOurs.concat('\\');
		d.documentsOurs = docsOurs;

		const AsciiString local = localAppDataFolder();
		AsciiString localOurs;
		if (!local.isEmpty())
		{
			localOurs = local;
			localOurs.concat(kCollection);
			localOurs.concat('\\');
			localOurs.concat(gameName);
			localOurs.concat('\\');
		}
		AsciiString marker = localOurs;
		marker.concat(kMarkerName);

		DWORD localError = 0;
		if (!localOurs.isEmpty() && isFile(marker))
		{
			d.ours = localOurs;
			d.fallback = TRUE;
			d.why = "marker found (the game moved here earlier; Documents is not used any more)";
		}
		else if (canWrite(makeChain(documents, gameName), &d.documentsError))
		{
			d.ours = docsOurs;
			d.why = "Documents is writable";
		}
		else if (!localOurs.isEmpty() && canWrite(makeChain(local, gameName), &localError))
		{
			d.ours = localOurs;
			d.fallback = TRUE;
			d.why = "Documents cannot be written (Controlled folder access?); moved here";
			if (isFolder(docsOurs)) {
				copyTreeOnce(docsOurs, localOurs, &d.copiedFiles, &d.copyFailures);
			}
			if (FILE *fp = fopen(marker.str(), "w"))
			{
				fprintf(fp, "This folder holds the user data of Generals Collection x64 (%s) because Windows did not let the\n", gameName);
				fprintf(fp, "game write to %s (error %lu; Controlled folder access?).\n", docsOurs.str(), (unsigned long)d.documentsError);
				fprintf(fp, "While this file exists the game uses this folder, even if Documents becomes writable again.\n");
				fprintf(fp, "Delete this file to use the Documents folder again (nothing is copied back).\n");
				fclose(fp);
			}
		}
		else
		{
			d.ours = docsOurs;
			d.why = "neither Documents nor LOCALAPPDATA could be written; Documents kept";
		}
	}
	const AsciiString ours = d.ours;

	// the original folder, for the one-time import only: the game's own computed one when it names a folder under
	// Documents, else Documents\<originalLeaf>\ (Generals' leaf comes from its INI and may still be empty here)
	AsciiString original = originalDir;
	AsciiString trimmed = original;
	trimmed.trimEnd('\\');
	AsciiString docsTrimmed = documents;
	docsTrimmed.trimEnd('\\');
	if (original.isEmpty() || trimmed.compareNoCase(docsTrimmed) == 0 || !original.startsWithNoCase(documents.str()))
	{
		original = documents;
		if (originalLeaf != nullptr) {
			original.concat(originalLeaf);
		}
		original.concat('\\');
	}

	// the player's settings carry over once: read the original Options.ini, never change it
	AsciiString ourOptions = ours;
	ourOptions.concat("Options.ini");
	const char *importNote = "kept (ours exists)";
	if (GetFileAttributesA(ourOptions.str()) == INVALID_FILE_ATTRIBUTES)
	{
		AsciiString originalOptions = original;
		originalOptions.concat("Options.ini");
		if (GetFileAttributesA(originalOptions.str()) != INVALID_FILE_ATTRIBUTES) {
			importNote = CopyFileA(originalOptions.str(), ourOptions.str(), TRUE) ? "imported" : "import failed";
		} else {
			importNote = "none (no original Options.ini)";
		}
	}

	// O121: a short note of what was chosen, for test runs and bug reports (ours only). O123 (S67): written once per
	// run, at the first call: later calls (a GlobalData override, Generals' INI parse) found our Options.ini already
	// imported and overwrote the note with "kept (ours exists)".
	static Bool s_noteWritten = FALSE;
	AsciiString notePath = ours;
	notePath.concat("UserDataFolders.log");
	FILE *fp = s_noteWritten ? nullptr : fopen(notePath.str(), "w");
	if (fp != nullptr)
	{
		fprintf(fp, "Documents = %s\n", documents.str());
		fprintf(fp, "Ours      = %s (%s)\n", ours.str(), isFolder(ours) ? "ok" : "MISSING");
		fprintf(fp, "Original  = %s (read only; Options.ini %s)\n", original.str(), importNote);
		fprintf(fp, "Chosen    = %s (%s)\n", d.fallback ? "LOCALAPPDATA" : "Documents", d.why);
		if (d.documentsError != 0) {
			fprintf(fp, "Documents write probe failed: error %lu in %s\n", (unsigned long)d.documentsError, d.documentsOurs.str());
		}
		if (d.copiedFiles != 0 || d.copyFailures != 0) {
			fprintf(fp, "Copied from Documents once: %d files (%d failed)\n", d.copiedFiles, d.copyFailures);
		}
		if (d.fallback) {
			fprintf(fp, "Notice    = Windows protects the Documents folder (Controlled folder access); settings, saves and replays are kept in %s\n", ours.str());
		}
		fclose(fp);
		s_noteWritten = TRUE;
	}
	return ours;
}

Bool CollectionUserData::usesFallback()
{
	return s_decision.made && s_decision.fallback;
}

void CollectionUserData::showFallbackNoticeOnce()
{
	if (s_noticeDone) {
		return;
	}
	if (!usesFallback()) {
		s_noticeDone = TRUE;
		return;
	}
	if (TheShell == nullptr || !TheShell->isShellActive() || TheShell->getScreenCount() <= 0 || !TheShell->isAnimFinished()) {
		return;
	}
	if (TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()) {
		return;
	}
	s_noticeDone = TRUE;

	// once per player, not per run: a file next to the marker remembers it
	AsciiString shownPath = s_decision.ours;
	shownPath.concat(kNoticeShownName);
	if (isFile(shownPath)) {
		return;
	}
	if (FILE *fp = fopen(shownPath.str(), "w"))
	{
		fprintf(fp, "The menu notice about the folder %s was shown.\n", s_decision.ours.str());
		fclose(fp);
	}

	// The message box is narrow: short lines, and the folder as %LOCALAPPDATA% plus our part on its own line (the
	// full path is in UserDataFolders.log). The fallback folder is always under LOCALAPPDATA (a test switch only
	// stands in for it).
	UnicodeString body = localizedFallback(
		L"Windows does not let the game\nwrite to the Documents folder.\nSettings and saves are\nkept in this folder:",
		L"Windows \x043D\x0435 \x0434\x0430\x0451\x0442 \x0438\x0433\x0440\x0435 \x043F\x0438\x0441\x0430\x0442\x044C\n"
		L"\x0432 \x043F\x0430\x043F\x043A\x0443 \x00AB\x0414\x043E\x043A\x0443\x043C\x0435\x043D\x0442\x044B\x00BB.\n"
		L"\x041D\x0430\x0441\x0442\x0440\x043E\x0439\x043A\x0438 \x0438 \x0441\x043E\x0445\x0440\x0430\x043D\x0435\x043D\x0438\x044F\n"
		L"\x0445\x0440\x0430\x043D\x044F\x0442\x0441\x044F \x0432 \x043F\x0430\x043F\x043A\x0435:");
	body.concat(L"\n%LOCALAPPDATA%\\\nGenerals Collection x64\\");
	body.concat(toUnicode(AsciiString(s_decision.gameName)));
	MessageBoxOk(localizedFallback(L"Settings folder",
		L"\x041F\x0430\x043F\x043A\x0430 \x043D\x0430\x0441\x0442\x0440\x043E\x0435\x043A"), body, nullptr);
}
