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

// ReplayStream.cpp /////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See ReplayStream.h.
//
// The two kinds of thread (server, mirror) use only their own heap memory, the process heap and Win32 calls: no game
// objects, no game memory pools. Each thread gets a copy of its settings when it starts and a generation number; it runs
// while its generation is the current one, so stopping it never waits (O33). What the game thread reads from them are
// volatile values that only the current generation writes.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <winsock.h>

#include "Common/ReplayStream.h"

#include "Common/GlobalData.h"

#include <stdio.h>
#include <stdarg.h>
#include "Common/LanClock.h"   // O75

namespace
{
	enum
	{
		MAX_VIEWERS = 100,
		MAX_PER_ADDRESS = 3,          ///< O33: connections one address may hold
		MAX_SOURCES = 32,             ///< players, room observers and the relays sources named (O34)
		MAX_LIVE = 16,                ///< O33: live (room observer) addresses
		OUT_BUFFER = 256 * 1024,      ///< per viewer, allocated after its hello
		CHUNK = 32 * 1024,            ///< the most file bytes in one DATA message
		IN_BUFFER = 2 * (CHUNK + 16),
		HELLO_SIZE = 28,              ///< O42: version 3 (by frame); older hellos are refused
		HELLO_VERSION = 4,            ///< O62: 4 = LAN protocol 1 (an older viewer or source is "another game")
		FIDX_MAX = 64,                ///< O42: frame ends in one FIDX message
		RESUME_WAIT_MS = 30000,       ///< O42: a viewer ahead of this source waits this long for the source to reach its frame
		PEND_START = 256 * 1024,      ///< O42: a mirror's bytes not yet part of a whole frame
		PEND_MAX = 64 * 1024 * 1024,
		MAX_REDIRECTS = 8,            ///< relays named in one MORE
		HELLO_MS = 3000,              ///< O33: a connection without a complete hello for this long is closed
		DROP_MS = 10000,              ///< a viewer whose data does not drain for this long is dropped
		LIVE_EVERY_MS = 500,
		SILENT_HOP_MS = 5000,         ///< a source silent for this long is left
		STUCK_HOP_MS = 8000,          ///< a source whose newest frame does not move for this long is left (live viewers)
		HEADER_WAIT_MS = 60000,
		NO_SOURCE_END_MS = 120000,    ///< O34: no source has given data for this long: the stream is lost ("reconnecting" before)
		COOLDOWN_MS = 10000,          ///< O34: a source that failed or refused is not asked again for this long
		REDIRECT_RECENT_MS = 30000,   ///< O34: coming back within this time to a source that sent us on: the last resort
		FIRST_CONTACT_MS = 180000,    ///< before any source answered (the players still load the map): wait this long
		HISTORY_MAX = 32768,          ///< O33: size/state samples a server keeps (one per change: 3 min of a match fit)
		MAX_DELAY_S = 180,
	};
	const UnsignedInt ALL_FRAMES = 0xFFFFFFFFu;   ///< O42: a hello's "from the start" (the header and every frame)

	struct LiveRecord
	{
		char magic[4];
		UnsignedInt frame;
		UnsignedInt ended;
		UnsignedInt counter;
	};

	//--- command line settings ---------------------------------------------------------------------
	Int s_cmdPort = 0;
	Int s_cmdDelayS = 0;
	Bool s_cmdDelaySet = FALSE;
	UnsignedInt s_cmdLive[MAX_LIVE];
	Int s_cmdLiveCount = 0;
	Int s_serveMax = 16;
	Int s_serveRateKB = 4096;
	Int s_serveStopAfterSec = 0;
	Int s_serveOutsideMax = 4;
	UnsignedInt s_serveLagIp = 0;   ///< O42 (test): -serveReplayLagFor <ip>:<seconds>
	Int s_serveLagMs = 0;

	//--- a LAN match's serving (game thread) --------------------------------------------------------
	Bool s_matchSet = FALSE;
	UnsignedInt s_matchBind = 0;      ///< host order
	Int s_matchPort = 0;
	Int s_matchDelayS = 60;
	Bool s_matchOutside = TRUE;
	Bool s_matchReceivesDelayed = FALSE;
	UnsignedInt s_matchLive[MAX_LIVE];
	Int s_matchLiveCount = 0;
	UnsignedInt s_matchRefused[MAX_LIVE];   ///< O62 (H5 #3): the match's players
	Int s_matchRefusedCount = 0;

	//--- viewer settings (game thread) ---------------------------------------------------------------
	/// O34: rank: 0 an outside viewer that relays, 1 a room observer that relays, 2 a player (or a -watchReplay source);
	/// an outside viewer asks the lowest rank first, so the players carry almost nothing.
	struct Source { char host[64]; unsigned short port; Int rank; DWORD retryAfterMs; DWORD redirectedMs; };
	Source s_sources[MAX_SOURCES];
	Int s_sourceCount = 0;
	UnsignedInt s_watchFlags = 0;
	UnsignedInt s_watchBind = 0;      ///< host order; 0: the system's choice
	enum { NOPE_REFUSED = 0, NOPE_ELSEWHERE = 1, NOPE_NOT_ALLOWED = 2, NOPE_OTHER_GAME = 3, NOPE_PLAYER = 4 };   ///< O34: NOPE's reason (O64: 4)
	enum { HELLO_ROOM = 1, HELLO_LAST_RESORT = 2 };   ///< hello flags

	//--- log -----------------------------------------------------------------------------------------
	CRITICAL_SECTION s_logLock;
	Bool s_logLockReady = FALSE;
	FILE *s_log = nullptr;
	DWORD s_startMs = 0;

	void openLog()
	{
		if (!s_logLockReady)
		{
			InitializeCriticalSection(&s_logLock);
			s_logLockReady = TRUE;
			s_startMs = LanClock::now();
		}
		EnterCriticalSection(&s_logLock);
		if (s_log == nullptr && TheGlobalData != nullptr)
		{
			char path[_MAX_PATH];
			snprintf(path, sizeof(path), "%sReplayStream.log", TheGlobalData->getPath_UserData().str());
			s_log = fopen(path, "w");
		}
		LeaveCriticalSection(&s_logLock);
	}

	void logLine(const char *format, ...)
	{
		if (!s_logLockReady) {
			return;
		}
		EnterCriticalSection(&s_logLock);
		if (s_log != nullptr)
		{
			fprintf(s_log, "[%8.3f] ", (LanClock::now() - s_startMs) / 1000.0);
			va_list args;
			va_start(args, format);
			vfprintf(s_log, format, args);
			va_end(args);
			fputc('\n', s_log);
			fflush(s_log);
		}
		LeaveCriticalSection(&s_logLock);
	}

	//--- helpers ------------------------------------------------------------------------------------
	void put32(unsigned char *p, UnsignedInt v)
	{
		p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
	}

