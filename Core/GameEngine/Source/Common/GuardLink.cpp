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
	const Text kFailedInMatch = { "GUI:GuardFailedInMatch", L"Anti-cheat: %ls failed the check.",
		L"\x0410\x043D\x0442\x0438\x0447\x0438\x0442: \x0438\x0433\x0440\x043E\x043A %ls \x043D\x0435 \x043F\x0440\x043E\x0448\x0451\x043B \x043F\x0440\x043E\x0432\x0435\x0440\x043A\x0443." };
	const Text kOwnFailure = { "GUI:GuardOwnFailure",
		L"Anti-cheat: your game or its files differ from the official build; other players see a red mark next to you.",
		L"\x0410\x043D\x0442\x0438\x0447\x0438\x0442: \x0432\x0430\x0448\x0430 \x0438\x0433\x0440\x0430 \x0438\x043B\x0438 \x0435\x0451 \x0444\x0430\x0439\x043B\x044B \x043E\x0442\x043B\x0438\x0447\x0430\x044E\x0442\x0441\x044F \x043E\x0442 \x043E\x0444\x0438\x0446\x0438\x0430\x043B\x044C\x043D\x043E\x0439 \x0441\x0431\x043E\x0440\x043A\x0438; \x0434\x0440\x0443\x0433\x0438\x0435 \x0438\x0433\x0440\x043E\x043A\x0438 \x0432\x0438\x0434\x044F\x0442 \x043A\x0440\x0430\x0441\x043D\x0443\x044E \x043E\x0442\x043C\x0435\x0442\x043A\x0443 \x0440\x044F\x0434\x043E\x043C \x0441 \x0432\x0430\x043C\x0438." };

	void closePipe()
	{
		if (s_pipe != INVALID_HANDLE_VALUE)
		{
			CloseHandle(s_pipe);
			s_pipe = INVALID_HANDLE_VALUE;
		}
		s_connected = FALSE;
		s_inbox.clear();
	}

	void lost(const char *why)
	{
		DEBUG_LOG(("GuardLink: guard lost (%s)", why));
		closePipe();
		s_dead = TRUE;
		for (std::map<UnsignedInt, Peer>::iterator it = s_peers.begin(); it != s_peers.end(); ++it) {
			it->second.verdict = GuardLink::VERDICT_NONE;
		}
	}

	Bool writeFrame(UnsignedShort type, const void *payload, UnsignedShort length)
	{
		if (!s_connected) {
			return FALSE;
		}
		UnsignedByte frame[sizeof(Header) + kMaxPayload];
		Header h = { kMagic, type, length };
		memcpy(frame, &h, sizeof(h));
		if (length > 0) {
			memcpy(frame + sizeof(h), payload, length);
		}
		DWORD written = 0;
		if (!WriteFile(s_pipe, frame, (DWORD)(sizeof(h) + length), &written, nullptr) || written != sizeof(h) + length)
		{
			lost("write");
			return FALSE;
		}
		return TRUE;
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
	LARGE_INTEGER qpc;
	QueryPerformanceCounter(&qpc);
	wchar_t name[96];
	swprintf(name, 96, L"\\\\.\\pipe\\GenGuard-%lu-%08X%08X", GetCurrentProcessId(), (unsigned)qpc.LowPart, (unsigned)GetTickCount());
	s_pipeName = name;
	std::wstring cmd = L"\"" + guard + L"\" --pid ";
	wchar_t pid[16];
	swprintf(pid, 16, L"%lu", GetCurrentProcessId());
	cmd += pid;
	cmd += L" --pipe " + s_pipeName;
	const std::wstring userData = userDataFolder();
	if (!userData.empty()) {
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
	if (!s_connected)
	{
		if (now - s_lastTry < 100) {
			return;
		}
		s_lastTry = now;
		s_pipe = CreateFileW(s_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (s_pipe == INVALID_HANDLE_VALUE)
		{
			if (now - s_startMs > kConnectGiveUpMs) {
				lost("no pipe");
			}
			return;
		}
		s_connected = TRUE;
		const UnsignedInt hello[2] = { kProtocol, (UnsignedInt)GetCurrentProcessId() };
		writeFrame(PIPE_HELLO, hello, sizeof(hello));
		DEBUG_LOG(("GuardLink: connected after %u ms", now - s_startMs));
	}

	// what the guard sent
	DWORD avail = 0;
	if (!PeekNamedPipe(s_pipe, nullptr, 0, nullptr, &avail, nullptr))
	{
		lost("peek");
		return;
	}
	while (avail > 0 && s_connected)
	{
		UnsignedByte buf[2048];
		DWORD got = 0;
		if (!ReadFile(s_pipe, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &got, nullptr) || got == 0)
		{
			lost("read");
			return;
		}
		avail -= got;
		s_inbox.insert(s_inbox.end(), buf, buf + got);
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
UnicodeString GuardLink::joinRefusedText() { return textOf(kJoinRefused); }
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
	return it != s_peers.end() ? it->second.verdict : VERDICT_NONE;
}

void GuardLink::notePeer(UnsignedInt ip, UnsignedInt ownIp)
{
	if (!s_started || ip == 0 || ip == ownIp) {
		return;
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
	if (!s_connected) {
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
