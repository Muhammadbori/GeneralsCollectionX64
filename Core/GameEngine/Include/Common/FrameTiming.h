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

// FrameTiming.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature Where the main thread's time goes, per engine update (item 22, counters only, O20).
//
// GameEngine::update is timed as a whole and in parts with QueryPerformanceCounter (a few calls per engine update):
//   logic    each TheGameLogic->UPDATE() of the update (and how many ran)
//   client   TheGameClient->UPDATE(): input, the client's own work and the draw
//   draw     TheDisplay->DRAW() inside the client update: building and submitting the frame, and the present
//   present  the swap chain's Present call inside the draw (DX12 device; WW3D_PresentMsTotal)
// FrameShots' frames.csv gets the means since its previous row (FrameShots::capture): engine_updates, eng_ms,
// logic_ms, logic_updates, client_ms (without the draw), draw_ms (without the present), present_ms, other_ms (radar,
// audio, message stream, network, the shots; everything in the engine update that is not logic or client).
// eng_ms is the engine update's own work; FramePacer's update_time_ms is the whole loop (that work, plus the frame
// limiter's wait and the OS messages).

#pragma once

#include "Lib/BaseType.h"

namespace FrameTiming
{
	void engineBegin();
	void engineEnd();
	void logicBegin();
	void logicEnd();
	void clientBegin();
	void clientEnd();
	void drawBegin();
	void drawEnd();

	struct Means
	{
		UnsignedInt engineUpdates;
		double engineMs, logicMs, logicUpdates, clientMs, drawMs, presentMs, otherMs;
	};
	/// The means per engine update since the previous call.
	Means takeMeans();
}

/// The time spent in the swap chain's Present, in milliseconds, since the start (WW3D2, the DX12 device).
extern double WW3D_PresentMsTotal;
