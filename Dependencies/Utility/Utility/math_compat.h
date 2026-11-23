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

// TheSuperHackers @build O152 (headless port) The Microsoft C runtime's floating point classification names (macOS, Linux).
#pragma once
#include <math.h>

#define _isnan(x) isnan(x)
#define _finite(x) isfinite(x)

// O154: the floating point control calls of GameLogic. These platforms (arm64, and x64 with SSE) have no x87 precision
// control, the same as the Windows x64 build, so _PC_24 changes nothing there either; the rounding mode is set as asked
// (the game asks for round to nearest, the default). This is the same arithmetic as the Windows x64 build.
#include <fenv.h>
#define _MCW_RC 0x00000300
#define _RC_NEAR 0x00000000
#define _RC_DOWN 0x00000100
#define _RC_UP 0x00000200
#define _RC_CHOP 0x00000300
#define _MCW_PC 0x00030000
#define _PC_64 0x00000000
#define _PC_53 0x00010000
#define _PC_24 0x00020000
inline void _fpreset(void) { fesetround(FE_TONEAREST); }
inline unsigned int _statusfp(void) { return 0; }
inline unsigned int _controlfp(unsigned int value, unsigned int mask)
{
	if (mask & _MCW_RC)
	{
		switch (value & _MCW_RC)
		{
			case _RC_DOWN: fesetround(FE_DOWNWARD); break;
			case _RC_UP: fesetround(FE_UPWARD); break;
			case _RC_CHOP: fesetround(FE_TOWARDZERO); break;
			default: fesetround(FE_TONEAREST); break;
		}
	}
	return value;
}
