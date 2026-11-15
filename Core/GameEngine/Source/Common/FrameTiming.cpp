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

// FrameTiming.cpp //////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See FrameTiming.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/FrameTiming.h"

namespace
{
	double s_toMs = 0.0;
	__int64 s_engineStart = 0, s_logicStart = 0, s_clientStart = 0, s_drawStart = 0;
	double s_presentAtDrawStart = 0.0;

	// the engine update in progress (added to the sums when it ends, so a row written in the middle of an update
	// covers whole updates only)
	double s_curLogic = 0.0, s_curClient = 0.0, s_curDraw = 0.0, s_curPresent = 0.0;
	UnsignedInt s_curLogicUpdates = 0;

	// sums since the last takeMeans
	UnsignedInt s_updates = 0;
	double s_engine = 0.0, s_logic = 0.0, s_client = 0.0, s_draw = 0.0, s_present = 0.0;
	UnsignedInt s_logicUpdates = 0;

	__int64 now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		if (s_toMs == 0.0)
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			s_toMs = 1000.0 / (double)f.QuadPart;
		}
		return t.QuadPart;
	}
}

void FrameTiming::engineBegin()
{
	s_engineStart = now();
	s_curLogic = s_curClient = s_curDraw = s_curPresent = 0.0;
	s_curLogicUpdates = 0;
}

void FrameTiming::engineEnd()
{
	s_engine += (now() - s_engineStart) * s_toMs;
	++s_updates;
	s_logic += s_curLogic;
	s_logicUpdates += s_curLogicUpdates;
	s_client += s_curClient;
	s_draw += s_curDraw;
	s_present += s_curPresent;
}

void FrameTiming::logicBegin() { s_logicStart = now(); }
void FrameTiming::logicEnd() { s_curLogic += (now() - s_logicStart) * s_toMs; ++s_curLogicUpdates; }
void FrameTiming::clientBegin() { s_clientStart = now(); }
void FrameTiming::clientEnd() { s_curClient += (now() - s_clientStart) * s_toMs; }

void FrameTiming::drawBegin()
{
	s_drawStart = now();
	s_presentAtDrawStart = WW3D_PresentMsTotal;
}

void FrameTiming::drawEnd()
{
	s_curDraw += (now() - s_drawStart) * s_toMs;
	s_curPresent += WW3D_PresentMsTotal - s_presentAtDrawStart;
}

FrameTiming::Means FrameTiming::takeMeans()
{
	Means m;
	const double n = s_updates > 0 ? (double)s_updates : 1.0;
	m.engineUpdates = s_updates;
	m.engineMs = s_engine / n;
	m.logicMs = s_logic / n;
	m.logicUpdates = s_logicUpdates / n;
	m.presentMs = s_present / n;
	m.drawMs = (s_draw - s_present) / n;           // building and submitting the frame
	m.clientMs = (s_client - s_draw) / n;          // the client update without its draw
	m.otherMs = (s_engine - s_logic - s_client) / n;
	s_updates = 0;
	s_engine = s_logic = s_client = s_draw = s_present = 0.0;
	s_logicUpdates = 0;
	return m;
}
