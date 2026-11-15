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

// NetLab.cpp ///////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See NetLab.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/NetLab.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"

#include <stdio.h>
#include <stdarg.h>

namespace
{
	enum { SIM_SLOTS = 512 };

	/// The lobby object of a NetLab match: a LANAPI that is never initialised (no lobby socket, no broadcast); it only
	/// carries the game, the local address and the name, as the lobby would have left them.
	/// TheSuperHackers @bugfix (O22) In the lobby's order: the local address and the name first (setIdentity, then
	/// TheLAN = the object), the game after (setGame). LANGameInfo's constructor reads TheLAN->GetLocalIP(), and a slot
	/// is the local player when its address equals TheLAN->GetLocalIP(); with the game built before TheLAN was set the
	/// constructor dereferenced a null TheLAN (S19 smoke run, LANGameInfo.cpp:98, 0xC0000005 on both instances).
	class NetLabLANAPI : public LANAPI
	{
	public:
		void setIdentity(UnsignedInt localIP, const UnicodeString &name)
		{
			m_localIP = localIP;
			m_name = name;
			m_inLobby = FALSE;
			m_isInLANMenu = TRUE;
		}
		void setGame(LANGameInfo *game)
		{
			m_currentGame = game;
		}
	};

	struct Member { Bool present; Int team; Int playerTemplate; SlotState ai; };

	// configuration
	Int s_slot = -1;
	Member s_members[MAX_SLOTS];
	char s_map[_MAX_PATH] = "Maps\\Tournament Desert\\Tournament Desert.map";
	Int s_seed = 12345;
	Int s_quitAfterSec = 0;

	// state
	Bool s_started = FALSE;
	Bool s_failed = FALSE;
	DWORD s_matchStartMs = 0;
	FILE *s_csv = nullptr;
	FILE *s_log = nullptr;
	DWORD s_lastSecondMs = 0;
	DWORD s_lastUpdateMs = 0;
	UnsignedInt s_framesAtSecond = 0;
	Bool s_disconnectShown = FALSE;
	UnsignedInt s_connectedMask = 0;
	DWORD s_lastSimFileCheckMs = 0;
	FILETIME s_simFileTime = { 0, 0 };

	// counters of the current second
	Int s_bytesIn = 0, s_bytesOut = 0, s_packetsIn = 0, s_packetsOut = 0, s_retries = 0, s_frameResends = 0;
	UnsignedInt s_waitingMask = 0;
	Real s_stallMs = 0.0f;

	// simulator
	struct SimSettings { Int delayMs, jitterMs, lossPct, reorderPct, kbps; };
	SimSettings s_sim = { 0, 0, 0, 0, 0 };
	Bool s_simOn = FALSE;
	struct SimSlot { Bool used; DWORD due; UnsignedInt seq; TransportMessage message; };
	UnsignedInt s_simSeq = 0;
	SimSlot *s_simQueue = nullptr;
	DWORD s_lastDue = 0;
	double s_linkFreeAt = 0.0;
	UnsignedInt s_rng = 0x2468ACE1u;
	Int s_simDropped = 0, s_simHeldMax = 0;

	UnsignedInt nextRandom()
	{
		s_rng = s_rng * 1664525u + 1013904223u;   // the simulator's own numbers: never the game's
		return s_rng >> 8;
	}

	UnsignedInt slotIP(Int slot)
	{
		return (127u << 24) | (UnsignedInt)(10 + slot);   // 127.0.0.(10 + slot), host order as GameSlot keeps it
	}

	void logLine(const char *format, ...)
	{
		if (s_log == nullptr) {
			return;
		}
		fprintf(s_log, "[%9.3f] ", (timeGetTime() - s_matchStartMs) / 1000.0);
		va_list args;
		va_start(args, format);
		vfprintf(s_log, format, args);
		va_end(args);
		fputc('\n', s_log);
		fflush(s_log);
	}

