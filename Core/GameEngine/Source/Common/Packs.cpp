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

// TheSuperHackers @feature See Packs.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/Packs.h"

#include "Common/FrameShots.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Shell.h"
#include "GameLogic/GameLogic.h"

#include <stdio.h>
#include <algorithm>

namespace
{
#if RTS_GENERALS
	const char *const kGameFolder = "Generals";
#else
	const char *const kGameFolder = "ZeroHour";
#endif

	const char *const kKeys[Packs::KIND_COUNT] = { "TextPack", "VoicePack", "MoviePack", "ControlBarPack", "HotkeyPack" };
	const char *const kFolders[Packs::KIND_COUNT] = { "Text", "Voice", "Movies", "ControlBar", "Hotkeys" };

	/// The hotkey pack's string file must beat the text pack's, and a control bar's lettered pictures the text pack's.
	const Packs::Kind kMountOrder[Packs::KIND_COUNT] =
	{
		Packs::KIND_HOTKEYS, Packs::KIND_CONTROLBAR, Packs::KIND_TEXT, Packs::KIND_VOICE, Packs::KIND_MOVIES
	};

	enum Problem { PROBLEM_NOT_FOUND, PROBLEM_BROKEN, PROBLEM_WRONG_TEXT };

	struct ProblemEntry
	{
		Packs::Kind kind;
		AsciiString name;
		Problem problem;
	};

	struct KindState
	{
		std::vector<Packs::Pack> packs;
		AsciiString defaultName;
		Bool showBase;
		UnicodeString baseEnglish;
		UnicodeString baseRussian;
		AsciiString baseTextLanguage;
		Int active;                  ///< index into packs, -1: the base data
		Int fallback;                ///< index into packs, -1: none
		Int activeGroup;
		Int fallbackGroup;
	};

	struct State
	{
		AsciiString exeFolder;       ///< ends with '\'
		KindState kinds[Packs::KIND_COUNT];
		std::vector<AsciiString> mounted;    ///< normalized full paths of the archives the packs mounted
		std::vector<AsciiString> mountedNames;   ///< the same as found, for the log
		std::vector<AsciiString> leftOut;    ///< list lines a pack left out, for the log
		std::vector<ProblemEntry> problems;
		Bool mountPrepared;
		Bool problemsShown;
	};

	State *s_state = nullptr;
	Bool s_restart = FALSE;

	//---------------------------------------------------------------------------------------------
	void exeFolderOf(char *out, Int size)
	{
		out[0] = '\0';
		char path[_MAX_PATH];
		const DWORD len = GetModuleFileName(nullptr, path, ARRAY_SIZE(path));
		if (len == 0 || len >= ARRAY_SIZE(path)) {
			return;
		}
		char *sep = strrchr(path, '\\');
		if (sep != nullptr) {
			sep[1] = '\0';
		}
		strlcpy(out, path, size);
	}

