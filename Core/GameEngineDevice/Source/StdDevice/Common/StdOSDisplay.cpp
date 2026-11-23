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

// TheSuperHackers @build O158 (headless port) OSDisplay on macOS and Linux, in place of Win32OSDisplay.cpp: the warning
// box of a headless game is a line on stderr (answered with OK); the busy state has nothing to keep awake.

#include <windows.h>

#include <stdio.h>

#include "Common/OSDisplay.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/GameText.h"

OSDisplayButtonType OSDisplayWarningBox(AsciiString p, AsciiString m, UnsignedInt, UnsignedInt)
{
	if (!TheGameText) {
		fprintf(stderr, "%s: %s\n", p.str(), m.str());
		return OSDBT_ERROR;
	}
	AsciiString prompt;
	AsciiString message;
	prompt.translate(TheGameText->fetch(p));
	message.translate(TheGameText->fetch(m));
	fprintf(stderr, "%s: %s\n", prompt.str(), message.str());
	return OSDBT_OK;
}

void OSDisplaySetBusyState(Bool, Bool)
{
}
