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

// TheSuperHackers @feature See ReplayFollow.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ReplayFollow.h"

#include "Common/FramePacer.h"
#include "Common/ReplayStream.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/GameText.h"

#include <windows.h>

namespace
{
	struct LiveRecord
	{
		char magic[4];          ///< "GLIV"
		UnsignedInt frame;      ///< the newest complete frame
		UnsignedInt ended;      ///< 1 once the recording stopped
		UnsignedInt counter;    ///< grows with every write
	};

	// writer
	HANDLE s_writer = INVALID_HANDLE_VALUE;
	UnsignedInt s_writeCounter = 0;
	UnsignedInt s_writerFrame = 0;

	// follower
	Bool s_on = FALSE;
	Int s_delayFrames = 0;
	Int s_reserveFrames = 0;     ///< O34: play-out reserve of a stream viewer (frames kept back to bridge a source change)
	enum { RATE_NORMAL, RATE_FAST, RATE_SLOW };
	Int s_rate = RATE_NORMAL;
	Real s_catchUp = 2.0f;
	char s_livePath[_MAX_PATH] = { 0 };
	Bool s_reading = FALSE;
	Int s_normalFps = LOGICFRAMES_PER_SECOND;
	Bool s_catchingUp = FALSE;
	LiveRecord s_last;
	Bool s_haveLast = FALSE;
	UnsignedInt s_lastChangeMs = 0;
	UnsignedInt s_lastPollMs = 0;
	UnsignedInt s_lastMessageMs = 0;
	UnsignedInt s_holdSinceMs = 0;

	void livePathOf(const AsciiString &replayPath, char *out, Int size)
	{
		snprintf(out, size, "%s.live", replayPath.str());
	}

	void writeRecord(UnsignedInt frame, UnsignedInt ended)
	{
		if (s_writer == INVALID_HANDLE_VALUE) {
			return;
		}
		LiveRecord r;
		memcpy(r.magic, "GLIV", 4);
		r.frame = frame;
		r.ended = ended;
		r.counter = ++s_writeCounter;
		SetFilePointer(s_writer, 0, nullptr, FILE_BEGIN);
		DWORD written = 0;
		WriteFile(s_writer, &r, sizeof(r), &written, nullptr);
	}

	/// Reads the side file (at most every 50 ms). FALSE when there is none.
	Bool poll()
	{
		const UnsignedInt now = timeGetTime();
		if (s_haveLast && now - s_lastPollMs < 50) {
			return TRUE;
		}
		s_lastPollMs = now;
		HANDLE h = CreateFileA(s_livePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			return s_haveLast;
		}
		LiveRecord r;
		DWORD got = 0;
		const BOOL ok = ReadFile(h, &r, sizeof(r), &got, nullptr);
		CloseHandle(h);
		if (!ok || got != sizeof(r) || memcmp(r.magic, "GLIV", 4) != 0) {
			return s_haveLast;
		}
		if (!s_haveLast || r.counter != s_last.counter) {
			s_lastChangeMs = now;
		}
		s_last = r;
		s_haveLast = TRUE;
		return TRUE;
	}

	void showWaiting()
	{
		const UnsignedInt now = timeGetTime();
		if (TheInGameUI == nullptr || now - s_lastMessageMs < 5000) {
			return;
		}
		s_lastMessageMs = now;
		// O34: the stream viewer's source is gone and the reserve is used up: a short line, the replay goes on when a new
		// source gives data
		if (ReplayStream::mirrorReconnecting())
		{
			TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayFollowReconnecting",
				localizedFallback(L"Reconnecting to the stream...", L"\x041F\x0435\x0440\x0435\x043F\x043E\x0434\x043A\x043B\x044E\x0447\x0435\x043D\x0438\x0435 \x043A \x0442\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x0438...")));
			return;
		}
		TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE("GUI:ReplayFollowWaiting",
			localizedFallback(L"Waiting for the game...", L"\x041E\x0436\x0438\x0434\x0430\x043D\x0438\x0435 \x0434\x0430\x043D\x043D\x044B\x0445 \x0438\x0433\x0440\x044B...")));
	}

	/// O34: normal, faster (catching up) or 10 % slower (building the reserve up again)
	void setRate(Int rate)
	{
		if (rate == s_rate || TheFramePacer == nullptr) {
			return;
		}
		s_rate = rate;
		s_catchingUp = rate == RATE_FAST;
		const Int fps = rate == RATE_FAST ? REAL_TO_INT((Real)s_normalFps * s_catchUp)
			: (rate == RATE_SLOW ? REAL_TO_INT((Real)s_normalFps * 0.9f) : s_normalFps);
		TheFramePacer->setLogicTimeScaleFps(fps > 0 ? fps : LOGICFRAMES_PER_SECOND);
	}

	void setCatchUp(Bool on)
	{
		setRate(on ? RATE_FAST : RATE_NORMAL);
	}
}

