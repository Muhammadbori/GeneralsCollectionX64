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

// TheSuperHackers @feature See Fingerprint.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/Fingerprint.h"

#include "Common/ArchiveFile.h"
#include "Common/crc.h"
#include "Common/file.h"
#include "Common/GlobalData.h"

#include <stdio.h>
#include <algorithm>
#include <vector>

namespace
{
	struct Source
	{
		AsciiString name;
		UnsignedInt hash;
		Int files;
	};

	std::vector<ArchiveFile *> s_archives;
	std::vector<Source> s_sources;
	Bool s_computed = FALSE;
	UnsignedInt s_overall = 0;

	/// The INI names the game reads into its INI CRC (GameEngine::init, GlobalData).
	const char *const kLogicIni[] =
	{
		"gamedata", "water", "weather", "science", "multiplayer", "terrain", "roads", "rank", "playertemplate", "fxlist",
		"weapon", "objectcreationlist", "locomotor", "specialpower", "damagefx", "armor", "object", "upgrade", "aidata",
		"crate",
	};

	/// TRUE for "Data\INI\[Default\]<name>.ini" or "Data\INI\[Default\]<name>\..." of a logic INI.
	Bool isLogicIni(const char *path)
	{
		char lower[_MAX_PATH];
		strlcpy(lower, path, ARRAY_SIZE(lower));
		for (char *c = lower; *c; ++c) {
			*c = (*c == '/') ? '\\' : (char)tolower((unsigned char)*c);
		}
		const char *rest = lower;
		if (strncmp(rest, "data\\ini\\", 9) != 0) {
			return FALSE;
		}
		rest += 9;
		if (strncmp(rest, "default\\", 8) == 0) {
			rest += 8;
		}
		for (size_t i = 0; i < ARRAY_SIZE(kLogicIni); ++i)
		{
			const size_t n = strlen(kLogicIni[i]);
			if (strncmp(rest, kLogicIni[i], n) == 0 && (rest[n] == '.' || rest[n] == '\\')) {
				return TRUE;
			}
		}
		return FALSE;
	}

	void addName(CRC &crc, const char *name)
	{
		char lower[_MAX_PATH];
		strlcpy(lower, name, ARRAY_SIZE(lower));
		for (char *c = lower; *c; ++c) {
			*c = (*c == '/') ? '\\' : (char)tolower((unsigned char)*c);
		}
		crc.computeCRC(lower, (Int)strlen(lower));
	}

	void addContents(CRC &crc, File *file)
	{
		char buffer[16384];
		for (;;)
		{
			const Int got = file->read(buffer, sizeof(buffer));
			if (got <= 0) {
				break;
			}
			crc.computeCRC(buffer, got);
		}
	}

	const char *baseName(const char *path)
	{
		const char *b = path;
		for (const char *p = path; *p; ++p) {
			if (*p == '\\' || *p == '/') {
				b = p + 1;
			}
		}
		return b;
	}

	void hashArchive(ArchiveFile *archive)
	{
		FilenameList files;
		archive->getFileListInDirectory(AsciiString::TheEmptyString, AsciiString("Data\\INI\\"), AsciiString("*.ini"), files, TRUE);
		CRC crc;
		Int count = 0;
		for (FilenameList::const_iterator it = files.begin(); it != files.end(); ++it)
		{
			if (!isLogicIni(it->str())) {
				continue;
			}
			File *file = archive->openFile(it->str(), File::READ | File::BINARY);
			if (file == nullptr) {
				continue;
			}
			addName(crc, it->str());
			addContents(crc, file);
			file->close();
			++count;
		}
		if (count > 0)
		{
			Source s;
			s.name = baseName(archive->getName().str());
			s.hash = crc.get();
			s.files = count;
			s_sources.push_back(s);
		}
	}

	void listLoose(const AsciiString &folder, std::vector<AsciiString> &out)
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
			if (data.cFileName[0] == '.') {
				continue;
			}
			AsciiString path = folder;
			path.concat(data.cFileName);
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				path.concat("\\");
				listLoose(path, out);
			}
			else if (isLogicIni(path.str()))
			{
				out.push_back(path);
			}
		} while (FindNextFile(find, &data));
		FindClose(find);
	}

	void hashLoose()
	{
		std::vector<AsciiString> files;
		listLoose(AsciiString("Data\\INI\\"), files);
		if (files.empty()) {
			return;
		}
		std::sort(files.begin(), files.end(), [](const AsciiString &a, const AsciiString &b) { return stricmp(a.str(), b.str()) < 0; });
		CRC crc;
		Int count = 0;
		for (size_t i = 0; i < files.size(); ++i)
		{
			FILE *fp = fopen(files[i].str(), "rb");
			if (fp == nullptr) {
				continue;
			}
			addName(crc, files[i].str());
			char buffer[16384];
			size_t got;
			while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
				crc.computeCRC(buffer, (Int)got);
			}
			fclose(fp);
			++count;
		}
		Source s;
		s.name = "loose Data\\INI";
		s.hash = crc.get();
		s.files = count;
		s_sources.push_back(s);
	}
}

//-------------------------------------------------------------------------------------------------
void Fingerprint::noteArchive(ArchiveFile *archive)
{
	if (archive != nullptr && !s_computed) {
		s_archives.push_back(archive);
	}
}

void Fingerprint::compute()
{
	if (s_computed || TheGlobalData == nullptr) {
		return;
	}
	s_computed = TRUE;
	hashLoose();   // loose files are read before the archives
	for (size_t i = 0; i < s_archives.size(); ++i) {
		hashArchive(s_archives[i]);
	}
	s_archives.clear();

	CRC all;
	UnsignedInt exe = TheGlobalData->m_exeCRC, ini = TheGlobalData->m_iniCRC;
	all.computeCRC(&exe, sizeof(exe));
	all.computeCRC(&ini, sizeof(ini));
	for (size_t i = 0; i < s_sources.size(); ++i)
	{
		addName(all, s_sources[i].name.str());
		all.computeCRC(&s_sources[i].hash, sizeof(s_sources[i].hash));
	}
	s_overall = all.get();

	AsciiString path = TheGlobalData->getPath_UserData();
	path.concat("Fingerprint.log");
	if (FILE *fp = fopen(path.str(), "w"))
	{
		fputs(text().str(), fp);
		fclose(fp);
	}
}

Bool Fingerprint::isComputed()
{
	return s_computed;
}

UnsignedInt Fingerprint::overall()
{
	return s_overall;
}

AsciiString Fingerprint::text()
{
	AsciiString out;
	if (TheGlobalData == nullptr) {
		return out;
	}
	AsciiString line;
	line.format("fingerprint %08X exe %08X ini %08X sources %d\n", s_overall, TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC,
		(Int)s_sources.size());
	out.concat(line);
	for (size_t i = 0; i < s_sources.size(); ++i)
	{
		line.format("source %08X files %d %s\n", s_sources[i].hash, s_sources[i].files, s_sources[i].name.str());
		out.concat(line);
	}
	return out;
}

void Fingerprint::shutdown()
{
	s_archives.clear();
	s_sources.clear();
}
