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

// NetLab.h /////////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature A real network (LAN) match on one PC, for tests (O19).
//
// Every instance gets the same roster on its command line and its own slot:
//   -netLab <slot> -netLabPlayers "0:1:-1,1:2:-1" [-netLabMap <map>] [-netLabSeed <n>] [-netLabSim "<settings>"]
//   [-netLabQuitAfter <seconds>]
// -netLabPlayers: <slot>:<team>:<player template>[:easy|medium|brutal] per slot; team -1 = none; template -1 = random;
// with an AI level the slot is a computer player (played on every computer, as in a LAN game).
// Human slot N is the computer at 127.0.0.(10 + N), UDP port 8088 (NETWORK_BASE_PORT_NUMBER, as in a LAN game, where every
// player has its own address and the same port). Nothing binds to another address: the whole game traffic stays on the
// loopback. When the shell's main menu is idle, the instance builds the LAN game object the lobby would have built (the
// same slots, map, seed, every slot "has the map") and gives it to a LANAPI object that is never initialised (so the
// lobby's broadcast socket is never opened), then calls the LAN code's own LANAPI::OnGameStart: TheNetwork, its
// transport, the connection manager, the packet router, the lockstep, the CRC exchange and the disconnect logic are the
// real ones. The players "find" each other through the roster on the command line instead of the lobby.
//
// Network condition simulator (test only, every received packet of the game transport, after the transport's own checks):
//   delay=<ms> jitter=<ms> loss=<percent> reorder=<percent> kbps=<incoming bandwidth, 0 = no cap> freeze=<ms>
// given by -netLabSim and changed during a run by writing <user data>\NetLabSim.txt (read twice a second when it
// changes). freeze=<ms> makes the game thread sleep once for that long (a starved computer). O35: resend=<slot> makes
// this computer treat that player's next frame as broken once (it resets the frame and asks for a resend). O41: dropcmd=<slot>
// makes this computer ack the next game command of that player but never use it (a lost command): the frame waits, and the
// stuck-frame request (after 2 s) must fetch it again. Packets keep their order
// unless reorder is set; a packet that would wait more than a second for the bandwidth is dropped.
//
// Measurement: <user data>\NetLab.csv, one line per second (time, logic frame, logic frames in the second, the
// network's frame rate and run-ahead, milliseconds stalled and the slots waited for, packets and bytes in and out,
// command resends, frame-data resend requests, simulator drops, disconnect screen, connected slots, and since O23 the extra
// command copies, the largest retry time and the run-ahead latency in ms), and NetLab.log
// for events (start, disconnect screen on and off, a player gone, the simulator's settings).

#pragma once

#include "Lib/BaseType.h"

struct TransportMessage;

namespace NetLab
{
	// command line
	void configureSlot(Int slot);
	void configurePlayers(const char *roster);
	void configureMap(const char *map);
	void configureSeed(Int seed);
	void configureSim(const char *settings);
	void configureQuitAfter(Int seconds);
	Bool isConfigured();

	/// Every client update: starts the match from the shell, writes the measurement, reads the simulator file.
	void update();
	void shutdown();

	// counters from the network code (cheap, game thread)
	void countSent(Int bytes);
	void countReceived(Int bytes);
	void countRetry();
	void countFrameResend();
	void countCopy();                   ///< O23: an extra copy of an unacknowledged command went out
	void noteRetryTime(Int ms);         ///< O23: a connection's current retry time
	void noteRunAheadLatency(Int ms);   ///< O23: the latency the run-ahead is computed from
	void noteStallNotice(UnsignedInt mask);   ///< O23: the "waiting for" notice was shown (a log line)
	void noteWaitingFor(Int slot);
	/// O35: TRUE once after "resend=<slot>" was given (-netLabSim or NetLabSim.txt): that slot's next frame counts as broken
	/// on this computer, so a frame resend is requested (the path of S21 R5).
	Bool takeForcedResend(Int slot);
	/// O41: "dropcmd=<slot>": TRUE for the next game command of that slot and for its copies (same command id) during the
	/// next second; the command is acked but never used (a lost command, for the stuck-frame request).
	Bool dropCommand(Int playerId, Bool isGameCommand, UnsignedShort commandId);
	void countStuckResend();                                        ///< O41: a resend request for a stuck frame (log line)
	void noteResendRefused(Int playerId, UnsignedInt frame, UnsignedInt now);   ///< O41: a request for a frame no longer kept

	// the simulator, from the transport: TRUE when it took the packet (to be delivered later, or dropped)
	Bool simTake(const TransportMessage &message);
	/// a held packet that is due now; FALSE when there is none
	Bool simRelease(TransportMessage &message);
}
