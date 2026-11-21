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

// TheSuperHackers @feature O136 Logic CRC of every logic frame, for comparing two builds (another compiler, another
// platform) frame by frame on the same replay.
//
// "-logicCrcLog <file>" on a test exe (CommandLine::isTestExe, a name with "uitest") writes one line per logic frame of a
// game or replay: "<frame> <crc>" in hex, after a header line with the exe CRC and the logic build identity. The CRC is
// the one the game sends and records (GameLogic::getCRC), computed at the same point of the frame; on frames where the
// game does not compute it anyway it is computed for the log only. It is read only, so the logic and the replay's own
// CRC checks are the same with and without the log. Product exes ignore the switch.
#pragma once

namespace LogicCrcLog
{
	Bool enabled();
	void frame(UnsignedInt frame, UnsignedInt crc);
}
