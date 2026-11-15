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

/** FrameMetrics.cpp */

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <numeric>

#include "GameNetwork/FrameMetrics.h"
#include "GameClient/Display.h"
#include "GameNetwork/networkutil.h"

FrameMetrics::FrameMetrics()
{
	m_averageFps = 0.0f;
	m_averageLatency = 0.0f;
	m_cushionIndex = 0;
	m_fpsListIndex = 0;
	m_lastFpsTimeThing = 0;
	m_minimumCushion = 0;
	m_smoothedLatency = 0.2f;
	m_latencyDeviation = 0.0f;
	m_haveLatency = FALSE;

	m_pendingLatencies = NEW time_t[MAX_FRAMES_AHEAD];
	m_pendingFrames = NEW UnsignedInt[MAX_FRAMES_AHEAD];
	m_uncleanSamples = 0;
	m_latencyFromUnclean = FALSE;
	for (Int i = 0; i < MAX_FRAMES_AHEAD; ++i) {
		m_pendingFrames[i] = 0xFFFFFFFF;
	}
	for(Int i = 0; i < MAX_FRAMES_AHEAD; i++)
		m_pendingLatencies[i] = 0;
	m_fpsList = NEW Real[TheGlobalData->m_networkFPSHistoryLength];
	m_latencyList = NEW Real[TheGlobalData->m_networkLatencyHistoryLength];
}

FrameMetrics::~FrameMetrics() {
	delete m_fpsList;
	m_fpsList = nullptr;

	delete m_latencyList;
	m_latencyList = nullptr;

	delete[] m_pendingLatencies;
	delete[] m_pendingFrames;
	m_pendingLatencies = nullptr;
	m_pendingFrames = nullptr;
}

void FrameMetrics::init() {
	m_averageFps = 30;
	m_averageLatency = (Real)0.2;
	m_minimumCushion = -1;

	UnsignedInt i = 0;
	for (; i < TheGlobalData->m_networkFPSHistoryLength; ++i) {
		m_fpsList[i] = 30.0;
	}
	m_fpsListIndex = 0;
	for (i = 0; i < TheGlobalData->m_networkLatencyHistoryLength; ++i) {
		m_latencyList[i] = (Real)0.2;
	}
	m_cushionIndex = 0;
	m_smoothedLatency = (Real)0.2;
	m_latencyDeviation = 0.0f;
	m_haveLatency = FALSE;
	m_uncleanSamples = 0;
	m_latencyFromUnclean = FALSE;
	for (Int k = 0; k < MAX_FRAMES_AHEAD; ++k) {
		m_pendingFrames[k] = 0xFFFFFFFF;   // O35
	}
}

void FrameMetrics::reset() {
	init();
}

void FrameMetrics::doPerFrameMetrics(UnsignedInt frame) {
	// Do the measurement of the fps.
	time_t curTime = timeGetTime();
	if ((curTime - m_lastFpsTimeThing) >= 1000) {
//		if ((m_fpsListIndex % 16) == 0) {
//			DEBUG_LOG(("FrameMetrics::doPerFrameMetrics - adding %f to fps history. average before: %f ", m_fpsList[m_fpsListIndex], m_averageFps));
//		}
		m_averageFps -= ((m_fpsList[m_fpsListIndex])) / TheGlobalData->m_networkFPSHistoryLength; // subtract out the old value from the average.
		m_fpsList[m_fpsListIndex] = TheDisplay->getAverageFPS();
//		m_fpsList[m_fpsListIndex] = TheGameClient->getFrame() - m_fpsStartingFrame;
		m_averageFps += ((Real)(m_fpsList[m_fpsListIndex])) / TheGlobalData->m_networkFPSHistoryLength; // add the new value to the average.
//		DEBUG_LOG(("average after: %f", m_averageFps));
		++m_fpsListIndex;
		m_fpsListIndex %= TheGlobalData->m_networkFPSHistoryLength;
		m_lastFpsTimeThing = curTime;
	}

	Int pendingLatenciesIndex = frame % MAX_FRAMES_AHEAD;
	m_pendingLatencies[pendingLatenciesIndex] = curTime;
	m_pendingFrames[pendingLatenciesIndex] = frame;   // O35

}

void FrameMetrics::processLatencyResponse(UnsignedInt frame, Bool cleanSample) {
	time_t curTime = timeGetTime();
	Int pendingIndex = frame % MAX_FRAMES_AHEAD;
	time_t timeDiff = curTime - m_pendingLatencies[pendingIndex];

	// O23: the ack of a frame info the retry timer sent again gives no latency (it waited for the timer, not the line)
	// O35: and only the ack of the frame info this computer sent for that very frame is a sample. The packet router got
	// here also for the acks of the frame infos it relayed for other players, often for a frame it had not sent its own
	// frame info for yet: the time then belonged to the frame MAX_FRAMES_AHEAD earlier (about 4 s), and the router's
	// estimate stayed at 5 to 6 s (S21 R4: run-ahead 64 with no delay at all). A sample longer than the retry limit (2 s)
	// is no latency either.
	if (timeDiff < 0 || timeDiff > 2000 || m_pendingFrames[pendingIndex] != frame) {
		return;
	}
	// O41 (review 5): a retried frame info's ack is no clean sample; but a computer that has had no clean sample yet after
	// 20 retried ones takes those (each at most 2 s, an overestimate), so it does not keep the 0.2 s start value on a link
	// where every first send needs the timer. The first clean sample replaces them.
	if (!cleanSample)
	{
		if (m_haveLatency || ++m_uncleanSamples < 20) {
			return;
		}
		m_latencyFromUnclean = TRUE;
	}
	else if (m_latencyFromUnclean)
	{
		m_haveLatency = FALSE;   // the first clean sample starts the estimate anew
		m_latencyFromUnclean = FALSE;
	}
	const Real sample = (Real)timeDiff / (Real)1000;
	if (!m_haveLatency) {
		m_smoothedLatency = sample;
		m_latencyDeviation = sample / 2.0f;
		m_haveLatency = TRUE;
	} else {
		const Real err = sample - m_smoothedLatency;
		m_smoothedLatency += err / 16.0f;
		m_latencyDeviation += (fabsf(err) - m_latencyDeviation) / 8.0f;
	}

	Int latencyListIndex = frame % TheGlobalData->m_networkLatencyHistoryLength;
	m_latencyList[latencyListIndex] = (Real)timeDiff / (Real)1000; // convert to seconds from milliseconds.
	const Real latencySum = std::accumulate(m_latencyList, m_latencyList + TheGlobalData->m_networkLatencyHistoryLength, 0.0f);
	m_averageLatency = latencySum / (Real)TheGlobalData->m_networkLatencyHistoryLength;

	if (frame % 16 == 0) {
//		DEBUG_LOG(("ConnectionManager::processFrameInfoAck - average latency = %f", m_averageLatency));
	}
}

void FrameMetrics::addCushion(Int cushion) {
	++m_cushionIndex;
	m_cushionIndex %= TheGlobalData->m_networkCushionHistoryLength;
	if (m_cushionIndex == 0) {
		m_minimumCushion = -1;
	}
	if ((cushion < m_minimumCushion) || (m_minimumCushion == -1)) {
		m_minimumCushion = cushion;
	}
}

Int FrameMetrics::getAverageFPS() {
	return (Int)m_averageFps;
}

Real FrameMetrics::getAverageLatency() {
	return m_averageLatency;
}

Real FrameMetrics::getRunAheadLatency() {
	return m_smoothedLatency + 2.0f * m_latencyDeviation;
}

Int FrameMetrics::getMinimumCushion() {
	return m_minimumCushion;
}
