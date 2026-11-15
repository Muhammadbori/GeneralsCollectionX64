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

// LanRoomLook.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O31 (row 21, step 3) The observer pieces of the LAN game room (LanRoom.h), kept minimal and in
// one place until the room's final look is chosen: the observers list where the right part of the room chat was (name,
// files mark, anti-cheat shield; the host removes one with the x at its right), and "Become observer" for a player in a
// slot, between Start and Back. Every window made here is destroyed at the menu's shutdown; nothing is shown before the
// menu's enter transition is over.

#pragma once

#include "Lib/BaseType.h"
#include "GameClient/GameWindow.h"

namespace LanRoomLook
{
	void init(GameWindow *parent);
	void update(Bool screenReady);
	void shutdown();
	/// The menu's system messages first: TRUE when it was one of ours.
	Bool onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2);
}
