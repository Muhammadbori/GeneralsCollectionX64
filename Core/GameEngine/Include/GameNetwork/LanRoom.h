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
// same for a running game, and relays its copy to later viewers.
// O33: live only for the room's observers. The host's list as it was at Start (never a player of the match) is frozen and
// goes to every player in START; the players' servers give the stream live only to those addresses. Everybody else is an
// outside viewer: the server sends nothing newer than (now - the host's delay) (Options.ini LanWatchDelay, 10 s to 3 min,
// default 60 s); the viewer's own slider only adds to it. The host may forbid outside viewers (LanAllowViewers, default
// yes). Only a LAN match serves, and only while it records; a match with no room observers and no outside viewers allowed
// does not listen at all.
//
// Messages: LANMessage::MSG_ROOM (appended; the size of LANMessage is unchanged; older builds ignore it):
//   OBSERVE  observer -> host   a place (O42: every second until the host's list has it, then every 4 s: "still here")
//   LEAVE    observer -> host   no longer observing
//   LIST     host -> everybody  the observers (address, name), the delay and the outside-viewers flag: O42: every 2 s
//                               while it hosts a room (also with no observers: the lobby shows "no viewers" by it), and
//                               on each change
//   RULES    host -> everybody  O42: during the match, every 2 s: the delay and the outside-viewers flag (a lobby that
//                               came later disables Watch for a match that allows no viewers)
//   REMOVED  host -> observer   removed by the host (count 0), the room is full (1), or no such room (2)
//   CHAT     observer -> host, then host -> the room's players and observers (one ready line of text)
//   START    host -> observers  the stream port, the players' addresses, the delay and the flag
//   START    host -> players    (flag bit 1) the frozen observer list, the delay and the flag (O33)
//   COUNT    player -> players  during the match: the outside viewers it serves (O33, for the players' HUD)
// O34 (the user's LAN rules):
//   - Room observers watch live and may relay to outside viewers, delayed by the host's delay (their server keeps it as a
//     player's does). During the match they have a channel of their own, text and voice, among the room's observers only
//     (the host's list at Start, players left out): text over UDP OBSERVER_CHAT_PORT on their LAN addresses, voice through
//     the voice chat with a roster of the observers. Nothing of it is sent to, or accepted from, any other address. Before
//     Start the room chat is shared with the players (CHAT above); from Start nothing from an observer reaches a player.
//   - Outside viewers can neither write nor speak, to anyone, at any time while watching: no chat input in their replay, no
//     voice roster, and no socket that would take their text; every receiver takes text and voice only from listed
//     observers. They need no anti-cheat. They take the stream from other outside viewers first, then from room observers,
//     then from the players (ReplayStream ranks), and keep a play-out reserve of 12 s (ReplayFollow) that bridges a source
//     change; with no source for longer they show "Reconnecting to the stream...".
// The host forgets an observer not heard from for 15 s. A person who joins a slot leaves the list (the host drops them
// when it accepts the join). O42: an observer counts the room as gone only when the lobby has no such game and the host's
// LIST has not come for 10 s (a player who just left a room has that game back in its lobby only with the host's next
// announce, up to 10 s later); an observer missing from the host's list asks again.

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
		OBSERVER_CHAT_PORT = 8091,  ///< O34: the room observers' text during the match (UDP, LAN address)
		VIEWER_RESERVE_SECONDS = 12,   ///< O34: an outside viewer's play-out reserve
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
	/// Observer places of a room. O56 (row 42): 12 - the slots that can hold a player (not closed): an 8-player map 4,
	/// a 6-player map 6, a 2-player map 10 (was 12 - the humans in slots). The lobby table shows the same number.
	Int capacity(const LANGameInfo *game);
	void hostRemove(UnsignedInt ip);                     ///< the host's remove button
	void hostDropJoined(UnsignedInt ip);                 ///< the host accepted that person's join into a slot
	void hostStart(LANGameInfo *game, UnsignedInt localIP);   ///< Start: the observers get the players' addresses
	/// O33: this host's rules for outside viewers: the delay chosen in the room (O56 row 43), else Options.ini
	/// LanWatchDelay 10..180, default 60. O56 (row 43): outside viewers are always allowed (an old LanAllowViewers = no
	/// is ignored).
	void hostViewerRules(Int *delaySeconds, Bool *outsideAllowed);
	/// O56 (row 43): the host's choice in the room (10..180 s), for this room and the match that starts from it; a new
	/// room (LANAPI::RequestGameCreate) starts again from Options.ini.
	void setHostDelay(Int seconds);
	void resetHostDelay();

	//--- players (every LAN match) ---------------------------------------------------------------
	/// At the match start: serve the replay on the LAN address, live to the room's observers the host named at Start, delayed
	/// to everybody else (O33); nothing when no observer and no outside viewer is allowed.
	void configureServing(UnsignedInt localIP);
	/// O33: during the match (LANAPI::pumpGuard): tell the other players how many outside viewers this player serves.
	void matchUpdate(UnsignedInt localIP);
	/// O33: a MSG_ROOM during the match; only a COUNT from a player of the match is used.
	void onMatchMessage(const LANMessage *msg, UnsignedInt senderIP);
	/// O33: for the players' HUD: the room's observers admitted at Start, and the outside viewers the players serve now.
	/// FALSE when this game is not a LAN match that serves.
	Bool matchWatchers(Int *roomObservers, Int *outsideViewers);

	//--- the room's observers (for the screens) ---------------------------------------------------
	std::vector<Observer> observersOf(UnsignedInt hostIP);
	/// O33: what a room's host said about outside viewers (from its LIST); FALSE when not known.
	Bool viewerRulesOf(UnsignedInt hostIP, Int *delaySeconds, Bool *outsideAllowed);

	//--- this game as an observer or viewer -------------------------------------------------------
	State state();
	UnsignedInt observedHost();
	Bool isWatcher();                                    ///< an outside viewer (delayed), not a room observer
	void startObserving(UnsignedInt hostIP);
	void stopObserving();
	void requestBecomeObserver(UnsignedInt hostIP);      ///< from a slot: after leaving the game, observe that room
	void sendChat(const UnicodeString &text);            ///< room chat of an observer (before the start)
	/// An outside viewer: 'extraSeconds' (0..180) on top of the delay the players' servers keep (O33).
	void startWatching(LANGameInfo *game, Int extraSeconds);
	/// O33: from the engine's update, outside the LAN screen's update: starts the stream's replay once its header is there,
	/// and ends the viewer state after that replay.
	void pollStart();
	/// O33: the LAN screen closes: a stream that has not started yet is given up.
	void cancelStarting();

	//--- O34: the room observers' own channel during the match ------------------------------------
	/// TRUE for a room observer whose stream started (an outside viewer: never), or a defeated player who joined the
	/// observers (O59).
	Bool observerChannelOpen();
	/// TheSuperHackers @feature O59 (row 47) A defeated player of a LAN match joins the room's observers: the channel
	/// with the room's observers of this match (the host's list at Start) and himself. Every second he says JOIN on the
	/// observers' text port to them and to the match's other players; a room observer (from its copy of the match) or
	/// another defeated player who joined takes the sender in only when the address is a player of this match and that
	/// player is defeated in its own copy of the game, so nobody still playing can listen to the observers.
	void openDefeatedObserverChannel(UnsignedInt localIP);
	void closeDefeatedObserverChannel();
	/// Sends a line to the other room observers (never to a player or an outside viewer) and shows it here.
	void sendObserverChat(const UnicodeString &text);
	/// The room's observers for the voice roster, this game included, in the same order on every observer (by address);
	/// 0 when this game is not a room observer in its stream.
	Int observerVoicePeers(UnsignedInt *ips, UnicodeString *names, Int maxCount, Int *localIndex);
	//--- O40 (row 25): "Play again" after a LAN match (also after one ended by a mismatch) ---------------
	/// The host at Start: keeps the room's slot layout (computer players with their army, colour, team and start, closed
	/// and open places; the humans' places become open places for them to join again). nullptr: forget it.
	void rememberMatchLayout(const LANGameInfo *game);
	/// The score screen: TRUE for the computer that hosted the LAN match that just ended.
	Bool canOfferRematch();
	/// The score screen's "Play again": back in the LAN screen, this computer opens a room again (TRUE once, for the
	/// LAN screen).
	void requestRematch();
	Bool takeRematchRequest();
	/// The host's new room (LanGameOptionsMenuInit, after the map's slots were set): the kept layout, once. The map,
	/// starting cash, superweapon limit, and the host's own army and colour come from the host's preferences as for any
	/// new room (written at every Start).
	void applyRematchLayout(LANGameInfo *game);

	/// O39: an outside viewer: the delay its sources keep (the host's), which the replay bar adds to the viewer's own lag
	/// so it shows the whole delay behind the match; 0 for everybody else.
	Int upstreamDelaySeconds();
	/// The LAN screen opens again: a viewer that played its stream goes back to normal (follow mode off, no mirror).
	void resetViewer();
	/// A line for the screen's chat (removed, full, waiting, lost ...); FALSE when none.
	Bool takeNotice(UnicodeString *text);
}
