/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// TheSuperHackers @feature O94 (row 60) The theme colours of the screens and panels this project draws itself (the LAN
// lobby and room additions, the player tables' group lines, the ticked sliders of the options, the watch panel's seek
// line). Zero Hour keeps the blue they were made with; the base game (Generals) gets the gold of its own windows, read
// from its Window.big beside the same controls of Zero Hour's WindowZH.big (both read only):
//   frame        ZH control border 47 55 168          Generals 255 190 1   (OptionsMenu, LanGameOptionsMenu borders)
//   frame dark   ZH list box border 49 55 168          Generals 122 48 1    (LanLobbyMenu ListboxGames/Players/Chat)
//   light        ZH (button pushed / NoImage) 47 55 168 Generals 255 255 0
//   dim text     ZH disabled text 62 64 92             Generals 159 143 95
// Ours were brighter than Zero Hour's own border (30 140 255 for 47 55 168); the base game's gold is bright already, so
// its values are used as they are. Colours that carry a meaning (the green shield and check, red, the admin's gold star,
// the held green of the sliders) are not part of the theme.

#pragma once

#include "GameClient/Color.h"

namespace LookPalette
{
#if RTS_GENERALS
	const Color kFrame = GameMakeColor(255, 190, 1, 255);          ///< panel frames, slider fill and ring
	const Color kFrameLight = GameMakeColor(255, 255, 0, 255);     ///< a ring under the mouse
	const Color kRowSelected = GameMakeColor(122, 48, 1, 230);     ///< the chosen row's fill
	const Color kTrack = GameMakeColor(122, 48, 1, 255);           ///< an empty slider track (the list box border)
	const Color kGroupLine = GameMakeColor(255, 190, 1, 230);      ///< the player tables' team group lines
	const Color kTextDim = GameMakeColor(159, 143, 95, 255);       ///< dim text, ticks
	const Color kTextLabel = GameMakeColor(159, 143, 95, 255);     ///< a slider's labels
#else
	const Color kFrame = GameMakeColor(30, 140, 255, 255);
	const Color kFrameLight = GameMakeColor(110, 190, 255, 255);
	const Color kRowSelected = GameMakeColor(20, 30, 110, 230);
	const Color kTrack = GameMakeColor(42, 46, 72, 255);
	const Color kGroupLine = GameMakeColor(110, 190, 255, 230);
	const Color kTextDim = GameMakeColor(150, 156, 172, 255);
	const Color kTextLabel = GameMakeColor(160, 166, 184, 255);
#endif
}
