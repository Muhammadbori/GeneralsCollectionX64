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

// CollectionUserData.h /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O118 Our own user data folders. The original game's Documents folders ("Command and Conquer
// Generals Zero Hour Data", "Command and Conquer Generals Data") are never written: sharing them broke the original
// game. Everything this build writes (Options.ini, saves, replays, screenshots, crash dumps, logs, map caches, received
// maps) goes to
//   Documents\Generals Collection x64\Zero Hour\     (Zero Hour)
//   Documents\Generals Collection x64\Generals\      (Generals)
// The original folders are not read either (the user's word, O118 addendum), with one exception: on the first run, when
// our Options.ini does not exist yet, the original Options.ini is copied once into our folder (read only), so the
// player's settings carry over. The player's own maps live in our folder's Maps (listed under "Unofficial maps"; a map
// received in a LAN room is written there too).
// -userDataDir still replaces our folder for tests (then the original folder is not used at all).

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

namespace CollectionUserData
{
	/// From the original folder (Documents\<leaf>\, ending in '\'), our folder for gameName ("Zero Hour" or "Generals"),
	/// ending in '\'. Creates it and copies Options.ini once. Empty in, empty out.
	AsciiString makeOurs(const AsciiString &originalDir, const char *gameName);
}
