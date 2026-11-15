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

// PlayerTable.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The player table of the skirmish and LAN game screens with pictures, and the same pictures
// on the loading screen and in the in-game player list (items 5, 8, 9).
//
// Lobby table: the colour cell is narrow and shows a spiral (a rainbow for a random colour, a white spiral tinted with
// the chosen colour); the colour names are only in the opened list. The army cell shows the general's logo (the
// template's side icon) or a "random army" emblem before the name. The team cell is narrow: the digit, or a pirate sign
// for no team. The room won goes to the army cell.
//
// Loading screen and in-game list (display only; nothing sent over the network or drawn by the logic changes): what a
// player left random stays random to the others who are not on the player's team (GameSlot::getApparent...), with the
// same pictures as in the lobby; the player, the team-mates and observers see the real values. The team is always
// shown; no team = the pirate sign.
//
// The pictures are textures that can be swapped without a code change (Art\Textures, loose or in an archive):
//   PlayerTable_RandomColor.tga   random colour (rainbow spiral)
//   PlayerTable_Color.tga         chosen colour (white spiral; the game tints it)
//   PlayerTable_NoTeam.tga        no team (pirate sign)
//   PlayerTable_RandomArmy.tga    random army
//   PlayerTable_AntiCheatOn.tga / PlayerTable_AntiCheatOff.tga   reserved for the anti-cheat shield (item 15), not used
// A picture that is missing leaves that cell as it was (text).

#pragma once

#include "Lib/BaseType.h"
#include "GameClient/Color.h"

class GameWindow;
class GameSlot;
class Image;

namespace PlayerTable
{
	enum Picture
	{
		PICTURE_RANDOM_COLOR, PICTURE_COLOR, PICTURE_NO_TEAM, PICTURE_RANDOM_ARMY, PICTURE_ANTICHEAT_ON, PICTURE_ANTICHEAT_OFF,
		PICTURE_COUNT
	};
	/// The picture, or nullptr when its texture is not there.
	const Image *picture(Picture which);

	/// Narrows the colour and team columns and widens the army column of a game screen. 'wndPrefix' is the window file
	/// with its colon ("SkirmishGameOptionsMenu.wnd:"); the headers StaticTextColor, StaticTextFaction and StaticTextTeam
	/// follow their columns. Call once after the screen's controls are found.
	void layout(const char *wndPrefix, GameWindow *comboColor[], GameWindow *comboPlayerTemplate[], GameWindow *comboTeam[],
		Int slots);

	/// The pictures of one lobby row from the slot (after the combo boxes show the slot's values).
	void applyLobbyRow(GameWindow *comboColor, GameWindow *comboPlayerTemplate, GameWindow *comboTeam, const GameSlot *slot);

	/// The pictures of one row of the loading screen or the in-game list, by the apparent values of the slot: a spiral
	/// before the name, the army picture before the army name, the pirate sign for no team. Any window may be nullptr.
	void decorateRow(GameWindow *nameText, GameWindow *sideText, GameWindow *teamText, const GameSlot *slot);
}
