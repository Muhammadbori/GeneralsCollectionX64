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
// The two threads (server, mirror) use only fixed buffers, the process heap and Win32 calls: no game objects, no game
// memory pools. What the game thread shares with them is set before they start or is a volatile flag.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <winsock.h>

#include "Common/ReplayStream.h"

#include "Common/GlobalData.h"

#include <stdio.h>
#include <stdarg.h>

namespace
{
	enum
	{
		MAX_VIEWERS = 100,
		MAX_SOURCES = 16,             ///< O31: 8 players and up to 8 relays a source pointed to
		OUT_BUFFER = 256 * 1024,      ///< per viewer
		CHUNK = 32 * 1024,            ///< the most file bytes in one DATA message
		IN_BUFFER = 2 * (CHUNK + 16),
		ID_BYTES = 1024,
		HELLO_SIZE = 20,
		HELLO2_SIZE = 28,             ///< O31: + flags, relay port
		MAX_REDIRECTS = 8,            ///< O31: relays named in one MORE
		DROP_MS = 10000,              ///< a viewer whose data does not drain for this long is dropped
		LIVE_EVERY_MS = 500,
		SILENT_HOP_MS = 5000,         ///< a source silent for this long is left
		STUCK_HOP_MS = 8000,          ///< a source whose newest frame does not move for this long is left
		HEADER_WAIT_MS = 60000,
		NO_SOURCE_END_MS = 30000,     ///< no source has answered for this long: the stream is lost, the replay plays to its end
		FIRST_CONTACT_MS = 180000,    ///< O31: before any source answered (the players still load the map): wait this long
	};

	struct LiveRecord
	{
		char magic[4];
		UnsignedInt frame;
		UnsignedInt ended;
		UnsignedInt counter;
	};

	//--- settings (set on the game thread before a thread starts) ---------------------------------
	Int s_servePort = 0;
	Int s_serveMax = 16;
	Int s_serveRateKB = 4096;
	Int s_serveStopAfterSec = 0;
	u_long s_serveBindIP = 0;          ///< O31: network order; 0: 127.0.0.1
	Int s_serveOutsideMax = 4;         ///< O31: outside viewers served directly; more are sent to relays
	UnsignedInt s_watchFlags = 0;      ///< O31: bit 0 room observer
	struct Source { char host[64]; unsigned short port; };
	Source s_sources[MAX_SOURCES];
	Int s_sourceCount = 0;

