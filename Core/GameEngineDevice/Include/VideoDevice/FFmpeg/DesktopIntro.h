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

// TheSuperHackers @feature O108 (band 61) The EA logo movie played over the Windows desktop, with a see-through
// background. O111: the game window with the splash picture comes first, alone; it is hidden just before the logo
// plays over the bare desktop, and shown again by WinMain afterwards. The start goes on as soon as the logo picture
// has gone (a longer sound plays on by itself).
//
// The movie is Data\Intro\EALogo_16x9.webm or Data\Intro\EALogo_16x10.webm next to the executable (the one closer
// to the primary monitor's shape is tried first, the other one after it). It must carry an alpha channel (VP9 or
// VP8 with alpha in WebM, decoded by libvpx; ProRes 4444, PNG or QuickTime Animation work too). Its sound is
// played with miniaudio.

#pragma once

#include <windows.h>

namespace DesktopIntro
{
	// TRUE when the command line holds -nologo or a switch of a test or automatic run (screenshots, UI scripts,
	// test commands, replays, -file, -skirmish, benchmarks, the quick restart, ...), which leave the intro out.
	bool isSkippedByCommandLine();

	// Plays the movie in a borderless topmost see-through window centred on the primary monitor and sized to fit
	// it. Blocks until the movie ends or ESC, Space, Enter or a click on the picture skips it.
	// TRUE when the movie was shown (also when it was skipped); FALSE when it could not be (no file, no alpha
	// channel, no decoder, ...), and then nothing was shown.
	// splashWindow (may be null) is hidden right before the first logo frame, but not before it has been shown for
	// splashMinMs since splashShownAt (GetTickCount). The caller shows it again when TRUE is returned.
	bool play(HINSTANCE instance, HWND splashWindow, DWORD splashShownAt, DWORD splashMinMs);
}
