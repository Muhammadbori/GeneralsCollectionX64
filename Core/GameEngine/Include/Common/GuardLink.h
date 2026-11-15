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

// GuardLink.h //////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O30 (row 15) The game's side of the anti-cheat. The anti-cheat itself is a separate program,
// GenGuard.exe, beside the game exe; it is not part of this source and is not linked into the game. This module only
// starts it, talks to it over a local named pipe, and relays its opaque bytes to the other players inside the game's own
// LAN messages (LANMessage::MSG_GUARD_DATA). Nothing here is secret.
//
// Start: once per game process, at engine start, when GenGuard.exe is beside the game exe:
//     GenGuard.exe --pid <this pid> --pipe \\.\pipe\GenGuard-<pid>-<random> --userdata "<user data folder>" [--test]
// in a job object that ends it when the game ends (also on a crash). The guard creates the pipe; the game connects.
//
// Pipe frames, both directions, little endian: u32 magic 0x314C4747 ("GGL1"), u16 type, u16 payload length (at most
// 512), then the payload.
//     1 HELLO    game -> guard   u32 protocol (1), u32 game pid
//     2 STATUS   guard -> game   u32 protocol, u32 guard version, u32 state (0 checking, 1 ready), u32 fail bits
//     3 CHECK    game -> guard   u32 peer ip, u32 own ip           (please check that player)
//     4 SEND     guard -> game   u32 peer ip, opaque bytes (<= 200)  (give these bytes to that player)
//     5 RECV     game -> guard   u32 from ip, u32 own ip, opaque bytes (that player sent these bytes)
//     6 VERDICT  guard -> game   u32 peer ip, u32 verdict (0 no answer, 1 verified, 2 failed), u32 fail bits
//     7 REASON   guard -> game   O32: u32 kind (0 none, 1 exe, 2 file, 3 module, 4 program, O38: 5 an unreadable file), UTF-16 text: what this
//                                player's own check found (a path in the game folder or a file name), for this player's
//                                own chat only; other players learn only the kind from the fail bits of a verdict
// Fail bits (informational): 0x1 exe not official, 0x2 gameplay file changed, 0x4 override file, 0x8 foreign module,
// 0x10 cheat program running, 0x20 official list missing, 0x40 wrong pipe client, 0x80 first check running, 0x100 test
// build seen by a normal guard, 0x200 another official list, 0x400 answer not signed by a guard, 0x800 (O32) another
// release file (art, sound, movies, packs; the second pass) missing or changed.
//
// LAN: MSG_GUARD_DATA carries u32 magic "GRD1", u16 length, the opaque bytes, to one address. The game passes what
// arrives to its guard and does not look inside.
//
// Rules in the game: no LAN screen without a connected guard ("Start the anti-cheat first"); the shield column shows
// the guard's verdict for each player; a host (setting LanRequireGuard, on by default) refuses a join from a player whose
// guard is not verified; during a match a player whose check fails is named in a message line.
// Test switches: -lanNoGuard (no guard started, LAN allowed, no shield), -guardTest (the guard accepts exe names with
// "uitest" as a test build; a normal guard marks such a player red).

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

namespace GuardLink
{
	enum Verdict { VERDICT_NONE = 0, VERDICT_VERIFIED = 1, VERDICT_FAILED = 2 };

	typedef void (*SendFunc)(UnsignedInt ip, const UnsignedByte *data, Int length);

	void setNoGuard(Bool on);       ///< -lanNoGuard
	void setTestMode(Bool on);      ///< -guardTest
	Bool noGuard();

	void start();                   ///< engine start: starts GenGuard.exe when it is beside the game
	void update();                  ///< every engine update; cheap (a peek at the pipe)
	void shutdown();                ///< engine end: the guard ends with the pipe and the job

	Bool connected();               ///< the guard runs and its pipe is open
	Bool lanEntryAllowed();         ///< connected, or -lanNoGuard
	UnicodeString refusalTitle();
	UnicodeString refusalText();    ///< "Start the anti-cheat first."

	Verdict ownVerdict();           ///< this player's own checks (verified when they all pass)
	UnsignedInt ownFailBits();
	Verdict verdictOf(UnsignedInt ip);

	/// A LAN message came from 'ip': that player is checked now and then while it is seen.
	void notePeer(UnsignedInt ip, UnsignedInt ownIp);
	void onLanData(UnsignedInt fromIp, UnsignedInt ownIp, const UnsignedByte *data, Int length);
	void setSender(SendFunc func);

	/// The host's rule: refuse players without a verified shield (Options.ini LanRequireGuard, default yes); only when
	/// this game's own guard runs (a host without a guard cannot verify anyone).
	Bool hostRequiresGuard();
	UnicodeString joinRefusedText();
	/// A player whose verdict turned to failed since the last call (for the match message line); FALSE: none.
	Bool takeNewFailure(UnsignedInt *ip);
	UnicodeString failedInMatchText(const UnicodeString &playerName, UnsignedInt ip);
	UnicodeString ownFailureText();
	/// O32: TRUE once each time this player's own check found something new (the line names the file, module or program)
	Bool takeOwnFailureNotice(UnicodeString *text);
	/// O32: the kind of another player's failure ("files differ", "foreign module", ...), never a path of their PC
	UnicodeString failKindOf(UnsignedInt ip);
	/// O32: the chat line for a player whose verdict turned to failed in the lobby
	UnicodeString failedInLobbyText(const UnicodeString &playerName, UnsignedInt ip);
}