	//--- log ----------------------------------------------------------------------------------------
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
			s_startMs = timeGetTime();
		}
		if (s_log == nullptr && TheGlobalData != nullptr)
		{
			char path[_MAX_PATH];
			snprintf(path, sizeof(path), "%sReplayStream.log", TheGlobalData->getPath_UserData().str());
			s_log = fopen(path, "w");
		}
	}

	void logLine(const char *format, ...)
	{
		if (!s_logLockReady || s_log == nullptr) {
			return;
		}
		EnterCriticalSection(&s_logLock);
		fprintf(s_log, "[%8.3f] ", (timeGetTime() - s_startMs) / 1000.0);
		va_list args;
		va_start(args, format);
		vfprintf(s_log, format, args);
		va_end(args);
		fputc('\n', s_log);
		fflush(s_log);
		LeaveCriticalSection(&s_logLock);
	}

	//--- helpers ------------------------------------------------------------------------------------
	UnsignedInt fnv(const unsigned char *data, Int length)
	{
		UnsignedInt h = 2166136261u;
		for (Int i = 0; i < length; ++i)
		{
			h ^= data[i];
			h *= 16777619u;
		}
		return h;
	}

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

	/// The thread runs below normal priority and prefers the last core, so the scheduler keeps it off the game thread's
	/// core when another is free (the game thread is not pinned; it keeps the system's choice).
	void startThread(HANDLE *thread, LPTHREAD_START_ROUTINE main)
	{
		*thread = CreateThread(nullptr, 0, main, nullptr, 0, nullptr);
		if (*thread != nullptr)
		{
			SetThreadPriority(*thread, THREAD_PRIORITY_BELOW_NORMAL);
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			if (si.dwNumberOfProcessors > 1) {
				SetThreadIdealProcessor(*thread, si.dwNumberOfProcessors - 1);
			}
		}
	}

	void stopThread(HANDLE *thread, volatile LONG *stop)
	{
		if (*thread == nullptr) {
			return;
		}
		InterlockedExchange(stop, 1);
		WaitForSingleObject(*thread, 5000);
		CloseHandle(*thread);
		*thread = nullptr;
		InterlockedExchange(stop, 0);
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

	/// "a.b.c.d" to an address in network order; FALSE when it is not one.
	Bool parseIPv4(const char *text, u_long *out)
	{
		unsigned int part[4];
		char tail = 0;
		if (sscanf(text, "%u.%u.%u.%u%c", &part[0], &part[1], &part[2], &part[3], &tail) != 4) {
			return FALSE;
		}
		for (Int i = 0; i < 4; ++i) {
			if (part[i] > 255) return FALSE;
		}
		*out = htonl((part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3]);
		return TRUE;
	}

	void setNonBlocking(SOCKET s)
	{
		u_long on = 1;
		ioctlsocket(s, FIONBIO, &on);
	}

	//=============================================================================================
	// Server
	//=============================================================================================
	struct Viewer
	{
		SOCKET socket;
		unsigned char hello[HELLO_SIZE];
		Int helloLen;
		Bool ready;
		UnsignedInt sent;          ///< file bytes queued for this viewer so far
		unsigned char *out;
		Int outLen, outPos;
		DWORD lastDrainMs;
		DWORD lastLiveMs;
		LiveRecord liveSent;
		Int number;
		Int helloWant;             ///< O31: 20, or 28 for a version 2 hello
		Bool roomObserver;         ///< O31
		u_long relayIP;            ///< O31: network order; the viewer's address
		unsigned short relayPort;  ///< O31: 0: it does not relay
	};

	HANDLE s_serverThread = nullptr;
	volatile LONG s_serverStop = 0;
	char s_servePath[_MAX_PATH] = { 0 };
	char s_serveLivePath[_MAX_PATH] = { 0 };
	Viewer s_viewers[MAX_VIEWERS];

	void dropViewer(Viewer &v, const char *why)
	{
		logLine("server: viewer %d dropped (%s), it had %u bytes", v.number, why, v.sent);
		closesocket(v.socket);
		v.socket = INVALID_SOCKET;
		if (v.out != nullptr) {
			HeapFree(GetProcessHeap(), 0, v.out);
			v.out = nullptr;
		}
	}

	void queue(Viewer &v, const char *type, const unsigned char *payload, Int length)
	{
		unsigned char *p = v.out + v.outLen;
		memcpy(p, type, 4);
		put32(p + 4, (UnsignedInt)length);
		if (length > 0) {
			memcpy(p + 8, payload, length);
		}
		v.outLen += 8 + length;
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

	DWORD WINAPI serverMain(LPVOID)
	{
		SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener == INVALID_SOCKET) {
			logLine("server: no socket (%d)", WSAGetLastError());
			return 0;
		}
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short)s_servePort);
		// 127.0.0.1 unless the LAN code gave this game's LAN address (O31); never every interface
		addr.sin_addr.s_addr = s_serveBindIP != 0 ? s_serveBindIP : htonl(INADDR_LOOPBACK);
		const UnsignedInt bindHost = ntohl(addr.sin_addr.s_addr);
		if (bind(listener, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 8) != 0)
		{
			logLine("server: cannot listen on %u.%u.%u.%u:%d (%d)", bindHost >> 24, (bindHost >> 16) & 255, (bindHost >> 8) & 255,
				bindHost & 255, s_servePort, WSAGetLastError());
			closesocket(listener);
			return 0;
		}
		setNonBlocking(listener);
		logLine("server: listening on %u.%u.%u.%u:%d for %s (at most %d viewers, %d outside ones, %d KB/s)", bindHost >> 24,
			(bindHost >> 16) & 255, (bindHost >> 8) & 255, bindHost & 255, s_servePort, s_servePath, s_serveMax, s_serveOutsideMax,
			s_serveRateKB);

		for (Int i = 0; i < MAX_VIEWERS; ++i) {
			s_viewers[i].socket = INVALID_SOCKET;
			s_viewers[i].out = nullptr;
		}
		HANDLE file = INVALID_HANDLE_VALUE;
		const DWORD startMs = timeGetTime();
		DWORD lastTickMs = startMs;
		double tokens = 0.0;
		const double rate = (double)s_serveRateKB * 1024.0;
		Int nextNumber = 1;
		unsigned char chunk[CHUNK + 4];

		while (InterlockedCompareExchange(&s_serverStop, 0, 0) == 0)
		{
			const DWORD now = timeGetTime();
			if (s_serveStopAfterSec > 0 && now - startMs > (DWORD)s_serveStopAfterSec * 1000)
			{
				logLine("server: stopping after %d seconds (-serveReplayStopAfter, a forced source loss)", s_serveStopAfterSec);
				break;
			}
			tokens = min(rate, tokens + rate * (now - lastTickMs) / 1000.0);
			lastTickMs = now;

			// new viewers
			for (;;)
			{
				SOCKET s = accept(listener, nullptr, nullptr);
				if (s == INVALID_SOCKET) {
					break;
				}
				Int slot = -1, used = 0;
				for (Int i = 0; i < MAX_VIEWERS; ++i)
				{
					if (s_viewers[i].socket != INVALID_SOCKET) ++used;
					else if (slot < 0) slot = i;
				}
				// O31: room observers and outside viewers are told apart by their hello; here only the table's room
				if (slot < 0)
				{
					unsigned char nope[8];
					memcpy(nope, "NOPE", 4);
					put32(nope + 4, 0);
					send(s, (const char *)nope, 8, 0);
					closesocket(s);
					logLine("server: a viewer refused (full: %d)", used);
					continue;
				}
				setNonBlocking(s);
				Viewer &v = s_viewers[slot];
				memset(&v, 0, sizeof(v));
				v.socket = s;
				v.out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, OUT_BUFFER);
				v.lastDrainMs = now;
				v.number = nextNumber++;
				v.helloWant = HELLO_SIZE;
				if (v.out == nullptr) {
					dropViewer(v, "no memory");
				} else {
					logLine("server: viewer %d connected", v.number);
				}
			}

			if (file == INVALID_HANDLE_VALUE) {
				file = openShared(s_servePath, GENERIC_READ, OPEN_EXISTING);
			}
			// the side file first, then the size: the bytes of the side file's frame are all in the file
			LiveRecord live;
			const Bool haveLive = readLive(s_serveLivePath, &live);
			const UnsignedInt size = (file != INVALID_HANDLE_VALUE) ? fileSize(file) : 0;

			for (Int i = 0; i < MAX_VIEWERS; ++i)
			{
				Viewer &v = s_viewers[i];
				if (v.socket == INVALID_SOCKET) {
					continue;
				}
				// the hello
				if (!v.ready)
				{
					const int got = recv(v.socket, (char *)v.hello + v.helloLen, v.helloWant - v.helloLen, 0);
					if (got == 0 || (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
						dropViewer(v, "closed before its hello");
						continue;
					}
					if (got > 0) {
						v.helloLen += got;
					}
					// O31: a version 2 hello has 8 more bytes
					if (v.helloLen >= 8 && memcmp(v.hello, "GRSV", 4) == 0 && get32(v.hello + 4) == 2) {
						v.helloWant = HELLO2_SIZE;
					}
					if (v.helloLen < v.helloWant)
					{
						if (now - v.lastDrainMs > DROP_MS) {
							dropViewer(v, "no hello");
						}
						continue;
					}
					const UnsignedInt offset = get32(v.hello + 8);
					const UnsignedInt idLength = get32(v.hello + 12);
					const UnsignedInt idHash = get32(v.hello + 16);
					const UnsignedInt version = get32(v.hello + 4);
					Bool same = memcmp(v.hello, "GRSV", 4) == 0 && (version == 1 || version == 2) && idLength <= ID_BYTES;
					if (same && idLength > 0)
					{
						unsigned char id[ID_BYTES];
						DWORD got2 = 0;
						same = file != INVALID_HANDLE_VALUE && readAt(file, 0, id, idLength, &got2) && got2 == idLength && fnv(id, idLength) == idHash;
					}
					if (!same)
					{
						queue(v, "NOPE", nullptr, 0);
						send(v.socket, (const char *)v.out, v.outLen, 0);
						dropViewer(v, "another game, or this source has less of it");
						continue;
					}
					// O31: who it is (a version 1 hello counts as a room observer: served as before)
					v.roomObserver = version != 2 || (get32(v.hello + 20) & 1) != 0;
					v.relayPort = version == 2 ? (unsigned short)get32(v.hello + 24) : 0;
					{
						sockaddr_in peer;
						int peerLen = sizeof(peer);
						v.relayIP = getpeername(v.socket, (sockaddr *)&peer, &peerLen) == 0 ? peer.sin_addr.s_addr : 0;
					}
					Int roomCount = 0, outsideCount = 0;
					for (Int k = 0; k < MAX_VIEWERS; ++k)
					{
						if (k != i && s_viewers[k].socket != INVALID_SOCKET && s_viewers[k].ready) {
							++(s_viewers[k].roomObserver ? roomCount : outsideCount);
						}
					}
					const Bool full = roomCount + outsideCount >= s_serveMax || (!v.roomObserver && outsideCount >= s_serveOutsideMax);
					if (full)
					{
						// MORE: the viewers that relay, then NOPE; the viewer goes to one of them
						unsigned char more[4 + MAX_REDIRECTS * 8];
						Int n = 0;
						for (Int k = 0; k < MAX_VIEWERS && n < MAX_REDIRECTS; ++k)
						{
							const Viewer &o = s_viewers[k];
							if (k != i && o.socket != INVALID_SOCKET && o.ready && o.relayPort != 0 && o.relayIP != 0)
							{
								put32(more + 4 + n * 8, (UnsignedInt)ntohl(o.relayIP));
								put32(more + 8 + n * 8, o.relayPort);
								++n;
							}
						}
						put32(more, (UnsignedInt)n);
						queue(v, "MORE", more, 4 + n * 8);
						queue(v, "NOPE", nullptr, 0);
						send(v.socket, (const char *)v.out, v.outLen, 0);
						logLine("server: viewer %d sent on (%s full; %d relays named)", v.number, v.roomObserver ? "room" : "outside", n);
						dropViewer(v, "full");
						continue;
					}
					v.ready = TRUE;
					v.sent = offset;
					logLine("server: viewer %d (%s%s) starts at byte %u", v.number, v.roomObserver ? "room observer" : "outside viewer",
						v.relayPort != 0 ? ", relays" : "", offset);
				}
				// file bytes, as the rate and the viewer's buffer allow
				compact(v);
				while (v.sent < size && roomOf(v) >= CHUNK + 16 && tokens >= 1.0)
				{
					DWORD want = min((DWORD)CHUNK, (DWORD)(size - v.sent));
					want = min(want, (DWORD)tokens);
					DWORD got = 0;
					if (!readAt(file, v.sent, chunk + 4, want, &got) || got == 0) {
						break;
					}
					put32(chunk, v.sent);
					queue(v, "DATA", chunk, (Int)got + 4);
					v.sent += got;
					tokens -= got;
				}
				// the live state, once the viewer has the bytes it covers
				if (haveLive && v.sent >= size && roomOf(v) >= 24)
				{
					const Bool changed = memcmp(&live, &v.liveSent, sizeof(live)) != 0;
					if (changed || now - v.lastLiveMs >= LIVE_EVERY_MS)
					{
						unsigned char payload[12];
						put32(payload, live.frame);
						put32(payload + 4, live.ended);
						put32(payload + 8, live.counter);
						queue(v, "LIVE", payload, 12);
						v.liveSent = live;
						v.lastLiveMs = now;
					}
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
				// anything from the viewer after its hello, or its close
				char sink[64];
				const int extra = recv(v.socket, sink, sizeof(sink), 0);
				if (extra == 0) {
					dropViewer(v, "it closed the connection");
				}
			}
			Sleep(20);
		}

		for (Int i = 0; i < MAX_VIEWERS; ++i) {
			if (s_viewers[i].socket != INVALID_SOCKET) {
				dropViewer(s_viewers[i], "the server stops");
			}
		}
		closesocket(listener);
		if (file != INVALID_HANDLE_VALUE) {
			CloseHandle(file);
		}
		logLine("server: stopped");
		return 0;
	}

	//=============================================================================================
	// Mirror (viewer)
	//=============================================================================================
	HANDLE s_mirrorThread = nullptr;
	volatile LONG s_mirrorStop = 0;
	volatile LONG s_mirrorHeaderReady = 0;
	volatile LONG s_mirrorEnded = 0;
	volatile LONG s_mirrorLost = 0;          ///< every source was gone for NO_SOURCE_END_MS (the game thread tells the watcher once)
	char s_mirrorPath[_MAX_PATH] = { 0 };
	char s_mirrorLivePath[_MAX_PATH] = { 0 };

	SOCKET connectTo(const Source &src, DWORD timeoutMs)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) {
			return INVALID_SOCKET;
		}
		setNonBlocking(s);
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(src.port);
		u_long ip = 0;
		parseIPv4(src.host, &ip);
		addr.sin_addr.s_addr = ip;
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
		const DWORD start = timeGetTime();
		while (done < length)
		{
			const int n = send(s, (const char *)data + done, length - done, 0);
			if (n > 0) {
				done += n;
			} else if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && timeGetTime() - start < 2000) {
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

	/// O31: the sources in the order of their connect time (unreachable ones last): the viewer starts at the player it
	/// reaches best (decision 6 of row 21).
	void orderSourcesByConnectTime()
	{
		if (s_sourceCount < 2) {
			return;
		}
		DWORD cost[MAX_SOURCES];
		for (Int i = 0; i < s_sourceCount; ++i)
		{
			const DWORD t0 = timeGetTime();
			SOCKET s = connectTo(s_sources[i], 500);
			cost[i] = s == INVALID_SOCKET ? 0xFFFFFFFFu : timeGetTime() - t0;
			if (s != INVALID_SOCKET) {
				closesocket(s);
			}
		}
		for (Int i = 1; i < s_sourceCount; ++i)
		{
			for (Int k = i; k > 0 && cost[k] < cost[k - 1]; --k)
			{
				const Source tmp = s_sources[k]; s_sources[k] = s_sources[k - 1]; s_sources[k - 1] = tmp;
				const DWORD c = cost[k]; cost[k] = cost[k - 1]; cost[k - 1] = c;
			}
		}
		for (Int i = 0; i < s_sourceCount; ++i)
		{
			if (cost[i] == 0xFFFFFFFFu) {
				logLine("mirror: source %d %s:%u (no answer to the probe)", i, s_sources[i].host, (unsigned)s_sources[i].port);
			} else {
				logLine("mirror: source %d %s:%u (connect %lu ms)", i, s_sources[i].host, (unsigned)s_sources[i].port, cost[i]);
			}
		}
	}

	/// O31: a relay a source named; FALSE when it is known already or there is no room.
	Bool addSource(UnsignedInt ipHost, UnsignedInt port)
	{
		char host[64];
		snprintf(host, sizeof(host), "%u.%u.%u.%u", ipHost >> 24, (ipHost >> 16) & 255, (ipHost >> 8) & 255, ipHost & 255);
		for (Int i = 0; i < s_sourceCount; ++i) {
			if (strcmp(s_sources[i].host, host) == 0 && s_sources[i].port == port) return FALSE;
		}
		if (s_sourceCount >= MAX_SOURCES || port == 0 || port > 65535) {
			return FALSE;
		}
		strlcpy(s_sources[s_sourceCount].host, host, sizeof(s_sources[s_sourceCount].host));
		s_sources[s_sourceCount].port = (unsigned short)port;
		++s_sourceCount;
		return TRUE;
	}

	DWORD WINAPI mirrorMain(LPVOID)
	{
		orderSourcesByConnectTime();
		HANDLE file = openShared(s_mirrorPath, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
		HANDLE liveFile = openShared(s_mirrorLivePath, GENERIC_WRITE, CREATE_ALWAYS);
		if (file == INVALID_HANDLE_VALUE || liveFile == INVALID_HANDLE_VALUE)
		{
			logLine("mirror: cannot create %s", s_mirrorPath);
			if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
			if (liveFile != INVALID_HANDLE_VALUE) CloseHandle(liveFile);
			return 0;
		}
		UnsignedInt have = fileSize(file);
		static unsigned char in[IN_BUFFER];
		Int source = 0;
		Int failures = 0;
		Bool ended = FALSE;
		DWORD lastContactMs = timeGetTime();   // the last message from any source
		Bool everContact = FALSE;              // O31: a LAN observer starts before the players' servers exist
		UnsignedInt lastFrameSeen = 0, lastCounterSeen = 0;

		while (InterlockedCompareExchange(&s_mirrorStop, 0, 0) == 0 && !ended && s_sourceCount > 0)
		{
			const Source &src = s_sources[source];
			SOCKET s = connectTo(src, 2000);
			if (s == INVALID_SOCKET)
			{
				logLine("mirror: no answer from %s:%u", src.host, (unsigned)src.port);
				source = (source + 1) % s_sourceCount;
				// TheSuperHackers @bugfix (S16) Every source gone for long enough: the stream is lost. The side file says
				// "ended", so the replay plays what it has to its end instead of waiting, and the watcher is told.
				if (timeGetTime() - lastContactMs > (everContact ? (DWORD)NO_SOURCE_END_MS : (DWORD)FIRST_CONTACT_MS))
				{
					writeLocalLive(liveFile, lastFrameSeen, 1, lastCounterSeen + 1);
					ended = TRUE;
					InterlockedExchange(&s_mirrorEnded, 1);
					InterlockedExchange(&s_mirrorLost, 1);
					logLine("mirror: no source has answered for %d seconds: the stream is lost; the replay plays its %u bytes "
						"(frame %u) to the end", NO_SOURCE_END_MS / 1000, have, lastFrameSeen);
					break;
				}
				if (++failures >= s_sourceCount) {
					Sleep(1000);   // every source tried: wait a moment before the next round
					failures = 0;
				}
				continue;
			}
			// the hello: where we are, and the first bytes we have (the same game)
			unsigned char hello[HELLO2_SIZE];
			unsigned char id[ID_BYTES];
			DWORD idLength = 0;
			if (have > 0) {
				readAt(file, 0, id, min((DWORD)ID_BYTES, (DWORD)have), &idLength);
			}
			memcpy(hello, "GRSV", 4);
			put32(hello + 4, 2);   // O31: version 2: + flags, relay port
			put32(hello + 8, have);
			put32(hello + 12, idLength);
			put32(hello + 16, idLength > 0 ? fnv(id, (Int)idLength) : 0);
			put32(hello + 20, s_watchFlags);
			put32(hello + 24, (UnsignedInt)s_servePort);
			if (!sendAll(s, hello, HELLO2_SIZE))
			{
				closesocket(s);
				source = (source + 1) % s_sourceCount;
				continue;
			}
			logLine("mirror: connected to %s:%u, asking from byte %u", src.host, (unsigned)src.port, have);
			failures = 0;

			Int inLen = 0;
			DWORD lastMessageMs = timeGetTime();
			DWORD lastFrameMoveMs = lastMessageMs;
			UnsignedInt lastFrame = 0xFFFFFFFF;
			const char *leaveWhy = nullptr;
			while (InterlockedCompareExchange(&s_mirrorStop, 0, 0) == 0 && leaveWhy == nullptr && !ended)
			{
				fd_set r;
				FD_ZERO(&r); FD_SET(s, &r);
				timeval tv = { 0, 200 * 1000 };
				if (select(0, &r, nullptr, nullptr, &tv) > 0)
				{
					const int got = recv(s, (char *)in + inLen, IN_BUFFER - inLen, 0);
					if (got == 0) { leaveWhy = "the source closed the connection"; break; }
					if (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK) { leaveWhy = "the connection broke"; break; }
					if (got > 0) inLen += got;
				}
				// whole messages
				Int at = 0;
				while (inLen - at >= 8 && leaveWhy == nullptr)
				{
					const UnsignedInt length = get32(in + at + 4);
					if (length > CHUNK + 4) { leaveWhy = "a broken message"; break; }
					if (inLen - at < 8 + (Int)length) break;
					const unsigned char *type = in + at;
					const unsigned char *payload = in + at + 8;
					lastMessageMs = timeGetTime();
					lastContactMs = lastMessageMs;
					everContact = TRUE;
					if (memcmp(type, "DATA", 4) == 0 && length >= 4)
					{
						const UnsignedInt offset = get32(payload);
						const UnsignedInt bytes = length - 4;
						if (offset > have) { leaveWhy = "a gap in the data"; break; }
						const UnsignedInt skip = have - offset;   // bytes we have already
						if (skip < bytes)
						{
							OVERLAPPED ov;
							memset(&ov, 0, sizeof(ov));
							ov.Offset = have;
							DWORD written = 0;
							if (!WriteFile(file, payload + 4 + skip, bytes - skip, &written, &ov) || written != bytes - skip) {
								leaveWhy = "the mirror file cannot be written";
								break;
							}
							have += written;
						}
					}
					else if (memcmp(type, "LIVE", 4) == 0 && length == 12)
					{
						const UnsignedInt frame = get32(payload);
						const UnsignedInt endedFlag = get32(payload + 4);
						const UnsignedInt counter = get32(payload + 8);
						writeLocalLive(liveFile, frame, endedFlag, counter);
						lastFrameSeen = frame;
						lastCounterSeen = counter;
						if (frame != lastFrame)
						{
							lastFrame = frame;
							lastFrameMoveMs = timeGetTime();
						}
						// the header is complete once the source wrote its first frame (the counter moved past the start)
						if (counter >= 2 || endedFlag != 0) {
							InterlockedExchange(&s_mirrorHeaderReady, 1);
						}
						if (endedFlag != 0)
						{
							ended = TRUE;
							InterlockedExchange(&s_mirrorEnded, 1);
							logLine("mirror: the source's game ended at frame %u; %u bytes", frame, have);
						}
					}
					else if (memcmp(type, "MORE", 4) == 0 && length >= 4)
					{
						// O31: this source is full: the viewers that relay it
						const UnsignedInt n = get32(payload);
						Int firstNew = -1;
						for (UnsignedInt k = 0; k < n && 4 + (k + 1) * 8 <= length; ++k)
						{
							if (addSource(get32(payload + 4 + k * 8), get32(payload + 8 + k * 8)) && firstNew < 0) {
								firstNew = s_sourceCount - 1;
							}
						}
						logLine("mirror: %s:%u is full and named %u relay(s)", src.host, (unsigned)src.port, n);
						if (firstNew >= 0) {
							source = (firstNew + s_sourceCount - 1) % s_sourceCount;   // the hop below moves to firstNew
						}
					}
					else if (memcmp(type, "NOPE", 4) == 0)
					{
						leaveWhy = "the source refused (another game, behind us, or full)";
					}
					at += 8 + (Int)length;
				}
				if (at > 0)
				{
					memmove(in, in + at, inLen - at);
					inLen -= at;
				}
				const DWORD now = timeGetTime();
				if (leaveWhy == nullptr && now - lastMessageMs > SILENT_HOP_MS) {
					leaveWhy = "the source is silent";
				}
				if (leaveWhy == nullptr && lastFrame != 0xFFFFFFFF && now - lastFrameMoveMs > STUCK_HOP_MS) {
					leaveWhy = "the source's newest frame does not move";
				}
			}
			closesocket(s);
			if (leaveWhy != nullptr)
			{
				const Int next = (source + 1) % s_sourceCount;
				logLine("mirror: leaving %s:%u (%s) at byte %u, frame %u; next %s:%u", src.host, (unsigned)src.port, leaveWhy, have,
					lastFrame, s_sources[next].host, (unsigned)s_sources[next].port);
				source = next;
			}
		}
		CloseHandle(file);
		CloseHandle(liveFile);
		logLine("mirror: stopped with %u bytes", have);
		return 0;
	}
}

//-------------------------------------------------------------------------------------------------
void ReplayStream::configureServe(Int port)
{
	s_servePort = (port > 0 && port < 65536) ? port : 0;
}

void ReplayStream::configureServeMax(Int viewers)
{
	s_serveMax = viewers < 1 ? 1 : (viewers > MAX_VIEWERS ? MAX_VIEWERS : viewers);
}

void ReplayStream::configureServeRate(Int kilobytesPerSecond)
{
	s_serveRateKB = kilobytesPerSecond < 16 ? 16 : kilobytesPerSecond;
}

void ReplayStream::configureServeStopAfter(Int seconds)
{
	s_serveStopAfterSec = seconds > 0 ? seconds : 0;
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
			Source &src = s_sources[s_sourceCount];
			strlcpy(src.host, item, sizeof(src.host));
			src.port = (unsigned short)atoi(colon + 1);
			u_long ip = 0;
			if (src.port != 0 && parseIPv4(src.host, &ip)) {
				++s_sourceCount;
			}
		}
		p = end != nullptr ? end + 1 : nullptr;
	}
}

