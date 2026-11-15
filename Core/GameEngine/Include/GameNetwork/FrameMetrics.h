/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

/** FrameMetrics.h */

#pragma once

#include "GameNetwork/NetworkDefs.h"

class FrameMetrics {
public:
	FrameMetrics();
	virtual ~FrameMetrics();

	void init();
	void reset();

	void doPerFrameMetrics(UnsignedInt frame);
	void processLatencyResponse(UnsignedInt frame, Bool cleanSample = TRUE);
	void addCushion(Int cushion);

	Real getAverageLatency();
	Real getRunAheadLatency();	///< O23: smoothed latency plus two mean deviations (seconds), what the run-ahead must cover
	Int getAverageFPS();
	Int getMinimumCushion();

protected:
	// These are used for keeping track of parameters to the run ahead equation.
	// frames per second history variables.
	Real *m_fpsList;								///< A record of how many game logic frames per second there were for the last 60 seconds.
	time_t m_lastFpsTimeThing;																///< The time when the last fps entry started being recorded.
	Int m_fpsListIndex;																				///< Index into the array of the current fps list entry being measured.
	Real m_averageFps;																				///< The current average logic fps, computed just like m_averageLatency below but with the fps numbers.

	// round trip time to packet router variables.
	// The lists are indexed off the frame number of the frame info packet they are associated with.
	// The index used should be the frame number mod the array length.
	Real *m_latencyList;				///< A record of the round trip latencies of the frame info packets to the packet router.  Values in seconds.
	time_t *m_pendingLatencies;																///< The latencies of frame info packets that are "in the air."
	Real m_averageLatency;																		///< The current average latency, this is used to save calculation time.
																														///< When a new latency value is received, the old one is subtracted out and the new
																														///< one is added in.

	// TheSuperHackers @bugfix O35 The frame each pending send time belongs to: an ack is a sample only for that frame.
	UnsignedInt *m_pendingFrames;
	Int m_uncleanSamples;   ///< O41: retried samples seen before the first clean one
	Bool m_latencyFromUnclean;   ///< O41: the estimate so far comes from retried samples only

	// TheSuperHackers @tweak O23 A faster latency estimate for the run-ahead: smoothed mean and mean deviation of the
	// clean samples (seconds). The average above over the last 200 frames reacts in seconds and took the samples of
	// frame infos that the retry timer had to send again (2 s and more each), which inflated the run-ahead under loss.
	Real m_smoothedLatency;
	Real m_latencyDeviation;
	Bool m_haveLatency;

	// packet arrival cushion variables.
	// Keeps track of the cushion for the incoming commands.
	UnsignedInt m_cushionIndex;															///< The next index to use for the cushion list.
	Int m_minimumCushion;																			///< The average cushion for the history.
};
