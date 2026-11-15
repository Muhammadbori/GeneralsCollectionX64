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

// LanRoom.h ////////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O31 (row 21, step 3) Observers of a LAN game room and outside viewers.
//
// The room is the host's LAN game (its 8 game slots and its lockstep are unchanged) plus an observer list the host keeps:
// at most 12 people in all, so at most 12 minus the humans in slots observers (at least 4 places are observer-only). An
// observer is not in the lockstep: at Start its game follows the players' replay stream (ReplayStream, port 8090 on the
// players' LAN addresses) live, from the player it reaches best, hopping when that one stalls. An outside viewer does the
// same for a running game with a delay of 10 s to 3 min, and relays its copy to later viewers.
//
// Messages: LANMessage::MSG_ROOM (appended; the size of LANMessage is unchanged; older builds ignore it):
//   OBSERVE  observer -> host   a place (again every 4 s while observing: "still here")
//   LEAVE    observer -> host   no longer observing
//   LIST     host -> everybody  the observers (address, name): every 2 s while there are any, and on each change
//   REMOVED  host -> observer   removed by the host (count 0), the room is full (1), or no such room (2)
//   CHAT     observer -> host, then host -> the room's players and observers (one ready line of text)
//   START    host -> observers  the stream port and the players' addresses
// The host forgets an observer not heard from for 15 s. A person who joins a slot leaves the list (the host drops them
// when it accepts the join).

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

#include <vector>

struct LANMessage;
class LANGameInfo;

namespace LanRoom
{
	enum
	{
		MAX_PEOPLE = 12,            ///< game slots and observers together
		MAX_OBSERVERS = 12,
		STREAM_PORT = 8090,         ///< the replay stream on each player's LAN address (ReplayStream)
		OUTSIDE_PER_SOURCE = 4,     ///< outside viewers a player serves itself; more go to relays
	};
	enum State { STATE_NONE, STATE_ASKED, STATE_IN_ROOM, STATE_STARTING, STATE_PLAYING };

	struct Observer
	{
		UnsignedInt ip;
		UnicodeString name;
	};

	//--- every LAN update (LANAPI) ------------------------------------------------------------------
	void onMessage(const LANMessage *msg, UnsignedInt senderIP, UnsignedInt localIP);
	void update(UnsignedInt localIP);

	//--- host -------------------------------------------------------------------------------------
	Int capacity(const LANGameInfo *game);               ///< observer places of a room (12 - humans in slots)
	void hostRemove(UnsignedInt ip);                     ///< the host's remove button
	void hostDropJoined(UnsignedInt ip);                 ///< the host accepted that person's join into a slot
	void hostStart(LANGameInfo *game, UnsignedInt localIP);   ///< Start: the observers get the players' addresses

	//--- players (every LAN match) ---------------------------------------------------------------
	void configureServing(UnsignedInt localIP);          ///< at the match start: serve the replay on the LAN address

	//--- the room's observers (for the screens) ---------------------------------------------------
	std::vector<Observer> observersOf(UnsignedInt hostIP);

	//--- this game as an observer or viewer -------------------------------------------------------
	State state();
	UnsignedInt observedHost();
	Bool isWatcher();                                    ///< an outside viewer (delayed), not a room observer
	void startObserving(UnsignedInt hostIP);
	void stopObserving();
	void requestBecomeObserver(UnsignedInt hostIP);      ///< from a slot: after leaving the game, observe that room
	void sendChat(const UnicodeString &text);            ///< room chat of an observer (before the start)
	void startWatching(LANGameInfo *game, Int delaySeconds);
	/// The LAN screen opens again: a viewer that played its stream goes back to normal (follow mode off, no mirror).
	void resetViewer();
	/// A line for the screen's chat (removed, full, waiting, lost ...); FALSE when none.
	Bool takeNotice(UnicodeString *text);
}
