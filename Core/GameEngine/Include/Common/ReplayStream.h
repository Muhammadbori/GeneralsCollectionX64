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
// "<replay>.live" (GLIV, newest complete frame, ended, counter) to viewers over TCP: -serveReplay <port>.
//   - The server runs on a thread of its own (below normal priority, its ideal processor another core than the one that
//     started it) and only reads the two files: the game never waits for it.
//   - At most -serveReplayMax viewers (default 16, at most 100), at most -serveReplayRate KB per second for all of them
//     together (default 4096). A viewer whose data does not drain for 10 seconds is dropped, never waited for.
//   - Tonight the sockets bind to 127.0.0.1 only (a listening socket on a real interface raises the Windows firewall
//     dialog).
// Viewer: -watchReplay <host:port>[,<host:port>...] mirrors the source's replay into <user data>\Replays\Watched_<pid>.rep
// and its .live on a thread of its own, and plays that file in the follow mode of ReplayFollow.h (-followDelay and
// -followCatchUp work the same). With -serveReplay too it serves its mirror to further viewers: a relay.
// Hop: when the source does not answer for 5 seconds, its newest frame does not move for 8 seconds, it drops the viewer
// or rejects it, the viewer connects to the next source of its list and asks for the bytes after those it has; the
// replay goes on from there, without a restart. A source is accepted only when the first bytes of its replay are the
// viewer's (the same game).
//
// Wire (little-endian 32-bit words): the viewer sends "GRSV", version 1, offset, idLength, idHash (FNV-1a of its first
// idLength bytes, at most 1024). The source sends messages "DATA" length {offset, bytes}, "LIVE" 12 {frame, ended,
// counter} (at least every half second), or "NOPE" 0 (rejected: another game, or full). The source sends LIVE for a frame
// only after the bytes that were in its file when it read that frame's side file.
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
	void configureWatch(const char *sources);

	/// TRUE when -watchReplay was given (the replay to play comes from a source).
	Bool isWatching();
	/// Starts the mirror and waits (at most 60 seconds) until the replay's header is in the mirror file. FALSE: no source
	/// gave it. 'localPath' is the mirror file to play.
	Bool startWatching(AsciiString &localPath);

	/// Serves this replay file (and its .live) when -serveReplay was given; a running server switches to it.
	void serveFile(const AsciiString &replayPath);

	void shutdown();
}