void ReplayStream::configureServeAddress(UnsignedInt ipHostOrder)
{
	s_serveBindIP = ipHostOrder != 0 ? htonl(ipHostOrder) : 0;
}

void ReplayStream::configureServeDefault(Int port)
{
	if (s_servePort == 0 && port > 0 && port < 65536) {
		s_servePort = port;
	}
}

void ReplayStream::configureWatchRoomObserver()
{
	s_watchFlags |= 1;
}

void ReplayStream::configureServeOutsideMax(Int viewers)
{
	s_serveOutsideMax = viewers < 0 ? 0 : (viewers > MAX_VIEWERS ? MAX_VIEWERS : viewers);
}

void ReplayStream::configureWatchList(const UnsignedInt *ipsHostOrder, Int count, Int port, Bool roomObserver)
{
	s_sourceCount = 0;
	for (Int i = 0; i < count && s_sourceCount < MAX_SOURCES; ++i) {
		addSource(ipsHostOrder[i], (UnsignedInt)port);
	}
	s_watchFlags = roomObserver ? 1 : 0;
}

Bool ReplayStream::beginWatching(AsciiString &localPath)
{
	if (s_sourceCount == 0 || TheGlobalData == nullptr || !startWinsock()) {
		return FALSE;
	}
	if (s_mirrorThread == nullptr)
	{
		openLog();
		AsciiString dir = TheGlobalData->getPath_UserData();
		dir.concat("Replays\\");
		CreateDirectoryA(dir.str(), nullptr);
		snprintf(s_mirrorPath, sizeof(s_mirrorPath), "%sWatched_%u.rep", dir.str(), (unsigned)GetCurrentProcessId());
		snprintf(s_mirrorLivePath, sizeof(s_mirrorLivePath), "%s.live", s_mirrorPath);
		DeleteFileA(s_mirrorPath);
		DeleteFileA(s_mirrorLivePath);
		InterlockedExchange(&s_mirrorHeaderReady, 0);
		InterlockedExchange(&s_mirrorEnded, 0);
		InterlockedExchange(&s_mirrorLost, 0);
		logLine("mirror: watching %d source(s) into %s (flags %u)", s_sourceCount, s_mirrorPath, s_watchFlags);
		startThread(&s_mirrorThread, mirrorMain);
	}
	localPath = s_mirrorPath;
	return s_mirrorThread != nullptr;
}

