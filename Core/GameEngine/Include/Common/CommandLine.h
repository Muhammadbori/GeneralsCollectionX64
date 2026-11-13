/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// CommandLine.h
// The command-line interface
// Author: Matthew D. Campbell, September 2001

#pragma once

class CommandLine
{
public:

	// Parses startup flags and applies the process working directory.
	static void parseCommandLineForStartup();
	static void parseCommandLineForEngineInit();

	// Returns true if command-line parsing consumed the zero-based argument index.
	// The index excludes the executable name.
	static bool wasCommandLineArgumentParsed(int argIndex);

	// TheSuperHackers @feature The folder given with -userDataDir, absolute and ending with a backslash,
	// or null when the option was not given. GlobalData uses it instead of Documents\<UserDataLeafName>.
	static const char * getUserDataDirOverride();

	// TheSuperHackers @feature The ordered archive list of this game, Archives_Generals.txt or Archives_ZeroHour.txt in the
	// folder of the executable, as an absolute path; null when the file does not exist (the game then loads every *.big
	// of its folder, and Zero Hour the archives of the base game from the registry, as before).
	static const char * getArchiveListFile();

	// TheSuperHackers @feature True with -noRegistry, and when the archive list exists (a self-contained folder): the
	// game then neither reads nor writes its registry keys. Safe to call before the command line is parsed.
	static bool isRegistryDisabled();

	// TheSuperHackers @feature True with -audioLog: the audio manager writes AudioEvents.log in the user data folder.
	static bool isAudioLogEnabled();

	// TheSuperHackers @feature True with -audioLenDump: after the audio subsystem is loaded the Miles audio manager writes
	// AudioLengths.tsv (every .wav/.mp3 under Data\Audio and the length Miles reports for it) to the user data folder and the
	// game quits. A test tool; it needs Miles, so on a 64-bit build it only writes a note.
	static bool isAudioLenDumpEnabled();

	// TheSuperHackers @feature -audio miles|miniaudio|miniaudio-null|null: the sound library for a test run, or null when
	// not given. miniaudio-null runs the miniaudio device on its null backend: everything but the output, silent.
	// The audio manager checks it against the libraries the build has (RTS_AUDIO_BACKEND).
	static const char * getAudioDevice();
};
