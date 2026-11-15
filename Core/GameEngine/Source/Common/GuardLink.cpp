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

// GuardLink.cpp ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O30 (row 15) See GuardLink.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GuardLink.h"

#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"

#include <map>
#include <vector>

// O32: random bytes for the pipe name (advapi32's RtlGenRandom; no new library for the game)
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);

namespace
{
	const UnsignedInt kMagic = 0x314C4747;   // "GGL1"
	enum { PIPE_HELLO = 1, PIPE_STATUS = 2, PIPE_CHECK = 3, PIPE_SEND = 4, PIPE_RECV = 5, PIPE_VERDICT = 6 };
	enum { kProtocol = 1, kMaxPayload = 512, kMaxRelay = 200, PENDING_BIT = 0x80 };
	const UnsignedInt kConnectGiveUpMs = 30000;
	const UnsignedInt kSeenWindowMs = 20000;     ///< a player is checked while a message came from it within this time
	const UnsignedInt kForgetMs = 120000;
	const UnsignedInt kRecheckMs = 30000;        ///< a verified or failed player is asked again
	const UnsignedInt kRetryMs = 10000;          ///< a player without an answer is asked again
	const UnsignedInt kSilentMs = 10000;         ///< O32: a guard that sends nothing for this long is gone (it sends every 2 s)
	const UnsignedInt kVerdictLifeMs = 45000;    ///< O32: a verdict not renewed for this long no longer counts
	const size_t kMaxQueuedFrames = 64;          ///< O32: frames waiting for the pipe; more are dropped

#pragma pack(push, 1)
	struct Header
	{
		UnsignedInt magic;
		UnsignedShort type;
		UnsignedShort length;
	};
#pragma pack(pop)

	struct Peer
	{
		UnsignedInt ownIp = 0;
		UnsignedInt lastSeen = 0;
		UnsignedInt lastCheck = 0;
		Bool asked = FALSE;
		Int misses = 0;             ///< answers missed in a row
		UnsignedInt verdictMs = 0;  ///< O32: when the verdict was given
		GuardLink::Verdict verdict = GuardLink::VERDICT_NONE;
		UnsignedInt bits = 0;
	};

