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

// StateTrace.cpp ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See StateTrace.h. The lines (space separated key=value, one record per line):
//   F f=<frame> adv=<0|1> seed=<logic seed CRC> ld=<draws in this update> lu=<Unchanged draws in it>
//     od=<draws outside an update since the last F> ou=<Unchanged ones> cs=<client seed CRC> cd=<client draws since the last F>
//     sync=<WW3D sync ms> lft=<last logic frame time handed to WW3D, ms> lfts=<their sum since the last F, ms>
//     du=<display updates since the last F> cu=<cloud updates since the last F> cts=<cloud frame time sum, s>
//     cx=<cloud x offset> cy=<cloud y offset> put=<frame pacer update time, s> step=<frame pacer logic step, ms>
//   O n=<number> f=<frame> v=<value> lo=<lo> hi=<hi> at=<file>:<line>      a logic draw outside a logic update
//   C f=<frame> in=<0|1> u=<0|1> v=<value> lo=<lo> hi=<hi> at=<file>:<line>  -stateTraceCalls A-B: every logic draw
//   K f=<frame> in=<0|1> n=<client draw number since the last F> v=<value> lo=<lo> hi=<hi> at=<file>:<line>
//     ret=<module offset of the caller> ret2=<module offset of the caller's caller, 0 when the stack walk cannot tell>
//                                                                          -stateTraceCalls A-B: every client draw
//   I f=<frame> seed=<seed> from=<time|value> caller=<module offset>       InitRandom
// 'f' is the logic frame at the start of the update for F, and TheGameLogic->getFrame() for the others.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/StateTrace.h"
#include "Common/CommandLine.h"   // O77

#include "Common/FramePacer.h"
#include "Common/GlobalData.h"
#include "Common/RandomValue.h"
#include "GameLogic/GameLogic.h"

#include <stdio.h>
#include <string>

namespace StateTrace
{
	enum
	{
		MAX_OUTSIDE_LINES = 200000,                       ///< O lines
		MAX_FILE_BYTES = 512 * 1024 * 1024,               ///< all lines
		MAX_EARLY_BYTES = 4 * 1024 * 1024,                ///< kept until the user data folder is known
	};

	static Bool readCommandLine(UnsignedInt *callsFirst, UnsignedInt *callsLast)
	{
		Bool on = FALSE;
		for (int arg = 1; arg < __argc; ++arg)
		{
			if (!CommandLine::isTestSwitchAllowed(__argv[arg])) {
				continue;   // O77: a test switch, *uitest*.exe only
			}
			if (stricmp(__argv[arg], "-stateTrace") == 0) {
				on = TRUE;
			} else if (stricmp(__argv[arg], "-stateTraceCalls") == 0 && arg + 1 < __argc) {
				unsigned int a = 0, b = 0;
				if (sscanf(__argv[arg + 1], "%u-%u", &a, &b) == 2 && a <= b) {
					*callsFirst = a;
					*callsLast = b;
					on = TRUE;
				}
			}
		}
		return on;
	}

	static UnsignedInt s_callsFirst = 1;
	static UnsignedInt s_callsLast = 0;      ///< no range: last < first
	Bool g_on = readCommandLine(&s_callsFirst, &s_callsLast);

	static FILE *s_file = nullptr;
	static Bool s_failed = FALSE;
	static Bool s_capped = FALSE;
	static std::string s_early;
	static size_t s_bytes = 0;

	static Int s_depth = 0;                  ///< inside GameLogic::update
	static UnsignedInt s_drawsIn = 0, s_unchangedIn = 0;
	static UnsignedInt s_drawsOut = 0, s_unchangedOut = 0;
	static UnsignedInt s_outsideLines = 0;
	static UnsignedInt s_clientDraws = 0;
	static UnsignedInt s_displayUpdates = 0, s_cloudUpdates = 0;
	static double s_logicFrameTimeSum = 0.0, s_cloudTimeSum = 0.0;
	static UnsignedInt s_syncTime = 0;
	static Real s_logicFrameTime = 0.0f;
	static Real s_cloudX = 0.0f, s_cloudY = 0.0f;

