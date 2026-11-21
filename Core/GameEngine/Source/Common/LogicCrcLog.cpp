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

// TheSuperHackers @feature See LogicCrcLog.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/LogicCrcLog.h"
#include "Common/CommandLine.h"
#include "Common/GlobalData.h"
#include "Common/version.h"

#include <stdio.h>

namespace
{
	Bool s_checked = FALSE;
	FILE *s_file = nullptr;
	UnsignedInt s_lines = 0;

	void closeLog()
	{
		if (s_file != nullptr)
		{
			fclose(s_file);
			s_file = nullptr;
		}
	}

	void openOnce()
	{
		if (s_checked)
			return;
		s_checked = TRUE;
		if (!CommandLine::isTestExe())
			return;
		for (int arg = 1; arg + 1 < __argc; ++arg)
		{
			if (stricmp(__argv[arg], "-logicCrcLog") == 0)
			{
				s_file = fopen(__argv[arg + 1], "w");
				if (s_file != nullptr)
				{
					const char *identity = Version::getLogicBuildIdentity();
					fprintf(s_file, "# exe %08X identity %s\n", TheGlobalData != nullptr ? TheGlobalData->m_exeCRC : 0u,
						identity != nullptr ? identity : "(exe crc)");
					atexit(closeLog);
				}
				return;
			}
		}
	}
}

Bool LogicCrcLog::enabled()
{
	openOnce();
	return s_file != nullptr;
}

void LogicCrcLog::frame(UnsignedInt frame, UnsignedInt crc)
{
	if (!enabled())
		return;
	fprintf(s_file, "%u %08X\n", frame, crc);
	// Flushed now and then, so that a crash or a killed process still leaves most of the frames.
	if ((++s_lines % 100) == 0)
		fflush(s_file);
}
