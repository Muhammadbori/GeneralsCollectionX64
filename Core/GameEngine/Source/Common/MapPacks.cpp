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

// MapPacks.cpp /////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See MapPacks.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/MapPacks.h"

#include "Common/CRC.h"
#include "Common/FrameShots.h"
#include "GameNetwork/NetLab.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Shell.h"
#include "GameLogic/GameLogic.h"

#include <algorithm>
#include <stdio.h>
#include <vector>

namespace
{
#if RTS_GENERALS
	const char *const kGameFolder = "Generals";
#else
	const char *const kGameFolder = "ZeroHour";
#endif

	struct Pack
	{
		AsciiString name;     ///< the folder name
		AsciiString stamp;
		UnicodeString nameEnglish, nameRussian;
	};

	struct Item
	{
		Int pack;
		AsciiString folder;   ///< the map folder name
		AsciiString source;   ///< the pack's map folder, with a trailing backslash
		Bool update;          ///< replaces an earlier installed copy
		UnsignedInt sourceCrc;
	};

	struct Record
	{
		AsciiString pack, folder;
		UnsignedInt crc;
	};

	enum Phase { PHASE_WAIT, PHASE_ASKING, PHASE_INSTALLING, PHASE_DONE };

	struct State
	{
		Phase phase = PHASE_WAIT;
		Bool testRuns = FALSE;
		std::vector<Pack> packs;
		std::vector<Item> items;
		std::vector<Record> records;
		std::vector<std::pair<AsciiString, AsciiString> > stamps;   ///< pack, recorded stamp
		size_t next = 0;
		Int installed = 0, updated = 0, leftAlone = 0;
		std::vector<UnicodeString> failures;
		Bool stopRequested = FALSE;
		GameWindow *progressBox = nullptr;
		AsciiString userMaps;   ///< with a trailing backslash
	};
	State *s_state = nullptr;

	State &state()
	{
		if (s_state == nullptr) {
			s_state = new State;
		}
		return *s_state;
	}

	UnicodeString text(const char *label, const WideChar *english, const WideChar *russian)
	{
		return TheGameText->FETCH_OR_SUBSTITUTE(label, localizedFallback(english, russian));
	}

	UnicodeString fromUtf8(const AsciiString &value)
	{
		WideChar buffer[512];
		const int n = MultiByteToWideChar(CP_UTF8, 0, value.str(), -1, buffer, ARRAY_SIZE(buffer));
		if (n <= 0) {
			UnicodeString plain;
			plain.translate(value);
			return plain;
		}
		return UnicodeString(buffer);
	}

	UnicodeString fromAnsi(const AsciiString &value)
	{
		WideChar buffer[512];
		const int n = MultiByteToWideChar(CP_ACP, 0, value.str(), -1, buffer, ARRAY_SIZE(buffer));
		if (n <= 0) {
			UnicodeString plain;
			plain.translate(value);
			return plain;
		}
		return UnicodeString(buffer);
	}

	/// TheSuperHackers @bugfix (M2) A path for the file functions that is not limited to 260 characters: the full path
	/// with the \\?\ prefix (a deep game folder plus a long map name can pass 260, and the copy failed with "path
	/// not found").
	AsciiString longPath(const char *path)
	{
		if (strncmp(path, "\\\\?\\", 4) == 0 || strncmp(path, "\\\\", 2) == 0) {
			return AsciiString(path);   // already long, or a network path (left as it is)
		}
		static char full[32768];
		const DWORD n = GetFullPathNameA(path, sizeof(full), full, nullptr);
		if (n == 0 || n >= sizeof(full)) {
			return AsciiString(path);
		}
		AsciiString out("\\\\?\\");
		out.concat(full);
		return out;
	}