	UnsignedInt get32(const unsigned char *p)
	{
		return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8) | ((UnsignedInt)p[2] << 16) | ((UnsignedInt)p[3] << 24);
	}

	HANDLE openShared(const char *path, DWORD access, DWORD creation)
	{
		return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	Bool readLive(const char *livePath, LiveRecord *out)
	{
		HANDLE h = openShared(livePath, GENERIC_READ, OPEN_EXISTING);
		if (h == INVALID_HANDLE_VALUE) {
			return FALSE;
		}
		DWORD got = 0;
		const BOOL ok = ReadFile(h, out, sizeof(*out), &got, nullptr);
		CloseHandle(h);
		return ok && got == sizeof(*out) && memcmp(out->magic, "GLIV", 4) == 0;
	}

	Bool readAt(HANDLE file, UnsignedInt offset, void *buffer, DWORD length, DWORD *got)
	{
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		ov.Offset = offset;
		*got = 0;
		return ReadFile(file, buffer, length, got, &ov) != FALSE;
	}

	UnsignedInt fileSize(HANDLE file)
	{
		LARGE_INTEGER size;
		return GetFileSizeEx(file, &size) ? (UnsignedInt)size.QuadPart : 0;
	}

	/// The thread runs below normal priority and prefers the last core. Its handle is closed at once: nobody joins it.
	Bool startThread(LPTHREAD_START_ROUTINE main, void *param)
	{
		HANDLE thread = CreateThread(nullptr, 0, main, param, 0, nullptr);
		if (thread == nullptr) {
			return FALSE;
		}
		SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		if (si.dwNumberOfProcessors > 1) {
			SetThreadIdealProcessor(thread, si.dwNumberOfProcessors - 1);
		}
		CloseHandle(thread);
		return TRUE;
	}

	Bool s_wsaReady = FALSE;
	Bool startWinsock()
	{
		if (s_wsaReady) {
			return TRUE;
		}
		WSADATA data;
		s_wsaReady = WSAStartup(MAKEWORD(1, 1), &data) == 0;
		return s_wsaReady;
	}

	/// "a.b.c.d" to an address in host order; FALSE when it is not one.
	Bool parseIPv4(const char *text, UnsignedInt *out)
	{
		unsigned int part[4];
		char tail = 0;
		if (sscanf(text, "%u.%u.%u.%u%c", &part[0], &part[1], &part[2], &part[3], &tail) != 4) {
			return FALSE;
		}
		for (Int i = 0; i < 4; ++i) {
			if (part[i] > 255) return FALSE;
		}
		*out = (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3];
		return TRUE;
	}

	Int parseIpList(const char *text, UnsignedInt *out, Int maxCount)
	{
		Int n = 0;
		const char *p = text;
		while (p != nullptr && *p != '\0' && n < maxCount)
		{
			const char *end = strchr(p, ',');
			char item[32];
			const size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
			const size_t k = len < sizeof(item) - 1 ? len : sizeof(item) - 1;
			memcpy(item, p, k);
			item[k] = '\0';
			if (parseIPv4(item, &out[n])) {
				++n;
			}
			p = end != nullptr ? end + 1 : nullptr;
		}
		return n;
	}

	void setNonBlocking(SOCKET s)
	{
		u_long on = 1;
		ioctlsocket(s, FIONBIO, &on);
	}

	//=============================================================================================
	// Server
	//=============================================================================================
	volatile LONG s_serverGen = 0;
	volatile LONG s_drainGen = 0;
	volatile LONG s_mirrorSourceIp = 0;      ///< O34: the address this game's mirror takes its data from now (host order)
	volatile LONG s_mirrorConnected = 0;     ///< O34: the mirror gets data from a source now            ///< O33: that generation's recording is over: no new viewers, finish the ones there
	volatile LONG s_countRoom = 0;
	volatile LONG s_mirrorNotAllowed = 0;    ///< O42: the last loss: a source said the host does not allow outside viewers
	volatile LONG s_mirrorIsPlayer = 0;      ///< O64: the last loss: a source said this computer is a player of the match
	volatile LONG s_countOutside = 0;

	struct ServeConfig
	{
		LONG gen;
		UnsignedInt bind;          ///< host order; 0: 127.0.0.1
		Int port;
		Int delayMs;               ///< for viewers that are not live
		Bool outsideAllowed;
		Bool preferRelays;         ///< O34: it holds live data (a player, a room observer): outside viewers go to relays first
		UnsignedInt live[MAX_LIVE];
		Int liveCount;
		UnsignedInt refused[MAX_LIVE];   ///< O62 (H5 #3): the match's players: never served
		Int refusedCount;
		Int max, outsideMax, rateKB, stopAfterSec;
		UnsignedInt lagIp;         ///< O42 (test, -serveReplayLagFor): this address is served lagMs further behind (a slow link)
		Int lagMs;
		char path[_MAX_PATH];
		char livePath[_MAX_PATH];
		char idxPath[_MAX_PATH];   ///< O42: the frame index beside the replay
	};

	//--- O42: the frame index ---------------------------------------------------------------------------------------------
	// Beside every replay that is served (written by the recorder through ReplayFollow, or by a mirror) lies "<replay>.idx":
	// "GIDX", the game key (FNV-1a of the match's slot list, the same on every computer of the match), the end of the
	// header, then one (frame, end offset) per frame: the commands of every frame up to that one lie below that offset.
	// The players' files differ in their headers (start time, local player), so a viewer that moves to another source asks
	// "after frame F", never for a byte offset; the commands of a frame are the same in every player's file.
	struct FrameIndex
	{
		UnsignedInt key;
		UnsignedInt headerEnd;
		Bool haveHeader;
		UnsignedInt first;          ///< the first frame in 'ends'
		UnsignedInt *ends;          ///< ends[f - first]
		UnsignedInt count, cap;
		UnsignedInt fileRead;       ///< bytes of the index file read so far
	};

	void indexFree(FrameIndex &x)
	{
		if (x.ends != nullptr) {
			HeapFree(GetProcessHeap(), 0, x.ends);
		}
		memset(&x, 0, sizeof(x));
	}

	void indexPut(FrameIndex &x, UnsignedInt frame, UnsignedInt end)
	{
		if (x.count == 0) {
			x.first = frame;
		}
		if (frame < x.first) {
			return;
		}
		const UnsignedInt pos = frame - x.first;
		if (pos < x.count)
		{
			x.ends[pos] = end;   // the same frame again (an update that did not advance the logic)
			return;
		}
		while (x.count <= pos)
		{
			if (x.count == x.cap)
			{
				const UnsignedInt cap = x.cap == 0 ? 4096 : x.cap * 2;
				UnsignedInt *grown = x.ends == nullptr ? (UnsignedInt *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(UnsignedInt))
					: (UnsignedInt *)HeapReAlloc(GetProcessHeap(), 0, x.ends, cap * sizeof(UnsignedInt));
				if (grown == nullptr) {
					return;
				}
				x.ends = grown;
				x.cap = cap;
			}
			// a frame without an entry of its own ends where the one before it ended
			x.ends[x.count] = (x.count == pos || x.count == 0) ? end : x.ends[x.count - 1];
			++x.count;
		}
	}

	/// Reads what was added to the index file since the last call (whole entries only).
	void indexRead(FrameIndex &x, const char *path)
	{
		HANDLE h = openShared(path, GENERIC_READ, OPEN_EXISTING);
		if (h == INVALID_HANDLE_VALUE) {
			return;
		}
		const UnsignedInt size = fileSize(h);
		if (!x.haveHeader && size >= 12)
		{
			unsigned char head[12];
			DWORD got = 0;
			if (readAt(h, 0, head, 12, &got) && got == 12 && memcmp(head, "GIDX", 4) == 0)
			{
				x.key = get32(head + 4);
				x.headerEnd = get32(head + 8);
				x.haveHeader = TRUE;
				x.fileRead = 12;
			}
		}
		if (x.haveHeader && size >= x.fileRead + 8)
		{
			const UnsignedInt want = ((size - x.fileRead) / 8) * 8;
			unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, want);
			DWORD got = 0;
			if (buf != nullptr && readAt(h, x.fileRead, buf, want, &got))
			{
				const UnsignedInt whole = (got / 8) * 8;
				for (UnsignedInt k = 0; k < whole; k += 8) {
					indexPut(x, get32(buf + k), get32(buf + k + 4));
				}
				x.fileRead += whole;
			}
			if (buf != nullptr) {
				HeapFree(GetProcessHeap(), 0, buf);
			}
		}
		CloseHandle(h);
	}

	Bool indexHas(const FrameIndex &x, UnsignedInt frame)
	{
		return x.count > 0 && frame >= x.first && frame - x.first < x.count;
	}

	UnsignedInt indexLast(const FrameIndex &x)
	{
		return x.first + x.count - 1;
	}

	UnsignedInt indexEnd(const FrameIndex &x, UnsignedInt frame)
	{
		return x.ends[frame - x.first];
	}

	struct Viewer
	{
		SOCKET socket;
		UnsignedInt ip;            ///< host order (the connection's source address)
		unsigned char hello[HELLO_SIZE];
		Int helloLen;
		Bool ready;                ///< its hello was accepted
		Bool started;              ///< O42: its place in this file is known (it may wait for a frame this source has not yet)
		UnsignedInt resume;        ///< O42: what it asked for: after this frame, or ALL_FRAMES
		Bool live;                 ///< O33: a listed room observer: gets the newest data; else delayed
		Bool roomFlag;             ///< what its hello said (counting only)
		UnsignedInt sent;          ///< bytes of this file queued for this viewer so far
		UnsignedInt nextFrame;     ///< O42: the next frame whose end it gets (FIDX)
		unsigned char *out;
		Int outLen, outPos;
		DWORD acceptMs;
		DWORD lastDrainMs;
		DWORD lastLiveMs;
		Int number;
		unsigned short relayPort;  ///< 0: it does not relay
		Int lagMs;                 ///< O42 (test): served this much further behind
	};

	struct Sample { DWORD ms; UnsignedInt size; LiveRecord live; Bool haveLive; };

	void dropViewer(Viewer &v, const char *why)
	{
		logLine("server: viewer %d dropped (%s), it had %u bytes", v.number, why, v.sent);
		closesocket(v.socket);
		v.socket = INVALID_SOCKET;
		if (v.out != nullptr)
		{
			HeapFree(GetProcessHeap(), 0, v.out);
			v.out = nullptr;
		}
	}

	/// Queues one message; FALSE (nothing queued) when it would not fit the viewer's buffer.
	Bool queue(Viewer &v, const char *type, const unsigned char *payload, Int length)
	{
		if (v.out == nullptr || length < 0 || v.outLen + 8 + length > OUT_BUFFER) {
			return FALSE;
		}
		unsigned char *p = v.out + v.outLen;
		memcpy(p, type, 4);
		put32(p + 4, (UnsignedInt)length);
		if (length > 0) {
			memcpy(p + 8, payload, length);
		}
		v.outLen += 8 + length;
		return TRUE;
	}

	Int roomOf(const Viewer &v)
	{
		return OUT_BUFFER - v.outLen;
	}

	void compact(Viewer &v)
	{
		if (v.outPos > 0)
		{
			memmove(v.out, v.out + v.outPos, v.outLen - v.outPos);
			v.outLen -= v.outPos;
			v.outPos = 0;
		}
	}

	Bool isLive(const ServeConfig &c, UnsignedInt ip)
	{
		for (Int i = 0; i < c.liveCount; ++i) {
			if (c.live[i] == ip) return TRUE;
		}
		return FALSE;
	}

	Bool isRefused(const ServeConfig &c, UnsignedInt ip)
	{
		for (Int i = 0; i < c.refusedCount; ++i) {
			if (c.refused[i] == ip) return TRUE;
		}
		return FALSE;
	}

	/// Sends the bytes queued so far, without waiting (used only for a refusal before the drop).
	void flushNow(Viewer &v)
	{
		if (v.out != nullptr && v.outLen > v.outPos) {
			send(v.socket, (const char *)v.out + v.outPos, v.outLen - v.outPos, 0);
		}
	}

	/// Refuses a viewer with a reason (a buffer for the reply if it has none yet, then the drop).
	void refuse(Viewer &v, UnsignedInt reason, const char *why)
	{
		if (v.out == nullptr) {
			v.out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, OUT_BUFFER);
		}
		if (v.out != nullptr)
		{
			unsigned char r[4];
			put32(r, reason);
			queue(v, "NOPE", r, 4);
			flushNow(v);
		}
		dropViewer(v, why);
	}

	/// O42: the frames whose commands the viewer has whole now, with their ends in this file's offsets.
	void queueFrameEnds(Viewer &v, const FrameIndex &index, UnsignedInt allowFrame, unsigned char *fidx)
	{
		if (index.count == 0) {
			return;
		}
		if (v.nextFrame < index.first) {
			v.nextFrame = index.first;   // a viewer that came before the first frame was written
		}
		while (v.nextFrame <= allowFrame && roomOf(v) >= 8 + 4 + FIDX_MAX * 8)
		{
			UnsignedInt n = 0;
			while (n < FIDX_MAX && v.nextFrame <= allowFrame && indexEnd(index, v.nextFrame) <= v.sent)
			{
				put32(fidx + 4 + n * 8, v.nextFrame);
				put32(fidx + 8 + n * 8, indexEnd(index, v.nextFrame));
				++n;
				++v.nextFrame;
			}
			if (n == 0) {
				break;
			}
			put32(fidx, n);
			queue(v, "FIDX", fidx, (Int)(4 + n * 8));
		}
	}

	DWORD WINAPI serverMain(LPVOID param)
	{
		ServeConfig *cfg = (ServeConfig *)param;
		const ServeConfig &c = *cfg;
		Viewer *viewers = (Viewer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Viewer) * MAX_VIEWERS);
		Sample *history = (Sample *)HeapAlloc(GetProcessHeap(), 0, sizeof(Sample) * HISTORY_MAX);
		const auto current = [&]() { return InterlockedCompareExchange(&s_serverGen, 0, 0) == c.gen; };
		if (viewers == nullptr || history == nullptr)
		{
			if (viewers) HeapFree(GetProcessHeap(), 0, viewers);
			if (history) HeapFree(GetProcessHeap(), 0, history);
			HeapFree(GetProcessHeap(), 0, cfg);
			return 0;
		}
		for (Int i = 0; i < MAX_VIEWERS; ++i) {
			viewers[i].socket = INVALID_SOCKET;
		}

		// the listener: the LAN address or 127.0.0.1, never every interface; an old server may still hold the port for a
		// moment (it ends by its generation), so the bind is retried for up to 3 seconds
		SOCKET listener = INVALID_SOCKET;
		const UnsignedInt bindHost = c.bind != 0 ? c.bind : 0x7F000001u;
		const DWORD bindStart = LanClock::now();
		while (current())
		{
			listener = socket(AF_INET, SOCK_STREAM, 0);
			sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons((unsigned short)c.port);
			addr.sin_addr.s_addr = htonl(bindHost);
			if (listener != INVALID_SOCKET && bind(listener, (sockaddr *)&addr, sizeof(addr)) == 0 && listen(listener, 8) == 0) {
				break;
			}
			if (listener != INVALID_SOCKET) {
				closesocket(listener);
			}
			listener = INVALID_SOCKET;
			if (LanClock::now() - bindStart > 3000) {
				break;
			}
			Sleep(100);
		}
		if (listener == INVALID_SOCKET)
		{
			logLine("server: cannot listen on %u.%u.%u.%u:%d (%d)", bindHost >> 24, (bindHost >> 16) & 255, (bindHost >> 8) & 255,
				bindHost & 255, c.port, WSAGetLastError());
			HeapFree(GetProcessHeap(), 0, viewers);
			HeapFree(GetProcessHeap(), 0, history);
			HeapFree(GetProcessHeap(), 0, cfg);
			return 0;
		}
		setNonBlocking(listener);
		logLine("server: listening on %u.%u.%u.%u:%d for %s (at most %d viewers, %d outside ones%s, delay %d s for viewers not listed "
			"live, %d live address(es), %d KB/s)", bindHost >> 24, (bindHost >> 16) & 255, (bindHost >> 8) & 255, bindHost & 255, c.port,
			c.path, c.max, c.outsideMax, c.outsideAllowed ? "" : " (none allowed)", c.delayMs / 1000, c.liveCount, c.rateKB);
		if (c.lagIp != 0)
		{
			logLine("server: test lag: %u.%u.%u.%u is served %d s further behind (-serveReplayLagFor)", c.lagIp >> 24, (c.lagIp >> 16) & 255,
				(c.lagIp >> 8) & 255, c.lagIp & 255, c.lagMs / 1000);
		}

		HANDLE file = INVALID_HANDLE_VALUE;
		FrameIndex index;
		memset(&index, 0, sizeof(index));
		const DWORD startMs = LanClock::now();
		DWORD lastTickMs = startMs;
		Bool draining = FALSE;
		DWORD drainStartMs = 0;
		double tokens = 0.0;
		const double rate = (double)c.rateKB * 1024.0;
		Int nextNumber = 1;
		Int historyFirst = 0, historyCount = 0;   // ring
		DWORD lastReportMs = startMs;
		const DWORD keepMs = (DWORD)(c.delayMs + c.lagMs) + 2000;
		unsigned char chunk[CHUNK + 4];
		unsigned char fidx[4 + FIDX_MAX * 8];

		// the newest sample at least 'ageMs' old (the state a viewer that far behind may have)
		const auto sampleAt = [&](DWORD now, DWORD ageMs, Sample *out) -> Bool {
			for (Int k = historyCount - 1; k >= 0; --k)
			{
				const Sample &s = history[(historyFirst + k) % HISTORY_MAX];
				if (now - s.ms >= ageMs)
				{
					*out = s;
					return TRUE;
				}
			}
			return FALSE;
		};

		while (current())
		{
			const DWORD now = LanClock::now();
			if (c.stopAfterSec > 0 && now - startMs > (DWORD)c.stopAfterSec * 1000)
			{
				logLine("server: stopping after %d seconds (-serveReplayStopAfter, a forced source loss)", c.stopAfterSec);
				break;
			}
			tokens = min(rate, tokens + rate * (now - lastTickMs) / 1000.0);
			lastTickMs = now;
			// O33 (review 4): the recording is over: the listener closes at once; the viewers there get the rest (a delayed
			// one the last delayMs of the match), then the server ends
			if (!draining && InterlockedCompareExchange(&s_drainGen, 0, 0) == c.gen)
			{
				draining = TRUE;
				drainStartMs = now;
				closesocket(listener);
				listener = INVALID_SOCKET;
				logLine("server: the recording is over: no new viewers; the ones here get the rest (at most %d s)", (Int)keepMs / 1000 + 3);
			}
			if (draining)
			{
				Bool anyone = FALSE;
				for (Int i = 0; i < MAX_VIEWERS && !anyone; ++i) {
					anyone = viewers[i].socket != INVALID_SOCKET && viewers[i].ready;
				}
				if (!anyone || now - drainStartMs > keepMs + 3000) {
					break;
				}
			}

			// new connections: only the table's room and the per-address limit here; the rest after the hello
			while (listener != INVALID_SOCKET)
			{
				sockaddr_in peer;
				int peerLen = sizeof(peer);
				SOCKET s = accept(listener, (sockaddr *)&peer, &peerLen);
				if (s == INVALID_SOCKET) {
					break;
				}
				const UnsignedInt ip = ntohl(peer.sin_addr.s_addr);
				Int slot = -1, sameAddress = 0;
				for (Int i = 0; i < MAX_VIEWERS; ++i)
				{
					if (viewers[i].socket == INVALID_SOCKET) {
						if (slot < 0) slot = i;
					} else if (viewers[i].ip == ip) {
						++sameAddress;
					}
				}
				if (slot < 0 || sameAddress >= MAX_PER_ADDRESS)
				{
					closesocket(s);   // O33 (review 9): the table is full, or that address has its share
					continue;
				}
				setNonBlocking(s);
				Viewer &v = viewers[slot];
				memset(&v, 0, sizeof(v));
				v.socket = s;
				v.ip = ip;
				v.acceptMs = now;
				v.lastDrainMs = now;
				v.number = nextNumber++;
				v.lagMs = (c.lagIp != 0 && ip == c.lagIp) ? c.lagMs : 0;
			}

			if (file == INVALID_HANDLE_VALUE) {
				file = openShared(c.path, GENERIC_READ, OPEN_EXISTING);
			}
			// the side file first, then the index and the size: the writer writes a frame's index entry before its side
			// file, so the index read after the side file has every frame the side file names
			Sample cur;
			cur.ms = now;
			cur.haveLive = readLive(c.livePath, &cur.live);
			indexRead(index, c.idxPath);
			cur.size = file != INVALID_HANDLE_VALUE ? fileSize(file) : 0;
			// O33: the history of (time, side file), one sample per change, for the delayed viewers
			{
				const Sample *last = historyCount > 0 ? &history[(historyFirst + historyCount - 1) % HISTORY_MAX] : nullptr;
				if (last == nullptr || last->haveLive != cur.haveLive || (cur.haveLive && memcmp(&last->live, &cur.live, sizeof(cur.live)) != 0))
				{
					if (historyCount == HISTORY_MAX)
					{
						historyFirst = (historyFirst + 1) % HISTORY_MAX;
						--historyCount;
					}
					history[(historyFirst + historyCount) % HISTORY_MAX] = cur;
					++historyCount;
				}
				// forget what no delayed viewer can need any more (keep one older sample)
				while (historyCount > 1)
				{
					const Sample &second = history[(historyFirst + 1) % HISTORY_MAX];
					if (now - second.ms <= keepMs) break;
					historyFirst = (historyFirst + 1) % HISTORY_MAX;
					--historyCount;
				}
			}

			if (now - lastReportMs >= 10000)
			{
				// O33: for the tests: what the live and the delayed viewers may have now
				lastReportMs = now;
				Sample d;
				const Bool haveD = sampleAt(now, (DWORD)c.delayMs, &d) && d.haveLive;
				logLine("server: newest frame %u; delayed (%d s) frame %u; index: %u frames from %u, key %08X, header %u bytes",
					cur.haveLive ? cur.live.frame : 0, c.delayMs / 1000, haveD ? d.live.frame : 0, index.count, index.first, index.key,
					index.headerEnd);
			}
			Int roomCount = 0, outsideCount = 0;
			for (Int i = 0; i < MAX_VIEWERS; ++i)
			{
				Viewer &v = viewers[i];
				if (v.socket == INVALID_SOCKET) {
					continue;
				}
				// the hello: read only what fits the array (O33, review 1: the length is checked before the read)
				if (!v.ready)
				{
					const Int want = HELLO_SIZE - v.helloLen;
					if (want > 0)
					{
						const int got = recv(v.socket, (char *)v.hello + v.helloLen, want, 0);
						if (got == 0 || (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
							dropViewer(v, "closed before its hello");
							continue;
						}
						if (got > 0) {
							v.helloLen += got;
						}
					}
					if (v.helloLen >= 4 && memcmp(v.hello, "GRSV", 4) != 0)
					{
						dropViewer(v, "not a viewer");
						continue;
					}
					if (v.helloLen >= 8 && get32(v.hello + 4) != HELLO_VERSION)
					{
						refuse(v, NOPE_OTHER_GAME, "another hello version");
						continue;
					}
					if (v.helloLen < HELLO_SIZE)
					{
						if (now - v.acceptMs > HELLO_MS) {
							dropViewer(v, "no hello within 3 seconds");
						}
						continue;
					}
					// the recorder writes the index header right after the replay's header: a moment at the start
					if (!index.haveHeader)
					{
						if (now - v.acceptMs > HELLO_MS + 2000) {
							refuse(v, NOPE_REFUSED, "no frame index beside the replay");
						}
						continue;
					}
					// O42 (hello version 3): "after frame F" (ALL_FRAMES: from the start, header included) and the game key
					// the viewer knows (0: none yet); the place in this file comes from its own frame index
					v.resume = get32(v.hello + 8);
					const UnsignedInt key = get32(v.hello + 12);
					const UnsignedInt flags = get32(v.hello + 20);
					if (key != 0 && key != index.key)
					{
						refuse(v, NOPE_OTHER_GAME, "another game (game key)");
						continue;
					}
					if (v.resume != ALL_FRAMES && index.count > 0 && v.resume < index.first)
					{
						refuse(v, NOPE_REFUSED, "it asks for a frame before this source's first one");
						continue;
					}
					// O62 (H5 #3): a player of this match (alive, or defeated: who joined the observers switches his own
					// view and needs no stream) never gets it, delayed or not
					if (isRefused(c, v.ip))
					{
						refuse(v, NOPE_PLAYER, "a player of this match");
						continue;
					}
					v.out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, OUT_BUFFER);
					if (v.out == nullptr)
					{
						dropViewer(v, "no memory");
						continue;
					}
					v.roomFlag = (flags & HELLO_ROOM) != 0;
					const Bool lastResort = (flags & HELLO_LAST_RESORT) != 0;
					v.relayPort = (unsigned short)get32(v.hello + 24);
					// O33 (review 2): live only for a listed room observer's address; everybody else is an outside viewer
					v.live = isLive(c, v.ip);
					Int rooms = 0, outs = 0;
					for (Int k = 0; k < MAX_VIEWERS; ++k)
					{
						if (k != i && viewers[k].socket != INVALID_SOCKET && viewers[k].ready) {
							++(viewers[k].live ? rooms : outs);
						}
					}
					// O34: the relays this server knows (outside viewers first, then room observers), never the asker itself
					unsigned char more[4 + MAX_REDIRECTS * 12];
					Int n = 0;
					for (Int pass = 0; pass < 2; ++pass)
					{
						for (Int k = 0; k < MAX_VIEWERS && n < MAX_REDIRECTS; ++k)
						{
							const Viewer &o = viewers[k];
							if (k != i && o.socket != INVALID_SOCKET && o.ready && o.relayPort != 0 && o.ip != v.ip && (o.live ? 1 : 0) == pass)
							{
								put32(more + 4 + n * 12, o.ip);
								put32(more + 8 + n * 12, o.relayPort);
								put32(more + 12 + n * 12, o.live ? 1 : 0);
								++n;
							}
						}
					}
					put32(more, (UnsignedInt)n);
					const Bool refuseOutside = !v.live && !c.outsideAllowed;
					const Bool full = rooms + outs >= c.max || (!v.live && outs >= c.outsideMax);
					// O34: a server with live data sends an outside viewer to the relays first; it serves it itself only when
					// none is known or the viewer comes back from them (last resort)
					const Bool elsewhere = !v.live && c.preferRelays && !lastResort && n > 0;
					// O34: never feed the source this game's own mirror reads from (a loop of two would starve both)
					const Bool loop = !v.live && v.ip != 0 && v.ip == (UnsignedInt)InterlockedCompareExchange(&s_mirrorSourceIp, 0, 0);
					if (refuseOutside || full || elsewhere || loop)
					{
						if (!refuseOutside && !loop && n > 0) {
							queue(v, "MORE", more, 4 + n * 12);
						}
						unsigned char reason[4];
						put32(reason, refuseOutside ? NOPE_NOT_ALLOWED : NOPE_ELSEWHERE);
						queue(v, "NOPE", reason, 4);
						flushNow(v);
						dropViewer(v, refuseOutside ? "outside viewers are not allowed" : (loop ? "it is this game's own source"
							: (elsewhere ? "sent on to the relays" : "full")));
						continue;
					}
					v.ready = TRUE;
					v.lastLiveMs = now;
					logLine("server: viewer %d (%u.%u.%u.%u, %s%s%s) asks after frame %d", v.number, v.ip >> 24, (v.ip >> 16) & 255,
						(v.ip >> 8) & 255, v.ip & 255, v.live ? "live: a room observer" : "delayed", v.relayPort != 0 ? ", relays" : "",
						v.lagMs > 0 ? ", test lag" : "", v.resume == ALL_FRAMES ? -1 : (Int)v.resume);
				}
				++(v.live ? roomCount : outsideCount);
				// O42: its place in this file: after the frame it asked for, once this source has that frame (another player
				// may be a few frames behind the one the viewer came from: it is held here for up to 30 seconds)
				if (!v.started)
				{
					if (v.resume == ALL_FRAMES || indexHas(index, v.resume))
					{
						v.started = TRUE;
						v.sent = v.resume == ALL_FRAMES ? 0 : indexEnd(index, v.resume);
						v.nextFrame = v.resume == ALL_FRAMES ? (index.count > 0 ? index.first : 0) : v.resume + 1;
						unsigned char gkey[12];
						put32(gkey, index.key);
						put32(gkey + 4, index.headerEnd);
						put32(gkey + 8, v.sent);
						queue(v, "GKEY", gkey, 12);
						logLine("server: viewer %d starts after frame %d at byte %u of this file", v.number,
							v.resume == ALL_FRAMES ? -1 : (Int)v.resume, v.sent);
					}
					else if (now - v.acceptMs > RESUME_WAIT_MS)
					{
						refuse(v, NOPE_REFUSED, "this source did not reach the frame it asked for");
						continue;
					}
					else if (now - v.lastLiveMs >= LIVE_EVERY_MS)
					{
						queue(v, "HOLD", nullptr, 0);
						v.lastLiveMs = now;
					}
				}
				// what this viewer may have: the newest frame (live), or the frame of delayMs ago (plus a test lag)
				Sample allow;
				Bool haveAllow = FALSE;
				const DWORD age = (DWORD)((v.live ? 0 : c.delayMs) + v.lagMs);
				if (age == 0)
				{
					allow = cur;
					haveAllow = cur.haveLive;
				}
				else
				{
					haveAllow = sampleAt(now, age, &allow) && allow.haveLive;
				}
				UnsignedInt allowFrame = 0;
				Bool frameAllowed = FALSE;
				Bool endedAllowed = FALSE;
				if (v.started && haveAllow && index.count > 0 && allow.live.frame >= index.first)
				{
					allowFrame = min(allow.live.frame, indexLast(index));
					endedAllowed = allow.live.ended != 0 && allowFrame == allow.live.frame;
					frameAllowed = v.resume == ALL_FRAMES || allowFrame > v.resume || (endedAllowed && allowFrame >= v.resume);
				}
				// nothing before its first allowed frame (a delayed viewer gets the header with it); then the commands up to
				// the end of that frame, and once the recording is over the rest of the file
				UnsignedInt allowEnd = v.sent;
				if (frameAllowed)
				{
					allowEnd = max(allowEnd, max(index.headerEnd, indexEnd(index, allowFrame)));
					if (endedAllowed) {
						allowEnd = max(allowEnd, cur.size);
					}
				}
				compact(v);
				if (frameAllowed) {
					queueFrameEnds(v, index, allowFrame, fidx);
				}
				while (v.sent < allowEnd && roomOf(v) >= CHUNK + 16 + 8 + 4 + FIDX_MAX * 8 && tokens >= 1.0)
				{
					DWORD want = min((DWORD)CHUNK, (DWORD)(allowEnd - v.sent));
					want = min(want, (DWORD)tokens);
					DWORD got = 0;
					if (!readAt(file, v.sent, chunk + 4, want, &got) || got == 0) {
						break;
					}
					put32(chunk, v.sent);
					queue(v, "DATA", chunk, (Int)got + 4);
					v.sent += got;
					tokens -= got;
					queueFrameEnds(v, index, allowFrame, fidx);
				}
				// the side file's state, once the viewer has every frame it covers; before its first allowed frame a viewer
				// gets HOLD (it waits, it does not hop)
				if (v.started && v.sent >= allowEnd && (!frameAllowed || v.nextFrame > allowFrame) && roomOf(v) >= 24
					&& now - v.lastLiveMs >= LIVE_EVERY_MS)
				{
					if (frameAllowed)
					{
						unsigned char payload[12];
						put32(payload, allowFrame);
						put32(payload + 4, endedAllowed ? 1 : 0);
						put32(payload + 8, allow.live.counter);
						queue(v, "LIVE", payload, 12);
					}
					else
					{
						queue(v, "HOLD", nullptr, 0);
					}
					v.lastLiveMs = now;
				}
				// send what can go now; never wait
				if (v.outLen > v.outPos)
				{
					const int sent = send(v.socket, (const char *)v.out + v.outPos, v.outLen - v.outPos, 0);
					if (sent > 0)
					{
						v.outPos += sent;
						v.lastDrainMs = now;
					}
					else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
					{
						dropViewer(v, "connection lost");
						continue;
					}
				}
				else
				{
					v.lastDrainMs = now;
				}
				if (now - v.lastDrainMs > DROP_MS)
				{
					dropViewer(v, "too slow: nothing drained for 10 seconds");
					continue;
				}
				// anything from the viewer after its hello is read and dropped; its close ends it
				char sink[64];
				const int extra = recv(v.socket, sink, sizeof(sink), 0);
				if (extra == 0) {
					dropViewer(v, "it closed the connection");
				}
			}
			if (current() && !draining)
			{
				InterlockedExchange(&s_countRoom, roomCount);
				InterlockedExchange(&s_countOutside, outsideCount);
			}
			Sleep(20);
		}

		for (Int i = 0; i < MAX_VIEWERS; ++i) {
			if (viewers[i].socket != INVALID_SOCKET) {
				dropViewer(viewers[i], "the server stops");
			}
		}
		if (listener != INVALID_SOCKET) {
			closesocket(listener);
		}
		if (file != INVALID_HANDLE_VALUE) {
			CloseHandle(file);
		}
		if (current() && !draining)
		{
			InterlockedExchange(&s_countRoom, 0);
			InterlockedExchange(&s_countOutside, 0);
		}
		logLine("server: stopped");
		indexFree(index);
		HeapFree(GetProcessHeap(), 0, viewers);
		HeapFree(GetProcessHeap(), 0, history);
		HeapFree(GetProcessHeap(), 0, cfg);
		return 0;
	}

	//=============================================================================================
	// Mirror (viewer)
	//=============================================================================================
	volatile LONG s_mirrorGen = 0;
	volatile LONG s_mirrorRunning = 0;       ///< the current generation's mirror thread exists
	volatile LONG s_mirrorHeaderReady = 0;
	volatile LONG s_mirrorEnded = 0;
	volatile LONG s_mirrorLost = 0;          ///< every source was gone for NO_SOURCE_END_MS (the game thread tells the watcher once)
	char s_mirrorPath[_MAX_PATH] = { 0 };
	Bool s_mirrorOutside = FALSE;            ///< O61 (row 49): the mirror is an outside viewer's (its files go at the end)

	// O61 (row 49): files still to delete (another handle held them at the first try)
	enum { MAX_PENDING_DELETES = 24, MAX_DELETE_TRIES = 10 };
	char s_pendingDeletes[MAX_PENDING_DELETES][_MAX_PATH];
	Int s_pendingDeleteTries[MAX_PENDING_DELETES];   ///< O64: a file that cannot be deleted is given up after a few tries
	Int s_pendingDeleteCount = 0;

	/// O64 (H6 #1): the folder of the outside viewers' mirrors: only the game writes there (never the replay folder).
	AsciiString viewerFolder()
	{
		AsciiString dir = TheGlobalData != nullptr ? TheGlobalData->getPath_UserData() : AsciiString::TheEmptyString;
		dir.concat("ViewerStreams\\");
		return dir;
	}

	/// O64 (H6 #1): exactly "Viewer_<digits>_<digits>.rep", ".rep.live" or ".rep.idx" (any case); 'pid' the first number.
	Bool isViewerFileName(const char *name, unsigned long *pid)
	{
		if (_strnicmp(name, "Viewer_", 7) != 0) {
			return FALSE;
		}
		const char *p = name + 7;
		unsigned long value = 0;
		Int digits = 0;
		for (; *p >= '0' && *p <= '9' && digits < 10; ++p, ++digits) {
			value = value * 10 + (unsigned long)(*p - '0');
		}
		if (digits == 0 || *p != '_') {
			return FALSE;
		}
		++p;
		digits = 0;
		for (; *p >= '0' && *p <= '9' && digits < 10; ++p, ++digits) {}
		if (digits == 0) {
			return FALSE;
		}
		if (_stricmp(p, ".rep") != 0 && _stricmp(p, ".rep.live") != 0 && _stricmp(p, ".rep.idx") != 0) {
			return FALSE;
		}
		*pid = value;
		return TRUE;
	}
	UnsignedInt s_lastDeleteTry = 0;

	/// Deletes 'path' now, or keeps it for a later try when another handle still holds it.
	void deleteOrKeep(const char *path)
	{
		if (path == nullptr || path[0] == 0) {
			return;
		}
		if (DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
			return;
		}
		for (Int i = 0; i < s_pendingDeleteCount; ++i) {
			if (_stricmp(s_pendingDeletes[i], path) == 0) return;
		}
		if (s_pendingDeleteCount < MAX_PENDING_DELETES)
		{
			s_pendingDeleteTries[s_pendingDeleteCount] = 1;
			strlcpy(s_pendingDeletes[s_pendingDeleteCount++], path, _MAX_PATH);
		}
	}

	struct MirrorConfig
	{
		LONG gen;
		Source sources[MAX_SOURCES];
		Int sourceCount;
		UnsignedInt flags;
		UnsignedInt bind;          ///< host order; 0: the system's choice
		Int relayPort;
		char path[_MAX_PATH];
		char livePath[_MAX_PATH];
		char idxPath[_MAX_PATH];   ///< O42: the mirror's own frame index (it serves it as a relay)
	};

	/// O34: the source to ask next: the lowest rank, then the list order, among those not cooling down; when all cool
	/// down, the one that is free first.
	Int pickSource(const MirrorConfig &m, DWORD now)
	{
		Int best = -1, soonest = 0;
		for (Int i = 0; i < m.sourceCount; ++i)
		{
			const Source &s = m.sources[i];
			if ((LONG)(s.retryAfterMs - now) <= 0 && (best < 0 || s.rank < m.sources[best].rank)) {
				best = i;
			}
			if ((LONG)(s.retryAfterMs - m.sources[soonest].retryAfterMs) < 0) {
				soonest = i;
			}
		}
		return best >= 0 ? best : soonest;
	}

	SOCKET connectTo(const Source &src, UnsignedInt bindHost, DWORD timeoutMs)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) {
			return INVALID_SOCKET;
		}
		if (bindHost != 0)
		{
			// O33: from this game's own LAN address (a server tells room observers by their address)
			sockaddr_in local;
			memset(&local, 0, sizeof(local));
			local.sin_family = AF_INET;
			local.sin_addr.s_addr = htonl(bindHost);
			bind(s, (sockaddr *)&local, sizeof(local));
		}
		setNonBlocking(s);
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(src.port);
		UnsignedInt ip = 0;
		parseIPv4(src.host, &ip);
		addr.sin_addr.s_addr = htonl(ip);
		connect(s, (sockaddr *)&addr, sizeof(addr));
		fd_set w, e;
		FD_ZERO(&w); FD_SET(s, &w);
		FD_ZERO(&e); FD_SET(s, &e);
		timeval tv = { (long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000) };
		if (select(0, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(s, &e))
		{
			closesocket(s);
			return INVALID_SOCKET;
		}
		return s;
	}

	Bool sendAll(SOCKET s, const unsigned char *data, Int length)
	{
		Int done = 0;
		const DWORD start = LanClock::now();
		while (done < length)
		{
			const int n = send(s, (const char *)data + done, length - done, 0);
			if (n > 0) {
				done += n;
			} else if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && LanClock::now() - start < 2000) {
				Sleep(5);
			} else {
				return FALSE;
			}
		}
		return TRUE;
	}

	void writeLocalLive(HANDLE liveFile, UnsignedInt frame, UnsignedInt ended, UnsignedInt counter)
	{
		LiveRecord r;
		memcpy(r.magic, "GLIV", 4);
		r.frame = frame;
		r.ended = ended;
		r.counter = counter;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		DWORD written = 0;
		WriteFile(liveFile, &r, sizeof(r), &written, &ov);
	}

	/// The sources in the order of their connect time (unreachable ones last); stops early when the mirror is stopped.
	void orderSourcesByConnectTime(MirrorConfig &m)
	{
		if (m.sourceCount < 2) {
			return;
		}
		DWORD cost[MAX_SOURCES];
		for (Int i = 0; i < m.sourceCount; ++i)
		{
			if (InterlockedCompareExchange(&s_mirrorGen, 0, 0) != m.gen) {
				return;
			}
			const DWORD t0 = LanClock::now();
			SOCKET s = connectTo(m.sources[i], m.bind, 500);
			cost[i] = s == INVALID_SOCKET ? 0xFFFFFFFFu : LanClock::now() - t0;
			if (s != INVALID_SOCKET) {
				closesocket(s);
			}
		}
		for (Int i = 1; i < m.sourceCount; ++i)
		{
			for (Int k = i; k > 0 && (m.sources[k].rank < m.sources[k - 1].rank
				|| (m.sources[k].rank == m.sources[k - 1].rank && cost[k] < cost[k - 1])); --k)
			{
				const Source tmp = m.sources[k]; m.sources[k] = m.sources[k - 1]; m.sources[k - 1] = tmp;
				const DWORD cc = cost[k]; cost[k] = cost[k - 1]; cost[k - 1] = cc;
			}
		}
		for (Int i = 0; i < m.sourceCount; ++i)
		{
			if (cost[i] == 0xFFFFFFFFu) {
				logLine("mirror: source %d %s:%u, rank %d (no answer to the probe)", i, m.sources[i].host, (unsigned)m.sources[i].port, m.sources[i].rank);
			} else {
				logLine("mirror: source %d %s:%u, rank %d (connect %lu ms)", i, m.sources[i].host, (unsigned)m.sources[i].port, m.sources[i].rank, cost[i]);
			}
		}
	}

	Bool addSource(Source *list, Int *count, UnsignedInt ipHost, UnsignedInt port, Int rank)
	{
		char host[64];
		snprintf(host, sizeof(host), "%u.%u.%u.%u", ipHost >> 24, (ipHost >> 16) & 255, (ipHost >> 8) & 255, ipHost & 255);
		for (Int i = 0; i < *count; ++i) {
			if (strcmp(list[i].host, host) == 0 && list[i].port == port) return FALSE;
		}
		if (*count >= MAX_SOURCES || port == 0 || port > 65535 || ipHost == 0) {
			return FALSE;
		}
		strlcpy(list[*count].host, host, sizeof(list[*count].host));
		list[*count].port = (unsigned short)port;
		list[*count].rank = rank;
		list[*count].retryAfterMs = 0;
		list[*count].redirectedMs = 0;
		++*count;
		return TRUE;
	}

	DWORD WINAPI mirrorMain(LPVOID param)
	{
		MirrorConfig *cfg = (MirrorConfig *)param;
		MirrorConfig &m = *cfg;
		const auto current = [&]() { return InterlockedCompareExchange(&s_mirrorGen, 0, 0) == m.gen; };
		const auto setFlag = [&](volatile LONG *flag) { if (current()) InterlockedExchange(flag, 1); };
		orderSourcesByConnectTime(m);
		HANDLE file = openShared(m.path, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		HANDLE liveFile = openShared(m.livePath, GENERIC_WRITE, CREATE_ALWAYS);
		HANDLE idxFile = openShared(m.idxPath, GENERIC_WRITE, CREATE_ALWAYS);
		unsigned char *in = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, IN_BUFFER);
		UnsignedInt pendCap = PEND_START;
		unsigned char *pend = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, pendCap);
		if (file == INVALID_HANDLE_VALUE || liveFile == INVALID_HANDLE_VALUE || idxFile == INVALID_HANDLE_VALUE || in == nullptr || pend == nullptr)
		{
			logLine("mirror: cannot create %s", m.path);
			if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
			if (liveFile != INVALID_HANDLE_VALUE) CloseHandle(liveFile);
			if (idxFile != INVALID_HANDLE_VALUE) CloseHandle(idxFile);
			if (in != nullptr) HeapFree(GetProcessHeap(), 0, in);
			if (pend != nullptr) HeapFree(GetProcessHeap(), 0, pend);
			HeapFree(GetProcessHeap(), 0, cfg);
			return 0;
		}
		// O42: the mirror file only ever holds the header and whole frames: a source's bytes wait in 'pend' until the source
		// names the end of a frame (FIDX); then they are written, with the frame's end in the mirror's own index. So after a
		// lost source the mirror asks the next one "after frame <lastFrame>": every player's file has the same commands per
		// frame (only the headers differ), and the new source finds the place in its own file.
		UnsignedInt have = 0;              // bytes written to the mirror file
		Bool haveHeader = FALSE;
		UnsignedInt gameKey = 0;           // 0: no source told it yet
		Bool haveFrame = FALSE;
		UnsignedInt lastFrame = 0;         // the newest frame written whole
		UnsignedInt idxPos = 12;
		UnsignedInt liveCounter = 0;
		Bool ended = FALSE;
		DWORD lastContactMs = LanClock::now();   // the last useful message (GKEY, DATA, FIDX, LIVE or HOLD)
		Bool everContact = FALSE;
		UnsignedInt maxFrameSeen = 0;      // the newest frame a source named (the log)
		const auto setConnected = [&](Bool on, UnsignedInt ip) {
			if (current())
			{
				InterlockedExchange(&s_mirrorConnected, on ? 1 : 0);
				InterlockedExchange(&s_mirrorSourceIp, on ? (LONG)ip : 0);
			}
		};
		const auto writeLive = [&](UnsignedInt endedFlag) {
			writeLocalLive(liveFile, haveFrame ? lastFrame : 0, endedFlag, ++liveCounter);
		};
		const auto writeAt = [&](HANDLE h, UnsignedInt offset, const unsigned char *data, UnsignedInt length) -> Bool {
			OVERLAPPED ov;
			memset(&ov, 0, sizeof(ov));
			ov.Offset = offset;
			DWORD written = 0;
			return length == 0 || (WriteFile(h, data, length, &written, &ov) && written == length);
		};
		const auto loseStream = [&](const char *why) {
			writeLive(1);
			ended = TRUE;
			setFlag(&s_mirrorEnded);
			setFlag(&s_mirrorLost);
			logLine("mirror: %s: the stream is lost; the replay plays its %u bytes (frame %u) to the end", why, have, lastFrame);
		};

		while (current() && !ended && m.sourceCount > 0)
		{
			// O34: no data for too long (a reserve bridges the first seconds; "reconnecting" shows meanwhile)
			if (LanClock::now() - lastContactMs > (everContact ? (DWORD)NO_SOURCE_END_MS : (DWORD)FIRST_CONTACT_MS))
			{
				loseStream("no source has given data");
				break;
			}
			const Int source = pickSource(m, LanClock::now());
			while (current() && (LONG)(m.sources[source].retryAfterMs - LanClock::now()) > 0) {
				Sleep(100);   // every source cools down: wait for the first free one
			}
			if (!current()) {
				break;
			}
			Source &src = m.sources[source];
			SOCKET s = connectTo(src, m.bind, 2000);
			if (s == INVALID_SOCKET)
			{
				logLine("mirror: no answer from %s:%u", src.host, (unsigned)src.port);
				src.retryAfterMs = LanClock::now() + COOLDOWN_MS;
				continue;
			}
			// the hello (version 3): after which frame, and the game key we know
			unsigned char hello[HELLO_SIZE];
			const UnsignedInt resume = haveFrame ? lastFrame : ALL_FRAMES;
			// O34: back at a source that sent us on not long ago: the relays did not take us
			const Bool lastResort = src.redirectedMs != 0 && LanClock::now() - src.redirectedMs < REDIRECT_RECENT_MS;
			memcpy(hello, "GRSV", 4);
			put32(hello + 4, HELLO_VERSION);
			put32(hello + 8, resume);
			put32(hello + 12, gameKey);
			put32(hello + 16, 0);
			put32(hello + 20, m.flags | (lastResort ? HELLO_LAST_RESORT : 0));
			put32(hello + 24, (UnsignedInt)m.relayPort);
			if (!sendAll(s, hello, HELLO_SIZE))
			{
				closesocket(s);
				src.retryAfterMs = LanClock::now() + COOLDOWN_MS;
				continue;
			}
			UnsignedInt srcIp = 0;
			parseIPv4(src.host, &srcIp);
			logLine("mirror: connected to %s:%u (rank %d%s), asking after frame %d (game key %08X)", src.host, (unsigned)src.port, src.rank,
				lastResort ? ", last resort" : "", resume == ALL_FRAMES ? -1 : (Int)resume, gameKey);

			// this connection: 'pend' holds the source's bytes from srcPos on that are not written yet
			Bool started = FALSE;
			UnsignedInt srcPos = 0, pendLen = 0, srcHeaderEnd = 0;
			Bool skipSourceHeader = FALSE;
			const auto consume = [&](UnsignedInt k) {
				memmove(pend, pend + k, pendLen - k);
				pendLen -= k;
				srcPos += k;
			};
			Int inLen = 0;
			DWORD lastReportMs = LanClock::now();
			DWORD lastMessageMs = LanClock::now();
			DWORD lastFrameMoveMs = lastMessageMs;
			UnsignedInt lastSourceFrame = 0xFFFFFFFF;
			Bool holding = FALSE;
			Bool sentOn = FALSE;
			const char *leaveWhy = nullptr;
			while (current() && leaveWhy == nullptr && !ended)
			{
				fd_set r;
				FD_ZERO(&r); FD_SET(s, &r);
				timeval tv = { 0, 200 * 1000 };
				if (select(0, &r, nullptr, nullptr, &tv) > 0)
				{
					// O33: the room left in the buffer is checked before the read
					const Int room = IN_BUFFER - inLen;
					if (room <= 0) { leaveWhy = "a message larger than the buffer"; break; }
					const int got = recv(s, (char *)in + inLen, room, 0);
					if (got == 0) { leaveWhy = "the source closed the connection"; break; }
					if (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK) { leaveWhy = "the connection broke"; break; }
					if (got > 0) inLen += got;
				}
				// whole messages
				Int at = 0;
				while (inLen - at >= 8 && leaveWhy == nullptr && !ended)
				{
					const UnsignedInt length = get32(in + at + 4);
					if (length > CHUNK + 4) { leaveWhy = "a broken message"; break; }
					if (inLen - at < 8 + (Int)length) break;
					const unsigned char *type = in + at;
					const unsigned char *payload = in + at + 8;
					lastMessageMs = LanClock::now();
					const Bool useful = memcmp(type, "DATA", 4) == 0 || memcmp(type, "LIVE", 4) == 0 || memcmp(type, "HOLD", 4) == 0
						|| memcmp(type, "FIDX", 4) == 0 || memcmp(type, "GKEY", 4) == 0;
					if (useful)
					{
						lastContactMs = lastMessageMs;
						everContact = TRUE;
						setConnected(TRUE, srcIp);
					}
					if (memcmp(type, "GKEY", 4) == 0 && length >= 12)
					{
						// O42: the game, the end of the source's header, and where its data starts (the end of our frame there)
						const UnsignedInt key = get32(payload);
						const UnsignedInt start = get32(payload + 8);
						if (gameKey != 0 && key != gameKey) { leaveWhy = "another game (game key)"; break; }
						if (resume == ALL_FRAMES && start != 0) { leaveWhy = "a start that is not the file's start"; break; }
						gameKey = key;
						srcHeaderEnd = get32(payload + 4);
						srcPos = start;
						pendLen = 0;
						started = TRUE;
						// a source asked from the start after we wrote our header (no frame yet): its own header is dropped
						skipSourceHeader = haveHeader && start == 0;
						logLine("mirror: %s:%u has the game (key %08X); its data starts at its byte %u", src.host, (unsigned)src.port, key, start);
					}
					else if (memcmp(type, "DATA", 4) == 0 && length >= 4)
					{
						if (!started) { leaveWhy = "data before the start"; break; }
						const UnsignedInt offset = get32(payload);
						const UnsignedInt bytes = length - 4;
						const UnsignedInt expect = srcPos + pendLen;
						if (offset > expect) { leaveWhy = "a gap in the data"; break; }
						const UnsignedInt skip = expect - offset;   // bytes we have already
						if (skip < bytes)
						{
							const UnsignedInt add = bytes - skip;
							if (pendLen + add > pendCap)
							{
								UnsignedInt cap = pendCap;
								while (cap < pendLen + add && cap < PEND_MAX) cap *= 2;
								unsigned char *grown = cap >= pendLen + add ? (unsigned char *)HeapReAlloc(GetProcessHeap(), 0, pend, cap) : nullptr;
								if (grown == nullptr) { leaveWhy = "too much data without a frame end"; break; }
								pend = grown;
								pendCap = cap;
							}
							memcpy(pend + pendLen, payload + 4 + skip, add);
							pendLen += add;
						}
						// the header (only from the start of the file)
						if (srcPos == 0 && srcHeaderEnd > 0 && pendLen >= srcHeaderEnd)
						{
							if (!haveHeader)
							{
								unsigned char head[12];
								memcpy(head, "GIDX", 4);
								put32(head + 4, gameKey);
								put32(head + 8, srcHeaderEnd);
								if (!writeAt(file, 0, pend, srcHeaderEnd) || !writeAt(idxFile, 0, head, 12)) {
									leaveWhy = "the mirror file cannot be written";
									break;
								}
								have = srcHeaderEnd;
								haveHeader = TRUE;
							}
							skipSourceHeader = FALSE;
							consume(srcHeaderEnd);
						}
					}
					else if (memcmp(type, "FIDX", 4) == 0 && length >= 4)
					{
						// O42: frame ends in the source's offsets: write the bytes of every whole frame
						const UnsignedInt n = get32(payload);
						if (!started || !haveHeader || skipSourceHeader || n > (length - 4) / 8) { leaveWhy = "a broken frame index"; break; }
						for (UnsignedInt k = 0; k < n && leaveWhy == nullptr; ++k)
						{
							const UnsignedInt frame = get32(payload + 4 + k * 8);
							const UnsignedInt srcEnd = get32(payload + 8 + k * 8);
							if (haveFrame && frame <= lastFrame) {
								continue;
							}
							if (srcEnd < srcPos || srcEnd - srcPos > pendLen) { leaveWhy = "a frame end outside the data"; break; }
							const UnsignedInt k2 = srcEnd - srcPos;
							unsigned char entry[8];
							put32(entry, frame);
							put32(entry + 4, have + k2);
							if (!writeAt(file, have, pend, k2) || !writeAt(idxFile, idxPos, entry, 8)) {
								leaveWhy = "the mirror file cannot be written";
								break;
							}
							have += k2;
							idxPos += 8;
							consume(k2);
							lastFrame = frame;
							haveFrame = TRUE;
						}
						if (leaveWhy == nullptr && haveFrame)
						{
							writeLive(0);
							setFlag(&s_mirrorHeaderReady);   // the header and a first whole frame
						}
					}
					else if (memcmp(type, "LIVE", 4) == 0 && length == 12)
					{
						holding = FALSE;
						const UnsignedInt frame = get32(payload);
						const UnsignedInt endedFlag = get32(payload + 4);
						maxFrameSeen = max(maxFrameSeen, frame);
						if (frame != lastSourceFrame)
						{
							lastSourceFrame = frame;
							lastFrameMoveMs = LanClock::now();
						}
						// the source sends LIVE only after the frame ends it covers: "ended" for our newest frame is the end
						if (endedFlag != 0 && haveFrame && frame <= lastFrame)
						{
							// the rest of the source's file (its last records after the last frame end)
							if (pendLen > 0 && writeAt(file, have, pend, pendLen))
							{
								have += pendLen;
								consume(pendLen);
							}
							writeLive(1);
							ended = TRUE;
							setFlag(&s_mirrorHeaderReady);
							setFlag(&s_mirrorEnded);
							logLine("mirror: the source's game ended at frame %u; %u bytes", lastFrame, have);
						}
						else
						{
							writeLive(0);   // the counter moves: the follower sees a live writer
						}
					}
					else if (memcmp(type, "HOLD", 4) == 0)
					{
						holding = TRUE;   // a delayed viewer, or a source still short of our frame: it waits
						lastFrameMoveMs = LanClock::now();
					}
					else if (memcmp(type, "MORE", 4) == 0 && length >= 4)
					{
						// O34: relays (12 bytes each: address, port, kind 0 outside viewer / 1 room observer), from any source;
						// never this game itself
						const UnsignedInt n = get32(payload);
						Int added = 0;
						for (UnsignedInt k = 0; k < n && k < MAX_REDIRECTS && 4 + (k + 1) * 12 <= length; ++k)
						{
							const UnsignedInt ip = get32(payload + 4 + k * 12);
							const UnsignedInt port = get32(payload + 8 + k * 12);
							if (ip == m.bind && (Int)port == m.relayPort) {
								continue;
							}
							// a room observer stays with live data: an outside viewer's relay only has the delayed stream
							if ((m.flags & HELLO_ROOM) != 0 && get32(payload + 12 + k * 12) != 1) {
								continue;
							}
							added += addSource(m.sources, &m.sourceCount, ip, port, get32(payload + 12 + k * 12) == 1 ? 1 : 0) ? 1 : 0;
						}
						logLine("mirror: %s:%u named %u relay(s), %d new", src.host, (unsigned)src.port, n, added);
					}
					else if (memcmp(type, "NOPE", 4) == 0)
					{
						const UnsignedInt reason = length >= 4 ? get32(payload) : NOPE_REFUSED;
						if (reason == NOPE_PLAYER)
						{
							// O64: every source refuses a player of the match: no use asking the others
							setFlag(&s_mirrorIsPlayer);
							loseStream("this computer is a player of the match");
							leaveWhy = "a player of the match";
						}
						else if (reason == NOPE_NOT_ALLOWED)
						{
							// O42: the host's rule; the room's screen says so instead of "the stream did not come"
							setFlag(&s_mirrorNotAllowed);
							loseStream("the host does not allow outside viewers");
							leaveWhy = "outside viewers are not allowed";
						}
						else if (reason == NOPE_ELSEWHERE)
						{
							sentOn = TRUE;
							leaveWhy = "the source sent us on (full, relays first, or it reads from us)";
						}
						else
						{
							leaveWhy = reason == NOPE_OTHER_GAME ? "another game (or another build)" : "the source refused (see its log)";
						}
					}
					at += 8 + (Int)length;
				}
				if (at > 0)
				{
					memmove(in, in + at, inLen - at);
					inLen -= at;
				}
				const DWORD now = LanClock::now();
				if (now - lastReportMs >= 10000)
				{
					lastReportMs = now;
					logLine("mirror: have %u bytes, frame %u (source names %u) from %s:%u%s", have, lastFrame, maxFrameSeen, src.host,
						(unsigned)src.port, holding ? " (held)" : "");
				}
				if (leaveWhy == nullptr && !ended && now - lastMessageMs > SILENT_HOP_MS) {
					leaveWhy = "the source is silent";
				}
				if (leaveWhy == nullptr && !ended && !holding && lastSourceFrame != 0xFFFFFFFF && now - lastFrameMoveMs > STUCK_HOP_MS) {
					leaveWhy = "the source's newest frame does not move";
				}
			}
			closesocket(s);
			setConnected(FALSE, 0);
			if (leaveWhy != nullptr)
			{
				const DWORD now = LanClock::now();
				if (sentOn)
				{
					src.redirectedMs = now;   // it may be asked again at once, as the last resort, once the relays failed
					src.retryAfterMs = now + 500;
				}
				else
				{
					src.retryAfterMs = now + COOLDOWN_MS;
				}
				logLine("mirror: leaving %s:%u (%s) after frame %u at byte %u (%u bytes of a frame not finished dropped)", src.host,
					(unsigned)src.port, leaveWhy, lastFrame, have, pendLen);
			}
		}
		setConnected(FALSE, 0);
		CloseHandle(file);
		CloseHandle(liveFile);
		CloseHandle(idxFile);
		HeapFree(GetProcessHeap(), 0, in);
		HeapFree(GetProcessHeap(), 0, pend);
		logLine("mirror: stopped with %u bytes, frame %u", have, lastFrame);
		if (current()) {
			InterlockedExchange(&s_mirrorRunning, 0);
		}
		HeapFree(GetProcessHeap(), 0, cfg);
		return 0;
	}

	Bool addWatchSource(UnsignedInt ipHost, UnsignedInt port, Int rank)
	{
		return addSource(s_sources, &s_sourceCount, ipHost, port, rank);
	}
}