	Bool fileExists(const char *path)
	{
		const DWORD attr = GetFileAttributes(path);
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	Bool folderExists(const char *path)
	{
		const DWORD attr = GetFileAttributes(path);
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	/// Lower case, '\' separators, full path.
	AsciiString normalizedPath(const char *path)
	{
		char full[_MAX_PATH];
		const DWORD len = GetFullPathName(path, ARRAY_SIZE(full), full, nullptr);
		AsciiString result = (len > 0 && len < ARRAY_SIZE(full)) ? AsciiString(full) : AsciiString(path);
		char buffer[_MAX_PATH];
		strlcpy(buffer, result.str(), ARRAY_SIZE(buffer));
		for (char *c = buffer; *c != '\0'; ++c) {
			*c = (*c == '/') ? '\\' : (char)tolower((unsigned char)*c);
		}
		return AsciiString(buffer);
	}

	/// A relative archive line as written in a list file or in pack.ini: lower case, '\' separators, no leading ".\".
	AsciiString normalizedEntry(const AsciiString &entry)
	{
		char buffer[_MAX_PATH];
		strlcpy(buffer, entry.str(), ARRAY_SIZE(buffer));
		for (char *c = buffer; *c != '\0'; ++c) {
			*c = (*c == '/') ? '\\' : (char)tolower((unsigned char)*c);
		}
		const char *start = buffer;
		while (start[0] == '.' && start[1] == '\\') {
			start += 2;
		}
		return AsciiString(start);
	}

	UnicodeString fromUtf8(const AsciiString &text)
	{
		WideChar buffer[256];
		const int n = MultiByteToWideChar(CP_UTF8, 0, text.str(), -1, buffer, ARRAY_SIZE(buffer));
		if (n <= 0) {
			UnicodeString plain;
			plain.translate(text);
			return plain;
		}
		return UnicodeString(buffer);
	}

	Bool isYes(const AsciiString &value)
	{
		return stricmp(value.str(), "yes") == 0 || stricmp(value.str(), "1") == 0 || stricmp(value.str(), "true") == 0;
	}

	/// Calls 'onPair' for each "Key = Value" line of a small description file.
	template <typename F>
	Bool readKeyValues(const char *path, F onPair)
	{
		FILE *fp = fopen(path, "rb");
		if (fp == nullptr) {
			return FALSE;
		}
		char line[1024];
		Bool first = TRUE;
		while (fgets(line, sizeof(line), fp) != nullptr)
		{
			char *text = line;
			if (first && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
				text += 3;   // UTF-8 byte order mark
			}
			first = FALSE;
			AsciiString all(text);
			all.trim();
			if (all.isEmpty() || all.getCharAt(0) == ';' || all.getCharAt(0) == '#') {
				continue;
			}
			const char *eq = strchr(all.str(), '=');
			if (eq == nullptr) {
				continue;
			}
			AsciiString key;
			key.set(all.str(), (Int)(eq - all.str()));
			AsciiString value(eq + 1);
			key.trim();
			value.trim();
			if (value.getLength() >= 2 && value.getCharAt(0) == '"' && value.getCharAt(value.getLength() - 1) == '"') {
				AsciiString inner;
				inner.set(value.str() + 1, value.getLength() - 2);
				value = inner;
			}
			if (!key.isEmpty()) {
				onPair(key, value);
			}
		}
		fclose(fp);
		return TRUE;
	}

	Bool keyIs(const AsciiString &key, const char *name, const char *gameSpecific = nullptr)
	{
		if (stricmp(key.str(), name) == 0) {
			return TRUE;
		}
		if (gameSpecific != nullptr)
		{
			AsciiString full(name);
			full.concat(gameSpecific);
			return stricmp(key.str(), full.str()) == 0;
		}
		return FALSE;
	}

	void listFiles(const AsciiString &folder, const char *mask, std::vector<AsciiString> &out)
	{
		std::vector<AsciiString> found;
		AsciiString pattern = folder;
		pattern.concat(mask);
		WIN32_FIND_DATA data;
		HANDLE find = FindFirstFile(pattern.str(), &data);
		if (find == INVALID_HANDLE_VALUE) {
			return;
		}
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
				AsciiString path = folder;
				path.concat(data.cFileName);
				found.push_back(path);
			}
		} while (FindNextFile(find, &data));
		FindClose(find);
		// the order of the game's own folder search: by name, first wins
		std::sort(found.begin(), found.end(), [](const AsciiString &a, const AsciiString &b) { return stricmp(a.str(), b.str()) < 0; });
		out.insert(out.end(), found.begin(), found.end());
	}

