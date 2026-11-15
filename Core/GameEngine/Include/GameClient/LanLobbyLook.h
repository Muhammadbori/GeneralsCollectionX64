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

// LanLobbyLook.h ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O26 (row 26) The LAN screen rebuilt in code over the unchanged LanLobbyMenu.wnd, as the
// options sections were: the mock's variant A (staging\mock-lan-lobby) by default, variant B with Options.ini
// "LanLobbyLayout = B" (all places of both in one table each, LanLobbyLook.cpp).
//
// What it shows:
// - title and main menu button "LAN" / "LOKALNAYA SET'" (Russian; both languages);
// - your name (the old entry), your address with a "Copy" button (for friends over Radmin VPN), and "connect by
//   address": a field with the last used addresses (Network.ini RemoteIP0.., as the direct connect screen keeps them)
//   and a "Connect" button;
// - the open games: game, map, players n/m, observers, speed, anti-cheat shield (reserved: drawn only with Options.ini
//   "LanShowAntiCheat = yes", item 15 is not built), files match (the host's files fingerprint, LANAPI.h LanFiles);
//   refreshed by itself (the LAN code's own game list); a click selects, a double click joins;
// - the selected game: its slots with colour, army and team pictures (PlayerTable), the files mark of each human,
//   the observers, players and money;
// - the players in the lobby with their files mark, the chat; the buttons Create game, Join, Direct connect, Main menu.
// The old lists stay (hidden) as the data the LAN code fills; the new tables draw from them, so joining, chat and
// the game list work as before. Everything made here is made hidden, shown when the enter fade is over, and destroyed
// in the screen's shutdown (row 27).
//
// Seams left: the shield column and LanLobbyLook's anti-cheat switch (item 15); the observers count is the observer
// players of the game (row 21 brings real observer places); voice chat is not on this screen.

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"
#include "GameClient/GameWindow.h"

namespace LanLobbyLook
{
	/// After LanLobbyMenuInit has found its windows and filled them.
	void init(GameWindow *parent);
	/// Every update of the screen; 'screenReady': the enter fade is over and the screen is not shutting down.
	void update(Bool screenReady);
	/// First thing of the screen's shutdown: destroys what init made (O31: and stops observing a room).
	void shutdown();
	/// O31: destroys what init made, nothing else (init calls it first).
	void teardown();
	/// O31: a player's anti-cheat shield and files mark side by side, each 'size' wide (the game room's observers list).
	void drawMarks(Int x, Int y, Int size, UnsignedInt ip);
	/// The screen's system messages first: TRUE when it was one of the controls made here.
	Bool onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2);
	/// "LAN" / "LOKALNAYA SET'" (Russian): the screen's title and the main menu button.
	UnicodeString screenName();
}