	Bool isDirectory(const char *path)
	{
		const DWORD a = GetFileAttributesA(longPath(path).str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	/// The names in 'folder' (with a trailing backslash): sub folders or files, sorted.
	void listNames(const AsciiString &folder, Bool directories, std::vector<AsciiString> &out)
	{
		AsciiString pattern = longPath(folder.str());
		pattern.concat("*");
		WIN32_FIND_DATAA data;
		HANDLE find = FindFirstFileA(pattern.str(), &data);
		if (find == INVALID_HANDLE_VALUE) {
			return;
		}
		do
		{
			if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
				continue;
			}
			const Bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (dir == directories) {
				out.push_back(AsciiString(data.cFileName));
			}
		} while (FindNextFileA(find, &data));
		FindClose(find);
		std::sort(out.begin(), out.end(), [](const AsciiString &a, const AsciiString &b) { return stricmp(a.str(), b.str()) < 0; });
	}

	/// The CRC of a map folder: its file names (lower case, sorted, sub folders included) and contents.
	void addFolderCrc(CRC &crc, const AsciiString &folder, const AsciiString &prefix)
	{
		std::vector<AsciiString> files;
		listNames(folder, FALSE, files);
		for (size_t i = 0; i < files.size(); ++i)
		{
			AsciiString name = prefix;
			name.concat(files[i]);
			name.toLower();
			crc.computeCRC(name.str(), name.getLength());
			AsciiString path = folder;
			path.concat(files[i]);
			HANDLE file = CreateFileA(longPath(path.str()).str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE)
			{
				static char buffer[65536];
				DWORD got = 0;
				while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
					crc.computeCRC(buffer, (Int)got);
				}
				CloseHandle(file);
			}
		}
		std::vector<AsciiString> dirs;
		listNames(folder, TRUE, dirs);
		for (size_t i = 0; i < dirs.size(); ++i)
		{
			AsciiString sub = folder;
			sub.concat(dirs[i]);
			sub.concat("\\");
			AsciiString subPrefix = prefix;
			subPrefix.concat(dirs[i]);
			subPrefix.concat("/");
			addFolderCrc(crc, sub, subPrefix);
		}
	}

	UnsignedInt folderCrc(const AsciiString &folder)
	{
		CRC crc;
		addFolderCrc(crc, folder, AsciiString::TheEmptyString);
		return crc.get();
	}

	AsciiString exeFolder()
	{
		char path[_MAX_PATH] = { 0 };
		GetModuleFileNameA(nullptr, path, ARRAY_SIZE(path));
		char *slash = strrchr(path, '\\');
		if (slash != nullptr) {
			slash[1] = '\0';
		}
		return AsciiString(path);
	}

	AsciiString trimmed(const char *from, const char *to)
	{
		AsciiString s;
		s.set(from, (Int)(to - from));
		s.trim();
		return s;
	}

	void readPackIni(const AsciiString &path, Pack &pack)
	{
		FILE *fp = fopen(path.str(), "rb");
		if (fp == nullptr) {
			return;
		}
		char line[1024];
		Bool first = TRUE;
		while (fgets(line, sizeof(line), fp) != nullptr)
		{
			char *t = line;
			if (first && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) {
				t += 3;
			}
			first = FALSE;
			const char *eq = strchr(t, '=');
			if (eq == nullptr || t[0] == ';' || t[0] == '#') {
				continue;
			}
			const AsciiString key = trimmed(t, eq);
			const AsciiString value = trimmed(eq + 1, eq + strlen(eq));
			if (stricmp(key.str(), "NameEnglish") == 0) pack.nameEnglish = fromUtf8(value);
			else if (stricmp(key.str(), "NameRussian") == 0) pack.nameRussian = fromUtf8(value);
			else if (stricmp(key.str(), "Stamp") == 0) pack.stamp = value;
		}
		fclose(fp);
	}

	UnicodeString displayName(const Pack &pack)
	{
		const UnicodeString first(localizedFallback(pack.nameEnglish.str(), pack.nameRussian.str()));
		if (!first.isEmpty()) {
			return first;
		}
		if (!pack.nameEnglish.isEmpty()) {
			return pack.nameEnglish;
		}
		return fromAnsi(pack.name);
	}