	Bool s_noGuard = FALSE;
	Bool s_testMode = FALSE;
	Bool s_started = FALSE;
	Bool s_connected = FALSE;
	Bool s_dead = FALSE;
	HANDLE s_job = nullptr;
	HANDLE s_process = nullptr;
	HANDLE s_pipe = INVALID_HANDLE_VALUE;
	DWORD s_childPid = 0;                 ///< O32: the guard process this game started (the pipe's server must be it)
	UnsignedInt s_lastFrameMs = 0;        ///< O32: the last frame from the guard
	UnsignedInt s_lastUpdateMs = 0;       ///< O32: the last update (a minimized game does not update: no silence counted)
	// O32 (review 3): overlapped pipe I/O, polled each update; the game never waits on the guard
	OVERLAPPED s_readOv;
	UnsignedByte s_readBuf[2048];
	Bool s_reading = FALSE;
	OVERLAPPED s_writeOv;
	std::vector<UnsignedByte> s_writeBuf;
	Bool s_writing = FALSE;
	std::vector<std::vector<UnsignedByte> > s_outQueue;
	UnsignedInt s_droppedFrames = 0;
	std::wstring s_pipeName;
	UnsignedInt s_startMs = 0;
	UnsignedInt s_lastTry = 0;
	UnsignedInt s_lastPeerPass = 0;
	UnsignedInt s_state = 0;
	UnsignedInt s_failBits = PENDING_BIT;
	Bool s_statusSeen = FALSE;
	std::vector<UnsignedByte> s_inbox;
	std::map<UnsignedInt, Peer> s_peers;
	std::vector<UnsignedInt> s_newFailures;
	GuardLink::SendFunc s_send = nullptr;

	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }

	const Text kTitle = { "GUI:GuardTitle", L"Anti-cheat", L"\x0410\x043D\x0442\x0438\x0447\x0438\x0442" };
	const Text kStartFirst = { "GUI:GuardStartFirst", L"Start the anti-cheat first.", L"\x0421\x043D\x0430\x0447\x0430\x043B\x0430 \x0432\x043A\x043B\x044E\x0447\x0438\x0442\x0435 \x0430\x043D\x0442\x0438\x0447\x0438\x0442." };
	const Text kJoinRefused = { "GUI:GuardJoinRefused",
		L"The host accepts only players with a verified anti-cheat. Yours is not verified (yet): wait a few seconds and try again.",
		L"\x0425\x043E\x0441\x0442 \x043F\x0440\x0438\x043D\x0438\x043C\x0430\x0435\x0442 \x0442\x043E\x043B\x044C\x043A\x043E \x0438\x0433\x0440\x043E\x043A\x043E\x0432 \x0441 \x043F\x0440\x043E\x0432\x0435\x0440\x0435\x043D\x043D\x044B\x043C \x0430\x043D\x0442\x0438\x0447\x0438\x0442\x043E\x043C. \x0412\x0430\x0448 \x0435\x0449\x0451 \x043D\x0435 \x043F\x0440\x043E\x0432\x0435\x0440\x0435\x043D: \x043F\x043E\x0434\x043E\x0436\x0434\x0438\x0442\x0435 \x043D\x0435\x0441\x043A\x043E\x043B\x044C\x043A\x043E \x0441\x0435\x043A\x0443\x043D\x0434 \x0438 \x043F\x043E\x043F\x0440\x043E\x0431\x0443\x0439\x0442\x0435 \x0441\x043D\x043E\x0432\x0430." };
	const Text kJoinRefusedOwnFailed = { "GUI:GuardJoinRefusedOwnFailed", L"The host accepts only players with a verified anti-cheat. Yours found a problem with your game or its files: see the line about it in the chat.", L"\x0425\x043E\x0441\x0442 \x043F\x0440\x0438\x043D\x0438\x043C\x0430\x0435\x0442 \x0442\x043E\x043B\x044C\x043A\x043E \x0438\x0433\x0440\x043E\x043A\x043E\x0432 \x0441 \x043F\x0440\x043E\x0432\x0435\x0440\x0435\x043D\x043D\x044B\x043C \x0430\x043D\x0442\x0438\x0447\x0438\x0442\x043E\x043C. \x0412\x0430\x0448 \x0430\x043D\x0442\x0438\x0447\x0438\x0442 \x043D\x0430\x0448\x0451\x043B \x043F\x0440\x043E\x0431\x043B\x0435\x043C\x0443 \x0432 \x0432\x0430\x0448\x0435\x0439 \x0438\x0433\x0440\x0435 \x0438\x043B\x0438 \x0435\x0451 \x0444\x0430\x0439\x043B\x0430\x0445: \x0441\x043C. \x0441\x0442\x0440\x043E\x043A\x0443 \x043E\x0431 \x044D\x0442\x043E\x043C \x0432 \x0447\x0430\x0442\x0435." };
	const Text kFailedInMatch = { "GUI:GuardFailedInMatch", L"Anti-cheat: %ls failed the check.",
		L"\x0410\x043D\x0442\x0438\x0447\x0438\x0442: \x0438\x0433\x0440\x043E\x043A %ls \x043D\x0435 \x043F\x0440\x043E\x0448\x0451\x043B \x043F\x0440\x043E\x0432\x0435\x0440\x043A\x0443." };
	const Text kOwnFailure = { "GUI:GuardOwnFailure",
		L"Anti-cheat: your game or its files differ from the official build; other players see a red mark next to you.",
		L"\x0410\x043D\x0442\x0438\x0447\x0438\x0442: \x0432\x0430\x0448\x0430 \x0438\x0433\x0440\x0430 \x0438\x043B\x0438 \x0435\x0451 \x0444\x0430\x0439\x043B\x044B \x043E\x0442\x043B\x0438\x0447\x0430\x044E\x0442\x0441\x044F \x043E\x0442 \x043E\x0444\x0438\x0446\x0438\x0430\x043B\x044C\x043D\x043E\x0439 \x0441\x0431\x043E\x0440\x043A\x0438; \x0434\x0440\x0443\x0433\x0438\x0435 \x0438\x0433\x0440\x043E\x043A\x0438 \x0432\x0438\x0434\x044F\x0442 \x043A\x0440\x0430\x0441\x043D\x0443\x044E \x043E\x0442\x043C\x0435\x0442\x043A\x0443 \x0440\x044F\x0434\x043E\x043C \x0441 \x0432\x0430\x043C\x0438." };

	void closePipe()
	{
		if (s_pipe != INVALID_HANDLE_VALUE)
		{
			CancelIoEx(s_pipe, nullptr);
			DWORD unused = 0;
			if (s_reading) GetOverlappedResult(s_pipe, &s_readOv, &unused, TRUE);    // cancelled: returns at once
			if (s_writing) GetOverlappedResult(s_pipe, &s_writeOv, &unused, TRUE);
			CloseHandle(s_pipe);
			s_pipe = INVALID_HANDLE_VALUE;
		}
		s_reading = s_writing = FALSE;
		s_outQueue.clear();
		s_connected = FALSE;
		s_inbox.clear();
	}

	/// The guard is gone (ended, silent, broken, or not ours): no shield, LAN refused from now on. A guard that is alive
	/// but silent is ended with the job (it may be hung or suspended).
	void lost(const char *why)
	{
		DEBUG_LOG(("GuardLink: guard lost (%s)", why));
		closePipe();
		s_dead = TRUE;
		if (s_job != nullptr)
		{
			CloseHandle(s_job);
			s_job = nullptr;
		}
		s_peers.clear();
		s_newFailures.clear();
	}

	/// O32: the frames queued for the guard, as far as the pipe takes them now; never waits.
	void pumpWrites()
	{
		if (!s_connected) {
			return;
		}
		if (s_writing)
		{
			if (!HasOverlappedIoCompleted(&s_writeOv)) {
				return;
			}
			DWORD written = 0;
			const BOOL ok = GetOverlappedResult(s_pipe, &s_writeOv, &written, FALSE);
			s_writing = FALSE;
			if (!ok || written != s_writeBuf.size())
			{
				lost("write");
				return;
			}
		}
		if (s_outQueue.empty()) {
			return;
		}
		s_writeBuf = s_outQueue.front();
		s_outQueue.erase(s_outQueue.begin());
		memset(&s_writeOv, 0, sizeof(s_writeOv));
		DWORD written = 0;
		if (WriteFile(s_pipe, &s_writeBuf[0], (DWORD)s_writeBuf.size(), &written, &s_writeOv)) {
			return;   // done at once
		}
		if (GetLastError() == ERROR_IO_PENDING) {
			s_writing = TRUE;
		} else {
			lost("write");
		}
	}

	/// O32 (review 3): queued, never written while the game waits; a full queue (a guard that does not read) drops it.
	Bool writeFrame(UnsignedShort type, const void *payload, UnsignedShort length)
	{
		if (!s_connected || length > kMaxPayload) {
			return FALSE;
		}
		if (s_outQueue.size() >= kMaxQueuedFrames)
		{
			++s_droppedFrames;
			return FALSE;
		}
		std::vector<UnsignedByte> frame(sizeof(Header) + length);
		Header h = { kMagic, type, length };
		memcpy(&frame[0], &h, sizeof(h));
		if (length > 0) {
			memcpy(&frame[sizeof(h)], payload, length);
		}
		s_outQueue.push_back(frame);
		pumpWrites();
		return s_connected;
	}

	void onFrame(const Header &h, const UnsignedByte *p)
	{
		switch (h.type)
		{
			case PIPE_STATUS:
				if (h.length >= 16)
				{
					memcpy(&s_state, p + 8, 4);
					memcpy(&s_failBits, p + 12, 4);
					s_statusSeen = TRUE;
				}
				break;
			case PIPE_SEND:
				if (h.length > 4 && h.length <= 4 + kMaxRelay && s_send != nullptr)
				{
					UnsignedInt ip;
					memcpy(&ip, p, 4);
					s_send(ip, p + 4, h.length - 4);
				}
				break;
			case PIPE_VERDICT:
				if (h.length >= 12)
				{
					UnsignedInt ip, verdict, bits;
					memcpy(&ip, p, 4);
					memcpy(&verdict, p + 4, 4);
					memcpy(&bits, p + 8, 4);
					Peer &peer = s_peers[ip];
					const GuardLink::Verdict v = verdict == 1 ? GuardLink::VERDICT_VERIFIED : (verdict == 2 ? GuardLink::VERDICT_FAILED : GuardLink::VERDICT_NONE);
					if (v == GuardLink::VERDICT_FAILED && peer.verdict != GuardLink::VERDICT_FAILED) {
						s_newFailures.push_back(ip);
					}
					// one missed answer keeps a verdict that was there (a lost packet is not a lost shield) and asks again
					// soon; two in a row clear it (that player's guard is gone)
					if (v == GuardLink::VERDICT_NONE)
					{
						if (++peer.misses >= 2 || !peer.asked) {
							peer.verdict = GuardLink::VERDICT_NONE;
						}
						peer.lastCheck = timeGetTime() - (kRecheckMs - kRetryMs);   // ask again in kRetryMs
					}
					else
					{
						peer.misses = 0;
						peer.verdict = v;
						peer.verdictMs = timeGetTime();
					}
					peer.bits = bits;
					peer.asked = TRUE;
				}
				break;
			default:
				break;
		}
	}

	std::wstring userDataFolder()
	{
		std::wstring w;
		if (TheGlobalData != nullptr)
		{
			const AsciiString &a = TheGlobalData->getPath_UserData();
			const int n = MultiByteToWideChar(CP_ACP, 0, a.str(), -1, nullptr, 0);
			if (n > 1)
			{
				w.resize(n - 1);
				MultiByteToWideChar(CP_ACP, 0, a.str(), -1, &w[0], n);
			}
		}
		while (!w.empty() && (w[w.size() - 1] == L'\\' || w[w.size() - 1] == L'/')) {
			w.resize(w.size() - 1);   // a trailing backslash would escape the closing quote of the argument
		}
		return w;
	}
}

