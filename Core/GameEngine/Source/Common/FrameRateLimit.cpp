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

#include "PreRTS.h"
#include "Common/FrameRateLimit.h"


FrameRateLimit::FrameRateLimit()
{
	LARGE_INTEGER freq;
	LARGE_INTEGER start;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	m_freq = freq.QuadPart;
	m_start = start.QuadPart;
}

namespace
{
	void (*s_idleWork)() = nullptr;
	HANDLE s_idleWake = nullptr;
	Bool s_idleWorkAllowed = FALSE;
}

void FrameRateLimit::setIdleWork(void (*work)(), void *wakeEvent)
{
	s_idleWork = work;
	s_idleWake = (HANDLE)wakeEvent;
}

void FrameRateLimit::setIdleWorkAllowed(Bool allowed)
{
	s_idleWorkAllowed = allowed;
}

Real FrameRateLimit::wait(UnsignedInt maxFps)
{
	PROFILER_SECTION;
	LARGE_INTEGER tick;
	QueryPerformanceCounter(&tick);
	double elapsedSeconds = static_cast<double>(tick.QuadPart - m_start) / m_freq;
	const double targetSeconds = 1.0 / maxFps;
	const double sleepSeconds = targetSeconds - elapsedSeconds - 0.002; // leave ~2ms for spin wait

	if (sleepSeconds > 0.0)
	{
		// Non busy wait with Munkee sleep
		DWORD dwMilliseconds = static_cast<DWORD>(sleepSeconds * 1000);
		if (s_idleWork != nullptr && s_idleWake != nullptr && s_idleWorkAllowed)
		{
			// O72: the same sleep, woken for the idle work (the audio's ends) as often as it is signalled
			LARGE_INTEGER sleepStart;
			QueryPerformanceCounter(&sleepStart);
			for (;;)
			{
				LARGE_INTEGER nowTick;
				QueryPerformanceCounter(&nowTick);
				const double sleptMs = static_cast<double>(nowTick.QuadPart - sleepStart.QuadPart) * 1000.0 / m_freq;
				if (sleptMs >= (double)dwMilliseconds) {
					break;
				}
				const DWORD left = (DWORD)((double)dwMilliseconds - sleptMs);
				if (left == 0 || WaitForSingleObject(s_idleWake, left) != WAIT_OBJECT_0) {
					break;
				}
				s_idleWork();
			}
		}
		else
		{
			Sleep(dwMilliseconds);
		}
	}

	// Busy wait for remaining time
	do
	{
		QueryPerformanceCounter(&tick);
		elapsedSeconds = static_cast<double>(tick.QuadPart - m_start) / m_freq;
	}
	while (elapsedSeconds < targetSeconds);

	m_start = tick.QuadPart;
	return (Real)elapsedSeconds;
}

void FrameRateLimit::reset()
{
	LARGE_INTEGER tick;
	QueryPerformanceCounter(&tick);
	m_start = tick.QuadPart;
}


// TheSuperHackers @tweak The render fps limits the settings offer (and the max render fps keys step through): common
// display refresh rates from 60 to 1000, and no limit. "Match the display" is RenderFpsPreset::MatchDisplayLimit.
const UnsignedInt RenderFpsPreset::s_fpsValues[] = {
	60, 75, 90, 100, 120, 144, 165, 180, 200, 240, 280, 300, 360, 400, 480, 500, 540, 600, 700, 720, 750, 1000, UncappedFpsValue };

static_assert(LOGICFRAMES_PER_SECOND <= 30, "Min FPS values need to be revisited!");

UnsignedInt RenderFpsPreset::getNextFpsValue(UnsignedInt value)
{
	const Int first = 0;
	const Int last = ARRAY_SIZE(s_fpsValues) - 1;
	for (Int i = first; i < last; ++i)
	{
		if (value >= s_fpsValues[i] && value < s_fpsValues[i + 1])
		{
			return s_fpsValues[i + 1];
		}
	}
	return s_fpsValues[last];
}

UnsignedInt RenderFpsPreset::getPrevFpsValue(UnsignedInt value)
{
	const Int first = 0;
	const Int last = ARRAY_SIZE(s_fpsValues) - 1;
	for (Int i = last; i > first; --i)
	{
		if (value <= s_fpsValues[i] && value > s_fpsValues[i - 1])
		{
			return s_fpsValues[i - 1];
		}
	}
	return s_fpsValues[first];
}

UnsignedInt RenderFpsPreset::changeFpsValue(UnsignedInt value, FpsValueChange change)
{
	switch (change)
	{
	default:
	case FpsValueChange_Increase: return getNextFpsValue(value);
	case FpsValueChange_Decrease: return getPrevFpsValue(value);
	}
}


Int RenderFpsPreset::parseUserLimit(const char *text)
{
	// Not below 30, the lowest render preset: in network games the logic rate follows the lowest average render fps of
	// all players (FrameMetrics), so a user limit below the nominal 30 could slow down a LAN game for everyone if a script
	// switches the limit on. Above 1000 there is nothing to gain over 0 (no limit).
	const Int minLimit = 30;
	const Int maxLimit = 1000;

	if (stricmp(text, "display") == 0)
		return MatchDisplayLimit;

	char *endPtr = nullptr;
	const long value = strtol(text, &endPtr, 10);
	if (endPtr == text || value < 0)
		return -1;
	if (value == 0)
		return 0;
	return std::max<Int>(minLimit, std::min<Int>(maxLimit, (Int)std::min<long>(value, 100000L)));
}


Int RenderFpsPreset::getDisplayRefreshRate()
{
	MONITORINFOEXA info;
	info.cbSize = sizeof(info);
	const HWND window = ::GetActiveWindow();
	const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
	const char *deviceName = nullptr;
	if (monitor != nullptr && ::GetMonitorInfoA(monitor, &info))
		deviceName = info.szDevice;

	DEVMODEA mode;
	memset(&mode, 0, sizeof(mode));
	mode.dmSize = sizeof(mode);
	if (!::EnumDisplaySettingsA(deviceName, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
		return 60; // 0 and 1 mean the hardware default
	return std::max<Int>(30, std::min<Int>(1000, (Int)mode.dmDisplayFrequency));
}


UnsignedInt LogicTimeScaleFpsPreset::getNextFpsValue(UnsignedInt value)
{
	if (value >= MaxSteppedFpsValue)
	{
		return RenderFpsPreset::UncappedFpsValue;
	}
	else
	{
		return std::min<UnsignedInt>(value + StepFpsValue, MaxSteppedFpsValue);
	}
}

UnsignedInt LogicTimeScaleFpsPreset::getPrevFpsValue(UnsignedInt value)
{
	if (value >= MaxFpsValue)
	{
		return MaxSteppedFpsValue;
	}
	else if (value < MinFpsValue + StepFpsValue)
	{
		return MinFpsValue;
	}
	else
	{
		return value - StepFpsValue;
	}
}

UnsignedInt LogicTimeScaleFpsPreset::changeFpsValue(UnsignedInt value, FpsValueChange change)
{
	switch (change)
	{
	default:
	case FpsValueChange_Increase: return getNextFpsValue(value);
	case FpsValueChange_Decrease: return getPrevFpsValue(value);
	}
}
