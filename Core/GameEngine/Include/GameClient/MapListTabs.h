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

// MapListTabs.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O112 Two more tabs in the map selection windows (skirmish, LAN): "GenTool" and "GenPatcher",
// beside the game's "Official maps" / "Unofficial maps". They list the collection's map packs read in place
// (MapUtil.h MapListTab, Packs\Maps\<Pack>\<Game>\Maps): GenTool's ranked maps and every other pack (GenPatcher's).
// A tab whose packs have no maps of this game is not made.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

class GameWindow;

namespace MapListTabs
{
	/// Adds the pack tabs beside the menu's two tabs, in their radio group (once per window).
	void attach(GameWindow *systemButton, GameWindow *userButton);
	/// The tab to open with: the one holding mapName, else official or user as before.
	Int startTab(const AsciiString &mapName, Bool usesSystemMapDir);
	/// Marks that tab's button (the user tab when a pack tab is not there).
	void selectTab(Int tab, GameWindow *systemButton, GameWindow *userButton);
	/// TRUE when control is one of the pack tabs; *tab gets its MapListTab.
	Bool isTabButton(GameWindow *control, Int *tab);
}