	void listFolders(const AsciiString &folder, std::vector<AsciiString> &out)
	{
		AsciiString pattern = folder;
		pattern.concat("*");
		WIN32_FIND_DATA data;
		HANDLE find = FindFirstFile(pattern.str(), &data);
		if (find == INVALID_HANDLE_VALUE) {
			return;
		}
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && data.cFileName[0] != '.') {
				out.push_back(AsciiString(data.cFileName));
			}
		} while (FindNextFile(find, &data));
		FindClose(find);
		std::sort(out.begin(), out.end(), [](const AsciiString &a, const AsciiString &b) { return stricmp(a.str(), b.str()) < 0; });
	}

	AsciiString textureNameOf(Packs::Kind kind, const AsciiString &packName, const char *extension)
	{
		char buffer[_MAX_PATH];
		snprintf(buffer, ARRAY_SIZE(buffer), "packpreview_%s_%s%s", kFolders[kind], packName.str(), extension);
		for (char *c = buffer; *c != '\0'; ++c) {
			*c = (*c == ' ' || *c == '\\' || *c == '/') ? '_' : (char)tolower((unsigned char)*c);
		}
		return AsciiString(buffer);
	}

	/// FALSE when the pack's Games line names only the other game.
	Bool readPack(Packs::Kind kind, const AsciiString &kindFolder, const AsciiString &name, Packs::Pack &pack)
	{
		Bool forThisGame = TRUE;
		pack.name = name;
		pack.folder = kindFolder;
		pack.folder.concat(name);
		pack.folder.concat("\\");
		pack.fullViewport = FALSE;
		pack.empty = FALSE;
		AsciiString preview;

		AsciiString ini = pack.folder;
		ini.concat("pack.ini");
		readKeyValues(ini.str(), [&](const AsciiString &key, const AsciiString &value)
		{
			if (keyIs(key, "NameEnglish")) pack.nameEnglish = fromUtf8(value);
			else if (keyIs(key, "NameRussian")) pack.nameRussian = fromUtf8(value);
			else if (keyIs(key, "Preview")) preview = value;
			else if (keyIs(key, "FullViewport")) pack.fullViewport = isYes(value);
			else if (keyIs(key, "TextLanguage")) { pack.textLanguage = value; pack.textLanguage.toLower(); }
			else if (keyIs(key, "Fallback")) pack.fallback = value;
			else if (keyIs(key, "Archive", kGameFolder)) pack.archives.push_back(value);
			else if (keyIs(key, "Exclude", kGameFolder)) pack.excludes.push_back(normalizedEntry(value));
			else if (keyIs(key, "Require", kGameFolder)) pack.requiredFiles.push_back(value);
			else if (keyIs(key, "Empty")) pack.empty = isYes(value);
			else if (keyIs(key, "Games"))
			{
				// for example "ZeroHour" or "Generals, ZeroHour"
				AsciiString games = value;
				games.toLower();
				AsciiString mine(kGameFolder);
				mine.toLower();
				forThisGame = strstr(games.str(), mine.str()) != nullptr;
			}
		});

		// the preview picture: named in pack.ini, else preview.tga / preview.dds; the game's own folder first
		const char *const defaults[] = { "preview.tga", "preview.dds" };
		const Int tries = preview.isEmpty() ? 2 : 1;
		for (Int t = 0; t < tries && pack.previewFile.isEmpty(); ++t)
		{
			const AsciiString file = preview.isEmpty() ? AsciiString(defaults[t]) : preview;
			AsciiString inGame = pack.folder;
			inGame.concat(kGameFolder);
			inGame.concat("\\");
			inGame.concat(file);
			AsciiString inPack = pack.folder;
			inPack.concat(file);
			if (fileExists(inGame.str())) pack.previewFile = inGame;
			else if (fileExists(inPack.str())) pack.previewFile = inPack;
		}
		if (pack.previewFile.isNotEmpty())
		{
			const char *dot = strrchr(pack.previewFile.str(), '.');
			pack.previewTexture = textureNameOf(kind, name, dot != nullptr ? dot : ".tga");
		}
		return forThisGame;
	}

	State &state()
	{
		Packs::scan();
		return *s_state;
	}

	/// The largest Res<height> sub folder of 'folder' not above 'height' (else the smallest); empty: none.
	AsciiString resolutionFolder(const AsciiString &folder, Int height)
	{
		std::vector<AsciiString> names;
		listFolders(folder, names);
		Int best = -1, smallest = -1;
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (strnicmp(names[i].str(), "Res", 3) != 0) {
				continue;
			}
			const Int h = atoi(names[i].str() + 3);
			if (h <= 0) {
				continue;
			}
			if (h <= height && h > best) best = h;
			if (smallest < 0 || h < smallest) smallest = h;
		}
		const Int pick = best > 0 ? best : smallest;
		if (pick <= 0) {
			return AsciiString::TheEmptyString;
		}
		AsciiString result = folder;
		AsciiString sub;
		sub.format("Res%d\\", pick);
		result.concat(sub);
		return result;
	}

	/// The archives of 'pack' in mount order.
	void packArchives(const Packs::Pack &pack, const AsciiString &textLanguage, Int height, const AsciiString &exeFolder,
		std::vector<AsciiString> &out)
	{
		AsciiString gameFolder = pack.folder;
		gameFolder.concat(kGameFolder);
		gameFolder.concat("\\");
		const AsciiString levels[2] = { gameFolder, pack.folder };
		for (Int l = 0; l < 2; ++l)
		{
			if (!folderExists(levels[l].str())) {
				continue;
			}
			if (textLanguage.isNotEmpty())
			{
				AsciiString lang = levels[l];
				lang.concat("Lang");
				lang.concat(textLanguage);
				lang.concat("\\");
				listFiles(lang, "*.big", out);
			}
			const AsciiString res = resolutionFolder(levels[l], height);
			if (res.isNotEmpty()) {
				listFiles(res, "*.big", out);
			}
			listFiles(levels[l], "*.big", out);
		}
		for (size_t i = 0; i < pack.archives.size(); ++i)
		{
			const AsciiString &entry = pack.archives[i];
			const Bool absolute = entry.getLength() > 1 && (entry.getCharAt(1) == ':' || entry.getCharAt(0) == '\\');
			AsciiString path = absolute ? AsciiString::TheEmptyString : exeFolder;
			path.concat(entry);
			out.push_back(path);
		}
	}

	/// FALSE when a file the pack needs is missing: a Require line, or nothing at all for this game (E9: a renamed or
	/// deleted archive must give the same message as a damaged one).
	Bool packComplete(const Packs::Pack &pack, const std::vector<AsciiString> &archives)
	{
		for (size_t i = 0; i < pack.requiredFiles.size(); ++i)
		{
			AsciiString path = pack.folder;
			path.concat(pack.requiredFiles[i]);
			if (!fileExists(path.str()))
			{
				DEBUG_LOG(("Packs: '%s' misses %s.", pack.name.str(), path.str()));
				return FALSE;
			}
		}
		if (pack.empty || !archives.empty() || !pack.excludes.empty() || !pack.requiredFiles.empty()) {
			return TRUE;
		}
		AsciiString gameData = pack.folder;
		gameData.concat(kGameFolder);
		gameData.concat("\\Data");
		AsciiString data = pack.folder;
		data.concat("Data");
		return folderExists(gameData.str()) || folderExists(data.str());
	}

	const Packs::Pack *packAt(Packs::Kind kind, Int index)
	{
		KindState &k = state().kinds[kind];
		return (index >= 0 && index < (Int)k.packs.size()) ? &k.packs[index] : nullptr;
	}

	Int indexOf(Packs::Kind kind, const AsciiString &name)
	{
		KindState &k = state().kinds[kind];
		for (size_t i = 0; i < k.packs.size(); ++i) {
			if (stricmp(k.packs[i].name.str(), name.str()) == 0) {
				return (Int)i;
			}
		}
		return -1;
	}

	void addProblem(Packs::Kind kind, const AsciiString &name, Problem problem)
	{
		ProblemEntry e;
		e.kind = kind;
		e.name = name;
		e.problem = problem;
		state().problems.push_back(e);
		DEBUG_LOG(("Packs: %s pack '%s' not used (problem %d), the base data is used.", kFolders[kind], name.str(), (Int)problem));
	}

	UnicodeString text(const char *label, const WideChar *english, const WideChar *russian)
	{
		if (TheGameText == nullptr) {
			return UnicodeString(localizedFallback(english, russian));
		}
		return TheGameText->FETCH_OR_SUBSTITUTE(label, localizedFallback(english, russian));
	}

	UnicodeString titleOf(Packs::Kind kind)
	{
		switch (kind)
		{
			case Packs::KIND_TEXT: return text("GUI:PackKindText", L"Interface language", L"\x042F\x0437\x044B\x043A \x0438\x043D\x0442\x0435\x0440\x0444\x0435\x0439\x0441\x0430");
			case Packs::KIND_VOICE: return text("GUI:PackKindVoice", L"Voices", L"\x041E\x0437\x0432\x0443\x0447\x043A\x0430");
			case Packs::KIND_MOVIES: return text("GUI:PackKindMovies", L"Movie language", L"\x042F\x0437\x044B\x043A \x0432\x0438\x0434\x0435\x043E\x0440\x043E\x043B\x0438\x043A\x043E\x0432");
			case Packs::KIND_CONTROLBAR: return text("GUI:PackKindControlBar", L"Control bar", L"\x041F\x0430\x043D\x0435\x043B\x044C \x0443\x043F\x0440\x0430\x0432\x043B\x0435\x043D\x0438\x044F");
			default: return text("GUI:PackKindHotkeys", L"Hotkeys", L"\x0413\x043E\x0440\x044F\x0447\x0438\x0435 \x043A\x043B\x0430\x0432\x0438\x0448\x0438");
		}
	}
}

