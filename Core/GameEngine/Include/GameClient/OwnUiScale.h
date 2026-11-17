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

// OwnUiScale.h ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O53 (row 39) One size factor for everything our own code builds on screen (the settings
// sections' rows, sliders, lists, check boxes, listen buttons; the LAN tables; the speed rows; the replay bar; the
// voice and watchers lines), on top of the proportional font rule (GlobalLanguage, O52). Options.ini "OwnUiScale",
// default 0.82, 0.6 .. 1.2. The original menus' own windows (the section buttons, Defaults / Accept / Cancel, the main
// menu) keep the game's size. Margins, row heights and control sizes follow it as they follow the font.

#pragma once

#include "Lib/BaseType.h"

class GameFont;

namespace OwnUi
{
	Real scale();                     ///< read from Options.ini the first time and after reload()
	void reload();                    ///< read Options.ini again (each time a screen of ours is built)
	Int size(Int pixels);             ///< a length of our layouts times the factor (at least 1)
	GameFont *font(GameFont *font);   ///< the same font at the factor's point size (never under 8 points; nullptr stays)
}
