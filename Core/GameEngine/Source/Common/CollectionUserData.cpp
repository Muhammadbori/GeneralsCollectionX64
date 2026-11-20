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

#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>

namespace
{
	// O121 (S65): the Documents folder itself, from Windows (OneDrive or Group Policy redirection included), never
	// derived from the original game's folder name (in S65 that gave the profile folder for Generals and nothing for
	// Zero Hour).
	AsciiString documentsFolder()
	{
		AsciiString out;
		typedef HRESULT(WINAPI *KnownFolderFunc)(const GUID &rfid, DWORD flags, HANDLE token, PWSTR *path);
		HMODULE shell = GetModuleHandleA("shell32.dll");
		if (shell == nullptr) {
			shell = LoadLibraryA("shell32.dll");
		}
		KnownFolderFunc knownFolder = shell != nullptr ? (KnownFolderFunc)(void *)GetProcAddress(shell, "SHGetKnownFolderPath") : nullptr;
		if (knownFolder != nullptr)
		{
			PWSTR path = nullptr;
			if (SUCCEEDED(knownFolder(FOLDERID_Documents, 0, nullptr, &path)) && path != nullptr)
			{
				char buffer[MAX_PATH * 2];
				if (WideCharToMultiByte(CP_ACP, 0, path, -1, buffer, sizeof(buffer), nullptr, nullptr) > 0) {
					out = buffer;
				}
				CoTaskMemFree(path);
			}
		}
		if (out.isEmpty())
		{
			char buffer[MAX_PATH + 1];
			if (SHGetSpecialFolderPathA(nullptr, buffer, CSIDL_PERSONAL, TRUE)) {
				out = buffer;
			}
		}
		if (!out.isEmpty() && !out.endsWith("\\")) {
			out.concat('\\');
		}
		return out;
	}

	Bool isFolder(const AsciiString &path)
	{
		const DWORD attr = GetFileAttributesA(path.str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}
}

AsciiString CollectionUserData::makeOurs(const AsciiString &originalDir, const char *originalLeaf, const char *gameName)
{
	const AsciiString documents = documentsFolder();
	if (documents.isEmpty() || gameName == nullptr) {
		return AsciiString::TheEmptyString;
	}

	AsciiString collection = documents;
	collection.concat("Generals Collection x64");
	CreateDirectoryA(collection.str(), nullptr);

	AsciiString ours = collection;
	ours.concat('\\');
	ours.concat(gameName);
	ours.concat('\\');
	CreateDirectoryA(ours.str(), nullptr);

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

	// O121: a short note of what was chosen, for test runs and bug reports (ours only)
	AsciiString notePath = ours;
	notePath.concat("UserDataFolders.log");
	if (FILE *fp = fopen(notePath.str(), "w"))
	{
		fprintf(fp, "Documents = %s\n", documents.str());
		fprintf(fp, "Ours      = %s (%s)\n", ours.str(), isFolder(ours) ? "ok" : "MISSING");
		fprintf(fp, "Original  = %s (read only; Options.ini %s)\n", original.str(), importNote);
		fclose(fp);
	}
	return ours;
}