//-------------------------------------------------------------------------------------------------
void ReplayStream::configureServe(Int port)
{
	s_cmdPort = (port > 0 && port < 65536) ? port : 0;
}

void ReplayStream::configureServeMax(Int viewers)
{
	s_serveMax = viewers < 1 ? 1 : (viewers > MAX_VIEWERS ? MAX_VIEWERS : viewers);
}

void ReplayStream::configureServeRate(Int kilobytesPerSecond)
{
	s_serveRateKB = kilobytesPerSecond < 1 ? 1 : kilobytesPerSecond;
}

void ReplayStream::configureServeStopAfter(Int seconds)
{
	s_serveStopAfterSec = seconds > 0 ? seconds : 0;
}

void ReplayStream::configureServeOutsideMax(Int viewers)
{
	s_serveOutsideMax = viewers < 0 ? 0 : (viewers > MAX_VIEWERS ? MAX_VIEWERS : viewers);
}

void ReplayStream::configureServeLagFor(const char *spec)
{
	// O42 (test): "a.b.c.d:seconds": that viewer address is served as if its link were that many seconds behind
	s_serveLagIp = 0;
	s_serveLagMs = 0;
	if (spec == nullptr) {
		return;
	}
	char ip[32];
	const char *colon = strrchr(spec, ':');
	const size_t len = colon != nullptr ? (size_t)(colon - spec) : 0;
	if (colon == nullptr || len == 0 || len >= sizeof(ip)) {
		return;
	}
	memcpy(ip, spec, len);
	ip[len] = '\0';
	UnsignedInt v = 0;
	const Int seconds = atoi(colon + 1);
	if (parseIPv4(ip, &v) && seconds > 0)
	{
		s_serveLagIp = v;
		s_serveLagMs = (seconds > MAX_DELAY_S ? MAX_DELAY_S : seconds) * 1000;
	}
}