//-------------------------------------------------------------------------------------------------
const char *const Packs::BaseValue = "Base";

const char *Packs::kindKey(Kind kind) { return kKeys[kind]; }
const char *Packs::kindFolder(Kind kind) { return kFolders[kind]; }

void Packs::scan()
{
	if (s_state != nullptr) {
		return;
	}
	s_state = new State;
	State &st = *s_state;
	st.mountPrepared = FALSE;
	st.problemsShown = FALSE;
	char exeFolder[_MAX_PATH];
	exeFolderOf(exeFolder, ARRAY_SIZE(exeFolder));
	st.exeFolder = exeFolder;

	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		KindState &k = st.kinds[kind];
		k.showBase = TRUE;
		k.active = -1;
		k.fallback = -1;
		k.activeGroup = -1;
		k.fallbackGroup = -1;

		AsciiString kindFolderPath = st.exeFolder;
		kindFolderPath.concat("Packs\\");
		kindFolderPath.concat(kFolders[kind]);
		kindFolderPath.concat("\\");

		AsciiString defaultIni = kindFolderPath;
		defaultIni.concat("default.ini");
		readKeyValues(defaultIni.str(), [&](const AsciiString &key, const AsciiString &value)
		{
			if (keyIs(key, "Default")) k.defaultName = value;
			else if (keyIs(key, "ShowBase")) k.showBase = isYes(value);
			else if (keyIs(key, "BaseNameEnglish")) k.baseEnglish = fromUtf8(value);
			else if (keyIs(key, "BaseNameRussian")) k.baseRussian = fromUtf8(value);
			else if (keyIs(key, "BaseTextLanguage")) { k.baseTextLanguage = value; k.baseTextLanguage.toLower(); }
		});

		std::vector<AsciiString> names;
		listFolders(kindFolderPath, names);
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (stricmp(names[i].str(), BaseValue) == 0) {
				continue;   // reserved for the base data
			}
			Pack pack;
			if (readPack((Kind)kind, kindFolderPath, names[i], pack)) {
				k.packs.push_back(pack);
			}
		}
		if (k.packs.empty()) {
			k.showBase = TRUE;   // nothing else to choose
		}
	}
}