	static UnsignedInt frameNow() { return TheGameLogic ? TheGameLogic->getFrame() : 0; }

	static unsigned long long moduleOffset(const void *address)
	{
		const uintptr_t base = (uintptr_t)::GetModuleHandleA(nullptr);
		const uintptr_t at = (uintptr_t)address;
		return (unsigned long long)(at >= base ? at - base : at);
	}

	static const char *baseName(const char *file)
	{
		if (file == nullptr) {
			return "?";
		}
		const char *b = file;
		for (const char *p = file; *p; ++p) {
			if (*p == '\\' || *p == '/') {
				b = p + 1;
			}
		}
		return b;
	}

	static void openFile()
	{
		if (s_file || s_failed || TheGlobalData == nullptr || TheGlobalData->getPath_UserData().isEmpty()) {
			return;
		}
		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat("StateTrace.log");
		s_file = fopen(path.str(), "w");
		if (s_file == nullptr) {
			s_failed = TRUE;
			return;
		}
		setvbuf(s_file, nullptr, _IOFBF, 1 << 20);
		fprintf(s_file, "# StateTrace.log version 1, %s build\n", sizeof(void *) == 8 ? "64-bit" : "32-bit");
		fputs(s_early.c_str(), s_file);
		std::string().swap(s_early);
	}

	static void writeLine(const char *text, Bool flush)
	{
		if (s_capped || s_failed) {
			return;
		}
		const size_t n = strlen(text);
		if (s_bytes + n > MAX_FILE_BYTES) {
			s_capped = TRUE;
			if (s_file) {
				fputs("# cap reached: 512 MB, nothing more is written\n", s_file);
				fflush(s_file);
			}
			return;
		}
		s_bytes += n;
		openFile();
		if (s_file) {
			fputs(text, s_file);
			if (flush) {
				fflush(s_file);
			}
		} else if (s_early.size() + n <= MAX_EARLY_BYTES) {
			s_early += text;
		}
	}

	void logicDraw(const char *file, Int line, double lo, double hi, double value)
	{
		const Bool inside = s_depth > 0;
		if (inside) {
			++s_drawsIn;
		} else {
			++s_drawsOut;
		}
		char buf[384];
		const UnsignedInt frame = frameNow();
		if (frame >= s_callsFirst && frame <= s_callsLast)
		{
			snprintf(buf, sizeof(buf), "C f=%u in=%d u=0 v=%.9g lo=%.9g hi=%.9g at=%s:%d\n", frame, (int)inside, value, lo, hi, baseName(file), (int)line);
			writeLine(buf, FALSE);
		}
		if (!inside && s_outsideLines < MAX_OUTSIDE_LINES)
		{
			++s_outsideLines;
			snprintf(buf, sizeof(buf), "O n=%u f=%u v=%.9g lo=%.9g hi=%.9g at=%s:%d\n", s_outsideLines, frame, value, lo, hi, baseName(file), (int)line);
			writeLine(buf, FALSE);
			if (s_outsideLines == MAX_OUTSIDE_LINES) {
				writeLine("# O lines capped at 200000; od/ou still count\n", TRUE);
			}
		}
	}

	void logicUnchanged(const char *file, Int line, double lo, double hi, double value)
	{
		const Bool inside = s_depth > 0;
		if (inside) {
			++s_unchangedIn;
		} else {
			++s_unchangedOut;
		}
		const UnsignedInt frame = frameNow();
		if (frame >= s_callsFirst && frame <= s_callsLast)
		{
			char buf[384];
			snprintf(buf, sizeof(buf), "C f=%u in=%d u=1 v=%.9g lo=%.9g hi=%.9g at=%s:%d\n", frame, (int)inside, value, lo, hi, baseName(file), (int)line);
			writeLine(buf, FALSE);
		}
	}

