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
// (O61, row 49: an outside viewer's is Viewer_<pid>_<n>.rep, deleted when the viewing ends)
// and its .live on a thread of its own, and plays that file in the follow mode of ReplayFollow.h (-followDelay and
// -followCatchUp work the same, and only add to the server's delay). With -serveReplay too it serves its mirror: a relay.
// -watchBind <ip> (test) makes its connections from that local address (the server tells room observers by address).
// Hop: when the source does not answer for 5 seconds, its newest frame does not move for 8 seconds (live viewers), it
// drops the viewer or rejects it, the viewer connects to the next source of its list and asks for the frames after the
// last one it has whole (O42). Each player's replay file has its own header (start time, local player), but the commands
// of a frame are the same in every file of the match, so a viewer resumes by frame, never by byte: every served replay
// has a frame index beside it ("<replay>.idx": "GIDX", game key, header end, then (frame, end offset) per frame), written
// by the recorder (ReplayFollow) or by a mirror, and a source finds the viewer's frame in its own file. A mirror writes
// only the header and whole frames to its file (bytes wait until the source names a frame's end), so it can always ask
// "after frame F". The game key (FNV-1a of the match's slot list, equal on every computer) marks the same game.
// O34 (sources of an outside viewer): each source has a rank: 0 an outside viewer that relays, 1 a room observer that
// relays, 2 a player. The viewer asks the lowest rank first. A server with live data (a player, a room observer) sends an
// outside viewer on to the relays it knows (MORE, then NOPE "elsewhere") and serves it itself only when it knows none or
// the viewer comes back from them within 30 s (hello flag bit 1, "last resort"); a full server does the same. A source
// that failed or refused is not asked again for 10 s. A server never feeds the address its own mirror reads from. The
// stream is lost only after 120 s without data (before: "reconnecting", bridged by the viewer's play-out reserve,
// ReplayFollow). A room observer keeps to live sources (it ignores relays of outside viewers).
//
// Wire (little-endian 32-bit words): the viewer sends "GRSV", version 3, resume frame (0xFFFFFFFF: from the start, header
// included; else the frames after this one), game key (0: not known yet), 0, flags (bit 0: it says it is a room observer;
// counted only, the address decides; bit 1: last resort), relay port (it serves its mirror there; 0: it does not): 28
// bytes. Older hellos are refused ("another game"). The source sends "GKEY" 12 {game key, its header end, the offset in its
// file where the data starts}, then "DATA" {offset, bytes} (offsets in the source's file), "FIDX" {count, count x (frame,
// end offset)} (frames whose bytes were all sent), "LIVE" 12 {frame, ended, counter} (at least every half second, only
// after the FIDX of that frame; once ended, after the rest of the file), "HOLD" 0 (a delayed viewer with no frame allowed
// yet, or a viewer ahead of this source, which waits up to 30 s for the source to reach its frame), "MORE" {count, count x
// (ip, port, kind 0 outside viewer / 1 room observer)} (relays), or "NOPE" 4 {reason: 0 refused, 1 go elsewhere, 2 outside
// viewers not allowed, 3 another game}.
// -serveReplayLagFor <ip>:<seconds> (test, O42): that viewer address is served that many seconds further behind (on top
// of its delay), as over a slow link; the replay itself grows by only tens of bytes a second, so -serveReplayRate cannot
// make a slow link.
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
	void configureServeLagFor(const char *spec);    ///< O42 (test): -serveReplayLagFor a.b.c.d:seconds
	void configureWatch(const char *sources);
	void configureWatchRoomObserver();              ///< -watchRoomObserver: the hello's flag (counting only)
	void configureWatchBind(const char *ip);        ///< O33 (test): -watchBind a.b.c.d

	/// O33: a LAN match's serving (players at the start; a viewer's relay): the address and port, the delay for viewers not
	/// listed live, whether outside viewers are allowed at all, and the live addresses (host order). 'receivesDelayed': this
	/// game's own copy is already delayed (an outside viewer's relay): it forwards what it has without a second delay.
	/// TheSuperHackers @bugfix O62 (H5 #3) 'refusedIps': the match's players (alive or defeated): this server never
	/// serves them (a second game on a player's PC would show him the whole map, only the host's delay behind).
	void configureMatchServing(UnsignedInt bindIpHostOrder, Int port, Int delaySeconds, Bool outsideAllowed,
		const UnsignedInt *liveIps, Int liveCount, Bool receivesDelayed, const UnsignedInt *refusedIps = nullptr,
		Int refusedCount = 0);
	/// O33: the recording (or the viewing) is over: the server closes its listener at once, gives the viewers it has the
	/// rest (a delayed viewer the last 'delay' of the match) and ends by itself; the match settings are forgotten.
	void stopServing();
	/// O33: the viewers this game serves now (for the players' HUD).
	void servingCounts(Int *roomObservers, Int *outsideViewers);

	/// TRUE when -watchReplay was given or a LAN viewer set its sources (the replay to play comes from a source).
	Bool isWatching();
	/// O31: the sources as addresses (host order) and one port; 'roomObserver' marks the hello; 'localIp' (O33): the address
	/// the viewer connects from (its LAN address: a server lets room observers in by address).
	/// O34: 'observerIps': the room's observers (an outside viewer asks their relays before the players).
	void configureWatchList(const UnsignedInt *ipsHostOrder, Int count, Int port, Bool roomObserver, UnsignedInt localIp,
		const UnsignedInt *observerIps, Int observerCount);
	/// O34: the mirror runs and its stream has not ended.
	Bool mirrorTrying();
	/// O34: the mirror runs but has no source giving it data right now.
	Bool mirrorReconnecting();
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
	/// O42: the last stream was lost because the host does not allow outside viewers (the room's text says so).
	Bool lostBecauseNotAllowed();
	/// O64 (H6, low): the last stream was refused because this computer is a player of the match (O62 H5 #3).
	Bool lostBecausePlayer();
	/// TheSuperHackers @feature O61 (row 49) Nothing of a watched match stays on an outside viewer's disk. Its mirror is
	/// "<user data>\ViewerStreams\Viewer_<pid>_<n>.rep" with its .live and .idx (O64, H6 #1: a folder of its own that only
	/// the game writes, never the replay folder; a room observer's mirror stays "Replays\Watched_..." and is kept, as
	/// before). discardViewerFiles (the viewing is over: LanRoom::resetViewer, the program's end) deletes them; a file
	/// another handle still holds is deleted as soon as it can be (pumpDeletes, every frame, at most 10 tries); the files
	/// of a viewer that crashed are deleted at the next start (sweepViewerLeftovers: in that folder only, and only names
	/// that are exactly "Viewer_<pid>_<n>.rep", ".rep.live" or ".rep.idx" of a process (pid not 0) that no longer runs).
	void discardViewerFiles();
	void pumpDeletes();
	void sweepViewerLeftovers();

	void shutdown();
}