void Packs::shutdown()
{
	delete s_state;
	s_state = nullptr;
}

Int Packs::count(Kind kind) { return (Int)state().kinds[kind].packs.size(); }
const Packs::Pack *Packs::get(Kind kind, Int index) { return packAt(kind, index); }
const Packs::Pack *Packs::find(Kind kind, const AsciiString &name) { return packAt(kind, indexOf(kind, name)); }
Bool Packs::baseShown(Kind kind) { return state().kinds[kind].showBase; }

UnicodeString Packs::baseName(Kind kind)
{
	const KindState &k = state().kinds[kind];
	if (!k.baseEnglish.isEmpty() || !k.baseRussian.isEmpty())
	{
		const UnicodeString &first = isRussianText() ? k.baseRussian : k.baseEnglish;
		return !first.isEmpty() ? first : (isRussianText() ? k.baseEnglish : k.baseRussian);
	}
	return text("GUI:PackBase", L"Original", L"\x041E\x0440\x0438\x0433\x0438\x043D\x0430\x043B");
}

UnicodeString Packs::displayName(Kind kind, const Pack *pack)
{
	if (pack == nullptr) {
		return baseName(kind);
	}
	const UnicodeString &first = isRussianText() ? pack->nameRussian : pack->nameEnglish;
	const UnicodeString &second = isRussianText() ? pack->nameEnglish : pack->nameRussian;
	if (!first.isEmpty()) return first;
	if (!second.isEmpty()) return second;
	UnicodeString plain;
	plain.translate(pack->name);
	return plain;
}

UnicodeString Packs::kindTitle(Kind kind)
{
	return titleOf(kind);
}

AsciiString Packs::textLanguageOf(const Pack *textPack)
{
	if (textPack != nullptr && textPack->textLanguage.isNotEmpty()) {
		return textPack->textLanguage;
	}
	return state().kinds[KIND_TEXT].baseTextLanguage;
}

Bool Packs::hotkeysFit(const Pack *hotkeyPack, const AsciiString &textLanguage)
{
	if (hotkeyPack == nullptr || hotkeyPack->textLanguage.isEmpty() || textLanguage.isEmpty()) {
		return TRUE;
	}
	return stricmp(hotkeyPack->textLanguage.str(), textLanguage.str()) == 0;
}

AsciiString Packs::chosenName(Kind kind, OptionPreferences &pref)
{
	OptionPreferences::const_iterator it = pref.find(kKeys[kind]);
	AsciiString value = (it != pref.end()) ? it->second : state().kinds[kind].defaultName;
	value.trim();
	if (value.isEmpty() || stricmp(value.str(), BaseValue) == 0) {
		return AsciiString::TheEmptyString;
	}
	return value;
}

const Packs::Pack *Packs::active(Kind kind)
{
	return packAt(kind, state().kinds[kind].active);
}

//-------------------------------------------------------------------------------------------------
void Packs::prepareMount(std::vector<MountStep> &steps)
{
	State &st = state();
	steps.clear();
	if (st.mountPrepared) {
		return;
	}
	st.mountPrepared = TRUE;

	OptionPreferences pref;
	Int xres = 0, yres = 0;
	pref.getResolution(&xres, &yres);

	// the choices
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		KindState &k = st.kinds[kind];
		const AsciiString name = chosenName((Kind)kind, pref);
		if (name.isEmpty()) {
			continue;
		}
		k.active = indexOf((Kind)kind, name);
		if (k.active < 0) {
			addProblem((Kind)kind, name, PROBLEM_NOT_FOUND);
			continue;
		}
		const Pack &pack = k.packs[k.active];
		if (pack.fallback.isNotEmpty() && stricmp(pack.fallback.str(), pack.name.str()) != 0)
		{
			k.fallback = indexOf((Kind)kind, pack.fallback);
			if (k.fallback < 0) {
				addProblem((Kind)kind, pack.fallback, PROBLEM_NOT_FOUND);
			}
		}
	}
	const AsciiString textLanguage = textLanguageOf(active(KIND_TEXT));
	{
		KindState &k = st.kinds[KIND_HOTKEYS];
		if (k.active >= 0 && !hotkeysFit(&k.packs[k.active], textLanguage))
		{
			addProblem(KIND_HOTKEYS, k.packs[k.active].name, PROBLEM_WRONG_TEXT);
			k.active = -1;
			k.fallback = -1;
		}
	}

	// the archives, in mount order
	Int group = 0;
	for (Int o = 0; o < KIND_COUNT; ++o)
	{
		const Kind kind = kMountOrder[o];
		KindState &k = st.kinds[kind];
		for (Int which = 0; which < 2; ++which)
		{
			const Int index = (which == 0) ? k.active : (k.active >= 0 ? k.fallback : -1);
			if (index < 0) {
				continue;
			}
			std::vector<AsciiString> archives;
			packArchives(k.packs[index], textLanguage, yres, st.exeFolder, archives);
			if (!packComplete(k.packs[index], archives))
			{
				addProblem(kind, k.packs[index].name, PROBLEM_BROKEN);
				if (which == 0) {
					k.active = -1;   // the base data; its fallback is not used either
				} else {
					k.fallback = -1;
				}
				continue;
			}
			(which == 0 ? k.activeGroup : k.fallbackGroup) = group;
			for (size_t i = 0; i < archives.size(); ++i)
			{
				MountStep step;
				step.path = archives[i];
				step.group = group;
				steps.push_back(step);
				st.mounted.push_back(normalizedPath(archives[i].str()));
				st.mountedNames.push_back(archives[i]);
			}
			++group;
		}
	}
}