void ReplayStream::configureServeDelay(Int seconds)
{
	s_cmdDelayS = seconds < 0 ? 0 : (seconds > MAX_DELAY_S ? MAX_DELAY_S : seconds);
	s_cmdDelaySet = TRUE;
}

void ReplayStream::configureServeLiveFor(const char *ips)
{
	s_cmdLiveCount = parseIpList(ips, s_cmdLive, MAX_LIVE);
}

void ReplayStream::configureWatch(const char *sources)
{
	s_sourceCount = 0;
	const char *p = sources;
	while (p != nullptr && *p != '\0' && s_sourceCount < MAX_SOURCES)
	{
		const char *end = strchr(p, ',');
		const size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
		char item[96];
		const size_t n = min(len, sizeof(item) - 1);
		memcpy(item, p, n);
		item[n] = '\0';
		char *colon = strrchr(item, ':');
		if (colon != nullptr)
		{
			*colon = '\0';
			UnsignedInt ip = 0;
			const Int port = atoi(colon + 1);
			if (parseIPv4(item, &ip)) {
				addWatchSource(ip, (UnsignedInt)port, 2);
			}
		}
		p = end != nullptr ? end + 1 : nullptr;
	}
}

void ReplayStream::configureWatchRoomObserver()
{
	s_watchFlags |= 1;
}