	//--- the record of installed maps: <user data>\MapPacks.ini -----------------------------------
	AsciiString recordPath()
	{
		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat("MapPacks.ini");
		return path;
	}

	void readRecord(State &st)
	{
		FILE *fp = fopen(recordPath().str(), "rb");
		if (fp == nullptr) {
			return;
		}
		char line[1024];
		while (fgets(line, sizeof(line), fp) != nullptr)
		{
			char *end = line + strlen(line);
			while (end > line && (end[-1] == '\n' || end[-1] == '\r')) {
				*--end = '\0';
			}
			std::vector<AsciiString> parts;
			const char *from = line;
			for (const char *p = line; ; ++p)
			{
				if (*p == '\t' || *p == '\0')
				{
					AsciiString part;
					part.set(from, (Int)(p - from));
					parts.push_back(part);
					from = p + 1;
					if (*p == '\0') {
						break;
					}
				}
			}
			if (parts.size() == 3 && parts[0] == "stamp") {
				st.stamps.push_back(std::make_pair(parts[1], parts[2]));
			}
			else if (parts.size() == 4 && parts[0] == "map")
			{
				Record r;
				r.pack = parts[1];
				r.folder = parts[2];
				r.crc = (UnsignedInt)strtoul(parts[3].str(), nullptr, 16);
				st.records.push_back(r);
			}
		}
		fclose(fp);
	}

	void writeRecord(const State &st)
	{
		FILE *fp = fopen(recordPath().str(), "wb");
		if (fp == nullptr) {
			return;
		}
		fprintf(fp, "; Maps installed from the map packs of the game (MapPacks.h). Do not edit: a map whose files differ from\r\n");
		fprintf(fp, "; its line here counts as changed by the player and is never replaced.\r\n");
		for (size_t i = 0; i < st.stamps.size(); ++i) {
			fprintf(fp, "stamp\t%s\t%s\r\n", st.stamps[i].first.str(), st.stamps[i].second.str());
		}
		for (size_t i = 0; i < st.records.size(); ++i) {
			fprintf(fp, "map\t%s\t%s\t%08X\r\n", st.records[i].pack.str(), st.records[i].folder.str(), st.records[i].crc);
		}
		fclose(fp);
	}

	Record *findRecord(State &st, const AsciiString &pack, const AsciiString &folder)
	{
		for (size_t i = 0; i < st.records.size(); ++i) {
			if (stricmp(st.records[i].pack.str(), pack.str()) == 0 && stricmp(st.records[i].folder.str(), folder.str()) == 0) {
				return &st.records[i];
			}
		}
		return nullptr;
	}

	const AsciiString *recordedStamp(const State &st, const AsciiString &pack)
	{
		for (size_t i = 0; i < st.stamps.size(); ++i) {
			if (stricmp(st.stamps[i].first.str(), pack.str()) == 0) {
				return &st.stamps[i].second;
			}
		}
		return nullptr;
	}

	void setStamp(State &st, const AsciiString &pack, const AsciiString &stamp)
	{
		for (size_t i = 0; i < st.stamps.size(); ++i) {
			if (stricmp(st.stamps[i].first.str(), pack.str()) == 0) {
				st.stamps[i].second = stamp;
				return;
			}
		}
		st.stamps.push_back(std::make_pair(pack, stamp));
	}