void Packs::groupFailed(Int group)
{
	State &st = state();
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		KindState &k = st.kinds[kind];
		if (k.activeGroup == group && k.active >= 0)
		{
			addProblem((Kind)kind, k.packs[k.active].name, PROBLEM_BROKEN);
			k.active = -1;   // the base data, and the fallback group is not mounted either
		}
		else if (k.fallbackGroup == group && k.fallback >= 0)
		{
			addProblem((Kind)kind, k.packs[k.fallback].name, PROBLEM_BROKEN);
			k.fallback = -1;
		}
	}
}

Bool Packs::groupWanted(Int group)
{
	State &st = state();
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		const KindState &k = st.kinds[kind];
		if (k.activeGroup == group) return k.active >= 0;
		if (k.fallbackGroup == group) return k.active >= 0 && k.fallback >= 0;
	}
	return FALSE;
}

Bool Packs::skipListEntry(const AsciiString &entry, const AsciiString &fullPath)
{
	State &st = state();
	const AsciiString line = normalizedEntry(entry);
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		const KindState &k = st.kinds[kind];
		const Int used[2] = { k.active, k.active >= 0 ? k.fallback : -1 };
		for (Int u = 0; u < 2; ++u)
		{
			if (used[u] < 0) {
				continue;
			}
			const Pack &pack = k.packs[used[u]];
			for (size_t i = 0; i < pack.excludes.size(); ++i) {
				if (stricmp(pack.excludes[i].str(), line.str()) == 0) {
					DEBUG_LOG(("Packs: the %s pack '%s' leaves out %s.", kFolders[kind], pack.name.str(), entry.str()));
					st.leftOut.push_back(entry);
					return TRUE;
				}
			}
		}
	}
	// an archive a pack in use has mounted already
	const AsciiString full = normalizedPath(fullPath.str());
	for (size_t i = 0; i < st.mounted.size(); ++i) {
		if (st.mounted[i] == full) {
			return TRUE;
		}
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool Packs::fullViewport()
{
	const Pack *pack = active(KIND_CONTROLBAR);
	return pack != nullptr && pack->fullViewport;
}

void Packs::getMovieRoots(std::vector<AsciiString> &roots)
{
	roots.clear();
	const KindState &k = state().kinds[KIND_MOVIES];
	const Int used[2] = { k.active, k.active >= 0 ? k.fallback : -1 };
	for (Int u = 0; u < 2; ++u)
	{
		if (used[u] < 0) {
			continue;
		}
		AsciiString game = k.packs[used[u]].folder;
		game.concat(kGameFolder);
		game.concat("\\");
		roots.push_back(game);
		roots.push_back(k.packs[used[u]].folder);
	}
}

AsciiString Packs::findFontFile(const AsciiString &fileName)
{
	if (fileName.isEmpty() || strchr(fileName.str(), '\\') != nullptr || strchr(fileName.str(), '/') != nullptr) {
		return AsciiString::TheEmptyString;
	}
	State &st = state();
	for (Int o = 0; o < KIND_COUNT; ++o)
	{
		const KindState &k = st.kinds[kMountOrder[o]];
		const Int used[2] = { k.active, k.active >= 0 ? k.fallback : -1 };
		for (Int u = 0; u < 2; ++u)
		{
			if (used[u] < 0) {
				continue;
			}
			AsciiString inGame = k.packs[used[u]].folder;
			inGame.concat(kGameFolder);
			inGame.concat("\\");
			inGame.concat(fileName);
			if (fileExists(inGame.str())) return inGame;
			AsciiString inPack = k.packs[used[u]].folder;
			inPack.concat(fileName);
			if (fileExists(inPack.str())) return inPack;
		}
	}
	return AsciiString::TheEmptyString;
}

const char *Packs::findPreviewPath(const char *textureName)
{
	if (s_state == nullptr || textureName == nullptr || strnicmp(textureName, "packpreview_", 12) != 0) {
		return nullptr;
	}
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		const KindState &k = s_state->kinds[kind];
		for (size_t i = 0; i < k.packs.size(); ++i) {
			if (k.packs[i].previewTexture.isNotEmpty() && stricmp(k.packs[i].previewTexture.str(), textureName) == 0) {
				return k.packs[i].previewFile.str();
			}
		}
	}
	return nullptr;
}