	void clientDraw(const char *file, Int line, double lo, double hi, double value, void *caller)
	{
		++s_clientDraws;
		const UnsignedInt frame = frameNow();
		if (frame < s_callsFirst || frame > s_callsLast) {
			return;
		}
		// The caller's caller names the code behind draws that come through a helper (GameClientRandomVariable::getValue,
		// whose file and line are its own). The stack walk finds the caller's frame and takes the next one; it is exact on
		// x64 (unwind data) and best effort on x86, where frames without a frame pointer can end the walk (then ret2=0).
		void *frames[12] = {};
		const USHORT got = ::RtlCaptureStackBackTrace(0, 12, frames, nullptr);
		void *caller2 = nullptr;
		for (USHORT i = 0; i + 1 < got; ++i)
		{
			if (frames[i] == caller) {
				caller2 = frames[i + 1];
				break;
			}
		}
		char buf[448];
		snprintf(buf, sizeof(buf), "K f=%u in=%d n=%u v=%.9g lo=%.9g hi=%.9g at=%s:%d ret=0x%llx ret2=0x%llx\n", frame,
			(int)(s_depth > 0), s_clientDraws, value, lo, hi, baseName(file), (int)line, moduleOffset(caller),
			caller2 ? moduleOffset(caller2) : 0ull);
		writeLine(buf, FALSE);
	}

	void initRandom(UnsignedInt seed, Bool fromTime, void *caller)
	{
		char buf[192];
		snprintf(buf, sizeof(buf), "I f=%u seed=%u from=%s caller=0x%llx\n", frameNow(), seed, fromTime ? "time" : "value",
			moduleOffset(caller));
		writeLine(buf, TRUE);
	}

	void LogicUpdateScope::begin()
	{
		m_frame = frameNow();
		if (s_depth++ == 0) {
			s_drawsIn = 0;
			s_unchangedIn = 0;
		}
	}

	void LogicUpdateScope::end()
	{
		if (--s_depth > 0) {
			return;
		}
		const UnsignedInt after = frameNow();
		char buf[640];
		snprintf(buf, sizeof(buf),
			"F f=%u adv=%d seed=%08X ld=%u lu=%u od=%u ou=%u cs=%08X cd=%u sync=%u lft=%.9g lfts=%.9g du=%u cu=%u cts=%.9g cx=%.9g cy=%.9g put=%.9g step=%.9g\n",
			m_frame, (int)(after != m_frame), GetGameLogicRandomSeedCRC(), s_drawsIn, s_unchangedIn, s_drawsOut, s_unchangedOut,
			GetGameClientRandomSeedCRC(), s_clientDraws, s_syncTime, (double)s_logicFrameTime, s_logicFrameTimeSum,
			s_displayUpdates, s_cloudUpdates, s_cloudTimeSum, (double)s_cloudX, (double)s_cloudY,
			TheFramePacer ? (double)TheFramePacer->getUpdateTime() : 0.0,
			TheFramePacer ? (double)TheFramePacer->getLogicTimeStepMilliseconds() : 0.0);
		writeLine(buf, TRUE);
		s_drawsOut = 0;
		s_unchangedOut = 0;
		s_clientDraws = 0;
		s_displayUpdates = 0;
		s_cloudUpdates = 0;
		s_logicFrameTimeSum = 0.0;
		s_cloudTimeSum = 0.0;
	}

	void displayUpdate(UnsignedInt syncTimeMS, Real logicFrameTimeMS)
	{
		++s_displayUpdates;
		s_syncTime = syncTimeMS;
		s_logicFrameTime = logicFrameTimeMS;
		s_logicFrameTimeSum += logicFrameTimeMS;
	}

	void cloudUpdate(Real frameTimeSeconds, Real xOffset, Real yOffset)
	{
		++s_cloudUpdates;
		s_cloudTimeSum += frameTimeSeconds;
		s_cloudX = xOffset;
		s_cloudY = yOffset;
	}
}