void ReplayStream::configureWatchBind(const char *ip)
{
	UnsignedInt v = 0;
	s_watchBind = (ip != nullptr && parseIPv4(ip, &v)) ? v : 0;
}

void ReplayStream::configureMatchServing(UnsignedInt bindIpHostOrder, Int port, Int delaySeconds, Bool outsideAllowed,
	const UnsignedInt *liveIps, Int liveCount, Bool receivesDelayed, const UnsignedInt *refusedIps, Int refusedCount)
{
	s_matchRefusedCount = 0;
	for (Int i = 0; refusedIps != nullptr && i < refusedCount && s_matchRefusedCount < MAX_LIVE; ++i) {
		if (refusedIps[i] != 0) s_matchRefused[s_matchRefusedCount++] = refusedIps[i];
	}
	s_matchSet = port > 0 && port < 65536;
	s_matchBind = bindIpHostOrder;
	s_matchPort = port;
	s_matchDelayS = delaySeconds < 10 ? 10 : (delaySeconds > MAX_DELAY_S ? MAX_DELAY_S : delaySeconds);
	s_matchOutside = outsideAllowed;
	s_matchReceivesDelayed = receivesDelayed;
	s_matchLiveCount = 0;
	for (Int i = 0; i < liveCount && s_matchLiveCount < MAX_LIVE; ++i) {
		if (liveIps[i] != 0) s_matchLive[s_matchLiveCount++] = liveIps[i];
	}
}