	//--- the scan ---------------------------------------------------------------------------------
	void scan(State &st)
	{
		st.userMaps = TheMapCache != nullptr ? TheMapCache->getUserMapDir() : AsciiString::TheEmptyString;
		if (st.userMaps.isEmpty())
		{
			st.userMaps = TheGlobalData->getPath_UserData();
			st.userMaps.concat("Maps");
		}
		st.userMaps.concat("\\");
		readRecord(st);

		AsciiString root = exeFolder();
		root.concat("Packs\\Maps\\");
		std::vector<AsciiString> packNames;
		listNames(root, TRUE, packNames);
		std::vector<AsciiString> seen;   // a map folder offered once, by the first pack that has it
		for (size_t p = 0; p < packNames.size(); ++p)
		{
			Pack pack;
			pack.name = packNames[p];
			AsciiString folder = root;
			folder.concat(pack.name);
			folder.concat("\\");
			AsciiString ini = folder;
			ini.concat("pack.ini");
			readPackIni(ini, pack);
			AsciiString maps = folder;
			maps.concat(kGameFolder);
			maps.concat("\\Maps\\");
			std::vector<AsciiString> mapNames;
			listNames(maps, TRUE, mapNames);
			if (mapNames.empty()) {
				continue;
			}
			const Int packIndex = (Int)st.packs.size();
			st.packs.push_back(pack);
			const AsciiString *oldStamp = recordedStamp(st, pack.name);
			const Bool stampChanged = oldStamp != nullptr && *oldStamp != pack.stamp;
			for (size_t m = 0; m < mapNames.size(); ++m)
			{
				Bool dup = FALSE;
				for (size_t k = 0; k < seen.size() && !dup; ++k) {
					dup = stricmp(seen[k].str(), mapNames[m].str()) == 0;
				}
				if (dup) {
					continue;
				}
				seen.push_back(mapNames[m]);
				Item item;
				item.pack = packIndex;
				item.folder = mapNames[m];
				item.source = maps;
				item.source.concat(mapNames[m]);
				item.source.concat("\\");
				item.update = FALSE;
				item.sourceCrc = 0;
				AsciiString target = st.userMaps;
				target.concat(mapNames[m]);
				if (!isDirectory(target.str()))
				{
					st.items.push_back(item);
					continue;
				}
				// installed before from this pack, the pack changed since, its files differ from what was installed, and the
				// player's copy is still that installed one (a copy the player changed is not offered at all)
				const Record *r = findRecord(st, pack.name, mapNames[m]);
				if (r != nullptr && stampChanged)
				{
					item.sourceCrc = folderCrc(item.source);
					AsciiString installed = target;
					installed.concat("\\");
					if (item.sourceCrc != r->crc && folderCrc(installed) == r->crc)
					{
						item.update = TRUE;
						st.items.push_back(item);
					}
				}
			}
		}
	}