Bool ReplayStream::headerReady()
{
	return InterlockedCompareExchange(&s_mirrorHeaderReady, 0, 0) != 0;
}

void ReplayStream::stopWatching()
{
	stopThread(&s_mirrorThread, &s_mirrorStop);
	s_sourceCount = 0;
	s_watchFlags = 0;
	InterlockedExchange(&s_mirrorHeaderReady, 0);
}

Bool ReplayStream::isWatching()
{
	return s_sourceCount > 0;
}

Bool ReplayStream::startWatching(AsciiString &localPath)
{
	if (s_sourceCount == 0 || TheGlobalData == nullptr || !startWinsock()) {
		return FALSE;
	}
	// O31: a LAN viewer started the mirror already (beginWatching); the command line viewer starts it here
	if (!beginWatching(localPath)) {
		return FALSE;
	}
	// the viewer's own start waits for the replay's header (its game is not running yet)
	const DWORD start = timeGetTime();
	while (InterlockedCompareExchange(&s_mirrorHeaderReady, 0, 0) == 0 && InterlockedCompareExchange(&s_mirrorLost, 0, 0) == 0
		&& timeGetTime() - start < HEADER_WAIT_MS) {
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
	if (s_servePort == 0 || replayPath.isEmpty() || !startWinsock()) {
		return;
	}
	openLog();
	stopThread(&s_serverThread, &s_serverStop);
	strlcpy(s_servePath, replayPath.str(), sizeof(s_servePath));
	snprintf(s_serveLivePath, sizeof(s_serveLivePath), "%s.live", s_servePath);
	startThread(&s_serverThread, serverMain);
}

Bool ReplayStream::takeLostNotice()
{
	return InterlockedExchange(&s_mirrorLost, 0) != 0;
}

void ReplayStream::shutdown()
{
	stopThread(&s_serverThread, &s_serverStop);
	stopThread(&s_mirrorThread, &s_mirrorStop);
	if (s_log != nullptr)
	{
		EnterCriticalSection(&s_logLock);
		fclose(s_log);
		s_log = nullptr;
		LeaveCriticalSection(&s_logLock);
	}
	if (s_wsaReady)
	{
		WSACleanup();
		s_wsaReady = FALSE;
	}
}