	void parseSim(const char *text, Int *freezeMs)
	{
		const char *keys[6] = { "delay=", "jitter=", "loss=", "reorder=", "kbps=", "freeze=" };
		Int *targets[6] = { &s_sim.delayMs, &s_sim.jitterMs, &s_sim.lossPct, &s_sim.reorderPct, &s_sim.kbps, freezeMs };
		for (Int k = 0; k < 6; ++k)
		{
			const char *at = strstr(text, keys[k]);
			if (at != nullptr && targets[k] != nullptr) {
				*targets[k] = atoi(at + strlen(keys[k]));
			}
		}
		s_simOn = s_sim.delayMs > 0 || s_sim.jitterMs > 0 || s_sim.lossPct > 0 || s_sim.reorderPct > 0 || s_sim.kbps > 0;
	}

	void logSim(const char *why)
	{
		logLine("simulator (%s): delay %d ms, jitter %d ms, loss %d %%, reorder %d %%, %d kbit/s%s", why, s_sim.delayMs,
			s_sim.jitterMs, s_sim.lossPct, s_sim.reorderPct, s_sim.kbps, s_simOn ? "" : " (off)");
	}

	void readSimFile(DWORD now)
	{
		if (now - s_lastSimFileCheckMs < 500 || TheGlobalData == nullptr) {
			return;
		}
		s_lastSimFileCheckMs = now;
		char path[_MAX_PATH];
		snprintf(path, sizeof(path), "%sNetLabSim.txt", TheGlobalData->getPath_UserData().str());
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
			return;
		}
		if (CompareFileTime(&info.ftLastWriteTime, &s_simFileTime) == 0) {
			return;
		}
		s_simFileTime = info.ftLastWriteTime;
		char text[256] = { 0 };
		if (FILE *fp = fopen(path, "rb"))
		{
			fread(text, 1, sizeof(text) - 1, fp);
			fclose(fp);
		}
		Int freezeMs = 0;
		parseSim(text, &freezeMs);
		logSim("NetLabSim.txt");
		if (freezeMs > 0)
		{
			logLine("freeze: the game thread sleeps %d ms (a starved computer)", freezeMs);
			Sleep((DWORD)freezeMs);
			logLine("freeze: awake");
		}
	}

	/// The LAN game object the lobby would have built from the roster.
	LANGameInfo *buildGame()
	{
		// TheLAN and its local address must exist here (the constructor reads them).
		LANGameInfo *game = NEW LANGameInfo;
		// TheSuperHackers @bugfix (O22) enterGame first: it resets the game info, and the reset draws a new seed from
		// GetTickCount(). With the seed set before it, every instance had its own seed (the lobby sets the seed after
		// the player entered the game too).
		game->enterGame();
		game->setSeed(s_seed);
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			LANGameSlot slot;
			const Member &m = s_members[i];
			if (!m.present)
			{
				slot.setState(SLOT_CLOSED);
			}
			else if (m.ai != SLOT_PLAYER)
			{
				slot.setState(m.ai);
			}
			else
			{
				UnicodeString name;
				name.format(L"NetLab%d", i);
				slot.setState(SLOT_PLAYER, name);
				slot.setIP(slotIP(i));
				slot.setPort(NETWORK_BASE_PORT_NUMBER);
				slot.setLastHeard(timeGetTime());
				slot.setLogin(AsciiString("netlab"));
				slot.setHost(AsciiString("netlab"));
			}
			if (m.present)
			{
				slot.setColor(-1);
				slot.setStartPos(-1);
				slot.setPlayerTemplate(m.playerTemplate);
				slot.setTeamNumber(m.team);
				slot.setMapAvailability(TRUE);
				slot.setAccept();
			}
			game->setSlot(i, slot);
		}
		game->setMap(AsciiString(s_map));
		const MapMetaData *meta = TheMapCache != nullptr ? TheMapCache->findMap(AsciiString(s_map)) : nullptr;
		if (meta != nullptr)
		{
			game->setMapCRC(meta->m_CRC);
			game->setMapSize(meta->m_filesize);
		}
		game->setLocalIP(slotIP(s_slot));
		UnicodeString gameName;
		gameName.format(L"NetLab match %d", s_seed);
		game->setName(gameName);
		return game;
	}

	Bool shellIdle()
	{
		return TheShell != nullptr && TheShell->isShellActive() && TheShell->getScreenCount() >= 1 && TheShell->isAnimFinished()
			&& TheWindowManager != nullptr && !TheWindowManager->winIsModalActive()
			&& TheGameLogic != nullptr && (!TheGameLogic->isInGame() || TheGameLogic->isInShellGame());
	}

	void startMatch()
	{
		s_started = TRUE;
		s_matchStartMs = timeGetTime();
		if (TheGlobalData != nullptr)
		{
			char path[_MAX_PATH];
			snprintf(path, sizeof(path), "%sNetLab.log", TheGlobalData->getPath_UserData().str());
			s_log = fopen(path, "w");
			snprintf(path, sizeof(path), "%sNetLab.csv", TheGlobalData->getPath_UserData().str());
			s_csv = fopen(path, "w");
			if (s_csv != nullptr)
			{
				fprintf(s_csv, "seconds,logic_frame,logic_frames_in_second,net_frame_rate,run_ahead,stalled_ms,waiting_for_slots,"
					"packets_in,packets_out,bytes_in,bytes_out,command_resends,frame_resend_requests,sim_dropped,sim_held_max,"
					"disconnect_screen,connected_slots\n");
				fflush(s_csv);
			}
		}
		if (TheMapCache != nullptr && TheMapCache->findMap(AsciiString(s_map)) == nullptr)
		{
			logLine("the map %s is not known to the map cache: no match", s_map);
			s_failed = TRUE;
			return;
		}
		if (TheLAN != nullptr || TheNetwork != nullptr)
		{
			logLine("a LAN object or a network already exists: no match");
			s_failed = TRUE;
			return;
		}
		NetLabLANAPI *lan = NEW NetLabLANAPI;
		UnicodeString name;
		name.format(L"NetLab%d", s_slot);
		lan->setIdentity(slotIP(s_slot), name);
		TheLAN = lan;
		LANGameInfo *game = buildGame();
		lan->setGame(game);
		const Int localSlot = game->getLocalSlotNum();
		if (localSlot != s_slot || game->getSeed() != s_seed)
		{
			logLine("the game built from the roster is wrong: local slot %d (should be %d), seed %d (should be %d): no match",
				localSlot, s_slot, game->getSeed(), s_seed);
			s_failed = TRUE;
			return;
		}
		logLine("slot %d at 127.0.0.%d:%d, seed %d, map %s", s_slot, 10 + s_slot, NETWORK_BASE_PORT_NUMBER, s_seed, s_map);
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const Member &m = s_members[i];
			if (m.present) {
				logLine("  slot %d: %s, team %d, template %d", i, m.ai == SLOT_PLAYER ? "human (an instance)" : "computer", m.team, m.playerTemplate);
			}
		}
		logSim("start");
		// the LAN code's own start: TheNetwork, the transport on 127.0.0.(10 + slot), the users, the new game
		TheLAN->OnGameStart();
		s_lastSecondMs = timeGetTime();
		s_framesAtSecond = 0;
	}

	void writeSecond(DWORD now)
	{
		if (s_csv == nullptr) {
			return;
		}
		const UnsignedInt frame = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;
		UnsignedInt connected = 0;
		if (TheNetwork != nullptr)
		{
			for (Int i = 0; i < MAX_SLOTS; ++i) {
				if (TheNetwork->isPlayerConnected(i)) connected |= (1u << i);
			}
		}
		fprintf(s_csv, "%.1f,%u,%u,%u,%u,%.0f,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u\n",
			(now - s_matchStartMs) / 1000.0, frame, frame - s_framesAtSecond,
			TheNetwork != nullptr ? TheNetwork->getFrameRate() : 0, TheNetwork != nullptr ? TheNetwork->getRunAhead() : 0,
			(double)s_stallMs, s_waitingMask, s_packetsIn, s_packetsOut, s_bytesIn, s_bytesOut, s_retries, s_frameResends,
			s_simDropped, s_simHeldMax, s_disconnectShown ? 1 : 0, connected);
		fflush(s_csv);
		s_framesAtSecond = frame;
		s_bytesIn = s_bytesOut = s_packetsIn = s_packetsOut = s_retries = s_frameResends = s_simDropped = s_simHeldMax = 0;
		s_waitingMask = 0;
		s_stallMs = 0.0f;
		if (connected != s_connectedMask)
		{
			logLine("connected slots %02X -> %02X", s_connectedMask, connected);
			s_connectedMask = connected;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void NetLab::configureSlot(Int slot) { s_slot = (slot >= 0 && slot < MAX_SLOTS) ? slot : -1; }

void NetLab::configurePlayers(const char *roster)
{
	memset(s_members, 0, sizeof(s_members));
	const char *p = roster;
	while (p != nullptr && *p != '\0')
	{
		Int slot = -1, team = -1, tmpl = -1;
		char level[16] = { 0 };
		const Int n = sscanf(p, "%d:%d:%d:%15[a-z]", &slot, &team, &tmpl, level);
		if (n >= 3 && slot >= 0 && slot < MAX_SLOTS)
		{
			Member &m = s_members[slot];
			m.present = TRUE;
			m.team = team;
			m.playerTemplate = tmpl;
			m.ai = SLOT_PLAYER;
			if (n == 4)
			{
				if (strcmp(level, "easy") == 0) m.ai = SLOT_EASY_AI;
				else if (strcmp(level, "medium") == 0) m.ai = SLOT_MED_AI;
				else m.ai = SLOT_BRUTAL_AI;
			}
		}
		p = strchr(p, ',');
		if (p != nullptr) ++p;
	}
}

void NetLab::configureMap(const char *map) { strlcpy(s_map, map, sizeof(s_map)); }
void NetLab::configureSeed(Int seed) { s_seed = seed; }
void NetLab::configureQuitAfter(Int seconds) { s_quitAfterSec = seconds > 0 ? seconds : 0; }

void NetLab::configureSim(const char *settings)
{
	Int freezeMs = 0;
	parseSim(settings, &freezeMs);
}

Bool NetLab::isConfigured()
{
	return s_slot >= 0 && s_members[s_slot].present && s_members[s_slot].ai == SLOT_PLAYER;
}

void NetLab::update()
{
	if (!isConfigured() || s_failed) {
		return;
	}
	const DWORD now = timeGetTime();
	if (!s_started)
	{
		if (shellIdle()) {
			startMatch();
		}
		return;
	}
	if (s_lastUpdateMs != 0 && TheNetwork != nullptr && TheNetwork->isStalling()) {
		s_stallMs += (Real)(now - s_lastUpdateMs);
	}
	s_lastUpdateMs = now;
	const Bool shown = TheDisconnectMenu != nullptr && TheDisconnectMenu->isScreenVisible();
	if (shown != s_disconnectShown)
	{
		s_disconnectShown = shown;
		logLine("disconnect screen %s at frame %u", shown ? "ON" : "off", TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0);
	}
	if (now - s_lastSecondMs >= 1000)
	{
		s_lastSecondMs += 1000;
		if (now - s_lastSecondMs >= 1000) s_lastSecondMs = now;   // a long freeze: one line, not a burst
		writeSecond(now);
	}
	readSimFile(now);
	if (s_quitAfterSec > 0 && now - s_matchStartMs > (DWORD)s_quitAfterSec * 1000 && TheGameEngine != nullptr && !TheGameEngine->getQuitting())
	{
		logLine("quitting after %d seconds (-netLabQuitAfter)", s_quitAfterSec);
		TheGameEngine->setQuitting(TRUE);
	}
}

void NetLab::shutdown()
{
	if (s_started && TheLAN != nullptr)
	{
		// the NetLab LAN object and its game: LANAPI's destructor frees only the lobby's game list, not the current game
		LANGameInfo *game = TheLAN->GetMyGame();
		delete TheLAN;
		TheLAN = nullptr;
		delete game;
	}
	if (s_csv != nullptr) { fclose(s_csv); s_csv = nullptr; }
	if (s_log != nullptr) { fclose(s_log); s_log = nullptr; }
	delete[] s_simQueue;
	s_simQueue = nullptr;
}

void NetLab::countSent(Int bytes) { ++s_packetsOut; s_bytesOut += bytes; }
void NetLab::countReceived(Int bytes) { ++s_packetsIn; s_bytesIn += bytes; }
void NetLab::countRetry() { ++s_retries; }
void NetLab::countFrameResend() { ++s_frameResends; }
void NetLab::noteWaitingFor(Int slot) { if (slot >= 0 && slot < MAX_SLOTS) s_waitingMask |= (1u << slot); }

Bool NetLab::simTake(const TransportMessage &message)
{
	if (!s_simOn || !isConfigured()) {
		return FALSE;
	}
	if (s_sim.lossPct > 0 && (Int)(nextRandom() % 100) < s_sim.lossPct)
	{
		++s_simDropped;
		return TRUE;
	}
	if (s_simQueue == nullptr) {
		s_simQueue = NEW SimSlot[SIM_SLOTS];
		for (Int i = 0; i < SIM_SLOTS; ++i) s_simQueue[i].used = FALSE;
	}
	const DWORD now = timeGetTime();
	Int delay = s_sim.delayMs;
	if (s_sim.jitterMs > 0) {
		delay += (Int)(nextRandom() % (UnsignedInt)(2 * s_sim.jitterMs + 1)) - s_sim.jitterMs;
	}
	if (delay < 0) delay = 0;
	DWORD due = now + (DWORD)delay;
	const Bool reorder = s_sim.reorderPct > 0 && (Int)(nextRandom() % 100) < s_sim.reorderPct;
	if (!reorder && (LONG)(s_lastDue - due) > 0) {
		due = s_lastDue;   // the path keeps the order unless reordering is asked for
	}
	if (s_sim.kbps > 0)
	{
		// the incoming link: one packet after another at the given rate
		const Int bytes = message.length + (Int)sizeof(TransportMessageHeader) + 28;
		const double start = s_linkFreeAt > (double)now ? s_linkFreeAt : (double)now;
		s_linkFreeAt = start + bytes * 8.0 / s_sim.kbps;   // kbit/s = bits per ms
		if (s_linkFreeAt - now > 1000.0)
		{
			s_linkFreeAt -= bytes * 8.0 / s_sim.kbps;
			++s_simDropped;   // more than a second queued: the router drops it
			return TRUE;
		}
		if ((LONG)((DWORD)s_linkFreeAt - due) > 0) due = (DWORD)s_linkFreeAt;
	}
	if (!reorder) s_lastDue = due;
	Int held = 0;
	for (Int i = 0; i < SIM_SLOTS; ++i)
	{
		if (s_simQueue[i].used) { ++held; continue; }
		s_simQueue[i].used = TRUE;
		s_simQueue[i].due = due;
		s_simQueue[i].seq = ++s_simSeq;
		memcpy(&s_simQueue[i].message, &message, sizeof(TransportMessage));
		if (held + 1 > s_simHeldMax) s_simHeldMax = held + 1;
		return TRUE;
	}
	++s_simDropped;   // the queue is full
	return TRUE;
}

Bool NetLab::simRelease(TransportMessage &message)
{
	if (s_simQueue == nullptr) {
		return FALSE;
	}
	const DWORD now = timeGetTime();
	Int best = -1;
	for (Int i = 0; i < SIM_SLOTS; ++i)
	{
		if (!s_simQueue[i].used || (LONG)(s_simQueue[i].due - now) > 0) continue;
		if (best < 0 || (LONG)(s_simQueue[i].due - s_simQueue[best].due) < 0
			|| (s_simQueue[i].due == s_simQueue[best].due && s_simQueue[i].seq < s_simQueue[best].seq)) best = i;   // ties: arrival order
	}
	if (best < 0) {
		return FALSE;
	}
	memcpy(&message, &s_simQueue[best].message, sizeof(TransportMessage));
	s_simQueue[best].used = FALSE;
	return TRUE;
}
