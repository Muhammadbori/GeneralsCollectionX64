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

#pragma once

// TheSuperHackers @feature Frame shots save the rendered image at given logic frames, so that replay runs
// can be compared and timed without a person. Command line: -shotFrames <list> -shotDir <path> -shotExit.
// Client side only: nothing here touches the game logic state, its random generator, the CRC or the recorder.
class FrameShots
{
public:
	static void setFrames(const char *commaSeparatedFrames); ///< Logic frame numbers, for example "300,900,1800".
	static void setDirectory(const char *path); ///< Output folder, created if missing. Default: user data folder + "FrameShots".
	static void setExitWhenDone(Bool exitWhenDone); ///< Quit through the normal quit path after the last listed frame was written.

	static void setNoImage(Bool noImage); ///< Write the frames.csv rows but no image files (-shotNoImage). Cheap for long timing runs.

	/// Test saves: -saveFrames <list> writes a save game when the logic reaches each listed frame, as the save menu does, to
	/// Save\frame_<8 digits>.sav in the user data folder, and a row per save to saves.csv in the shot folder. Frames that a
	/// game starts after (for example in a loaded save) are skipped. With -shotExit, list a shot frame after the last save frame.
	/// Load a save without the menus with the existing -loadsave <file>.
	static void setSaveFrames(const char *commaSeparatedFrames);

	/// Test input: -testInput "<kind>:<arg>@<from>-<to>[;...]" injects the raw messages a device would create while the logic frame
	/// is in [from,to). Kinds: key:left|right|up|down, rmb:<dx>,<dy> (right button drag from the screen center), edge:left|right|up|down.
	static void setTestInput(const char *specs);
	static void setTestLogicFps(Int fps); ///< -testLogicFps: forces the logic time scale fps while a game runs (1000000 = as fast as possible). Test only.
	static void injectTestInput(); ///< Call once per client update after the device messages were appended to the stream.

	static Bool isEnabled() { return s_enabled; }

	/// Ends a command line started game that cannot run (for example the -file map does not exist): logs the
	/// message, shows it in a message box unless frame shots are on (unattended runs must not block), and makes the
	/// engine quit with a non-zero exit code, see getFailureExitCode().
	static void failStartup(const char *message, Int exitCode);
	static Int getFailureExitCode() { return s_failureExitCode; } ///< 0 unless failStartup was called.

	static void onLogicUpdated(); ///< Call after every regular logic update.
	static void onClientUpdated(); ///< Call once per engine update, after the client update that draws.

private:
	static void beginGame();
	static void capture(UnsignedInt listedFrame);
	static Bool prepareOutput();
	static void exitGame();
	static void writeSave(UnsignedInt frame);
	static void updateEnabled();

	static Bool s_enabled;
	static Int s_failureExitCode;
};
