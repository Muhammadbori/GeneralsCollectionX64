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

// ReplayStream.h ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The live replay over a connection (item 21, second step), built on ReplayFollow.h.
//
// Source: a game that records (a player) or a viewer that mirrors (a relay) serves its replay file and its side file
// "<replay>.live" (GLIV, newest complete frame, ended, counter) to viewers over TCP.
//   - The server runs on a thread of its own (below normal priority, its ideal processor another core than the one that
//     started it) and only reads the two files: the game never waits for it, and never joins it (O33: a server or mirror
//     is stopped by a generation number; it ends by itself within a fraction of a second).
//   - At most -serveReplayMax viewers (default 16, at most 100), at most -serveReplayRate KB per second for all of them
//     together (default 4096). A viewer whose data does not drain for 10 seconds is dropped, never waited for. O33: a
//     connection without a complete, valid hello within 3 seconds is closed; at most 3 connections per address.
// O33 (the delay is the server's, not the viewer's):
//   - A LAN match configures its serving at the match start (configureMatchServing): the LAN address, port 8090, the
//     host's delay (10 s to 3 min, default 60 s), whether outside viewers are allowed, and the room observers' addresses
//     (frozen at the start, from the host). Only those addresses get the stream live. Every other viewer gets nothing newer
//     than (now - delay): the server keeps the size of the replay and its side file over time and serves each delayed
//     viewer the state of that moment. A relay that itself receives a delayed stream forwards what it has (never faster
//     than it received it); a room observer's relay delays outside viewers as a player does.
//   - Serving ends when the recording ends (stopServing); a later game serves only when it is again a LAN match that
//     allows viewers, or with -serveReplay.
//   - -serveReplay <port> (test, no LAN): serves on 127.0.0.1 to everybody live, unless -serveReplayDelay <seconds> is
//     given; -serveReplayLiveFor <ip,ip> names the live addresses. Both also apply to a LAN match's serving (NetLab has no
//     lobby and so no START: these give its players a live list and a short delay). Server and mirror log their newest and
//     delayed frame every 10 s.
// Viewer: -watchReplay <host:port>[,<host:port>...] mirrors the source's replay into <user data>\Replays\Watched_<pid>_<n>.rep
// and its .live on a thread of its own, and plays that file in the follow mode of ReplayFollow.h (-followDelay and
// -followCatchUp work the same, and only add to the server's delay). With -serveReplay too it serves its mirror: a relay.
// -watchBind <ip> (test) makes its connections from that local address (the server tells room observers by address).
// Hop: when the source does not answer for 5 seconds, its newest frame does not move for 8 seconds (live viewers), it
// drops the viewer or rejects it, the viewer connects to the next source of its list and asks for the bytes after those
// it has. A source is accepted only when the first bytes of its replay are the viewer's (the same game).
//
// Wire (little-endian 32-bit words): the viewer sends "GRSV", version 2, offset, idLength, idHash (FNV-1a of its first
// idLength bytes, at most 1024), flags (bit 0: it says it is a room observer; counted only, the address decides), relay
// port (it serves its mirror there; 0: it does not): 28 bytes (version 1: the first 20). The source sends messages "DATA"
// length {offset, bytes}, "LIVE" 12 {frame, ended, counter} (at least every half second), "HOLD" 0 (a delayed viewer: no
// data allowed yet), "MORE" {count, count x (ip, port)} (full: relays), or "NOPE" 0 (rejected). The source sends LIVE for a
// frame only after the bytes that were in its file when it read that frame's side file.
// -serveReplayStopAfter <seconds> (test only): the server stops after that time (its game goes on): a forced source loss.
// Each game with a server or a mirror writes ReplayStream.log in its user data folder.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

namespace ReplayStream
{
	// command line
	void configureServe(Int port);
	void configureServeMax(Int viewers);
	void configureServeRate(Int kilobytesPerSecond);
	void configureServeStopAfter(Int seconds);
	void configureServeOutsideMax(Int viewers);
	void configureServeDelay(Int seconds);          ///< O33 (test): -serveReplay's delay for every viewer not listed live
	void configureServeLiveFor(const char *ips);    ///< O33 (test): -serveReplayLiveFor a.b.c.d[,a.b.c.d...]
	void configureWatch(const char *sources);
	void configureWatchRoomObserver();              ///< -watchRoomObserver: the hello's flag (counting only)
	void configureWatchBind(const char *ip);        ///< O33 (test): -watchBind a.b.c.d

	/// O33: a LAN match's serving (players at the start; a viewer's relay): the address and port, the delay for viewers not
	/// listed live, whether outside viewers are allowed at all, and the live addresses (host order). 'receivesDelayed': this
	/// game's own copy is already delayed (an outside viewer's relay): it forwards what it has without a second delay.
	void configureMatchServing(UnsignedInt bindIpHostOrder, Int port, Int delaySeconds, Bool outsideAllowed,
		const UnsignedInt *liveIps, Int liveCount, Bool receivesDelayed);
	/// O33: the recording (or the viewing) is over: the server closes its listener at once, gives the viewers it has the
	/// rest (a delayed viewer the last 'delay' of the match) and ends by itself; the match settings are forgotten.
	void stopServing();
	/// O33: the viewers this game serves now (for the players' HUD).
	void servingCounts(Int *roomObservers, Int *outsideViewers);

	/// TRUE when -watchReplay was given or a LAN viewer set its sources (the replay to play comes from a source).
	Bool isWatching();
	/// O31: the sources as addresses (host order) and one port; 'roomObserver' marks the hello; 'localIp' (O33): the address
	/// the viewer connects from (its LAN address: a server lets room observers in by address).
	void configureWatchList(const UnsignedInt *ipsHostOrder, Int count, Int port, Bool roomObserver, UnsignedInt localIp);
	/// O31: starts the mirror without waiting for the header; 'localPath' is the mirror file.
	Bool beginWatching(AsciiString &localPath);
	Bool headerReady();
	/// O31/O33: stops the mirror (without waiting for it) and forgets the sources.
	void stopWatching();
	/// Starts the mirror if needed and waits (at most 60 seconds) until the replay's header is in it (the command line
	/// viewer at its start). FALSE: no source gave it.
	Bool startWatching(AsciiString &localPath);

	/// Serves this replay file (and its .live) when serving is configured (a LAN match, or -serveReplay); a running server
	/// switches to it.
	void serveFile(const AsciiString &replayPath);

	/// TRUE once after every source was gone for 30 seconds (the stream is lost; the replay then plays to its end).
	Bool takeLostNotice();

	void shutdown();
}
