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

#include "PreRTS.h"

#include "Common/FrameShots.h"

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/Recorder.h"

#include "GameClient/Display.h"

#include "GameLogic/GameLogic.h"

#include <algorithm>
#include <vector>


Bool FrameShots::s_enabled = FALSE;
Int FrameShots::s_failureExitCode = 0;

namespace
{
	std::vector<UnsignedInt> s_frames; ///< Listed logic frames, ascending and unique.
	size_t s_nextFrameIndex = 0; ///< First listed frame that the logic has not reached yet.
	std::vector<UnsignedInt> s_pendingFrames; ///< Listed frames reached by the logic, waiting for the next render.
	UnsignedInt s_renderCountAtLogicUpdate = 0; ///< Rendered frame count when the last pending frame was reached.

	AsciiString s_directory;
	Bool s_exitWhenDone = FALSE;
	Bool s_outputReady = FALSE;
	Bool s_outputFailed = FALSE;
	FILE *s_csv = nullptr;

	Bool s_gameStarted = FALSE;
	UnsignedInt s_lastLogicFrame = 0;
	Int64 s_gameStartTicks = 0;
	UnsignedInt s_engineUpdatesSinceGameStart = 0;
	Bool s_exitRequested = FALSE;

	Int64 queryTicks()
	{
		LARGE_INTEGER ticks;
		QueryPerformanceCounter(&ticks);
		return ticks.QuadPart;
	}

	double ticksToMilliseconds(Int64 ticks)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		return (double)ticks * 1000.0 / (double)freq.QuadPart;
	}

	// Creates each folder of the path in turn. Existing folders are fine.
	void createDirectories(const AsciiString &path)
	{
		char buffer[_MAX_PATH];
		strlcpy(buffer, path.str(), ARRAY_SIZE(buffer));
		for (char *p = buffer; *p != '\0'; ++p)
		{
			if ((*p == '\\' || *p == '/') && p != buffer && *(p - 1) != ':')
			{
				const char c = *p;
				*p = '\0';
				CreateDirectory(buffer, nullptr);
				*p = c;
			}
		}
		CreateDirectory(buffer, nullptr);
	}
}

void FrameShots::setFrames(const char *commaSeparatedFrames)
{
	s_frames.clear();
	const char *p = commaSeparatedFrames;
	while (p != nullptr && *p != '\0')
	{
		char *end = nullptr;
		const unsigned long value = strtoul(p, &end, 10);
		if (end == p)
		{
			++p; // skip a separator
			continue;
		}
		s_frames.push_back((UnsignedInt)value);
		p = end;
	}
	std::sort(s_frames.begin(), s_frames.end());
	s_frames.erase(std::unique(s_frames.begin(), s_frames.end()), s_frames.end());
	s_nextFrameIndex = 0;
	s_enabled = !s_frames.empty();
}

void FrameShots::setDirectory(const char *path)
{
	s_directory = path;
}

void FrameShots::setExitWhenDone(Bool exitWhenDone)
{
	s_exitWhenDone = exitWhenDone;
}

void FrameShots::beginGame()
{
	s_gameStarted = TRUE;
	s_gameStartTicks = queryTicks();
	s_engineUpdatesSinceGameStart = 0;
}

void FrameShots::onLogicUpdated()
{
	if (!s_enabled)
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();

	// The time base is the first logic update of a game. A new game counts its frames from the start again.
	if (!s_gameStarted || frame < s_lastLogicFrame)
	{
		beginGame();
	}
	s_lastLogicFrame = frame;

	// Every listed frame at or below the current frame becomes pending. The logic frame grows by one per
	// update and the comparison is <=, so no listed frame is skipped, even when more than one logic update
	// passes before the next render; those frames then share the first render after them.
	Bool reachedListedFrame = FALSE;
	while (s_nextFrameIndex < s_frames.size() && s_frames[s_nextFrameIndex] <= frame)
	{
		s_pendingFrames.push_back(s_frames[s_nextFrameIndex]);
		++s_nextFrameIndex;
		reachedListedFrame = TRUE;
	}

	if (reachedListedFrame && TheDisplay != nullptr)
	{
		s_renderCountAtLogicUpdate = TheDisplay->getRenderedFrameCount();
	}
}