Bool Packs::findSplash(const char *fileName, char *path, Int pathSize)
{
	// Before the engine: a light read that keeps nothing.
	char exeFolder[_MAX_PATH];
	exeFolderOf(exeFolder, ARRAY_SIZE(exeFolder));
	AsciiString name;
	{
		OptionPreferences pref;
		OptionPreferences::const_iterator it = pref.find(kKeys[KIND_TEXT]);
		if (it != pref.end()) {
			name = it->second;
		}
	}
	if (name.isEmpty())
	{
		AsciiString defaultIni(exeFolder);
		defaultIni.concat("Packs\\Text\\default.ini");
		readKeyValues(defaultIni.str(), [&](const AsciiString &key, const AsciiString &value)
		{
			if (keyIs(key, "Default")) name = value;
		});
	}
	name.trim();
	if (name.isEmpty() || stricmp(name.str(), BaseValue) == 0 || strchr(name.str(), '\\') != nullptr || strchr(name.str(), '/') != nullptr) {
		return FALSE;
	}
	char candidate[_MAX_PATH];
	snprintf(candidate, ARRAY_SIZE(candidate), "%sPacks\\Text\\%s\\%s\\%s", exeFolder, name.str(), kGameFolder, fileName);
	if (!fileExists(candidate)) {
		snprintf(candidate, ARRAY_SIZE(candidate), "%sPacks\\Text\\%s\\%s", exeFolder, name.str(), fileName);
	}
	if (!fileExists(candidate)) {
		return FALSE;
	}
	strlcpy(path, candidate, pathSize);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void Packs::writeLog(const std::vector<AsciiString> &files, const std::vector<AsciiString> &origins)
{
	if (TheGlobalData == nullptr) {
		return;
	}
	State &st = state();
	AsciiString path = TheGlobalData->getPath_UserData();
	path.concat("Packs.log");
	FILE *fp = fopen(path.str(), "w");
	if (fp == nullptr) {
		return;
	}
	fprintf(fp, "Packs of this run (%s)\n", kGameFolder);
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		const KindState &k = st.kinds[kind];
		const Pack *pack = packAt((Kind)kind, k.active);
		const Pack *fallback = (k.active >= 0) ? packAt((Kind)kind, k.fallback) : nullptr;
		fprintf(fp, "%s = %s%s%s\n", kKeys[kind], pack != nullptr ? pack->name.str() : "Base",
			fallback != nullptr ? ", fallback " : "", fallback != nullptr ? fallback->name.str() : "");
	}
	// TheSuperHackers @feature What the options offer for each kind (H5): "Base" when the base data is shown, then every
	// pack of this game; a pack whose Require file is missing says which (it is offered, and reported when chosen).
	for (Int kind = 0; kind < KIND_COUNT; ++kind)
	{
		const KindState &k = st.kinds[kind];
		fprintf(fp, "choices %s:%s", kKeys[kind], k.showBase ? " Base" : "");
		for (size_t i = 0; i < k.packs.size(); ++i)
		{
			const Pack &pack = k.packs[i];
			fprintf(fp, " %s", pack.name.str());
			for (size_t r = 0; r < pack.requiredFiles.size(); ++r)
			{
				AsciiString required = pack.folder;
				required.concat(pack.requiredFiles[r]);
				if (!fileExists(required.str()))
				{
					fprintf(fp, " (missing %s)", pack.requiredFiles[r].str());
					break;
				}
			}
		}
		fprintf(fp, "\n");
	}
	for (size_t i = 0; i < st.mountedNames.size(); ++i) {
		fprintf(fp, "mounted: %s\n", st.mountedNames[i].str());
	}
	for (size_t i = 0; i < st.leftOut.size(); ++i) {
		fprintf(fp, "left out of the list: %s\n", st.leftOut[i].str());
	}
	for (size_t i = 0; i < st.problems.size(); ++i)
	{
		const ProblemEntry &e = st.problems[i];
		fprintf(fp, "not used: %s %s (%s)\n", kFolders[e.kind], e.name.str(),
			e.problem == PROBLEM_NOT_FOUND ? "not found" : (e.problem == PROBLEM_BROKEN ? "a file is missing or damaged" : "does not fit the text language"));
	}
	for (size_t i = 0; i < files.size() && i < origins.size(); ++i) {
		fprintf(fp, "%s is read from %s\n", files[i].str(), origins[i].isEmpty() ? "(no archive)" : origins[i].str());
	}
	fclose(fp);
}

