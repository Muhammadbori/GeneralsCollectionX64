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

// ReplayFollow.h ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature A replay that can be watched while it is still being written (item 21, first step).
//
// The writer (the recording game) keeps a small side file next to the replay, "<replay>.live", 16 bytes:
//   "GLIV", the newest logic frame whose commands are all in the replay file (written after the flush of that frame),
//   an "ended" flag (set when the recording stops), and a counter that grows with every write.
// The game never waits for a follower: the writer only writes this file.
//
// The follower plays the replay with -followReplay <file>. At the end of the replay data it does not stop while the
// writer is alive (side file present, not ended, its counter moved in the last 10 seconds): it holds the logic before
// a frame the writer has not finished, shows that it waits, and goes on when more data is there. -followDelay <seconds>
// (0 to 180) keeps it that far behind the writer's newest frame; when it is further behind than that, it runs faster
// (-followCatchUp <factor>, 1 to 8, default 2) until it is at the delay again. The replay's own CRC checks compare the
// follower's logic with the writer's as in any replay.
//
// -followReplay also lets the follower run next to the writer on the same PC (another instance: multi-instance, the
// primary instance skipped), as -replay does. It is the explicit test switch for two game instances at once.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

namespace ReplayFollow
{
	//--- writer ------------------------------------------------------------------------------------
	void writerStarted(const AsciiString &replayPath);
	/// O42: the header is written (and flushed): its end, and the match's slot list (the game key, equal on every computer
	/// of the match). Starts the frame index "<replay>.idx" (ReplayStream.h).
	void writerHeader(UnsignedInt headerEnd, const AsciiString &slotList);
	/// Call after the commands of 'frame' were written and flushed; 'endOffset' (O42): the file's size then.
	void writerFrame(UnsignedInt frame, UnsignedInt endOffset);
	void writerEnded();

	//--- follower -----------------------------------------------------------------------------------
	void configure(Int delaySeconds, Real catchUpFactor);   ///< from the command line; turns following on
	/// O34: a stream viewer's play-out reserve (0..60 s): it plays this far behind the allowed frame, so the loss of a source
	/// is bridged while the mirror finds another; inside the reserve it plays 10 % slower until the reserve is full again.
	void configureReserve(Int seconds);
	void turnOff();                                          ///< O31: a LAN viewer's replay is over: later replays play normally
	Bool isOn();
	void readerStarted(const AsciiString &replayPath, Int normalLogicFps);
	void readerEnded();
	/// TRUE while the writer may still add data (wait at the end of the file instead of stopping).
	Bool writerAlive();
	/// Call before each logic update of a followed replay with the frame it would run. FALSE: hold it this time.
	Bool mayRunFrame(UnsignedInt frame);
	/// The writer's newest finished frame (0 before the side file was read). For the replay control bar.
	UnsignedInt newestFrame();
}