void FrameShots::onClientUpdated()
{
	if (!s_enabled)
		return;

	if (s_gameStarted)
	{
		++s_engineUpdatesSinceGameStart;
	}

	if (s_exitRequested)
	{
		// Wait until the screenshot threads have written their files before the process ends.
		if (TheDisplay == nullptr || !TheDisplay->isScreenShotWritePending())
		{
			s_exitRequested = FALSE;
			s_enabled = FALSE;
			exitGame();
		}
		return;
	}

	if (s_pendingFrames.empty() || TheDisplay == nullptr)
		return;

	// Capture after the first render that completed after the logic update which reached the listed frame.
	// The image is read back from the last presented frame, as the screenshot key does after the client
	// update, so it includes the HUD. A render that was skipped (minimized window, time multiplier) does not
	// count. When the render rate is above the logic rate, this render draws the logic frame phase
	// FramePacer::getLogicFramePhase() = min(1, logic fps / update fps), for example 0.5 at 60 render fps and
	// 30 logic fps: the client side interpolation (drawable tilt, particles) is halfway from the previous
	// logic frame to the listed one. At or below the logic rate the phase is 1, the exact logic frame.
	if (TheDisplay->getRenderedFrameCount() == s_renderCountAtLogicUpdate)
		return;

	for (size_t i = 0; i < s_pendingFrames.size(); ++i)
	{
		capture(s_pendingFrames[i]);
	}
	s_pendingFrames.clear();

	if (s_exitWhenDone && s_nextFrameIndex >= s_frames.size())
	{
		s_exitRequested = TRUE;
	}
}

Bool FrameShots::prepareOutput()
{
	if (s_outputReady)
		return TRUE;
	if (s_outputFailed)
		return FALSE;

	if (s_directory.isEmpty())
	{
		s_directory = TheGlobalData->getPath_UserData();
		s_directory.concat("FrameShots");
	}
	while (s_directory.endsWith("\\") || s_directory.endsWith("/"))
	{
		s_directory.removeLastChar();
	}
	createDirectories(s_directory);

	AsciiString csvPath = s_directory;
	csvPath.concat("\\frames.csv");
	s_csv = fopen(csvPath.str(), "w");
	if (s_csv == nullptr)
	{
		s_outputFailed = TRUE;
		return FALSE;
	}
	fprintf(s_csv, "logic_frame,ms_since_game_start,engine_updates_since_game_start,render_fps_limit,"
		"actual_update_fps_limit,logic_time_scale_fps,actual_logic_time_scale_fps,current_logic_frame,logic_frame_phase\n");
	fflush(s_csv);

	s_outputReady = TRUE;
	return TRUE;
}

void FrameShots::capture(UnsignedInt listedFrame)
{
	if (!prepareOutput())
		return;

	AsciiString filePath;
	filePath.format("%s\\frame_%08u.png", s_directory.str(), listedFrame);
	TheDisplay->takeScreenShotToFile(SCREENSHOT_PNG, filePath.str());

	const double ms = ticksToMilliseconds(queryTicks() - s_gameStartTicks);
	fprintf(s_csv, "%u,%.3f,%u,%d,%d,%d,%d,%u,%.3f\n",
		listedFrame,
		ms,
		s_engineUpdatesSinceGameStart,
		TheFramePacer->getFramesPerSecondLimit(),
		TheFramePacer->getActualFramesPerSecondLimit(),
		TheFramePacer->getLogicTimeScaleFps(),
		TheFramePacer->getActualLogicTimeScaleFps(),
		TheGameLogic->getFrame(),
		(double)TheFramePacer->getLogicFramePhase());
	fflush(s_csv);
}

void FrameShots::failStartup(const char *message, Int exitCode)
{
	DEBUG_LOG(("Startup failure: %s", message));
	s_failureExitCode = (exitCode != 0) ? exitCode : 1;
	if (!s_enabled)
	{
		// A person is probably watching. With -shotFrames nobody may be there, so no dialog then.
		MessageBoxA(nullptr, message, "Command & Conquer", MB_OK | MB_ICONERROR);
	}
	if (TheGameEngine != nullptr)
	{
		TheGameEngine->setQuitting(TRUE);
	}
}

void FrameShots::exitGame()
{
	if (s_csv != nullptr)
	{
		fclose(s_csv);
		s_csv = nullptr;
	}

	// The normal quit path, as used by the benchmark timer in GameEngine::execute.
	if (TheGameLogic->isInGame())
	{
		if (TheRecorder != nullptr && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
		{
			TheRecorder->stopRecording();
		}
		TheGameLogic->clearGameData();
	}
	TheGameEngine->setQuitting(TRUE);
}