//-------------------------------------------------------------------------------------------------
void ReplayFollow::writerStarted(const AsciiString &replayPath)
{
	writerEnded();
	char path[_MAX_PATH];
	livePathOf(replayPath, path, ARRAY_SIZE(path));
	s_writer = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	s_writeCounter = 0;
	s_writerFrame = 0;
	writeRecord(0, 0);
}

void ReplayFollow::writerFrame(UnsignedInt frame)
{
	s_writerFrame = frame;
	writeRecord(frame, 0);
}

void ReplayFollow::writerEnded()
{
	if (s_writer == INVALID_HANDLE_VALUE) {
		return;
	}
	writeRecord(s_writerFrame, 1);   // the last frame, marked ended
	CloseHandle(s_writer);
	s_writer = INVALID_HANDLE_VALUE;
}

//-------------------------------------------------------------------------------------------------
void ReplayFollow::configure(Int delaySeconds, Real catchUpFactor)
{
	s_on = TRUE;
	delaySeconds = delaySeconds < 0 ? 0 : (delaySeconds > 180 ? 180 : delaySeconds);
	s_delayFrames = delaySeconds * LOGICFRAMES_PER_SECOND;
	s_catchUp = catchUpFactor < 1.0f ? 1.0f : (catchUpFactor > 8.0f ? 8.0f : catchUpFactor);
}

void ReplayFollow::configureReserve(Int seconds)
{
	seconds = seconds < 0 ? 0 : (seconds > 60 ? 60 : seconds);
	s_reserveFrames = seconds * LOGICFRAMES_PER_SECOND;
}

void ReplayFollow::turnOff()
{
	s_on = FALSE;
	s_reserveFrames = 0;
}

Bool ReplayFollow::isOn()
{
	return s_on && s_reading;
}

void ReplayFollow::readerStarted(const AsciiString &replayPath, Int normalLogicFps)
{
	if (!s_on) {
		return;
	}
	livePathOf(replayPath, s_livePath, ARRAY_SIZE(s_livePath));
	s_reading = TRUE;
	s_haveLast = FALSE;
	s_catchingUp = FALSE;
	s_rate = RATE_NORMAL;
	s_holdSinceMs = 0;
	s_normalFps = normalLogicFps > 0 && normalLogicFps < 1000 ? normalLogicFps : LOGICFRAMES_PER_SECOND;
	poll();
}

void ReplayFollow::readerEnded()
{
	if (s_reading) {
		setCatchUp(FALSE);
	}
	s_reading = FALSE;
}

Bool ReplayFollow::writerAlive()
{
	if (!isOn() || !poll()) {
		return FALSE;
	}
	if (s_last.ended != 0) {
		return FALSE;
	}
	// a writer that stopped writing (closed, crashed) for 10 seconds counts as ended; O34: a stream viewer's mirror that is
	// still looking for a source does not (it gives up by itself after 2 minutes and marks the end)
	return timeGetTime() - s_lastChangeMs < 10000 || ReplayStream::mirrorTrying();
}

Bool ReplayFollow::mayRunFrame(UnsignedInt frame)
{
	if (!isOn()) {
		return TRUE;
	}
	if (!poll() || s_last.ended != 0 || !writerAlive())
	{
		// no live writer (any more): the replay plays to its end as any replay
		setCatchUp(FALSE);
		return TRUE;
	}
	const Int newest = (Int)s_last.frame;
	const Int allowed = newest - s_delayFrames;
	if ((Int)frame > allowed)
	{
		// the writer has not finished this frame (or it lies within the delay): hold, and say so after a second
		setCatchUp(FALSE);
		const UnsignedInt now = timeGetTime();
		if (s_holdSinceMs == 0) {
			s_holdSinceMs = now;
		}
		if (now - s_holdSinceMs > 1000) {
			showWaiting();
		}
		return FALSE;
	}
	s_holdSinceMs = 0;
	// O34: a stream viewer keeps a reserve behind the allowed frame. Inside it (after a source change used some of it, or
	// while the source is gone) it plays 10 % slower, so the reserve grows back without a stop; more than two seconds
	// behind the reserve's point it runs faster until caught up.
	const Int target = allowed - s_reserveFrames;
	if (s_reserveFrames > 0 && (Int)frame > target) {
		setRate(RATE_SLOW);
	} else {
		setCatchUp(target - (Int)frame > 2 * LOGICFRAMES_PER_SECOND);
	}
	return TRUE;
}

UnsignedInt ReplayFollow::newestFrame()
{
	if (!isOn() || !poll()) {
		return 0;
	}
	return s_last.frame;
}
