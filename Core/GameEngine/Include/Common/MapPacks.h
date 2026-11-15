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

// MapPacks.h ///////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature Map packs: community maps that come with the game and are installed into the player's own
// maps folder from inside the game (item 15, the map updater without a server).
//
// Layout, next to the game's exe:
//   Packs\Maps\<Pack>\pack.ini
//   Packs\Maps\<Pack>\ZeroHour\Maps\<Map folder>\<files>     maps for Zero Hour
//   Packs\Maps\<Pack>\Generals\Maps\<Map folder>\<files>     maps for Generals
// A map folder is copied as it is, as the game expects a custom map: <user data>\Maps\<Map folder>\<Map folder>.map with
// its .tga, map.ini, map.str and other files.
// pack.ini (UTF-8, "Key = Value", ';' comments):
//   NameEnglish = Skirmish maps        the name shown in English
//   NameRussian = ...                  the name shown in Russian
//   Stamp = 2.15                       the content stamp: any text, changed whenever the pack's maps change
//
// In the shell (main menu, no other box up) the game checks the packs of this game once per run:
//   - a map folder that is not in the player's maps folder is new;
//   - a map installed from a pack earlier is an update when the pack's stamp changed and its files differ from what was
//     installed, unless the player changed the installed copy (its CRC is not the one recorded): then it is left alone.
// When there is anything, one box asks: "Install" / "Later" / "Do not ask again". Install copies the maps one per frame
// with a progress box (it can be stopped), then says what was installed, updated, left alone and what failed. Nothing is
// ever deleted; a new map never replaces an existing folder; the game stays usable after a failure.
// What was installed is recorded in <user data>\MapPacks.ini. "Do not ask again" writes MapPacksAsk = no into
// Options.ini; set it to yes (or remove it) to be asked again. There is no row in the options screen.
// Test runs (-shotFrames, -uiScript) are not asked unless -mapPacksTest is given.

#pragma once

#include "Lib/BaseType.h"

namespace MapPacks
{
	/// -mapPacksTest: ask in a test run too.
	void enableInTestRuns();
	/// Every shell update: the question once per run, then the installation.
	void update();
	void shutdown();
}