void ReplayStream::stopServing()
{
	// the running server closes its listener and ends by itself once its viewers have the rest (no wait here)
	InterlockedExchange(&s_drainGen, InterlockedCompareExchange(&s_serverGen, 0, 0));
	s_matchSet = FALSE;
	s_matchLiveCount = 0;
	s_matchRefusedCount = 0;   // O62
	InterlockedExchange(&s_countRoom, 0);
	InterlockedExchange(&s_countOutside, 0);
}

void ReplayStream::servingCounts(Int *roomObservers, Int *outsideViewers)
{
	*roomObservers = (Int)InterlockedCompareExchange(&s_countRoom, 0, 0);
	*outsideViewers = (Int)InterlockedCompareExchange(&s_countOutside, 0, 0);
}

void ReplayStream::configureWatchList(const UnsignedInt *ipsHostOrder, Int count, Int port, Bool roomObserver, UnsignedInt localIp,
	const UnsignedInt *observerIps, Int observerCount)
{
	s_sourceCount = 0;
	for (Int i = 0; i < count && s_sourceCount < MAX_SOURCES; ++i) {
		addWatchSource(ipsHostOrder[i], (UnsignedInt)port, 2);
	}
	// O34: an outside viewer asks the room's observers (their relays) before the players
	for (Int i = 0; observerIps != nullptr && i < observerCount && s_sourceCount < MAX_SOURCES; ++i) {
		if (observerIps[i] != localIp) addWatchSource(observerIps[i], (UnsignedInt)port, 1);
	}
	s_watchFlags = roomObserver ? 1 : 0;
	s_watchBind = localIp;
}

