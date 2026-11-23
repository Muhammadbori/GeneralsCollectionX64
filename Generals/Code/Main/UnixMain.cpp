/*
**	Command & Conquer Generals(tm)
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

// TheSuperHackers @build O158 (headless port, part 3) The entry point of the headless game on macOS and Linux, in place
// of WinMain.cpp: the same start as WinMain without a window (the games run with -headless there, which needs no
// renderer). -help prints how to run it.

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/Packs.h"
#include "Common/version.h"
#include "GameClient/GameClient.h"
#include "Win32Device/Common/Win32GameEngine.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#include "BuildVersion.h"
#include "GeneratedVersion.h"

#ifndef RTS_UNIX_MAIN_GAME_NAME
#define RTS_UNIX_MAIN_GAME_NAME "Command & Conquer Generals"
#endif

// GLOBALS (the ones WinMain.cpp defines for the rest of the game) ////////////////////////////////////////////////////
HINSTANCE ApplicationHInstance = nullptr;
HWND ApplicationHWnd = nullptr;
Win32Mouse *TheWin32Mouse = nullptr;
DWORD TheMessageTime = 0;

const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
const char *gAppPrefix = "";

static CriticalSection critSec1, critSec2, critSec3, critSec4;

static void printUsage(const char *exe)
{
	printf("%s (headless build for macOS and Linux)\n", RTS_UNIX_MAIN_GAME_NAME);
	printf("Usage: %s -headless [options]\n", exe);
	printf("  Runs the game without a window, renderer or sound, from the game data in the working directory.\n");
	printf("  -replay <file.rep>   play a replay and exit (with -headless)\n");
	printf("  -userDataDir <dir>   folder for options, replays and logs\n");
	printf("  -help                this text\n");
	printf("The other command line options of the Windows game apply as well.\n");
}

int main(int argc, char *argv[])
{
	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			printUsage(argv[0]);
			return 0;
		}
	}

	Int exitcode = 1;

	try
	{
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;

		initMemoryManager();

		CommandLine::parseCommandLineForStartup();

		// There is no window on these platforms: the game always runs headless.
		TheWritableGlobalData->m_headless = TRUE;

		TheVersion = NEW Version;
		TheVersion->setVersion(VERSION_MAJOR, VERSION_MINOR, VERSION_BUILDNUM, VERSION_LOCALBUILDNUM,
			AsciiString(VERSION_BUILDUSER), AsciiString(VERSION_BUILDLOC),
			AsciiString(__TIME__), AsciiString(__DATE__));

		exitcode = GameMain();

		delete TheVersion;
		TheVersion = nullptr;

		shutdownMemoryManager();
	}
	catch (...)
	{
		fprintf(stderr, "%s: stopped by an unhandled exception\n", argv[0]);
		exitcode = 1;
	}

	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;

	return exitcode;
}

GameEngine *CreateGameEngine()
{
	Win32GameEngine *engine = NEW Win32GameEngine;
	engine->setIsActive(TRUE);
	return engine;
}
