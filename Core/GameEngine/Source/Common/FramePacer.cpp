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
#include "PreRTS.h"

#include "Common/FramePacer.h"

#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"

#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"


FramePacer* TheFramePacer = nullptr;

FramePacer::FramePacer()
{
	// Set the time slice size to 1 ms.
	timeBeginPeriod(1);

	m_maxFPS = BaseFps;
	m_logicTimeScaleFPS = LOGICFRAMES_PER_SECOND;
	m_updateTime = 1.0f / (Real)BaseFps; // initialized to something to avoid division by zero on first use
	m_logicFramePhase = 1.0f;
	m_enableFpsLimit = FALSE;
	m_matchRenderLimitDisabled = FALSE;
	// TheSuperHackers @tweak The logic time scale is enabled by default so that the simulation runs at
	// LOGICFRAMES_PER_SECOND outside of network games, independent of the render frame rate.
	m_enableLogicTimeScale = TRUE;
	m_isTimeFrozen = FALSE;
	m_isGameHalted = FALSE;
}

FramePacer::~FramePacer()
{
	// Restore the previous time slice for Windows.
	timeEndPeriod(1);
}

void FramePacer::update()
{
	// Uses a high resolution counter to cap the frame rate more accurately to the desired limit than retail did.
	const UnsignedInt maxFps = getActualFramesPerSecondLimit();
	m_updateTime = m_frameRateLimit.wait(maxFps);

	if (TheGameLogic != nullptr)
	{
		// Set or advance the logic frame phase by the render step that the next update will draw.
		// It is capped at a whole logic frame, because the render steps in between can add up to more than one.
		// Consumers are expected to interpolate towards the next logic frame and not extrapolate past it.
		const Real timeScale = getActualLogicTimeScaleOverFpsRatio();

		if (TheGameLogic->hasUpdated())
		{
			// The ratio can be above 1 when an engine update runs several logic updates; the phase stays within one frame.
			m_logicFramePhase = min(1.0f, timeScale);
		}
		else
		{
			m_logicFramePhase = min(1.0f, m_logicFramePhase + timeScale);
		}
	}
}

void FramePacer::reset()
{
	m_frameRateLimit.reset();
	m_updateTime = 1.0f / (Real)getActualFramesPerSecondLimit();
	m_logicFramePhase = 1.0f;
}

void FramePacer::setFramesPerSecondLimit( Int fps )
{
	DEBUG_LOG(("FramePacer::setFramesPerSecondLimit() - setting max fps to %d (TheGlobalData->m_useFpsLimit == %d)", fps, TheGlobalData->m_useFpsLimit));
	m_maxFPS = fps;
}

Int FramePacer::getFramesPerSecondLimit()  const
{
	return m_maxFPS;
}

void FramePacer::enableFramesPerSecondLimit( Bool enable )
{
	m_enableFpsLimit = enable;
}

Bool FramePacer::isFramesPerSecondLimitEnabled() const
{
	return m_enableFpsLimit;
}

void FramePacer::setMatchRenderLimitDisabled( Bool disabled )
{
	m_matchRenderLimitDisabled = disabled;
}

Bool FramePacer::isMatchRenderLimitDisabled() const
{
	return m_matchRenderLimitDisabled;
}

Bool FramePacer::isActualFramesPerSecondLimitEnabled() const
{
	Bool allowFpsLimit = true;

	allowFpsLimit &= !m_matchRenderLimitDisabled;

	allowFpsLimit &= !isScriptFastForward();

	if (TheGameLogic != nullptr)
	{
#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		allowFpsLimit &= !(!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode);
#else	//always allow this cheat key if we're in a replay game.
		allowFpsLimit &= !(!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame());
#endif
	}

	allowFpsLimit &= TheGlobalData->m_useFpsLimit;
	allowFpsLimit &= isFramesPerSecondLimitEnabled();

	return allowFpsLimit;
}

Bool FramePacer::isScriptFastForward() const
{
	if (TheTacticalView != nullptr)
	{
		return TheTacticalView->getTimeMultiplier() > 1 || TheScriptEngine->isTimeFast();
	}
	return false;
}