Bool ReplayStream::isWatching()
{
	return s_sourceCount > 0;
}

Bool ReplayStream::beginWatching(AsciiString &localPath)
{
	if (s_sourceCount == 0 || TheGlobalData == nullptr || !startWinsock()) {
		return FALSE;
	}
	if (InterlockedCompareExchange(&s_mirrorRunning, 0, 0) == 0)
	{
		openLog();
		MirrorConfig *m = (MirrorConfig *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MirrorConfig));
		if (m == nullptr) {
			return FALSE;
		}
		m->gen = InterlockedIncrement(&s_mirrorGen);
		memcpy(m->sources, s_sources, sizeof(s_sources));
		m->sourceCount = s_sourceCount;
		m->flags = s_watchFlags;
		m->bind = s_watchBind;
		m->relayPort = s_matchSet ? s_matchPort : s_cmdPort;
		// O33: a file of its own per mirror (an old mirror may still close its file). O61 (row 49): an outside viewer's is
		// "Viewer_...", deleted when the viewing ends (discardViewerFiles) or at the next start after a crash; O64 (H6 #1):
		// in a folder of its own (ViewerStreams), never in the replay folder
		s_mirrorOutside = (s_watchFlags & 1) == 0;
		AsciiString dir = TheGlobalData->getPath_UserData();
		dir.concat(s_mirrorOutside ? "ViewerStreams\\" : "Replays\\");
		CreateDirectoryA(dir.str(), nullptr);
		snprintf(m->path, sizeof(m->path), "%s%s_%u_%ld.rep", dir.str(), s_mirrorOutside ? "Viewer" : "Watched",
			(unsigned)GetCurrentProcessId(), (long)m->gen);
		snprintf(m->livePath, sizeof(m->livePath), "%s.live", m->path);
		snprintf(m->idxPath, sizeof(m->idxPath), "%s.idx", m->path);
		DeleteFileA(m->path);
		DeleteFileA(m->livePath);
		DeleteFileA(m->idxPath);
		InterlockedExchange(&s_mirrorNotAllowed, 0);
		InterlockedExchange(&s_mirrorIsPlayer, 0);   // O64
		strlcpy(s_mirrorPath, m->path, sizeof(s_mirrorPath));
		InterlockedExchange(&s_mirrorHeaderReady, 0);
		InterlockedExchange(&s_mirrorEnded, 0);
		InterlockedExchange(&s_mirrorLost, 0);
		logLine("mirror: watching %d source(s) into %s (flags %u)", s_sourceCount, m->path, s_watchFlags);
		InterlockedExchange(&s_mirrorRunning, 1);
		if (!startThread(mirrorMain, m))
		{
			InterlockedExchange(&s_mirrorRunning, 0);
			HeapFree(GetProcessHeap(), 0, m);
			return FALSE;
		}
	}
	localPath = s_mirrorPath;
	return TRUE;
}