	//--- the boxes --------------------------------------------------------------------------------
	Int textWidth(GameWindow *window, const UnicodeString &str)
	{
		if (window == nullptr || TheDisplayStringManager == nullptr) {
			return 0;
		}
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return 0;
		}
		ds->setFont(window->winGetFont());
		ds->setText(str);
		Int w = 0, h = 0;
		ds->getSize(&w, &h);
		TheDisplayStringManager->freeDisplayString(ds);
		return w;
	}

	GameWindow *boxChild(GameWindow *box, const char *name)
	{
		return box != nullptr ? TheWindowManager->winGetWindowFromId(box, (Int)TheNameKeyGenerator->nameToKey(AsciiString(name))) : nullptr;
	}

	/// A button's text: the long one when it fits the button, else the short one.
	void setButtonText(GameWindow *box, const char *name, const UnicodeString &longText, const UnicodeString &shortText)
	{
		GameWindow *button = boxChild(box, name);
		if (button == nullptr) {
			return;
		}
		Int w = 0, h = 0;
		button->winGetSize(&w, &h);
		GadgetButtonSetText(button, textWidth(button, longText) <= w - 12 ? longText : shortText);
	}

	/// TheSuperHackers @bugfix (M1) The question's three buttons sized by their texts and spread over the box (the
	/// box's own button cells are too narrow for "Установить / Позже / Больше не спрашивать"); a short text is used
	/// only when the long ones cannot fit side by side.
	void layoutButtons(GameWindow *box, const UnicodeString longText[3], const UnicodeString shortText[3])
	{
		static const char *const names[3] = { "MessageBox.wnd:ButtonYes", "MessageBox.wnd:ButtonNo", "MessageBox.wnd:ButtonCancel" };
		GameWindow *button[3];
		for (Int i = 0; i < 3; ++i)
		{
			button[i] = boxChild(box, names[i]);
			if (button[i] == nullptr) return;
		}
		GameWindow *parent = button[0]->winGetParent();
		if (parent == nullptr) return;
		Int pw = 0, ph = 0, bx = 0, by = 0, bw = 0, bh = 0;
		parent->winGetSize(&pw, &ph);
		button[0]->winGetPosition(&bx, &by);
		button[0]->winGetSize(&bw, &bh);
		const Int pad = max(16, bh / 2);
		const Int margin = max(8, pw / 40);
		const Int gap = max(6, pw / 60);
		UnicodeString chosen[3];
		Int width[3];
		Int total = 0;
		for (Int i = 0; i < 3; ++i)
		{
			chosen[i] = longText[i];
			width[i] = max(bw / 2, textWidth(button[i], chosen[i]) + pad);
			total += width[i];
		}
		// too wide: the short texts, longest first, until it fits
		for (Int pass = 0; pass < 3 && total + 2 * gap + 2 * margin > pw; ++pass)
		{
			Int widest = 0;
			for (Int i = 1; i < 3; ++i) if (width[i] > width[widest]) widest = i;
			const Int w = max(bw / 2, textWidth(button[widest], shortText[widest]) + pad);
			total += w - width[widest];
			width[widest] = w;
			chosen[widest] = shortText[widest];
		}
		if (total + 2 * gap + 2 * margin > pw)
		{
			// still too wide: three equal cells (the text may be cut)
			const Int w = (pw - 2 * gap - 2 * margin) / 3;
			for (Int i = 0; i < 3; ++i) width[i] = w;
			total = 3 * w;
		}
		Int x = (pw - total - 2 * gap) / 2;
		for (Int i = 0; i < 3; ++i)
		{
			button[i]->winSetPosition(x, by);
			button[i]->winSetSize(width[i], bh);
			GadgetButtonSetText(button[i], chosen[i]);
			x += width[i] + gap;
		}
	}

	UnicodeString title()
	{
		return text("GUI:MapPacksTitle", L"Maps", L"\x041A\x0430\x0440\x0442\x044B");
	}

	void onInstall();
	void onLater();
	void onNever();
	void onStop();

	void ask(State &st)
	{
		Int fresh = 0, updates = 0;
		std::vector<Int> packsUsed;
		for (size_t i = 0; i < st.items.size(); ++i)
		{
			(st.items[i].update ? updates : fresh) += 1;
			if (std::find(packsUsed.begin(), packsUsed.end(), st.items[i].pack) == packsUsed.end()) {
				packsUsed.push_back(st.items[i].pack);
			}
		}
		UnicodeString updatePart;
		if (updates > 0) {
			updatePart = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:MapPacksUpdates", localizedFallback(L", %d updated", L", \x043E\x0431\x043D\x043E\x0432\x043B\x0451\x043D\x043D\x044B\x0445 %d"), updates);
		}
		UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:MapPacksQuestion",
			localizedFallback(L"Community maps are ready to install: %d new%ls.", L"\x0413\x043E\x0442\x043E\x0432\x044B \x043A \x0443\x0441\x0442\x0430\x043D\x043E\x0432\x043A\x0435 \x043A\x0430\x0440\x0442\x044B \x0441\x043E\x043E\x0431\x0449\x0435\x0441\x0442\x0432\x0430: \x043D\x043E\x0432\x044B\x0445 %d%ls."),
			fresh, updatePart.str());
		UnicodeString names;
		for (size_t i = 0; i < packsUsed.size(); ++i)
		{
			if (i > 0) {
				names.concat(L", ");
			}
			names.concat(displayName(st.packs[packsUsed[i]]));
		}
		UnicodeString line;
		line.format(L"\n(%ls)\n", names.str());
		body.concat(line);
		body.concat(text("GUI:MapPacksQuestion2",
			L"They are copied into your maps folder. Your own maps are never changed or removed.",
			L"\x041E\x043D\x0438 \x0431\x0443\x0434\x0443\x0442 \x0441\x043A\x043E\x043F\x0438\x0440\x043E\x0432\x0430\x043D\x044B \x0432 \x0432\x0430\x0448\x0443 \x043F\x0430\x043F\x043A\x0443 \x043A\x0430\x0440\x0442. \x0412\x0430\x0448\x0438 \x0441\x043E\x0431\x0441\x0442\x0432\x0435\x043D\x043D\x044B\x0435 \x043A\x0430\x0440\x0442\x044B \x043D\x0435 \x0438\x0437\x043C\x0435\x043D\x044F\x044E\x0442\x0441\x044F \x0438 \x043D\x0435 \x0443\x0434\x0430\x043B\x044F\x044E\x0442\x0441\x044F."));
		GameWindow *box = MessageBoxYesNoCancel(title(), body, onInstall, onLater, onNever);
		{
			const UnicodeString longText[3] = { text("GUI:MapPacksInstall", L"Install", L"\x0423\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C"),
				text("GUI:MapPacksLater", L"Later", L"\x041F\x043E\x0437\x0436\x0435"),
				text("GUI:MapPacksNever", L"Do not ask again", L"\x0411\x043E\x043B\x044C\x0448\x0435 \x043D\x0435 \x0441\x043F\x0440\x0430\x0448\x0438\x0432\x0430\x0442\x044C") };
			const UnicodeString shortText[3] = { text("GUI:MapPacksInstallShort", L"Install", L"\x0423\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C"),
				text("GUI:MapPacksLater", L"Later", L"\x041F\x043E\x0437\x0436\x0435"),
				text("GUI:MapPacksNeverShort", L"Never", L"\x041D\x0435 \x0441\x043F\x0440\x0430\x0448\x0438\x0432\x0430\x0442\x044C") };
			layoutButtons(box, longText, shortText);
		}
		st.phase = PHASE_ASKING;
	}

	void showProgress(State &st)
	{
		UnicodeString body;
		const size_t total = st.items.size();
		const size_t at = st.next < total ? st.next : total;
		body.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapPacksProgress", localizedFallback(L"Installing maps: %d of %d", L"\x0423\x0441\x0442\x0430\x043D\x043E\x0432\x043A\x0430 \x043A\x0430\x0440\x0442: %d \x0438\x0437 %d")).str(),
			(Int)at, (Int)total);
		if (st.next < total)
		{
			body.concat(L"\n");
			body.concat(fromAnsi(st.items[st.next].folder));
		}
		if (st.progressBox == nullptr)
		{
			st.progressBox = MessageBoxCancel(title(), body, onStop);
			setButtonText(st.progressBox, "MessageBox.wnd:ButtonCancel", text("GUI:MapPacksStop", L"Stop", L"\x041E\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C"),
				text("GUI:MapPacksStop", L"Stop", L"\x041E\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C"));
			return;
		}
		if (GameWindow *message = boxChild(st.progressBox, "MessageBox.wnd:StaticTextMessage")) {
			GadgetStaticTextSetText(message, body);
		}
	}

	UnicodeString lastErrorText(DWORD error)
	{
		char buffer[512] = { 0 };
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, buffer, sizeof(buffer), nullptr);
		AsciiString msg(buffer);
		msg.trim();
		UnicodeString out = fromAnsi(msg);
		if (out.isEmpty()) {
			out.format(L"error %u", (unsigned)error);
		}
		return out;
	}

	/// Copies the files of 'from' into 'to' (both with a trailing backslash), sub folders too; the .map file last, so a
	/// half copied map has no .map and the game does not list it. 'overwrite': replace files of the same name. FALSE
	/// and 'error' on the first failure.
	Bool copyFolder(const AsciiString &from, const AsciiString &to, Bool overwrite, DWORD *error)
	{
		if (!isDirectory(to.str()) && !CreateDirectoryA(longPath(to.str()).str(), nullptr))
		{
			*error = GetLastError();
			return FALSE;
		}
		std::vector<AsciiString> dirs;
		listNames(from, TRUE, dirs);
		for (size_t i = 0; i < dirs.size(); ++i)
		{
			AsciiString a = from, b = to;
			a.concat(dirs[i]); a.concat("\\");
			b.concat(dirs[i]); b.concat("\\");
			if (!copyFolder(a, b, overwrite, error)) {
				return FALSE;
			}
		}
		std::vector<AsciiString> files;
		listNames(from, FALSE, files);
		std::stable_partition(files.begin(), files.end(), [](const AsciiString &f) { return !f.endsWithNoCase(".map"); });
		for (size_t i = 0; i < files.size(); ++i)
		{
			AsciiString a = from, b = to;
			a.concat(files[i]);
			b.concat(files[i]);
			if (!CopyFileA(longPath(a.str()).str(), longPath(b.str()).str(), overwrite ? FALSE : TRUE))
			{
				*error = GetLastError();
				return FALSE;
			}
		}
		return TRUE;
	}

	void installOne(State &st, const Item &item)
	{
		const Pack &pack = st.packs[item.pack];
		AsciiString target = st.userMaps;
		target.concat(item.folder);
		target.concat("\\");
		DWORD error = 0;
		if (item.update)
		{
			// replace only the copy installed before, unchanged by the player
			Record *r = findRecord(st, pack.name, item.folder);
			if (r == nullptr || folderCrc(target) != r->crc)
			{
				++st.leftAlone;
				return;
			}
			if (copyFolder(item.source, target, TRUE, &error))
			{
				r->crc = folderCrc(target);
				++st.updated;
				return;
			}
		}
		else
		{
			if (isDirectory(target.str()))
			{
				++st.leftAlone;   // appeared meanwhile: it is the player's
				return;
			}
			if (copyFolder(item.source, target, FALSE, &error))
			{
				Record rec;
				rec.pack = pack.name;
				rec.folder = item.folder;
				rec.crc = folderCrc(target);
				st.records.push_back(rec);
				++st.installed;
				return;
			}
		}
		UnicodeString line;
		line.format(L"%ls: %ls", fromAnsi(item.folder).str(), lastErrorText(error).str());
		st.failures.push_back(line);
	}

	void finish(State &st)
	{
		if (st.progressBox != nullptr)
		{
			TheWindowManager->winDestroy(st.progressBox);
			st.progressBox = nullptr;
		}
		const Bool complete = st.next >= st.items.size();
		if (complete)
		{
			// every map of these packs is handled: their stamps are now the installed ones
			for (size_t p = 0; p < st.packs.size(); ++p) {
				setStamp(st, st.packs[p].name, st.packs[p].stamp);
			}
		}
		writeRecord(st);
		if (TheMapCache != nullptr) {
			TheMapCache->updateCache();
		}

		UnicodeString body;
		body.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapPacksDone", localizedFallback(L"Installed: %d. Updated: %d.", L"\x0423\x0441\x0442\x0430\x043D\x043E\x0432\x043B\x0435\x043D\x043E: %d. \x041E\x0431\x043D\x043E\x0432\x043B\x0435\x043D\x043E: %d.")).str(),
			st.installed, st.updated);
		if (st.leftAlone > 0)
		{
			UnicodeString line;
			line.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapPacksLeftAlone", localizedFallback(L"\nNot replaced because you changed them: %d.", L"\n\x041D\x0435 \x0437\x0430\x043C\x0435\x043D\x0435\x043D\x044B, \x0442\x0430\x043A \x043A\x0430\x043A \x0432\x044B \x0438\x0445 \x0438\x0437\x043C\x0435\x043D\x0438\x043B\x0438: %d.")).str(),
				st.leftAlone);
			body.concat(line);
		}
		if (!complete) {
			body.concat(text("GUI:MapPacksStopped", L"\nStopped: the rest can be installed at the next start.",
				L"\n\x041E\x0441\x0442\x0430\x043D\x043E\x0432\x043B\x0435\x043D\x043E: \x043E\x0441\x0442\x0430\x043B\x044C\x043D\x044B\x0435 \x043C\x043E\x0436\x043D\x043E \x0443\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C \x043F\x0440\x0438 \x0441\x043B\x0435\x0434\x0443\x044E\x0449\x0435\x043C \x0437\x0430\x043F\x0443\x0441\x043A\x0435."));
		}
		if (!st.failures.empty())
		{
			UnicodeString line;
			line.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:MapPacksFailed", localizedFallback(L"\nCould not install %d:", L"\n\x041D\x0435 \x0443\x0434\x0430\x043B\x043E\x0441\x044C \x0443\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C %d:")).str(),
				(Int)st.failures.size());
			body.concat(line);
			for (size_t i = 0; i < st.failures.size() && i < 4; ++i)
			{
				body.concat(L"\n");
				body.concat(st.failures[i]);
			}
			if (st.failures.size() > 4) {
				body.concat(L"\n...");
			}
		}
		MessageBoxOk(title(), body, nullptr);
		st.phase = PHASE_DONE;
	}

	void onInstall()
	{
		State &st = state();
		st.phase = PHASE_INSTALLING;
		st.next = 0;
		st.stopRequested = FALSE;
		CreateDirectoryA(longPath(st.userMaps.str()).str(), nullptr);
	}

	void onLater()
	{
		state().phase = PHASE_DONE;
	}

	void onNever()
	{
		state().phase = PHASE_DONE;
		OptionPreferences pref;
		pref["MapPacksAsk"] = "no";
		pref.write();
	}

	void onStop()
	{
		State &st = state();
		st.stopRequested = TRUE;
		st.progressBox = nullptr;   // the box closes itself
	}

	Bool askingAllowed()
	{
		OptionPreferences pref;
		OptionPreferences::const_iterator it = pref.find("MapPacksAsk");
		return it == pref.end() || stricmp(it->second.str(), "no") != 0;
	}

	/// The main menu is up and idle: no match, no box, no screen above it, no animation.
	Bool shellIdle()
	{
		if (TheShell == nullptr || !TheShell->isShellActive() || TheShell->getScreenCount() != 1 || !TheShell->isAnimFinished()) {
			return FALSE;
		}
		if (TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()) {
			return FALSE;
		}
		return !TheWindowManager->winIsModalActive();
	}
}

