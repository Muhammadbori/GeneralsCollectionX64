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

	static UnsignedInt getNextFpsValue(UnsignedInt value);
	static UnsignedInt getPrevFpsValue(UnsignedInt value);
	static UnsignedInt changeFpsValue(UnsignedInt value, FpsValueChange change);

private:
	static const UnsignedInt s_fpsValues[];

public:
	/// Parses a render fps limit given by the user (Options.ini RenderFpsLimit, -fps): -1 if it is not a non-negative
	/// number, 0 for no limit, else the number clamped to 30..1000.
	static Int parseUserLimit(const char *text);
};


class LogicTimeScaleFpsPreset
{
public:
	enum CPP_11(: UnsignedInt)
	{
#if RTS_DEBUG
		MinFpsValue = 5,
#else
		MinFpsValue = 15, ///< Matches the slowest skirmish game speed.
#endif
		StepFpsValue = 5,
		MaxSteppedFpsValue = 60, ///< Matches the fastest numbered skirmish game speed. The next step is "as fast as possible".
		MaxFpsValue = 1000, ///< Values at or above this mean as fast as possible.
	};

	static UnsignedInt getNextFpsValue(UnsignedInt value);
	static UnsignedInt getPrevFpsValue(UnsignedInt value);
	static UnsignedInt changeFpsValue(UnsignedInt value, FpsValueChange change);
};

