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

#pragma once

#include "Common/GameCommon.h"


class FrameRateLimit
{
public:
	FrameRateLimit();

	Real wait(UnsignedInt maxFps);

	/// TheSuperHackers @feature O72 Work for the main thread while it sleeps out a frame: when wakeEvent (a Win32 event)
	/// is set during the sleep, work runs at once and the sleep goes on. Used by the audio for the ends of its sounds
	/// (the next loop or portion starts within about a millisecond instead of at the next frame). Only while allowed
	/// (the engine's main loop allows it around its own wait; other waits, e.g. a load screen's fade, do not).
	static void setIdleWork(void (*work)(), void *wakeEvent);
	static void setIdleWorkAllowed(Bool allowed);
	void reset(); ///< Move the timing anchor to now, discarding any time elapsed since the last call to wait.

private:
	Int64 m_freq;
	Int64 m_start;
};


enum FpsValueChange
{
	FpsValueChange_Increase,
	FpsValueChange_Decrease,
};


class RenderFpsPreset
{
public:
	enum CPP_11(: UnsignedInt)
	{
		UncappedFpsValue = 1000000,
	};

	/// TheSuperHackers @feature A user render fps limit of this value means "match the display": the refresh rate of the
	/// display the game window is on (Options.ini RenderFpsLimit = display; the default when the key is absent).
	enum { MatchDisplayLimit = -2 };

	static UnsignedInt getNextFpsValue(UnsignedInt value);
	static UnsignedInt getPrevFpsValue(UnsignedInt value);
	static UnsignedInt changeFpsValue(UnsignedInt value, FpsValueChange change);

private:
	static const UnsignedInt s_fpsValues[];

public:
	/// Parses a render fps limit given by the user (Options.ini RenderFpsLimit, -fps): MatchDisplayLimit for "display",
	/// -1 if it is not a non-negative number, 0 for no limit, else the number clamped to 30..1000.
	static Int parseUserLimit(const char *text);

	/// The refresh rate of the display the active window is on (the primary display without one), clamped to
	/// 30..1000; 60 when Windows does not say.
	static Int getDisplayRefreshRate();
};


class LogicTimeScaleFpsPreset
{
public:
	enum CPP_11(: UnsignedInt)
	{
		// TheSuperHackers @tweak The game speed steps of the skirmish slider: 0.5x to 3x of LOGICFRAMES_PER_SECOND in
		// steps of 0.5x (GameSpeedControl.h).
		MinFpsValue = 15, ///< 0.5x, the slowest game speed.
		StepFpsValue = 15, ///< 0.5x
		MaxSteppedFpsValue = 90, ///< 3x, the fastest game speed. The next step is "as fast as possible".
		MaxFpsValue = 1000, ///< Values at or above this mean as fast as possible.
	};

	static UnsignedInt getNextFpsValue(UnsignedInt value);
	static UnsignedInt getPrevFpsValue(UnsignedInt value);
	static UnsignedInt changeFpsValue(UnsignedInt value, FpsValueChange change);
};