//-------------------------------------------------------------------------------------------------
void MapPacks::enableInTestRuns()
{
	state().testRuns = TRUE;
}

void MapPacks::update()
{
	State &st = state();
	switch (st.phase)
	{
	case PHASE_WAIT:
		if (!shellIdle()) {
			return;
		}
		if ((FrameShots::isEnabled() && !st.testRuns) || NetLab::isConfigured())
		{
			st.phase = PHASE_DONE;   // unattended test runs are not asked
			return;
		}
		scan(st);
		if (st.items.empty())
		{
			// nothing to offer; a changed stamp with no changed map is taken over silently
			Bool stampsMoved = FALSE;
			for (size_t p = 0; p < st.packs.size(); ++p)
			{
				const AsciiString *old = recordedStamp(st, st.packs[p].name);
				if (old != nullptr && *old != st.packs[p].stamp)
				{
					setStamp(st, st.packs[p].name, st.packs[p].stamp);
					stampsMoved = TRUE;
				}
			}
			if (stampsMoved) {
				writeRecord(st);
			}
			st.phase = PHASE_DONE;
			return;
		}
		if (!askingAllowed())
		{
			st.phase = PHASE_DONE;
			return;
		}
		ask(st);
		return;

	case PHASE_INSTALLING:
		if (st.stopRequested || st.next >= st.items.size())
		{
			finish(st);
			return;
		}
		showProgress(st);
		installOne(st, st.items[st.next]);
		++st.next;
		if (st.next >= st.items.size()) {
			showProgress(st);
		}
		return;

	default:
		return;
	}
}

void MapPacks::shutdown()
{
	delete s_state;
	s_state = nullptr;
}