//-------------------------------------------------------------------------------------------------
void Packs::showProblemsOnce()
{
	if (s_state == nullptr || s_state->problemsShown || s_state->problems.empty()) {
		return;
	}
	if (TheShell == nullptr || !TheShell->isShellActive() || TheShell->getScreenCount() <= 0 || !TheShell->isAnimFinished()) {
		return;
	}
	if (TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()) {
		return;
	}
	s_state->problemsShown = TRUE;

	UnicodeString body = text("GUI:PackProblemBody",
		L"These packs could not be used; the original game data is used instead:",
		L"\x042D\x0442\x0438 \x043F\x0430\x043A\x0435\x0442\x044B \x043D\x0435 \x0443\x0434\x0430\x043B\x043E\x0441\x044C \x0438\x0441\x043F\x043E\x043B\x044C\x0437\x043E\x0432\x0430\x0442\x044C; \x0432\x043C\x0435\x0441\x0442\x043E \x043D\x0438\x0445 \x0438\x0441\x043F\x043E\x043B\x044C\x0437\x0443\x044E\x0442\x0441\x044F \x0438\x0441\x0445\x043E\x0434\x043D\x044B\x0435 \x0434\x0430\x043D\x043D\x044B\x0435 \x0438\x0433\x0440\x044B:");
	for (size_t i = 0; i < s_state->problems.size(); ++i)
	{
		const ProblemEntry &e = s_state->problems[i];
		UnicodeString reason;
		switch (e.problem)
		{
			case PROBLEM_NOT_FOUND:
				reason = text("GUI:PackProblemNotFound", L"not found", L"\x043D\x0435 \x043D\x0430\x0439\x0434\x0435\x043D");
				break;
			case PROBLEM_BROKEN:
				reason = text("GUI:PackProblemBroken", L"a file is missing or damaged", L"\x0444\x0430\x0439\x043B \x043E\x0442\x0441\x0443\x0442\x0441\x0442\x0432\x0443\x0435\x0442 \x0438\x043B\x0438 \x043F\x043E\x0432\x0440\x0435\x0436\x0434\x0451\x043D");
				break;
			default:
				reason = text("GUI:PackProblemWrongText", L"does not fit the interface language", L"\x043D\x0435 \x043F\x043E\x0434\x0445\x043E\x0434\x0438\x0442 \x043A \x044F\x0437\x044B\x043A\x0443 \x0438\x043D\x0442\x0435\x0440\x0444\x0435\x0439\x0441\x0430");
				break;
		}
		UnicodeString name;
		name.translate(e.name);
		UnicodeString line;
		line.format(L"\n%ls: %ls (%ls)", titleOf(e.kind).str(), name.str(), reason.str());
		body.concat(line);
	}
	MessageBoxOk(text("GUI:PackProblemTitle", L"Packs", L"\x041F\x0430\x043A\x0435\x0442\x044B"), body, nullptr);
}

//-------------------------------------------------------------------------------------------------
void Packs::requestRestart()
{
	// A test run (frame shots, -uiScript) only quits: a new process would play the same script again.
	s_restart = !FrameShots::isEnabled();
	DEBUG_LOG(("Packs: restart requested%s.", s_restart ? "" : " (test run: quit only)"));
	if (TheGameEngine != nullptr) {
		TheGameEngine->setQuitting(TRUE);
	}
}

void Packs::restartIfRequested()
{
	if (!s_restart) {
		return;
	}
	s_restart = FALSE;
	// The new process must be the first instance again, so this one lets go of its instance first.
	rts::ClientInstance::release();

	wchar_t exePath[MAX_PATH];
	const DWORD len = GetModuleFileNameW(nullptr, exePath, ARRAY_SIZE(exePath));
	if (len == 0 || len >= ARRAY_SIZE(exePath)) {
		return;
	}
	static wchar_t commandLine[32768];
	wcsncpy(commandLine, GetCommandLineW(), ARRAY_SIZE(commandLine) - 1);
	commandLine[ARRAY_SIZE(commandLine) - 1] = L'\0';
	wchar_t currentFolder[MAX_PATH];
	const DWORD folderLen = GetCurrentDirectoryW(ARRAY_SIZE(currentFolder), currentFolder);

	STARTUPINFOW startup;
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process;
	memset(&process, 0, sizeof(process));
	if (CreateProcessW(exePath, commandLine, nullptr, nullptr, FALSE, 0, nullptr,
		(folderLen > 0 && folderLen < ARRAY_SIZE(currentFolder)) ? currentFolder : nullptr, &startup, &process))
	{
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
}