Int FramePacer::getActualFramesPerSecondLimit() const
{
	if (!isActualFramesPerSecondLimitEnabled())
	{
		return RenderFpsPreset::UncappedFpsValue;
	}

	Int fpsLimit = getFramesPerSecondLimit();

	// TheSuperHackers @tweak The game speed no longer raises the render fps limit: the engine update runs as many logic
	// updates as the game speed asks for (GameEngine::getGameLogicUpdateCount), so the render rate comes from the
	// settings only. Only "as fast as possible" (UncappedFpsValue, a restart after fast forward) lifts the limit, as it
	// runs one logic update per engine update with no wait.
	if (TheNetwork == nullptr && isLogicTimeScaleEnabled() && (UnsignedInt)getLogicTimeScaleFps() >= RenderFpsPreset::UncappedFpsValue)
	{
		fpsLimit = RenderFpsPreset::UncappedFpsValue;
	}

	return fpsLimit;
}

Real FramePacer::getUpdateTime()  const
{
	return m_updateTime;
}

Real FramePacer::getUpdateFps()  const
{
	return 1.0f / m_updateTime;
}

Real FramePacer::getBaseOverUpdateFpsRatio(Real minUpdateFps)
{
	// Update fps is floored to default 5 fps, 200 ms.
	// Useful to prevent insane ratios on frame spikes/stalls.
	return (Real)BaseFps / std::max(getUpdateFps(), minUpdateFps);
}

void FramePacer::setTimeFrozen(Bool frozen)
{
	m_isTimeFrozen = frozen;
}

void FramePacer::setGameHalted(Bool halted)
{
	m_isGameHalted = halted;
}

Bool FramePacer::isTimeFrozen() const
{
	return m_isTimeFrozen;
}

Bool FramePacer::isGameHalted() const
{
	return m_isGameHalted;
}

void FramePacer::setLogicTimeScaleFps( Int fps )
{
	m_logicTimeScaleFPS = fps;
}

Int FramePacer::getLogicTimeScaleFps() const
{
	return m_logicTimeScaleFPS;
}

void FramePacer::enableLogicTimeScale( Bool enable )
{
	m_enableLogicTimeScale = enable;
}

Bool FramePacer::isLogicTimeScaleEnabled() const
{
	return m_enableLogicTimeScale;
}

Int FramePacer::getActualLogicTimeScaleFps(LogicTimeQueryFlags flags) const
{
	if (m_isTimeFrozen && (flags & IgnoreFrozenTime) == 0)
	{
		return 0;
	}

	if (m_isGameHalted && (flags & IgnoreHaltedGame) == 0)
	{
		return 0;
	}

	if (TheNetwork != nullptr)
	{
		return TheNetwork->getFrameRate();
	}

	if (isLogicTimeScaleEnabled())
	{
		return getLogicTimeScaleFps();
	}

	// Returns uncapped value to align with the render update as per the original game behavior.
	// This is only used while the time scale is temporarily disabled, for example for the single frame step.
	return RenderFpsPreset::UncappedFpsValue;
}

Real FramePacer::getActualLogicTimeScaleRatio(LogicTimeQueryFlags flags) const
{
	return (Real)getActualLogicTimeScaleFps(flags) / LOGICFRAMES_PER_SECONDS_REAL;
}

Real FramePacer::getActualLogicTimeScaleOverFpsRatio(LogicTimeQueryFlags flags) const
{
	// TheSuperHackers @tweak An engine update now runs several logic updates when the game speed is above the render
	// rate (3x on a 60 Hz display), so the client steps by more than one logic frame per render update too. Capped at
	// MaxLogicFramesPerRenderUpdate, as many logic updates as one engine update runs at the most.
	return min((Real)MaxLogicFramesPerRenderUpdate, (Real)getActualLogicTimeScaleFps(flags) / getUpdateFps());
}

Real FramePacer::getLogicTimeStepSeconds(LogicTimeQueryFlags flags) const
{
	return SECONDS_PER_LOGICFRAME_REAL * getActualLogicTimeScaleOverFpsRatio(flags);
}

Real FramePacer::getLogicTimeStepMilliseconds(LogicTimeQueryFlags flags) const
{
	return MSEC_PER_LOGICFRAME_REAL * getActualLogicTimeScaleOverFpsRatio(flags);
}

Real FramePacer::getLogicFramesPerRenderUpdate() const
{
	return getActualLogicTimeScaleOverFpsRatio(IgnoreFrozenTime | IgnoreHaltedGame);
}

Real FramePacer::getLogicFramePhase() const
{
	return m_logicFramePhase;
}