void GuardLink::setNoGuard(Bool on) { s_noGuard = on; }
void GuardLink::setTestMode(Bool on) { s_testMode = on; }
Bool GuardLink::noGuard() { return s_noGuard; }

void GuardLink::start()
{
	if (s_started || s_noGuard) {
		return;
	}
	wchar_t exe[MAX_PATH * 2];
	const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
	std::wstring guard(exe, n);
	const size_t slash = guard.find_last_of(L"\\/");
	guard = (slash == std::wstring::npos ? std::wstring() : guard.substr(0, slash + 1)) + L"GenGuard.exe";
	if (GetFileAttributesW(guard.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		DEBUG_LOG(("GuardLink: no GenGuard.exe beside the game"));
		return;
	}
	// O32 (review 9): a random name (the game still checks that the pipe's server is the guard it started)
	unsigned int r[2] = { 0, 0 };
	if (!SystemFunction036(r, sizeof(r)))
	{
		LARGE_INTEGER qpc;
		QueryPerformanceCounter(&qpc);
		r[0] = (unsigned)qpc.LowPart;
		r[1] = (unsigned)GetTickCount();
	}
	wchar_t name[96];
	swprintf(name, 96, L"\\\\.\\pipe\\GenGuard-%lu-%08X%08X", GetCurrentProcessId(), r[0], r[1]);
	s_pipeName = name;
	std::wstring cmd = L"\"" + guard + L"\" --pid ";
	wchar_t pid[16];
	swprintf(pid, 16, L"%lu", GetCurrentProcessId());
	cmd += pid;
	cmd += L" --pipe " + s_pipeName;
	const std::wstring userData = userDataFolder();
	if (!userData.empty() && userData.find(L'"') == std::wstring::npos) {   // O32 (review 25): a quote would break the line
		cmd += L" --userdata \"" + userData + L"\"";
	}
	if (s_testMode) {
		cmd += L" --test";
	}

	// a job that ends the guard with the game, also when the game crashes
	s_job = CreateJobObjectW(nullptr, nullptr);
	if (s_job != nullptr)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
		memset(&limits, 0, sizeof(limits));
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(s_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}
	STARTUPINFOW si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	std::vector<wchar_t> line(cmd.begin(), cmd.end());
	line.push_back(L'\0');
	if (!CreateProcessW(guard.c_str(), &line[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
	{
		DEBUG_LOG(("GuardLink: GenGuard.exe did not start (%lu)", GetLastError()));
		return;
	}
	if (s_job != nullptr) {
		AssignProcessToJobObject(s_job, pi.hProcess);
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	s_process = pi.hProcess;
	s_childPid = pi.dwProcessId;
	s_started = TRUE;
	s_startMs = timeGetTime();
}

void GuardLink::update()
{
	if (!s_started || s_dead) {
		return;
	}
	const UnsignedInt now = timeGetTime();
	if (WaitForSingleObject(s_process, 0) == WAIT_OBJECT_0)
	{
		lost("the guard process ended");
		return;
	}
	// a pause of the game itself (minimized, a long load) is not the guard's silence
	if (s_lastUpdateMs != 0 && now - s_lastUpdateMs > 2000) {
		s_lastFrameMs = now;
	}
	s_lastUpdateMs = now;
	if (!s_connected)
	{
		if (now - s_lastTry < 100) {
			return;
		}
		s_lastTry = now;
		s_pipe = CreateFileW(s_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (s_pipe == INVALID_HANDLE_VALUE)
		{
			if (now - s_startMs > kConnectGiveUpMs) {
				lost("no pipe");
			}
			return;
		}
		// O32 (review 9): the pipe must be served by the guard this game started, not by another program
		ULONG serverPid = 0;
		if (!GetNamedPipeServerProcessId(s_pipe, &serverPid) || serverPid != s_childPid)
		{
			CloseHandle(s_pipe);
			s_pipe = INVALID_HANDLE_VALUE;
			lost("the pipe's server is not the guard this game started");
			return;
		}
		s_connected = TRUE;
		s_lastFrameMs = now;
		const UnsignedInt hello[2] = { kProtocol, (UnsignedInt)GetCurrentProcessId() };
		writeFrame(PIPE_HELLO, hello, sizeof(hello));
		DEBUG_LOG(("GuardLink: connected after %u ms", now - s_startMs));
	}

	// what the guard sent: overlapped reads, polled (O32)
	for (Int round = 0; round < 8 && s_connected; ++round)
	{
		if (!s_reading)
		{
			memset(&s_readOv, 0, sizeof(s_readOv));
			DWORD got = 0;
			if (ReadFile(s_pipe, s_readBuf, sizeof(s_readBuf), &got, &s_readOv))
			{
				if (got == 0) {
					break;
				}
				s_inbox.insert(s_inbox.end(), s_readBuf, s_readBuf + got);
				continue;   // more may be there
			}
			if (GetLastError() != ERROR_IO_PENDING)
			{
				lost("read");
				return;
			}
			s_reading = TRUE;
		}
		if (!HasOverlappedIoCompleted(&s_readOv)) {
			break;
		}
		DWORD got = 0;
		s_reading = FALSE;
		if (!GetOverlappedResult(s_pipe, &s_readOv, &got, FALSE) || got == 0)
		{
			lost("read");
			return;
		}
		s_inbox.insert(s_inbox.end(), s_readBuf, s_readBuf + got);
	}
	if (!s_inbox.empty()) {
		s_lastFrameMs = now;
	}
	pumpWrites();
	if (!s_connected) {
		return;
	}
	// O32 (review 3): the guard sends STATUS every 2 s; silent for 10 s (hung, suspended) it counts as gone
	if (now - s_lastFrameMs > kSilentMs)
	{
		lost("silent");
		return;
	}
	while (s_inbox.size() >= sizeof(Header))
	{
		Header h;
		memcpy(&h, &s_inbox[0], sizeof(h));
		if (h.magic != kMagic || h.length > kMaxPayload)
		{
			lost("bad frame");
			return;
		}
		if (s_inbox.size() < sizeof(h) + h.length) {
			break;
		}
		onFrame(h, &s_inbox[0] + sizeof(h));
		s_inbox.erase(s_inbox.begin(), s_inbox.begin() + sizeof(h) + h.length);
	}

	// the players seen lately: asked now and then
	if (now - s_lastPeerPass < 500) {
		return;
	}
	s_lastPeerPass = now;
	for (std::map<UnsignedInt, Peer>::iterator it = s_peers.begin(); it != s_peers.end();)
	{
		Peer &p = it->second;
		if (now - p.lastSeen > kForgetMs)
		{
			it = s_peers.erase(it);
			continue;
		}
		if (now - p.lastSeen < kSeenWindowMs && (p.lastCheck == 0 || now - p.lastCheck >= kRecheckMs))
		{
			p.lastCheck = now;
			const UnsignedInt check[2] = { it->first, p.ownIp };
			writeFrame(PIPE_CHECK, check, sizeof(check));
			if (!s_connected) {
				return;
			}
		}
		++it;
	}
}

void GuardLink::shutdown()
{
	closePipe();   // the guard sees the pipe close and ends
	if (s_job != nullptr)
	{
		CloseHandle(s_job);   // and the job ends it if it did not
		s_job = nullptr;
	}
	if (s_process != nullptr)
	{
		CloseHandle(s_process);
		s_process = nullptr;
	}
	s_started = FALSE;
	s_peers.clear();
	s_newFailures.clear();
}

Bool GuardLink::connected() { return s_connected; }
Bool GuardLink::lanEntryAllowed() { return s_noGuard || s_connected; }
UnicodeString GuardLink::refusalTitle() { return textOf(kTitle); }
UnicodeString GuardLink::refusalText() { return textOf(kStartFirst); }
UnicodeString GuardLink::joinRefusedText()
{
	// O32 (review 24): a player whose own check failed is not told to wait
	return textOf(ownVerdict() == VERDICT_FAILED ? kJoinRefusedOwnFailed : kJoinRefused);
}
UnicodeString GuardLink::ownFailureText() { return textOf(kOwnFailure); }

UnicodeString GuardLink::failedInMatchText(const UnicodeString &playerName)
{
	UnicodeString t;
	t.format(textOf(kFailedInMatch).str(), playerName.str());
	return t;
}

GuardLink::Verdict GuardLink::ownVerdict()
{
	if (!s_connected || !s_statusSeen || s_state != 1 || (s_failBits & PENDING_BIT) != 0) {
		return VERDICT_NONE;
	}
	return s_failBits == 0 ? VERDICT_VERIFIED : VERDICT_FAILED;
}

UnsignedInt GuardLink::ownFailBits() { return s_failBits; }

GuardLink::Verdict GuardLink::verdictOf(UnsignedInt ip)
{
	if (!s_connected) {
		return VERDICT_NONE;
	}
	std::map<UnsignedInt, Peer>::const_iterator it = s_peers.find(ip);
	if (it == s_peers.end() || it->second.verdict == VERDICT_NONE) {
		return VERDICT_NONE;
	}
	// O32 (review 20): a verdict counts only while it is renewed (an address another player takes later starts empty)
	return timeGetTime() - it->second.verdictMs <= kVerdictLifeMs ? it->second.verdict : VERDICT_NONE;
}

void GuardLink::notePeer(UnsignedInt ip, UnsignedInt ownIp)
{
	if (!s_started || s_dead || ip == 0 || ip == ownIp) {
		return;   // O32 (review 21): nothing is collected once the guard is gone
	}
	Peer &p = s_peers[ip];
	p.ownIp = ownIp;
	p.lastSeen = timeGetTime();
}

void GuardLink::onLanData(UnsignedInt fromIp, UnsignedInt ownIp, const UnsignedByte *data, Int length)
{
	if (!s_connected || length <= 0 || length > kMaxRelay) {
		return;
	}
	UnsignedByte payload[8 + kMaxRelay];
	memcpy(payload, &fromIp, 4);
	memcpy(payload + 4, &ownIp, 4);
	memcpy(payload + 8, data, length);
	writeFrame(PIPE_RECV, payload, (UnsignedShort)(8 + length));
}

void GuardLink::setSender(SendFunc func) { s_send = func; }

Bool GuardLink::hostRequiresGuard()
{
	// O32 (review 19): the rule holds also when this host's own guard has gone (then nobody is verified); only a test run
	// without a guard (-lanNoGuard) has no rule
	if (s_noGuard) {
		return FALSE;
	}
	OptionPreferences pref;
	OptionPreferences::const_iterator it = pref.find("LanRequireGuard");
	return it == pref.end() || stricmp(it->second.str(), "no") != 0;
}

Bool GuardLink::takeNewFailure(UnsignedInt *ip)
{
	if (s_newFailures.empty()) {
		return FALSE;
	}
	*ip = s_newFailures.front();
	s_newFailures.erase(s_newFailures.begin());
	return TRUE;
}
