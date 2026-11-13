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

// StateTrace.h /////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature State trace (-stateTrace): StateTrace.log in the user data folder, for finding where two runs
// that should be equal part. One line per logic update with the logic and client random generator states and the client
// clock; one line per logic random draw made outside a logic update; one line per InitRandom; with -stateTraceCalls A-B
// one line per logic random draw in logic frames A to B. Off by default: no file, and each hook below is one flag test.
// The trace only reads state; it draws no random number and changes no order or CRC.

#pragma once

#include "Lib/BaseType.h"

namespace StateTrace
{
	extern Bool g_on;    ///< -stateTrace or -stateTraceCalls on the command line

	// Random generators (RandomValue.cpp)
	void logicDraw(const char *file, Int line, double lo, double hi, double value);       ///< a draw that moved the logic seed
	void logicUnchanged(const char *file, Int line, double lo, double hi, double value);  ///< GetGameLogicRandomValue[Real]Unchanged
	void clientDraw();
	void initRandom(UnsignedInt seed, Bool fromTime, void *caller);

	// Logic update (GameLogic::update of both games): a line at the end of each
	struct LogicUpdateScope
	{
		LogicUpdateScope() { if (g_on) begin(); }
		~LogicUpdateScope() { if (g_on) end(); }
		void begin();
		void end();
		UnsignedInt m_frame;
	};

	// Client clock (W3DDisplay::update, the terrain cloud layer)
	void displayUpdate(UnsignedInt syncTimeMS, Real logicFrameTimeMS);
	void cloudUpdate(Real frameTimeSeconds, Real xOffset, Real yOffset);
}

#define STATE_TRACE(call)	do { if (StateTrace::g_on) StateTrace::call; } while (0)
