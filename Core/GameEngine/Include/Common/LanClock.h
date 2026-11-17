/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// TheSuperHackers @bugfix O75 The clock of the LAN lobby, the room, the anti-cheat link and the match stream.
//
// - now() is timeGetTime() plus a test offset (-testClockOffset <ms>, *uitest*.exe only), so a test can run these
//   timers at 26 days of uptime or across the 49.7-day wrap. The game logic and the in-match network do not use it.
// - now() never returns 0, so a stamp of 0 means "never set" and nothing else.
// - due() and silentFor() test that "never" explicitly and compare the rest as a signed difference, which is right at
//   any uptime and across the wrap for stamps less than 24.8 days old. A bare (Int)(now - 0) is negative from 24.86
//   days of uptime on: that was the O73 regression (O75).

#pragma once

namespace LanClock
{
	extern UnsignedInt g_testOffsetMs;   ///< CommandLine.cpp: -testClockOffset (tests only)

	inline UnsignedInt now()
	{
		const UnsignedInt t = (UnsignedInt)timeGetTime() + g_testOffsetMs;
		return t != 0 ? t : 1;
	}

	/// a stamp computed from now() (a deadline now + n): never 0, which means "never set"
	inline UnsignedInt stamp(UnsignedInt t) { return t != 0 ? t : 1; }

	/// a periodic action: due when it never ran (last == 0) or ran periodMs or more ago
	inline Bool due(UnsignedInt nowMs, UnsignedInt last, UnsignedInt periodMs)
	{
		return last == 0 || (Int)(nowMs - last) >= (Int)periodMs;
	}

	/// silence: never heard (heard == 0), or not for more than ms
	inline Bool silentFor(UnsignedInt nowMs, UnsignedInt heard, UnsignedInt ms)
	{
		return heard == 0 || (Int)(nowMs - heard) > (Int)ms;
	}
}