Bool ReplayStream::mirrorTrying()
{
	return InterlockedCompareExchange(&s_mirrorRunning, 0, 0) != 0 && InterlockedCompareExchange(&s_mirrorEnded, 0, 0) == 0;
}

Bool ReplayStream::mirrorReconnecting()
{
	return mirrorTrying() && InterlockedCompareExchange(&s_mirrorConnected, 0, 0) == 0;
}

Bool ReplayStream::headerReady()
{
	return InterlockedCompareExchange(&s_mirrorHeaderReady, 0, 0) != 0;
}

void ReplayStream::stopWatching()
{
	InterlockedIncrement(&s_mirrorGen);   // the mirror ends by itself within a fraction of a second (no wait)
	InterlockedExchange(&s_mirrorRunning, 0);
	InterlockedExchange(&s_mirrorHeaderReady, 0);
	s_sourceCount = 0;
	s_watchFlags = 0;
	s_watchBind = 0;
}

Bool ReplayStream::startWatching(AsciiString &localPath)
{
	if (!beginWatching(localPath)) {
		return FALSE;
	}
	// the command line viewer's own start waits for the replay's header (its game is not running yet); a LAN viewer comes
	// here only once the header is there
	const DWORD start = LanClock::now();
	while (InterlockedCompareExchange(&s_mirrorHeaderReady, 0, 0) == 0 && InterlockedCompareExchange(&s_mirrorLost, 0, 0) == 0
		&& LanClock::now() - start < HEADER_WAIT_MS) {
		Sleep(50);
	}
	if (InterlockedCompareExchange(&s_mirrorHeaderReady, 0, 0) == 0)
	{
		logLine("mirror: no source gave the replay's header in %d seconds", HEADER_WAIT_MS / 1000);
		return FALSE;
	}
	localPath = s_mirrorPath;
	// a relay: serve the mirror to further viewers
	serveFile(localPath);
	return TRUE;
}

void ReplayStream::serveFile(const AsciiString &replayPath)
{
	// a LAN match's settings, or the command line's -serveReplay; nothing else serves (O33, review 4)
	const Int port = s_matchSet ? s_matchPort : s_cmdPort;
	if (port == 0 || replayPath.isEmpty() || !startWinsock()) {
		return;
	}
	openLog();
	ServeConfig *c = (ServeConfig *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ServeConfig));
	if (c == nullptr) {
		return;
	}
	c->gen = InterlockedIncrement(&s_serverGen);   // a running server ends by itself
	c->port = port;
	c->max = s_serveMax;
	c->outsideMax = s_serveOutsideMax;
	c->rateKB = s_serveRateKB;
	c->stopAfterSec = s_serveStopAfterSec;
	c->lagIp = s_serveLagIp;
	c->lagMs = s_serveLagIp != 0 ? s_serveLagMs : 0;
	if (s_matchSet)
	{
		c->bind = s_matchBind;
		c->delayMs = s_matchReceivesDelayed ? 0 : s_matchDelayS * 1000;   // a delayed copy is forwarded as it comes
		c->preferRelays = !s_matchReceivesDelayed;   // O34: players and room observers send outside viewers to relays first
		c->outsideAllowed = s_matchOutside;
		c->liveCount = s_matchLiveCount;
		memcpy(c->live, s_matchLive, sizeof(UnsignedInt) * s_matchLiveCount);
		c->refusedCount = s_matchRefusedCount;   // O62 (H5 #3)
		memcpy(c->refused, s_matchRefused, sizeof(UnsignedInt) * s_matchRefusedCount);
		// test runs (NetLab has no lobby, so no START): -serveReplayLiveFor adds live addresses, -serveReplayDelay sets
		// the delay
		for (Int i = 0; i < s_cmdLiveCount && c->liveCount < MAX_LIVE; ++i) {
			c->live[c->liveCount++] = s_cmdLive[i];
		}
		if (s_cmdDelaySet) {
			c->delayMs = s_cmdDelayS * 1000;
		}
	}
	else
	{
		c->bind = s_watchBind;   // O34: a test relay serves on the address it watches from (0: 127.0.0.1)
		c->delayMs = s_cmdDelayS * 1000;
		c->outsideAllowed = TRUE;
		// O34: a source with live data (a player, or a -watchRoomObserver viewer) sends outside viewers to relays first; an
		// outside viewer's relay serves what it has
		c->preferRelays = s_sourceCount == 0 || (s_watchFlags & HELLO_ROOM) != 0;
		c->liveCount = s_cmdLiveCount;
		memcpy(c->live, s_cmdLive, sizeof(UnsignedInt) * s_cmdLiveCount);
		if (s_cmdDelayS == 0 && s_cmdLiveCount == 0)
		{
			// the plain test server of before: everybody live, no relays first
			c->liveCount = 0;
			c->delayMs = 0;
			c->preferRelays = FALSE;
		}
	}
	strlcpy(c->path, replayPath.str(), sizeof(c->path));
	snprintf(c->livePath, sizeof(c->livePath), "%s.live", c->path);
	snprintf(c->idxPath, sizeof(c->idxPath), "%s.idx", c->path);
	if (!startThread(serverMain, c)) {
		HeapFree(GetProcessHeap(), 0, c);
	}
}

Bool ReplayStream::takeLostNotice()
{
	return InterlockedExchange(&s_mirrorLost, 0) != 0;
}

Bool ReplayStream::lostBecauseNotAllowed()
{
	return InterlockedCompareExchange(&s_mirrorNotAllowed, 0, 0) != 0;
}

Bool ReplayStream::lostBecausePlayer()
{
	return InterlockedCompareExchange(&s_mirrorIsPlayer, 0, 0) != 0;
}

void ReplayStream::discardViewerFiles()
{
	if (!s_mirrorOutside || s_mirrorPath[0] == 0) {
		return;
	}
	char live[_MAX_PATH];
	char idx[_MAX_PATH];
	snprintf(live, sizeof(live), "%s.live", s_mirrorPath);
	snprintf(idx, sizeof(idx), "%s.idx", s_mirrorPath);
	deleteOrKeep(s_mirrorPath);
	deleteOrKeep(live);
	deleteOrKeep(idx);
	logLine("mirror: the viewer's files of %s deleted (%d still held, deleted later)", s_mirrorPath, s_pendingDeleteCount);
	s_mirrorPath[0] = 0;
	s_mirrorOutside = FALSE;
}

void ReplayStream::pumpDeletes()
{
	if (s_pendingDeleteCount == 0) {
		return;
	}
	const UnsignedInt now = LanClock::now();
	if (now - s_lastDeleteTry < 1000) {
		return;
	}
	s_lastDeleteTry = now;
	Int kept = 0;
	for (Int i = 0; i < s_pendingDeleteCount; ++i)
	{
		const Bool gone = DeleteFileA(s_pendingDeletes[i]) || GetLastError() == ERROR_FILE_NOT_FOUND
			|| GetLastError() == ERROR_PATH_NOT_FOUND;
		if (gone) {
			continue;
		}
		// O64: a file that cannot be deleted (a read-only folder) is given up after a few tries, said once
		if (++s_pendingDeleteTries[i] > MAX_DELETE_TRIES)
		{
			logLine("mirror: %s could not be deleted (error %lu): given up", s_pendingDeletes[i], (unsigned long)GetLastError());
			continue;
		}
		if (kept != i)
		{
			strlcpy(s_pendingDeletes[kept], s_pendingDeletes[i], _MAX_PATH);
			s_pendingDeleteTries[kept] = s_pendingDeleteTries[i];
		}
		++kept;
	}
	s_pendingDeleteCount = kept;
}

void ReplayStream::sweepViewerLeftovers()
{
	if (TheGlobalData == nullptr) {
		return;
	}
	// O64 (H6 #1): the viewers' own folder only, never the replay folder
	const AsciiString dir = viewerFolder();
	AsciiString pattern = dir;
	pattern.concat("Viewer_*");
	WIN32_FIND_DATAA found;
	HANDLE h = FindFirstFileA(pattern.str(), &found);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	Int deleted = 0;
	do
	{
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			continue;
		}
		// O64 (H6 #1): exactly "Viewer_<pid>_<n>.rep[.live|.idx]" with a pid; any other name is left alone
		unsigned long pid = 0;
		if (!isViewerFileName(found.cFileName, &pid) || pid == 0) {
			continue;
		}
		Bool running = pid == GetCurrentProcessId();
		if (!running)
		{
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
			if (process != nullptr)
			{
				DWORD code = 0;
				running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
				CloseHandle(process);
			}
			else
			{
				running = GetLastError() == ERROR_ACCESS_DENIED;   // a process we may not look at: leave its files
			}
		}
		if (!running)
		{
			AsciiString path = dir;
			path.concat(found.cFileName);
			deleteOrKeep(path.str());
			++deleted;
		}
	} while (FindNextFileA(h, &found));
	FindClose(h);
	if (deleted > 0) {
		logLine("mirror: %d file(s) of an earlier viewer deleted at the start", deleted);
	}
}

void ReplayStream::shutdown()
{
	InterlockedIncrement(&s_serverGen);
	InterlockedIncrement(&s_mirrorGen);
	Sleep(300);   // at the end of the program only: the threads see their generation and close their sockets
	// O61 (row 49): an outside viewer's files go with the program (a file the replay still holds: at the next start)
	discardViewerFiles();
	for (Int i = 0; i < s_pendingDeleteCount; ++i) {
		DeleteFileA(s_pendingDeletes[i]);
	}
	s_pendingDeleteCount = 0;
	if (s_logLockReady)
	{
		EnterCriticalSection(&s_logLock);
		if (s_log != nullptr)
		{
			fclose(s_log);
			s_log = nullptr;
		}
		LeaveCriticalSection(&s_logLock);
	}
	if (s_wsaReady)
	{
		WSACleanup();
		s_wsaReady = FALSE;
	}
}
