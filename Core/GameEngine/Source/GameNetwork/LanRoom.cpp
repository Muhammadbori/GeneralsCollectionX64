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

// LanRoom.cpp //////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O31 (row 21, step 3) See LanRoom.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/VictoryConditions.h"

#include "GameNetwork/LanRoom.h"

#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/Recorder.h"
#include "Common/ReplayFollow.h"
#include "Common/ReplayStream.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/LocalizedFallback.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/FirewallHelper.h"
#include "Common/UserPreferences.h"
#include "GameLogic/GameLogic.h"

#include <algorithm>
#include <map>
#include "Common/LanClock.h"   // O75

namespace
{
	enum Kind { KIND_OBSERVE = 1, KIND_LEAVE = 2, KIND_LIST = 3, KIND_REMOVED = 4, KIND_CHAT = 5, KIND_START = 6, KIND_COUNT = 7,
		KIND_RULES = 8, KIND_ASK_MAP = 9 /* O62: the host asks a player to say again whether it has the map */ };
	enum { FLAG_OUTSIDE_ALLOWED = 1, FLAG_TO_PLAYER = 2, FLAG_HOST_WATCHES = 4 /* O60: START to observers */ };
	const Int kDefaultDelay = 60;
	const UnsignedInt kKnownKeepMs = 600000;    ///< O33: a room's LIST is kept this long (its delay and flag)
	const size_t kKnownMax = 64;                ///< O33: at most this many rooms remembered
	const UnsignedInt kCountMs = 3000;          ///< O33: COUNT during a match
	enum { REASON_BY_HOST = 0, REASON_FULL = 1, REASON_NO_ROOM = 2 };
	const UnsignedInt kHeartbeatMs = 4000;
	const UnsignedInt kAskMs = 1000;            ///< O42: OBSERVE while the host's list does not have this observer yet
	const UnsignedInt kRulesMs = 5000;          ///< O42: RULES during the match (O46: every 5 s, not 2)
	const UnsignedInt kGoneMs = 10000;          ///< O42: no such game in the lobby and no LIST from its host for this long
	const UnsignedInt kListMs = 2000;
	const UnsignedInt kEmptyListMs = 5000;      ///< O46: a room without observers sends its (empty) list this often
	const UnsignedInt kForgetMs = 15000;
	const UnsignedInt kKnownMs = 6000;          ///< a room's list seen in the lobby counts this long
	const UnsignedInt kStreamWaitMs = 180000;   ///< the players load the map before the stream has its first frame

	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }
	const Text kRole = { "GUI:LanRoomObserverRole", L"observer", L"\x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C" };
	const Text kObserving = { "GUI:LanRoomObserving", L"You observe this game. When the match starts you watch it live.", L"\x0412\x044B \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0435\x0442\x0435 \x0437\x0430 \x044D\x0442\x043E\x0439 \x0438\x0433\x0440\x043E\x0439. \x041A\x043E\x0433\x0434\x0430 \x043C\x0430\x0442\x0447 \x043D\x0430\x0447\x043D\x0451\x0442\x0441\x044F, \x0432\x044B \x0431\x0443\x0434\x0435\x0442\x0435 \x0441\x043C\x043E\x0442\x0440\x0435\x0442\x044C \x0435\x0433\x043E \x0432\x0436\x0438\x0432\x0443\x044E." };
	const Text kRemoved = { "GUI:LanRoomRemoved", L"The host removed you from the observers.", L"\x0425\x043E\x0441\x0442 \x0443\x0431\x0440\x0430\x043B \x0432\x0430\x0441 \x0438\x0437 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x0439." };
	const Text kFull = { "GUI:LanRoomFull", L"No observer place is free in this game.", L"\x0412 \x044D\x0442\x043E\x0439 \x0438\x0433\x0440\x0435 \x043D\x0435\x0442 \x0441\x0432\x043E\x0431\x043E\x0434\x043D\x043E\x0433\x043E \x043C\x0435\x0441\x0442\x0430 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044F." };
	const Text kNoRoom = { "GUI:LanRoomNoRoom", L"That game does not take observers now.", L"\x042D\x0442\x0430 \x0438\x0433\x0440\x0430 \x0441\x0435\x0439\x0447\x0430\x0441 \x043D\x0435 \x043F\x0440\x0438\x043D\x0438\x043C\x0430\x0435\x0442 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x0439." };
	const Text kStarting = { "GUI:LanRoomStarting", L"The match has started: waiting for its stream...", L"\x041C\x0430\x0442\x0447 \x043D\x0430\x0447\x0430\x043B\x0441\x044F: \x0436\x0434\x0451\x043C \x0435\x0433\x043E \x0442\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x044E..." };
	const Text kLost = { "GUI:LanRoomLost", L"The match stream did not come.", L"\x0422\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x044F \x043C\x0430\x0442\x0447\x0430 \x043D\x0435 \x043F\x0440\x0438\x0448\x043B\x0430." };
	const Text kWatching = { "GUI:LanRoomWatching", L"Watching with a delay of %d s: waiting for the stream...", L"\x041F\x0440\x043E\x0441\x043C\x043E\x0442\x0440 \x0441 \x0437\x0430\x0434\x0435\x0440\x0436\x043A\x043E\x0439 %d \x0441: \x0436\x0434\x0451\x043C \x0442\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x044E..." };
	const Text kGone = { "GUI:LanRoomGone", L"The game you observed is gone.", L"\x0418\x0433\x0440\x0430, \x0437\x0430 \x043A\x043E\x0442\x043E\x0440\x043E\x0439 \x0432\x044B \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x043B\x0438, \x0438\x0441\x0447\x0435\x0437\x043B\x0430." };
	const Text kWatchingTotal = { "GUI:LanRoomWatchingTotal", L"Watching with a delay of %d s (the host's %d s + %d s): waiting for the stream...", L"\x041F\x0440\x043E\x0441\x043C\x043E\x0442\x0440 \x0441 \x0437\x0430\x0434\x0435\x0440\x0436\x043A\x043E\x0439 %d \x0441 (\x0445\x043E\x0441\x0442\x0430 %d \x0441 + %d \x0441): \x0436\x0434\x0451\x043C \x0442\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x044E..." };
	const Text kNoOutside = { "GUI:LanRoomNoOutside", L"The host of this game does not allow outside viewers.", L"\x0425\x043E\x0441\x0442 \x044D\x0442\x043E\x0439 \x0438\x0433\x0440\x044B \x043D\x0435 \x0440\x0430\x0437\x0440\x0435\x0448\x0430\x0435\x0442 \x043F\x043E\x0441\x0442\x043E\x0440\x043E\x043D\x043D\x0438\x0445 \x0437\x0440\x0438\x0442\x0435\x043B\x0435\x0439." };
	const Text kObserverLine = { "GUI:LanObserverChatLine", L"%ls (observer): %ls", L"%ls (\x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C): %ls" };
	const Text kObserverOnly = { "GUI:LanObserverChatOnly", L"Your messages go to the room's other observers only.", L"\x0412\x0430\x0448\x0438 \x0441\x043E\x043E\x0431\x0449\x0435\x043D\x0438\x044F \x0432\x0438\x0434\x044F\x0442 \x0442\x043E\x043B\x044C\x043A\x043E \x0434\x0440\x0443\x0433\x0438\x0435 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438 \x043A\x043E\x043C\x043D\x0430\x0442\x044B." };
	const Text kAdminNoMap = { "GUI:LanRoomAdminNoMap", L"The host watches, so nobody can send the map. These players need it first: %ls", L"\x0425\x043E\x0441\x0442 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0435\x0442, \x043F\x043E\x044D\x0442\x043E\x043C\x0443 \x043A\x0430\x0440\x0442\x0443 \x043D\x0435\x043A\x043E\x043C\x0443 \x043F\x0435\x0440\x0435\x0434\x0430\x0442\x044C. \x0421\x043D\x0430\x0447\x0430\x043B\x0430 \x043E\x043D\x0430 \x043D\x0443\x0436\x043D\x0430 \x044D\x0442\x0438\x043C \x0438\x0433\x0440\x043E\x043A\x0430\x043C: %ls" };
	const Text kAdminNobody = { "GUI:LanRoomAdminNobody", L"Nobody sits in a seat: a match needs at least one player.", L"\x041D\x0438\x043A\x0442\x043E \x043D\x0435 \x0441\x0438\x0434\x0438\x0442 \x043D\x0430 \x043C\x0435\x0441\x0442\x0435: \x0434\x043B\x044F \x043C\x0430\x0442\x0447\x0430 \x043D\x0443\x0436\x0435\x043D \x0445\x043E\x0442\x044F \x0431\x044B \x043E\x0434\x0438\x043D \x0438\x0433\x0440\x043E\x043A." };
	const Text kOtherVersion = { "GUI:LanOtherVersion", L"This game is another version of the game: it cannot be joined or watched.", L"\x042D\x0442\x0430 \x0438\x0433\x0440\x0430 \x0434\x0440\x0443\x0433\x043E\x0439 \x0432\x0435\x0440\x0441\x0438\x0438: \x0432\x043E\x0439\x0442\x0438 \x0432 \x043D\x0435\x0451 \x0438\x043B\x0438 \x0441\x043C\x043E\x0442\x0440\x0435\x0442\x044C \x0435\x0451 \x043D\x0435\x043B\x044C\x0437\x044F." };
	const Text kThisPcPlays = { "GUI:LanRoomThisPcPlays", L"This computer is a player of this match: it cannot watch it (a second game on a player's computer is refused).", L"\x042D\x0442\x043E\x0442 \x043A\x043E\x043C\x043F\x044C\x044E\x0442\x0435\x0440 \x2014 \x0438\x0433\x0440\x043E\x043A \x044D\x0442\x043E\x0433\x043E \x043C\x0430\x0442\x0447\x0430: \x0441\x043C\x043E\x0442\x0440\x0435\x0442\x044C \x0435\x0433\x043E \x043E\x0442\x0441\x044E\x0434\x0430 \x043D\x0435\x043B\x044C\x0437\x044F (\x0432\x0442\x043E\x0440\x0430\x044F \x0438\x0433\x0440\x0430 \x043D\x0430 \x043A\x043E\x043C\x043F\x044C\x044E\x0442\x0435\x0440\x0435 \x0438\x0433\x0440\x043E\x043A\x0430 \x043D\x0435 \x0434\x043E\x043F\x0443\x0441\x043A\x0430\x0435\x0442\x0441\x044F)." };   // O64
	const Text kLeft = { "GUI:LanRoomLeft", L"You no longer observe that game.", L"\x0412\x044B \x0431\x043E\x043B\x044C\x0448\x0435 \x043D\x0435 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0435\x0442\x0435 \x0437\x0430 \x044D\x0442\x043E\x0439 \x0438\x0433\x0440\x043E\x0439." };

	struct Entry { UnsignedInt ip; UnicodeString name; UnsignedInt lastHeard; };

	// host
	std::vector<Entry> s_hosted;
	Bool s_listDirty = FALSE;
	Bool s_listSentNonEmpty = FALSE;
	UnsignedInt s_lastList = 0;
	// what the lobby saw
	struct Known { std::vector<LanRoom::Observer> list; UnsignedInt seen; Int delay; Bool outsideAllowed; };
	std::map<UnsignedInt, Known> s_known;
	// O33: what a player of the coming match got in its host's START (or, as the host, froze itself)
	struct MatchStart { Bool valid; UnsignedInt host; std::vector<UnsignedInt> observers; Int delay; Bool outsideAllowed; };
	MatchStart s_matchStart = { FALSE, 0, std::vector<UnsignedInt>(), kDefaultDelay, TRUE };
	// O33: the running match this game serves as a player
	Bool s_matchServing = FALSE;
	Int s_matchObservers = 0;
	std::map<UnsignedInt, Int> s_matchOthers;   ///< outside viewers each other player serves (COUNT)
	UnsignedInt s_lastCount = 0;
	Bool s_sawGame = FALSE;                      ///< a viewer's replay was running
	// O34: a room observer's own channel during the match
	struct Peer { UnsignedInt ip; UnicodeString name; };
	std::vector<Peer> s_peers;                   ///< the room's observers at Start (this game included), by address
	UnsignedInt s_peerLocalIP = 0;
	SOCKET s_chatSocket = INVALID_SOCKET;
	Bool s_chatHinted = FALSE;
	const UnsignedInt kChatMagic = 0x0048434Fu | ((UnsignedInt)('0' + LANProtocolVersion) << 24);   ///< "OCH<n>" (O64: the protocol number)
	Int s_upstreamDelay = 0;                     ///< O39: an outside viewer's sources' delay (the host's)
	// O40: the room layout the host kept at Start (plain values only: nothing of it outlives the memory manager)
	struct SlotKeep { Int state, playerTemplate, color, team, startPos; };
	SlotKeep s_layout[MAX_SLOTS];
	Bool s_layoutValid = FALSE;
	Bool s_layoutHostObserves = FALSE;   ///< O60: the admin watched from outside the seats
	Bool s_rematchRequested = FALSE;
	Bool s_rematchApplyPending = FALSE;
	// O42: the host's rules of the running match (RULES), frozen at Start
	Bool s_hostRulesValid = FALSE;
	Int s_hostRulesDelay = 60;
	Bool s_hostRulesOutside = TRUE;
	UnsignedInt s_lastRules = 0;
	// O60 (row 46): the admin who watches his room's match from outside the seats
	Bool s_adminStarting = FALSE;      ///< his stream began at Start; the lobby he goes back to keeps it
	Bool s_adminWatchedMatch = FALSE;  ///< for the replay's score screen ("Play again")

	/// O39: the observers' channel writes what it sends, shows and drops into ObserverChat.log (user data folder), for the
	/// tests (a picture shows a message line only for a few seconds)
	void chatLog(const char *what, UnsignedInt ip, const UnicodeString &text)
	{
		if (TheGlobalData == nullptr) {
			return;
		}
		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat("ObserverChat.log");
		if (FILE *f = fopen(path.str(), "a"))
		{
			AsciiString line;
			line.translate(text);
			fprintf(f, "[%10u] %s %u.%u.%u.%u: %s\n", (unsigned)LanClock::now(), what, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
				line.str());
			fclose(f);
		}
	}

	// O59 (row 47): a defeated player's channel; the room's observers of the match this game plays (the host's START)
	Bool s_defeatedChannel = FALSE;
	UnsignedInt s_lastJoinSent = 0;
	std::vector<Peer> s_matchRoomObservers;
	const UnsignedInt kJoinMagic = 0x004A4F4Fu | ((UnsignedInt)('0' + LANProtocolVersion) << 24);   // "OOJ<n>" (O64: the protocol number)

	void closeObserverChannel()
	{
		if (s_chatSocket != INVALID_SOCKET)
		{
			closesocket(s_chatSocket);
			s_chatSocket = INVALID_SOCKET;
		}
		s_peers.clear();
		s_peerLocalIP = 0;
		s_chatHinted = FALSE;
		s_defeatedChannel = FALSE;
	}

	/// The text socket on this game's LAN address only (never the wildcard address).
	Bool bindChatSocket(UnsignedInt localIP)
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s == INVALID_SOCKET) {
			return FALSE;
		}
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short)LanRoom::OBSERVER_CHAT_PORT);
		addr.sin_addr.s_addr = htonl(localIP);
		if (localIP == 0 || bind(s, (sockaddr *)&addr, sizeof(addr)) != 0)
		{
			closesocket(s);
			return FALSE;
		}
		u_long on = 1;
		ioctlsocket(s, FIONBIO, &on);
		s_chatSocket = s;
		return TRUE;
	}

	/// O59: 'ip' is a player of this match (a human slot) and that player is defeated in this game's copy of the match;
	/// the slot's name in 'name'.
	Bool defeatedMatchPlayer(UnsignedInt ip, UnicodeString *name)
	{
		if (ip == 0 || TheGameInfo == nullptr || ThePlayerList == nullptr || TheVictoryConditions == nullptr) {
			return FALSE;
		}
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const GameSlot *slot = TheGameInfo->getConstSlot(i);
			if (slot == nullptr || !slot->isHuman() || slot->getIP() != ip) {
				continue;
			}
			Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
			if (player == nullptr || player->isPlayerObserver() || !TheVictoryConditions->hasSinglePlayerBeenDefeated(player)) {
				return FALSE;
			}
			*name = slot->getName();
			return TRUE;
		}
		return FALSE;
	}

	/// The observers of the host's last list, without the players; this game included.
	/// O60 (row 46): 'adminIP' (the host, who then is no player) watches from outside the seats and is one of them.
	void openObserverChannel(UnsignedInt hostIP, UnsignedInt localIP, const UnsignedInt *players, Int playerCount,
		UnsignedInt adminIP = 0, const UnicodeString &adminName = UnicodeString::TheEmptyString)
	{
		closeObserverChannel();
		std::map<UnsignedInt, Known>::const_iterator k = s_known.find(hostIP);
		if (k == s_known.end()) {
			return;
		}
		Bool meListed = FALSE;
		for (size_t i = 0; i < k->second.list.size(); ++i)
		{
			const UnsignedInt ip = k->second.list[i].ip;
			Bool player = ip == hostIP && adminIP == 0;
			for (Int p = 0; p < playerCount; ++p) player = player || players[p] == ip;
			if (ip == 0 || player || ip == adminIP) {
				continue;
			}
			Peer peer;
			peer.ip = ip;
			peer.name = k->second.list[i].name;
			s_peers.push_back(peer);
			meListed = meListed || ip == localIP;
		}
		if (adminIP != 0 && meListed)
		{
			Peer admin;
			admin.ip = adminIP;
			admin.name = adminName;
			s_peers.push_back(admin);
		}
		if (!meListed || s_peers.size() < 2)
		{
			s_peers.clear();   // nobody to talk to (or not listed: never a channel)
			return;
		}
		std::sort(s_peers.begin(), s_peers.end(), [](const Peer &a, const Peer &b) { return a.ip < b.ip; });
		s_peerLocalIP = localIP;
		// the text socket: this game's LAN address only, never the wildcard address
		bindChatSocket(localIP);
	}

	const Peer *peerOf(UnsignedInt ip)
	{
		for (size_t i = 0; i < s_peers.size(); ++i) {
			if (s_peers[i].ip == ip) return &s_peers[i];
		}
		return nullptr;
	}

	/// Reads what came on the observers' text socket: only from a listed observer's address and port, bounded.
	void pumpObserverChat()
	{
		if (s_chatSocket == INVALID_SOCKET) {
			return;
		}
		for (Int guard = 0; guard < 32; ++guard)
		{
			unsigned char packet[8 + (g_lanMaxChatLength + 1) * 2];
			sockaddr_in from;
			int fromLen = sizeof(from);
			const int got = recvfrom(s_chatSocket, (char *)packet, sizeof(packet), 0, (sockaddr *)&from, &fromLen);
			if (got <= 0) {
				break;
			}
			const UnsignedInt ip = ntohl(from.sin_addr.s_addr);
			const Peer *peer = peerOf(ip);
			UnsignedInt magic = 0;
			memcpy(&magic, packet, 4);
			// O59 (row 47): a defeated player of this match joins the observers: taken in only when this game's copy of the
			// match has that address as a player who is defeated
			if (got >= 4 && magic == kJoinMagic)
			{
				UnicodeString name;
				if (peer == nullptr && ip != s_peerLocalIP && ntohs(from.sin_port) == LanRoom::OBSERVER_CHAT_PORT
					&& defeatedMatchPlayer(ip, &name))
				{
					Peer joined;
					joined.ip = ip;
					joined.name = name;
					s_peers.push_back(joined);
					std::sort(s_peers.begin(), s_peers.end(), [](const Peer &a, const Peer &b) { return a.ip < b.ip; });
					chatLog("JOINED (defeated player)", ip, name);
				}
				continue;
			}
			if (peer == nullptr || ip == s_peerLocalIP || ntohs(from.sin_port) != LanRoom::OBSERVER_CHAT_PORT || got < 6 || magic != kChatMagic) {
				chatLog("DROPPED from", ip, UnicodeString::TheEmptyString);
				continue;   // not a room observer of this match: dropped unseen
			}
			WideChar line[g_lanMaxChatLength + 1];
			const Int chars = (got - 4) / 2;
			Int n = 0;
			for (; n < chars && n < g_lanMaxChatLength; ++n)
			{
				WideChar c;
				memcpy(&c, packet + 4 + n * 2, 2);
				if (c == 0) break;
				line[n] = c;
			}
			line[n] = 0;
			UnicodeString textLine(line);
			if (TheLanguageFilter != nullptr) {
				TheLanguageFilter->filterLine(textLine);
			}
			if (TheInGameUI != nullptr)
			{
				UnicodeString shown;
				shown.format(textOf(kObserverLine).str(), peer->name.str(), textLine.str());
				TheInGameUI->messageNoFormat(shown);
			}
			chatLog("SHOWN from", ip, textLine);
		}
	}
	// this game as an observer / viewer
	LanRoom::State s_state = LanRoom::STATE_NONE;
	UnsignedInt s_roomHost = 0;
	UnsignedInt s_lastBeat = 0;
	UnsignedInt s_stateSince = 0;
	UnsignedInt s_lastHostList = 0;   ///< O42: the last LIST from the observed room's host
	UnsignedInt s_pendingBecome = 0;
	Bool s_watcher = FALSE;
	AsciiString s_mirror;
	std::vector<UnicodeString> s_notices;

	void notice(const UnicodeString &t) { s_notices.push_back(t); }

	/// O32 (review point 1, applied to O31): text from a packet may lack its terminator; copy at most 'size' - 1 characters
	/// and stop at the end of the field (wcslcpy would read on to the source's terminator, past the packet).
	void copyFromPacket(WideChar *dst, const WideChar *src, Int size)
	{
		Int i = 0;
		for (; i < size - 1 && src[i] != 0; ++i) {
			dst[i] = src[i];
		}
		dst[i] = 0;
	}

	LANGameInfo *hostedGame()
	{
		if (TheLAN == nullptr) {
			return nullptr;
		}
		LANGameInfo *g = TheLAN->GetMyGame();
		return (g != nullptr && g->amIHost() && !g->isGameInProgress()) ? g : nullptr;
	}

	Int humansInSlots(const LANGameInfo *game)
	{
		Int n = 0;
		for (Int i = 0; game != nullptr && i < MAX_SLOTS; ++i)
		{
			const GameSlot *slot = game->getConstSlot(i);
			if (slot != nullptr && slot->isHuman()) {
				++n;
			}
		}
		return n;
	}

	Bool inSlot(const LANGameInfo *game, UnsignedInt ip)
	{
		for (Int i = 0; game != nullptr && i < MAX_SLOTS; ++i)
		{
			const GameSlot *slot = game->getConstSlot(i);
			if (slot != nullptr && slot->isHuman() && slot->getIP() == ip) {
				return TRUE;
			}
		}
		return FALSE;
	}

	void prepare(LANMessage &msg, Int kind, UnsignedInt gameIP)
	{
		memset(&msg, 0, sizeof(msg));
		msg.messageType = LANMessage::MSG_ROOM;
		msg.Room.magic = LANRoomMagic;
		msg.Room.kind = (UnsignedByte)kind;
		msg.Room.gameIP = gameIP;
	}

	void send(LANMessage &msg, UnsignedInt ip)
	{
		if (TheLAN != nullptr) {
			TheLAN->sendRoomMessage(&msg, ip);
		}
	}

	void sendSimple(Int kind, UnsignedInt gameIP, UnsignedInt to, Int count)
	{
		LANMessage msg;
		prepare(msg, kind, gameIP);
		msg.Room.count = (UnsignedByte)count;
		send(msg, to);
	}

	Int clampDelay(Int d) { return d < 10 ? 10 : (d > 180 ? 180 : d); }

	/// The remembered room of 'host' (O33 (low): old rooms are forgotten; at most kKnownMax are kept).
	Known &knownRoom(UnsignedInt host, UnsignedInt now)
	{
		for (std::map<UnsignedInt, Known>::iterator it = s_known.begin(); it != s_known.end();)
		{
			if (now - it->second.seen > kKnownKeepMs) {
				s_known.erase(it++);
			} else {
				++it;
			}
		}
		if (s_known.size() >= kKnownMax && s_known.find(host) == s_known.end())
		{
			std::map<UnsignedInt, Known>::iterator oldest = s_known.begin();
			for (std::map<UnsignedInt, Known>::iterator it = s_known.begin(); it != s_known.end(); ++it) {
				if (now - it->second.seen > now - oldest->second.seen) oldest = it;
			}
			s_known.erase(oldest);
		}
		std::map<UnsignedInt, Known>::iterator found = s_known.find(host);
		if (found == s_known.end())
		{
			Known fresh;
			fresh.seen = now;
			fresh.delay = kDefaultDelay;
			fresh.outsideAllowed = TRUE;
			found = s_known.insert(std::make_pair(host, fresh)).first;
		}
		return found->second;
	}

	void broadcastList(UnsignedInt localIP)
	{
		LANMessage msg;
		prepare(msg, KIND_LIST, localIP);
		Int delay = kDefaultDelay;
		Bool outside = TRUE;
		LanRoom::hostViewerRules(&delay, &outside);
		msg.Room.delaySeconds = (UnsignedShort)delay;
		msg.Room.flags = outside ? FLAG_OUTSIDE_ALLOWED : 0;
		Int n = 0;
		for (size_t i = 0; i < s_hosted.size() && n < LanRoom::MAX_OBSERVERS; ++i, ++n)
		{
			msg.Room.ips[n] = s_hosted[i].ip;
			wcslcpy(msg.Room.u.names[n], s_hosted[i].name.str(), g_lanPlayerNameLength + 1);
		}
		msg.Room.count = (UnsignedByte)n;
		send(msg, 0);
		s_lastList = LanClock::now();
		s_listDirty = FALSE;
		s_listSentNonEmpty = n > 0;
	}

	void setState(LanRoom::State st)
	{
		s_state = st;
		s_stateSince = LanClock::now();
	}

	/// A chat line on this computer's current LAN chat (lobby or game room).
	void showLine(const UnicodeString &line)
	{
		if (TheLAN != nullptr) {
			TheLAN->OnChat(UnicodeString::TheEmptyString, 0, line, LANAPIInterface::LANCHAT_SYSTEM);
		}
	}

	/// The players' addresses of a game (the stream sources).
	Int playerAddresses(const LANGameInfo *game, UnsignedInt *ips, Int maxCount)
	{
		Int n = 0;
		for (Int i = 0; game != nullptr && i < MAX_SLOTS && n < maxCount; ++i)
		{
			const GameSlot *slot = game->getConstSlot(i);
			if (slot != nullptr && slot->isHuman() && slot->getIP() != 0) {
				ips[n++] = slot->getIP();
			}
		}
		return n;
	}

	/// 'hostDelay' and 'outsideAllowed': the host's rules; 'liveIps': for a room observer the other room observers (they get
	/// its relay live too); 'extraSeconds': the viewer's own added delay.
	void beginStream(const UnsignedInt *ips, Int count, Int port, UnsignedInt localIP, Bool roomObserver, Int extraSeconds,
		Int hostDelay, Bool outsideAllowed, const UnsignedInt *liveIps, Int liveCount, const UnsignedInt *observerIps = nullptr,
		Int observerCount = 0)
	{
		s_adminWatchedMatch = FALSE;   // O60: hostStart sets it again for the admin's own stream
		// the viewer relays its copy on its own LAN address (viewers feed viewers). O33: a room observer's relay is live for
		// the other room observers and delayed by the host's delay for everybody else; an outside viewer's copy is delayed
		// already, so its relay forwards what it has (never more than it was given); none when outside viewers are not
		// allowed and nobody else may have it live.
		// O62 (H5 #3): the match's players ('ips') are never served by this relay either
		if (outsideAllowed || liveCount > 0) {
			ReplayStream::configureMatchServing(localIP, LanRoom::STREAM_PORT, hostDelay, outsideAllowed, liveIps, liveCount, !roomObserver,
				ips, count);
		} else {
			ReplayStream::stopServing();
		}
		ReplayStream::configureWatchList(ips, count, port, roomObserver, localIP, observerIps, observerCount);
		ReplayFollow::configure(extraSeconds, 2.0f);
		// O34: an outside viewer keeps a play-out reserve that bridges a source change; a room observer watches live
		ReplayFollow::configureReserve(roomObserver ? 0 : LanRoom::VIEWER_RESERVE_SECONDS);
		if (!ReplayStream::beginWatching(s_mirror))
		{
			notice(textOf(kLost));
			setState(LanRoom::STATE_NONE);
			return;
		}
		setState(LanRoom::STATE_STARTING);
	}
}

//-------------------------------------------------------------------------------------------------
Int LanRoom::capacity(const LANGameInfo *game)
{
	// O56 (row 42): the player places of the map (the slots not closed) and the observer places make 12
	Int playerPlaces = 0;
	for (Int i = 0; game != nullptr && i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot != nullptr && slot->getState() != SLOT_CLOSED) {
			++playerPlaces;
		}
	}
	// O60: the admin watching from outside the seats takes one of them
	const Int adminPlace = (game != nullptr && game->getHostObserves()) ? 1 : 0;
	// never fewer places than the people a room had by the old rule (12 - humans in slots) already holds
	const Int places = max(MAX_PEOPLE - playerPlaces - adminPlace, min((Int)s_hosted.size(), MAX_PEOPLE - humansInSlots(game) - adminPlace));
	return places > MAX_OBSERVERS ? MAX_OBSERVERS : (places < 0 ? 0 : places);
}

void LanRoom::onMessage(const LANMessage *msg, UnsignedInt senderIP, UnsignedInt localIP)
{
	const UnsignedInt now = LanClock::now();
	switch (msg->Room.kind)
	{
		case KIND_OBSERVE:
		{
			LANGameInfo *game = hostedGame();
			if (game == nullptr || msg->Room.gameIP != localIP)
			{
				sendSimple(KIND_REMOVED, msg->Room.gameIP, senderIP, REASON_NO_ROOM);
				return;
			}
			if (inSlot(game, senderIP)) {
				return;   // a player of this room asks while leaving its slot: it comes again from the lobby
			}
			for (size_t i = 0; i < s_hosted.size(); ++i)
			{
				if (s_hosted[i].ip == senderIP)
				{
					s_hosted[i].lastHeard = now;
					return;
				}
			}
			if ((Int)s_hosted.size() >= capacity(game))
			{
				sendSimple(KIND_REMOVED, localIP, senderIP, REASON_FULL);
				return;
			}
			Entry e;
			e.ip = senderIP;
			// O33 (review 6): the name this host has for that address (its lobby list); the packet's name only when the lobby
			// does not know the address
			LANPlayer *known = TheLAN != nullptr ? TheLAN->LookupPlayer(senderIP) : nullptr;
			if (known != nullptr) {
				e.name = known->getName();
			} else {
				WideChar sender[g_lanPlayerNameLength + 1];
				copyFromPacket(sender, msg->name, g_lanPlayerNameLength + 1);
				e.name = UnicodeString(sender);
			}
			e.lastHeard = now;
			s_hosted.push_back(e);
			s_listDirty = TRUE;
			break;
		}
		case KIND_LEAVE:
			for (size_t i = 0; i < s_hosted.size(); ++i)
			{
				if (s_hosted[i].ip == senderIP)
				{
					s_hosted.erase(s_hosted.begin() + i);
					s_listDirty = TRUE;
					break;
				}
			}
			break;
		case KIND_LIST:
		{
			if (msg->Room.gameIP != senderIP) {
				return;
			}
			Known &k = knownRoom(senderIP, now);
			k.list.clear();
			k.seen = now;
			// a list without the O33 fields (delay 0) means the defaults
			k.delay = msg->Room.delaySeconds != 0 ? clampDelay(msg->Room.delaySeconds) : kDefaultDelay;
			k.outsideAllowed = msg->Room.delaySeconds == 0 || (msg->Room.flags & FLAG_OUTSIDE_ALLOWED) != 0;
			Bool me = FALSE;
			for (Int i = 0; i < msg->Room.count && i < MAX_OBSERVERS; ++i)
			{
				Observer o;
				o.ip = msg->Room.ips[i];
				WideChar name[g_lanPlayerNameLength + 1];
				copyFromPacket(name, msg->Room.u.names[i], g_lanPlayerNameLength + 1);
				o.name = UnicodeString(name);
				k.list.push_back(o);
				me = me || o.ip == localIP;
			}
			if (s_roomHost == senderIP) {
				s_lastHostList = now;   // O42: the room is there
			}
			if (s_roomHost == senderIP && me && s_state == STATE_ASKED)
			{
				setState(STATE_IN_ROOM);
				notice(textOf(kObserving));
			}
			else if (s_roomHost == senderIP && !me && s_state == STATE_IN_ROOM)
			{
				// O42: the host's list lost this observer (it had it while this computer still sat in a slot, or it missed a
				// heartbeat): ask again at once; a removal by the host comes as REMOVED
				s_state = STATE_ASKED;
				s_lastBeat = now;
				sendSimple(KIND_OBSERVE, s_roomHost, s_roomHost, 0);
			}
			break;
		}
		case KIND_ASK_MAP:
		{
			// O62 (H5 #6): this player's room's host did not hear whether it has the map: it says so again
			LANGameInfo *mine = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
			if (mine != nullptr && mine->getHostIP() == senderIP && msg->Room.gameIP == senderIP && !mine->isGameInProgress()) {
				TheLAN->RequestHasMap();
			}
			break;
		}
		case KIND_RULES:
		{
			// O42: a running match's rules from its host: the lobby disables Watch when it allows no outside viewers
			if (msg->Room.gameIP != senderIP || msg->Room.delaySeconds == 0) {
				return;
			}
			Known &k = knownRoom(senderIP, now);
			k.seen = now;
			k.delay = clampDelay(msg->Room.delaySeconds);
			k.outsideAllowed = (msg->Room.flags & FLAG_OUTSIDE_ALLOWED) != 0;
			break;
		}
		case KIND_REMOVED:
			if (s_roomHost == senderIP && (s_state == STATE_ASKED || s_state == STATE_IN_ROOM))
			{
				setState(STATE_NONE);
				s_roomHost = 0;
				notice(textOf(msg->Room.count == REASON_FULL ? kFull : (msg->Room.count == REASON_NO_ROOM ? kNoRoom : kRemoved)));
			}
			break;
		case KIND_CHAT:
		{
			WideChar text[g_lanMaxChatLength + 1];
			copyFromPacket(text, msg->Room.u.text, g_lanMaxChatLength + 1);
			LANGameInfo *game = hostedGame();
			const Entry *from = nullptr;
			for (size_t i = 0; i < s_hosted.size() && from == nullptr; ++i) {
				if (s_hosted[i].ip == senderIP) from = &s_hosted[i];
			}
			if (game != nullptr && from != nullptr)
			{
				// the host: the line with the observer's name (O33, review 6: the name the host keeps for that address, not
				// the packet's), here and to everybody in the room
				UnicodeString line;
				line.format(L"%ls (%ls): %ls", from->name.str(), textOf(kRole).str(), text);
				showLine(line);
				LANMessage relay;
				prepare(relay, KIND_CHAT, localIP);
				wcslcpy(relay.Room.u.text, line.str(), g_lanMaxChatLength + 1);
				for (Int i = 0; i < MAX_SLOTS; ++i)
				{
					const GameSlot *slot = game->getConstSlot(i);
					if (slot != nullptr && slot->isHuman() && slot->getIP() != localIP) {
						send(relay, slot->getIP());
					}
				}
				for (size_t i = 0; i < s_hosted.size(); ++i) {
					send(relay, s_hosted[i].ip);
				}
			}
			else
			{
				// a player of the room, or an observer: a ready line from the room's host
				LANGameInfo *mine = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
				const Bool myRoom = (mine != nullptr && mine->getHostIP() == senderIP) || (s_roomHost == senderIP && s_state == STATE_IN_ROOM);
				if (myRoom) {
					showLine(UnicodeString(text));
				}
			}
			break;
		}
		case KIND_START:
		{
			const Int delay = msg->Room.delaySeconds != 0 ? clampDelay(msg->Room.delaySeconds) : kDefaultDelay;
			const Bool outside = msg->Room.delaySeconds == 0 || (msg->Room.flags & FLAG_OUTSIDE_ALLOWED) != 0;
			if (msg->Room.flags & FLAG_TO_PLAYER)
			{
				// O33: a player of this host's room: the observers frozen at Start (only from the room's host)
				LANGameInfo *mine = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
				if (mine == nullptr || mine->getHostIP() != senderIP || msg->Room.gameIP != senderIP) {
					return;
				}
				s_matchStart.valid = TRUE;
				s_matchStart.host = senderIP;
				s_matchStart.observers.clear();
				for (Int i = 0; i < msg->Room.count && i < MAX_OBSERVERS; ++i) {
					if (msg->Room.ips[i] != 0 && !inSlot(mine, msg->Room.ips[i])) s_matchStart.observers.push_back(msg->Room.ips[i]);
				}
				s_matchStart.delay = delay;
				s_matchStart.outsideAllowed = outside;
				return;
			}
			if (s_roomHost != senderIP || (s_state != STATE_ASKED && s_state != STATE_IN_ROOM)) {
				return;
			}
			UnsignedInt ips[MAX_SLOTS];
			Int n = 0;
			for (Int i = 0; i < msg->Room.count && i < MAX_SLOTS; ++i) {
				ips[n++] = msg->Room.ips[i];
			}
			// the other room observers get this observer's relay live too (the host's last list)
			UnsignedInt live[MAX_OBSERVERS];
			Int liveCount = 0;
			std::map<UnsignedInt, Known>::const_iterator k = s_known.find(senderIP);
			for (size_t i = 0; k != s_known.end() && i < k->second.list.size() && liveCount < MAX_OBSERVERS; ++i)
			{
				const UnsignedInt ip = k->second.list[i].ip;
				Bool player = FALSE;
				for (Int p = 0; p < n; ++p) player = player || ips[p] == ip;
				if (ip != 0 && ip != localIP && !player) live[liveCount++] = ip;
			}
			// O60 (row 46): the admin watches the match from outside the seats: he gets this observer's relay live too and
			// is one of the room's observers in their channel
			const Bool hostWatches = (msg->Room.flags & FLAG_HOST_WATCHES) != 0;
			if (hostWatches && liveCount < MAX_OBSERVERS) {
				live[liveCount++] = senderIP;
			}
			UnicodeString adminName;
			if (hostWatches && TheLAN != nullptr && TheLAN->LookupGameByHost(senderIP) != nullptr) {
				adminName = TheLAN->LookupGameByHost(senderIP)->getHostName();
			}
			s_watcher = FALSE;
			notice(textOf(kStarting));
			beginStream(ips, n, msg->Room.port != 0 ? msg->Room.port : STREAM_PORT, localIP, TRUE, 0, delay, outside, live, liveCount);
			// O34: the room observers' own text and voice during the match (the host's last list, players left out)
			openObserverChannel(senderIP, localIP, ips, n, hostWatches ? senderIP : 0, adminName);
			break;
		}
		default:
			break;
	}
}

void LanRoom::update(UnsignedInt localIP)
{
	const UnsignedInt now = LanClock::now();
	// host: forget the silent ones, send the list
	LANGameInfo *game = hostedGame();
	if (game != nullptr)
	{
		for (size_t i = 0; i < s_hosted.size();)
		{
			if (now - s_hosted[i].lastHeard > kForgetMs || inSlot(game, s_hosted[i].ip))
			{
				s_hosted.erase(s_hosted.begin() + i);
				s_listDirty = TRUE;
			}
			else
			{
				++i;
			}
		}
		// O33 (low): a join filled a slot: the newest observers over the room's places go
		while ((Int)s_hosted.size() > capacity(game))
		{
			sendSimple(KIND_REMOVED, localIP, s_hosted.back().ip, REASON_FULL);
			s_hosted.pop_back();
			s_listDirty = TRUE;
		}
		// O42: also with no observers: the lobby learns the delay and the outside-viewers flag of every room
		// O46: with no observers only every 5 s (the lobby needs only the delay and the flag from it); each LIST is one
		// packet per lobby, in the loopback test eight of them
		if (s_listDirty || now - s_lastList >= (s_hosted.empty() ? kEmptyListMs : kListMs)) {
			broadcastList(localIP);
		}
	}
	else if (!s_hosted.empty() || s_listSentNonEmpty)
	{
		s_hosted.clear();   // no longer hosting a room in the lobby
		s_listSentNonEmpty = FALSE;
	}

	// observer: "still here"; a room that vanished
	if (s_state == STATE_ASKED || s_state == STATE_IN_ROOM)
	{
		// O42: every second until the host's list has this observer (the host ignores it while this computer still sits
		// in one of its slots), then every 4 s
		if (now - s_lastBeat >= (s_state == STATE_ASKED ? kAskMs : kHeartbeatMs))
		{
			s_lastBeat = now;
			sendSimple(KIND_OBSERVE, s_roomHost, s_roomHost, 0);
		}
		// O42: gone only when the lobby has no such game and its host's LIST has not come for 10 s (a player who just
		// left a room gets that game back in its lobby only with the host's next announce, up to 10 s later)
		if (TheLAN != nullptr && TheLAN->LookupGameByHost(s_roomHost) == nullptr && now - s_stateSince > kGoneMs
			&& now - s_lastHostList > kGoneMs)
		{
			setState(STATE_NONE);
			s_roomHost = 0;
			notice(textOf(kGone));
		}
	}
	// a player who chose "Become observer": once back in the lobby, ask that room for a place
	if (s_pendingBecome != 0 && TheLAN != nullptr && TheLAN->GetMyGame() == nullptr)
	{
		const UnsignedInt host = s_pendingBecome;
		s_pendingBecome = 0;
		startObserving(host);
	}
	// the stream: lost, or too long without its header (O33, review 8: the replay itself starts in pollStart)
	if (s_state == STATE_STARTING && !ReplayStream::headerReady())
	{
		Int hostDelay = kDefaultDelay;
		Bool outside = TRUE;
		const Bool rulesKnown = s_watcher && viewerRulesOf(s_roomHost, &hostDelay, &outside);
		if (rulesKnown && !outside)
		{
			// O42: the host's RULES say no outside viewers (its players do not even listen): a clear text, no wait
			notice(textOf(kNoOutside));
			resetViewer();
		}
		else if (ReplayStream::takeLostNotice() || now - s_stateSince > kStreamWaitMs)
		{
			// O42: a source that refused because the host allows no outside viewers says so
			notice(textOf(ReplayStream::lostBecausePlayer() ? kThisPcPlays : (ReplayStream::lostBecauseNotAllowed() ? kNoOutside : kLost)));
			resetViewer();
		}
	}
}

void LanRoom::pollStart()
{
	// O61 (row 49): an outside viewer's files: the ones still held are deleted when they can be; an earlier crashed
	// viewer's at the start
	static Bool s_swept = FALSE;
	if (!s_swept && TheGlobalData != nullptr)
	{
		s_swept = TRUE;
		ReplayStream::sweepViewerLeftovers();
	}
	ReplayStream::pumpDeletes();
	pumpObserverChat();
	if (s_state == STATE_STARTING)
	{
		if (ReplayStream::headerReady() && TheRecorder != nullptr && TheWritableGlobalData != nullptr
			&& (TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()))
		{
			setState(STATE_PLAYING);
			s_sawGame = FALSE;
			TheWritableGlobalData->m_loadReplayGame = s_mirror;
			TheRecorder->loadQueuedReplay();   // the mirror, in the follow mode (ReplayStream::isWatching)
		}
		return;
	}
	if (s_state == STATE_PLAYING && TheGameLogic != nullptr && !ReplayFollow::restarting())   // O61: not a rewind's restart
	{
		// O33: after the replay (back in the menus, wherever): follow mode off, no mirror, no relay
		if (TheGameLogic->isInReplayGame()) {
			s_sawGame = TRUE;
		} else if (s_sawGame || LanClock::now() - s_stateSince > 120000) {
			resetViewer();
		}
	}
}

void LanRoom::cancelStarting()
{
	if (s_state == STATE_STARTING) {
		resetViewer();
	}
}

static Int s_hostDelay = -1;   ///< O56 (row 43): the delay the host chose in the room; -1: Options.ini's

void LanRoom::setHostDelay(Int seconds)
{
	s_hostDelay = clampDelay(seconds);
}

void LanRoom::resetHostDelay()
{
	s_hostDelay = -1;
}

void LanRoom::hostViewerRules(Int *delaySeconds, Bool *outsideAllowed)
{
	if (s_hostDelay >= 0) {
		*delaySeconds = s_hostDelay;
	} else {
		OptionPreferences pref;
		OptionPreferences::const_iterator it = pref.find("LanWatchDelay");
		*delaySeconds = it != pref.end() ? clampDelay(atoi(it->second.str())) : kDefaultDelay;
	}
	// TheSuperHackers @tweak O56 (row 43) Outside viewers are always allowed ("people want to watch anyway"); an old
	// Options.ini LanAllowViewers = no counts as yes.
	*outsideAllowed = TRUE;
}

Bool LanRoom::viewerRulesOf(UnsignedInt hostIP, Int *delaySeconds, Bool *outsideAllowed)
{
	std::map<UnsignedInt, Known>::const_iterator it = s_known.find(hostIP);
	if (it == s_known.end()) {
		return FALSE;
	}
	*delaySeconds = it->second.delay;
	*outsideAllowed = it->second.outsideAllowed;
	return TRUE;
}

void LanRoom::hostRemove(UnsignedInt ip)
{
	for (size_t i = 0; i < s_hosted.size(); ++i)
	{
		if (s_hosted[i].ip == ip)
		{
			s_hosted.erase(s_hosted.begin() + i);
			s_listDirty = TRUE;
			sendSimple(KIND_REMOVED, TheLAN != nullptr ? TheLAN->GetLocalIP() : 0, ip, REASON_BY_HOST);
			return;
		}
	}
}

void LanRoom::hostDropJoined(UnsignedInt ip)
{
	for (size_t i = 0; i < s_hosted.size(); ++i)
	{
		if (s_hosted[i].ip == ip)
		{
			s_hosted.erase(s_hosted.begin() + i);
			s_listDirty = TRUE;
			return;
		}
	}
}

void LanRoom::hostStart(LANGameInfo *game, UnsignedInt localIP)
{
	if (game == nullptr) {
		return;
	}
	Int delay = kDefaultDelay;
	Bool outside = TRUE;
	hostViewerRules(&delay, &outside);
	// O33 (review 2): the observers valid at Start, frozen; a player of the match is never one
	std::vector<UnsignedInt> observers;
	for (size_t i = 0; i < s_hosted.size() && (Int)observers.size() < MAX_OBSERVERS; ++i) {
		if (s_hosted[i].ip != 0 && s_hosted[i].ip != localIP && !inSlot(game, s_hosted[i].ip)) observers.push_back(s_hosted[i].ip);
	}
	// O34: the final list once more, so every observer starts its channel with the same list
	if (!observers.empty()) {
		broadcastList(localIP);
	}
	s_matchStart.valid = TRUE;
	s_matchStart.host = localIP;
	s_matchStart.observers = observers;
	s_matchStart.delay = delay;
	s_matchStart.outsideAllowed = outside;
	// O42: the rules the host repeats during the match (RULES)
	s_hostRulesValid = TRUE;
	s_hostRulesDelay = delay;
	s_hostRulesOutside = outside;
	s_lastRules = 0;

	UnsignedInt ips[MAX_SLOTS];
	const Int n = playerAddresses(game, ips, MAX_SLOTS);
	// O60 (row 46): the admin who watches from outside the seats is not in the match; the players' servers have him live
	// like a room observer
	const Bool adminWatches = game->getHostObserves() && !inSlot(game, localIP);
	// to the players: the observer list, the delay and the flag (their servers decide by it)
	{
		LANMessage msg;
		prepare(msg, KIND_START, localIP);
		Int count = 0;
		for (size_t i = 0; i < observers.size() && count < MAX_OBSERVERS; ++i) {
			msg.Room.ips[count++] = observers[i];
		}
		if (adminWatches && count < MAX_OBSERVERS) {
			msg.Room.ips[count++] = localIP;
		}
		msg.Room.count = (UnsignedByte)count;
		msg.Room.port = STREAM_PORT;
		msg.Room.delaySeconds = (UnsignedShort)delay;
		msg.Room.flags = (UnsignedShort)(FLAG_TO_PLAYER | (outside ? FLAG_OUTSIDE_ALLOWED : 0));
		for (Int i = 0; i < n; ++i) {
			if (ips[i] != localIP) send(msg, ips[i]);
		}
	}
	// to the observers: the players' addresses
	if (!observers.empty())
	{
		LANMessage msg;
		prepare(msg, KIND_START, localIP);
		for (Int i = 0; i < n; ++i) {
			msg.Room.ips[i] = ips[i];
		}
		msg.Room.count = (UnsignedByte)n;
		msg.Room.port = STREAM_PORT;
		msg.Room.delaySeconds = (UnsignedShort)delay;
		msg.Room.flags = (UnsignedShort)((outside ? FLAG_OUTSIDE_ALLOWED : 0) | (adminWatches ? FLAG_HOST_WATCHES : 0));
		for (size_t i = 0; i < observers.size(); ++i) {
			send(msg, observers[i]);
		}
	}
	if (adminWatches)
	{
		// O60: the admin follows the players' stream live as a room observer does (LANAPI::OnGameStart takes him out of
		// the room without a word to the players), and talks with the room's observers frozen above
		s_matchStart.valid = FALSE;
		UnsignedInt live[MAX_OBSERVERS];
		Int liveCount = 0;
		for (size_t i = 0; i < observers.size() && liveCount < MAX_OBSERVERS; ++i) {
			live[liveCount++] = observers[i];
		}
		s_watcher = FALSE;
		s_roomHost = localIP;
		notice(textOf(kStarting));
		beginStream(ips, n, STREAM_PORT, localIP, TRUE, 0, delay, outside, live, liveCount);
		const Bool started = s_state == STATE_STARTING;
		s_adminStarting = started;
		s_adminWatchedMatch = started;
		closeObserverChannel();
		for (size_t i = 0; started && i < s_hosted.size(); ++i)
		{
			if (std::find(observers.begin(), observers.end(), s_hosted[i].ip) == observers.end()) {
				continue;
			}
			Peer p;
			p.ip = s_hosted[i].ip;
			p.name = s_hosted[i].name;
			s_peers.push_back(p);
		}
		if (!s_peers.empty())
		{
			Peer me;
			me.ip = localIP;
			me.name = TheLAN != nullptr ? TheLAN->GetMyName() : UnicodeString::TheEmptyString;
			s_peers.push_back(me);
			std::sort(s_peers.begin(), s_peers.end(), [](const Peer &a, const Peer &b) { return a.ip < b.ip; });
			s_peerLocalIP = localIP;
			if (!bindChatSocket(localIP))
			{
				s_peers.clear();
				s_peerLocalIP = 0;
			}
		}
	}
	s_hosted.clear();   // they follow the stream now; nobody waits for them
}

void LanRoom::configureServing(UnsignedInt localIP)
{
	s_adminWatchedMatch = FALSE;   // O60: this computer plays this match
	{
		// O40: only the host of this match may offer "Play again" afterwards
		LANGameInfo *mine = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
		if (mine == nullptr || !mine->amIHost()) {
			s_layoutValid = FALSE;
		}
	}
	// every player serves its replay on its LAN address: live to the room's observers of the host's START, delayed by the
	// host's delay to everybody else (outside viewers per source: 4, or -serveReplayOutsideMax)
	LANGameInfo *game = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
	const UnsignedInt host = game != nullptr ? game->getHostIP() : 0;
	std::vector<UnsignedInt> observers;
	Int delay = kDefaultDelay;
	Bool outside = TRUE;
	if (s_matchStart.valid && s_matchStart.host == host)
	{
		observers = s_matchStart.observers;
		delay = s_matchStart.delay;
		outside = s_matchStart.outsideAllowed;
	}
	else
	{
		// no START came: the host's last list (LIST is from the host's own address only)
		std::map<UnsignedInt, Known>::const_iterator it = s_known.find(host);
		if (it != s_known.end() && LanClock::now() - it->second.seen < 60000)
		{
			for (size_t i = 0; i < it->second.list.size(); ++i) observers.push_back(it->second.list[i].ip);
			delay = it->second.delay;
			outside = it->second.outsideAllowed;
		}
	}
	s_matchStart.valid = FALSE;
	// O59 (row 47): the room's observers of this match, for a player who joins them after a defeat (names from the
	// host's last LIST)
	s_matchRoomObservers.clear();
	{
		std::map<UnsignedInt, Known>::const_iterator k = s_known.find(host);
		for (size_t i = 0; i < observers.size(); ++i)
		{
			if (observers[i] == 0 || observers[i] == localIP || inSlot(game, observers[i])) {
				continue;
			}
			Peer p;
			p.ip = observers[i];
			if (k != s_known.end()) {
				for (size_t n = 0; n < k->second.list.size(); ++n) {
					if (k->second.list[n].ip == observers[i]) p.name = k->second.list[n].name;
				}
			}
			// O60 (row 46): the admin who watches from outside the seats is not in his own LIST
			if (p.name.isEmpty() && game != nullptr && observers[i] == host) {
				p.name = game->getHostName();
			}
			s_matchRoomObservers.push_back(p);
		}
	}
	UnsignedInt live[MAX_OBSERVERS];
	Int liveCount = 0;
	for (size_t i = 0; i < observers.size() && liveCount < MAX_OBSERVERS; ++i) {
		if (observers[i] != 0 && observers[i] != localIP && !inSlot(game, observers[i])) live[liveCount++] = observers[i];
	}
	s_matchOthers.clear();
	s_matchObservers = liveCount;
	s_lastCount = 0;
	if (liveCount == 0 && !outside)
	{
		// nobody may watch: no listening at all
		ReplayStream::stopServing();
		s_matchServing = FALSE;
		return;
	}
	// O62 (H5 #3): this match's players (this computer too: a second game on it) never get its stream
	UnsignedInt players[MAX_SLOTS];
	const Int playerCount = playerAddresses(game, players, MAX_SLOTS);
	ReplayStream::configureMatchServing(localIP, STREAM_PORT, delay, outside, live, liveCount, FALSE, players, playerCount);
	s_matchServing = TRUE;
}

void LanRoom::matchUpdate(UnsignedInt localIP)
{
	LANGameInfo *game = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
	const UnsignedInt now = LanClock::now();
	// O59 (row 47): a defeated player who joined the observers: their text, and JOIN every second to the observers and
	// the match's other players (each takes him in once its copy of the match shows him defeated)
	if (s_defeatedChannel && s_chatSocket != INVALID_SOCKET)
	{
		pumpObserverChat();
		if (now - s_lastJoinSent >= 1000)
		{
			s_lastJoinSent = now;
			std::vector<UnsignedInt> to;
			for (size_t i = 0; i < s_peers.size(); ++i) {
				if (s_peers[i].ip != s_peerLocalIP) to.push_back(s_peers[i].ip);
			}
			for (Int i = 0; TheGameInfo != nullptr && i < MAX_SLOTS; ++i)
			{
				const GameSlot *slot = TheGameInfo->getConstSlot(i);
				if (slot != nullptr && slot->isHuman() && slot->getIP() != 0 && slot->getIP() != s_peerLocalIP
					&& std::find(to.begin(), to.end(), slot->getIP()) == to.end()) {
					to.push_back(slot->getIP());
				}
			}
			for (size_t i = 0; i < to.size(); ++i)
			{
				sockaddr_in addr;
				memset(&addr, 0, sizeof(addr));
				addr.sin_family = AF_INET;
				addr.sin_port = htons((unsigned short)OBSERVER_CHAT_PORT);
				addr.sin_addr.s_addr = htonl(to[i]);
				sendto(s_chatSocket, (const char *)&kJoinMagic, 4, 0, (sockaddr *)&addr, sizeof(addr));
			}
		}
	}
	// O42: the host tells the lobby its rules while the match runs (also when nobody may watch and nothing serves)
	if (game != nullptr && s_hostRulesValid && game->isGameInProgress() && game->getHostIP() == localIP && now - s_lastRules >= kRulesMs)
	{
		s_lastRules = now;
		LANMessage rules;
		prepare(rules, KIND_RULES, localIP);
		rules.Room.delaySeconds = (UnsignedShort)s_hostRulesDelay;
		rules.Room.flags = (UnsignedShort)(s_hostRulesOutside ? FLAG_OUTSIDE_ALLOWED : 0);
		send(rules, 0);
	}
	if (!s_matchServing || game == nullptr || now - s_lastCount < kCountMs) {
		return;
	}
	s_lastCount = now;
	Int room = 0, outside = 0;
	ReplayStream::servingCounts(&room, &outside);
	LANMessage msg;
	prepare(msg, KIND_COUNT, game->getHostIP());
	msg.Room.count = (UnsignedByte)(outside > 255 ? 255 : outside);
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() != 0 && slot->getIP() != localIP) {
			send(msg, slot->getIP());
		}
	}
}

void LanRoom::onMatchMessage(const LANMessage *msg, UnsignedInt senderIP)
{
	LANGameInfo *game = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
	if (msg->Room.kind == KIND_COUNT && game != nullptr && inSlot(game, senderIP) && msg->Room.gameIP == game->getHostIP()) {
		s_matchOthers[senderIP] = msg->Room.count;
	}
}

Bool LanRoom::matchWatchers(Int *roomObservers, Int *outsideViewers)
{
	if (!s_matchServing || TheGameLogic == nullptr || !TheGameLogic->isInLanGame()) {
		return FALSE;
	}
	Int room = 0, outside = 0;
	ReplayStream::servingCounts(&room, &outside);
	for (std::map<UnsignedInt, Int>::const_iterator it = s_matchOthers.begin(); it != s_matchOthers.end(); ++it) {
		outside += it->second;
	}
	*roomObservers = s_matchObservers;
	*outsideViewers = outside;
	return TRUE;
}

std::vector<LanRoom::Observer> LanRoom::observersOf(UnsignedInt hostIP)
{
	std::vector<Observer> out;
	if (TheLAN != nullptr && hostIP == TheLAN->GetLocalIP() && hostedGame() != nullptr)
	{
		for (size_t i = 0; i < s_hosted.size(); ++i)
		{
			Observer o;
			o.ip = s_hosted[i].ip;
			o.name = s_hosted[i].name;
			out.push_back(o);
		}
		return out;
	}
	std::map<UnsignedInt, Known>::const_iterator it = s_known.find(hostIP);
	if (it != s_known.end() && LanClock::now() - it->second.seen < kKnownMs) {
		out = it->second.list;
	}
	return out;
}

LanRoom::State LanRoom::state() { return s_state; }
UnsignedInt LanRoom::observedHost() { return s_roomHost; }
Bool LanRoom::isWatcher() { return s_watcher; }

void LanRoom::startObserving(UnsignedInt hostIP)
{
	if (hostIP == 0 || s_state == STATE_STARTING || s_state == STATE_PLAYING) {
		return;
	}
	if (TheLAN != nullptr && TheLAN->LookupGameByHost(hostIP) != nullptr && !TheLAN->LookupGameByHost(hostIP)->isHostVerified()) {
		return;   // O64 (H6 #2): only a room its host himself announced is observed
	}
	if (TheLAN != nullptr && TheLAN->LookupGameByHost(hostIP) != nullptr
		&& !LanSameVersion(TheLAN->LookupGameByHost(hostIP)))   // O62/O63
	{
		notice(textOf(kOtherVersion));   // O62: another version's room is not observed
		return;
	}
	if (TheLAN != nullptr && hostIP == TheLAN->GetLocalIP()) {
		return;   // O33 (low): not one's own room
	}
	if (s_roomHost != 0 && s_roomHost != hostIP) {
		stopObserving();
	}
	s_roomHost = hostIP;
	s_watcher = FALSE;
	setState(STATE_ASKED);
	s_lastBeat = LanClock::now();
	s_lastHostList = s_lastBeat;   // O42: 10 s for its first LIST or the lobby's game
	sendSimple(KIND_OBSERVE, hostIP, hostIP, 0);
}

void LanRoom::stopObserving()
{
	if ((s_state == STATE_ASKED || s_state == STATE_IN_ROOM) && s_roomHost != 0)
	{
		sendSimple(KIND_LEAVE, s_roomHost, s_roomHost, 0);
		notice(textOf(kLeft));
	}
	if (s_state != STATE_STARTING && s_state != STATE_PLAYING)
	{
		setState(STATE_NONE);
		s_roomHost = 0;
	}
}

void LanRoom::requestBecomeObserver(UnsignedInt hostIP)
{
	s_pendingBecome = hostIP;
}

void LanRoom::sendChat(const UnicodeString &text)
{
	if (s_state != STATE_IN_ROOM || s_roomHost == 0 || text.isEmpty()) {
		return;
	}
	LANMessage msg;
	prepare(msg, KIND_CHAT, s_roomHost);
	wcslcpy(msg.Room.u.text, text.str(), g_lanMaxChatLength + 1);
	send(msg, s_roomHost);
}

void LanRoom::startWatching(LANGameInfo *game, Int extraSeconds)
{
	if (game == nullptr || TheLAN == nullptr || s_state == STATE_STARTING || s_state == STATE_PLAYING) {
		return;
	}
	if (!LanSameVersion(game))
	{
		notice(textOf(kOtherVersion));   // O62/O63: another version's (or build's) match is not watched
		return;
	}
	if (game->getHostIP() == TheLAN->GetLocalIP()) {
		return;
	}
	Int hostDelay = kDefaultDelay;
	Bool outside = TRUE;
	viewerRulesOf(game->getHostIP(), &hostDelay, &outside);
	if (!outside)
	{
		notice(textOf(kNoOutside));
		return;
	}
	stopObserving();
	UnsignedInt ips[MAX_SLOTS];
	const Int n = playerAddresses(game, ips, MAX_SLOTS);
	if (n == 0) {
		return;
	}
	// O33: the players' servers keep the host's delay; the viewer's own choice only adds to it
	extraSeconds = extraSeconds < 0 ? 0 : (extraSeconds > 180 ? 180 : extraSeconds);
	// O34: the room's observers (their relays) are asked before the players
	UnsignedInt observerIps[MAX_OBSERVERS];
	Int observerCount = 0;
	std::map<UnsignedInt, Known>::const_iterator k = s_known.find(game->getHostIP());
	for (size_t i = 0; k != s_known.end() && i < k->second.list.size() && observerCount < MAX_OBSERVERS; ++i)
	{
		const UnsignedInt ip = k->second.list[i].ip;
		Bool player = FALSE;
		for (Int p = 0; p < n; ++p) player = player || ips[p] == ip;
		if (ip != 0 && !player && ip != TheLAN->GetLocalIP()) observerIps[observerCount++] = ip;
	}
	s_watcher = TRUE;
	s_roomHost = game->getHostIP();
	UnicodeString t;
	t.format(textOf(kWatchingTotal).str(), hostDelay + extraSeconds, hostDelay, extraSeconds);
	notice(t);
	beginStream(ips, n, STREAM_PORT, TheLAN->GetLocalIP(), FALSE, extraSeconds, hostDelay, TRUE, nullptr, 0, observerIps, observerCount);
	s_upstreamDelay = hostDelay;   // O39
}

void LanRoom::resetViewer()
{
	if (s_state == STATE_STARTING || s_state == STATE_PLAYING)
	{
		if (s_state == STATE_STARTING) {
			s_adminWatchedMatch = FALSE;   // O60: the admin's stream never played
		}
		s_adminStarting = FALSE;
		ReplayStream::stopWatching();
		ReplayStream::stopServing();   // O33: the relay ends with the viewing
		ReplayStream::discardViewerFiles();   // O61 (row 49): nothing of the match stays on an outside viewer's disk
		closeObserverChannel();        // O34
		s_upstreamDelay = 0;           // O39
		ReplayFollow::turnOff();
		setState(STATE_NONE);
		s_roomHost = 0;
		s_watcher = FALSE;
	}
}

void LanRoom::rememberMatchLayout(const LANGameInfo *game)
{
	s_layoutValid = FALSE;
	if (game == nullptr) {
		return;
	}
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		SlotKeep &k = s_layout[i];
		k.state = slot != nullptr ? (Int)slot->getState() : (Int)SLOT_CLOSED;
		k.playerTemplate = slot != nullptr ? slot->getPlayerTemplate() : -1;
		k.color = slot != nullptr ? slot->getColor() : -1;
		k.team = slot != nullptr ? slot->getTeamNumber() : -1;
		k.startPos = slot != nullptr ? slot->getStartPos() : -1;
	}
	s_layoutHostObserves = game->getHostObserves();
	s_layoutValid = TRUE;
}

Bool LanRoom::canOfferRematch()
{
	return s_layoutValid && TheLAN != nullptr;
}

void LanRoom::requestRematch()
{
	s_rematchRequested = s_layoutValid;
}

Bool LanRoom::takeRematchRequest()
{
	if (!s_rematchRequested) {
		return FALSE;
	}
	s_rematchRequested = FALSE;
	s_rematchApplyPending = TRUE;
	return TRUE;
}

void LanRoom::applyRematchLayout(LANGameInfo *game)
{
	if (!s_rematchApplyPending || !s_layoutValid || game == nullptr) {
		return;
	}
	s_rematchApplyPending = FALSE;
	// O60: an admin who watched from outside the seats does so again (slot 0 opens for a player)
	if (s_layoutHostObserves) {
		hostSetObserves(game, TRUE);
	}
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		const SlotKeep &k = s_layout[i];
		if (slot == nullptr) {
			continue;
		}
		if (i == 0 && !s_layoutHostObserves)
		{
			// the host itself: its team and start place (army and colour came from its preferences already)
			slot->setTeamNumber(k.team);
			slot->setStartPos(k.startPos);
			continue;
		}
		if (slot->getState() == SLOT_CLOSED && k.state != SLOT_CLOSED) {
			continue;   // the map does not have this place (the map changed since): leave it closed
		}
		if (k.state == SLOT_EASY_AI || k.state == SLOT_MED_AI || k.state == SLOT_BRUTAL_AI)
		{
			slot->setState((SlotState)k.state);
			slot->setPlayerTemplate(k.playerTemplate);
			slot->setColor(k.color);
			slot->setTeamNumber(k.team);
			slot->setStartPos(k.startPos);
		}
		else if (k.state == SLOT_CLOSED)
		{
			slot->setState(SLOT_CLOSED);
		}
		else
		{
			slot->setState(SLOT_OPEN);   // a human's place (or an open one): open for the players to join again
		}
	}
}

Bool LanRoom::hostStartProblem(const LANGameInfo *game, UnicodeString *text)
{
	if (game == nullptr || TheLAN == nullptr || !game->getHostObserves() || inSlot(game, TheLAN->GetLocalIP())) {
		return FALSE;
	}
	// O62 (H5 #5): a match needs somebody in a seat
	Bool anyone = FALSE;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		anyone = anyone || (game->getConstLANSlot(i) != nullptr && game->getConstLANSlot(i)->isHuman());
	}
	if (!anyone)
	{
		*text = textOf(kAdminNobody);
		return TRUE;
	}
	UnicodeString missing;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot *slot = game->getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && !slot->hasMap())
		{
			if (!missing.isEmpty()) {
				missing.concat(L", ");
			}
			missing.concat(slot->getName());
			// O62 (H5 #6): a map flag lost on the way is asked for again, so the refusal is never a dead end
			sendSimple(KIND_ASK_MAP, TheLAN->GetLocalIP(), slot->getIP(), 0);
		}
	}
	if (missing.isEmpty()) {
		return FALSE;
	}
	text->format(textOf(kAdminNoMap).str(), missing.str());
	return TRUE;
}

Bool LanRoom::hostSeatFirst(LANGameInfo *game)
{
	if (game == nullptr || TheLAN == nullptr || !game->amIHost()) {
		return FALSE;
	}
	Int from = game->getLocalSlotNum();
	if (from < 0)
	{
		// the admin watches: the lowest player, when nobody sits in the first seat
		const GameSlot *first = game->getConstSlot(0);
		if (first != nullptr && first->isHuman()) {
			return FALSE;
		}
		for (Int i = 1; i < MAX_SLOTS && from < 0; ++i) {
			if (game->getConstSlot(i) != nullptr && game->getConstSlot(i)->isHuman()) from = i;
		}
	}
	if (from <= 0) {
		return FALSE;
	}
	LANGameSlot *first = game->getLANSlot(0);
	LANGameSlot *moved = game->getLANSlot(from);
	if (first == nullptr || moved == nullptr) {
		return FALSE;
	}
	// whole slots, not setSlot: that would mark the first seat accepted and with the map as the host's
	const LANGameSlot keep = *first;
	*first = *moved;
	*moved = keep;
	return TRUE;
}

void LanRoom::lobbyOpened()
{
	if (s_adminStarting && s_state == STATE_STARTING)
	{
		s_adminStarting = FALSE;   // O60: the admin left his room at Start to watch its match: his stream goes on
		return;
	}
	s_adminStarting = FALSE;
	resetViewer();
}

Bool LanRoom::takeAdminWatchedMatch()
{
	const Bool watched = s_adminWatchedMatch;
	s_adminWatchedMatch = FALSE;
	return watched;
}

Bool LanRoom::hostHasFreeSeat(const LANGameInfo *game)
{
	for (Int i = 0; game != nullptr && i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot != nullptr && slot->getState() == SLOT_OPEN) {
			return TRUE;
		}
	}
	return FALSE;
}

Bool LanRoom::hostSetObserves(LANGameInfo *game, Bool observe)
{
	// O62 (H5 #7): not while a start is under way (the countdown, or the start check)
	if (game == nullptr || TheLAN == nullptr || !game->amIHost() || game->isGameInProgress() || TheLAN->IsStartPending()) {
		return FALSE;
	}
	const Int mine = game->getLocalSlotNum();
	if (observe)
	{
		if (game->getHostObserves() && mine < 0) {
			return TRUE;
		}
		if (mine >= 0)
		{
			LANGameSlot open;
			open.setState(SLOT_OPEN);
			game->setSlot(mine, open);
		}
		game->setHostObserves(TRUE);
	}
	else
	{
		if (!game->getHostObserves() || mine >= 0) {
			game->setHostObserves(FALSE);
			return mine >= 0;
		}
		Int seat = -1;
		if (game->getConstSlot(0) != nullptr && game->getConstSlot(0)->getState() == SLOT_OPEN) {
			seat = 0;
		}
		for (Int i = 0; seat < 0 && i < MAX_SLOTS; ++i) {
			if (game->getConstSlot(i) != nullptr && game->getConstSlot(i)->getState() == SLOT_OPEN) seat = i;
		}
		if (seat < 0) {
			return FALSE;   // every seat is taken or closed
		}
		// the admin's seat as at the room's creation (LANAPI::RequestGameCreate), with his saved army and colour
		LANGameSlot me;
		me.setState(SLOT_PLAYER, TheLAN->GetMyName());
		me.setIP(TheLAN->GetLocalIP());
		me.setPort(NETWORK_BASE_PORT_NUMBER);
		me.setLastHeard(0);
		me.setLogin(TheLAN->GetMyUserName());
		me.setHost(TheLAN->GetMyHostName());
		LANPreferences pref;
		me.setColor(pref.getPreferredColor());
		me.setPlayerTemplate(pref.getPreferredFaction());
		me.setNATBehavior(FirewallHelperClass::FIREWALL_TYPE_SIMPLE);
		me.setMapAvailability(TRUE);
		game->setSlot(seat, me);
		game->setHostObserves(FALSE);
	}
	game->resetAccepted();
	TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
	lanUpdateSlotList();
	updateGameOptions();
	return TRUE;
}

Int LanRoom::upstreamDelaySeconds()
{
	return (s_watcher && (s_state == STATE_STARTING || s_state == STATE_PLAYING)) ? s_upstreamDelay : 0;
}

Bool LanRoom::observerChannelOpen()
{
	if (s_defeatedChannel && s_peerLocalIP != 0 && s_chatSocket != INVALID_SOCKET) {
		return TRUE;   // O59: a defeated player who joined the observers
	}
	return !s_watcher && s_peerLocalIP != 0 && (s_state == STATE_STARTING || s_state == STATE_PLAYING);
}

void LanRoom::openDefeatedObserverChannel(UnsignedInt localIP)
{
	closeObserverChannel();
	if (localIP == 0) {
		return;
	}
	for (size_t i = 0; i < s_matchRoomObservers.size(); ++i) {
		if (s_matchRoomObservers[i].ip != localIP) s_peers.push_back(s_matchRoomObservers[i]);
	}
	Peer me;
	me.ip = localIP;
	if (TheGameInfo != nullptr && TheGameInfo->getLocalSlotNum() >= 0 && TheGameInfo->getConstSlot(TheGameInfo->getLocalSlotNum()) != nullptr) {
		me.name = TheGameInfo->getConstSlot(TheGameInfo->getLocalSlotNum())->getName();
	}
	s_peers.push_back(me);
	std::sort(s_peers.begin(), s_peers.end(), [](const Peer &a, const Peer &b) { return a.ip < b.ip; });
	s_peerLocalIP = localIP;
	if (!bindChatSocket(localIP))
	{
		s_peers.clear();
		s_peerLocalIP = 0;
		return;
	}
	s_defeatedChannel = TRUE;
	s_lastJoinSent = 0;
	chatLog("OPENED (defeated player joins the observers)", localIP, me.name);
}

void LanRoom::closeDefeatedObserverChannel()
{
	if (s_defeatedChannel) {
		closeObserverChannel();
	}
}

void LanRoom::sendObserverChat(const UnicodeString &text)
{
	if (!observerChannelOpen() || s_chatSocket == INVALID_SOCKET || text.isEmpty()) {
		return;
	}
	unsigned char packet[4 + (g_lanMaxChatLength + 1) * 2];
	memcpy(packet, &kChatMagic, 4);
	Int n = 0;
	for (const WideChar *c = text.str(); *c != 0 && n < g_lanMaxChatLength; ++c, ++n) {
		memcpy(packet + 4 + n * 2, c, 2);
	}
	const WideChar zero = 0;
	memcpy(packet + 4 + n * 2, &zero, 2);
	const int length = 4 + (n + 1) * 2;
	for (size_t i = 0; i < s_peers.size(); ++i)
	{
		if (s_peers[i].ip == s_peerLocalIP) {
			continue;
		}
		sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_port = htons((unsigned short)OBSERVER_CHAT_PORT);
		to.sin_addr.s_addr = htonl(s_peers[i].ip);
		sendto(s_chatSocket, (const char *)packet, length, 0, (sockaddr *)&to, sizeof(to));
		chatLog("SENT to", s_peers[i].ip, text);
	}
	if (TheInGameUI != nullptr)
	{
		const Peer *me = peerOf(s_peerLocalIP);
		UnicodeString shown;
		shown.format(textOf(kObserverLine).str(), me != nullptr ? me->name.str() : L"", text.str());
		TheInGameUI->messageNoFormat(shown);
		if (!s_chatHinted)
		{
			TheInGameUI->messageNoFormat(textOf(kObserverOnly));
			s_chatHinted = TRUE;
		}
	}
}

Int LanRoom::observerVoicePeers(UnsignedInt *ips, UnicodeString *names, Int maxCount, Int *localIndex)
{
	if (!observerChannelOpen() || (s_state != STATE_PLAYING && !s_defeatedChannel)) {
		return 0;
	}
	Int n = 0;
	*localIndex = -1;
	for (size_t i = 0; i < s_peers.size() && n < maxCount; ++i, ++n)
	{
		ips[n] = s_peers[i].ip;
		names[n] = s_peers[i].name;
		if (s_peers[i].ip == s_peerLocalIP) *localIndex = n;
	}
	return *localIndex >= 0 ? n : 0;
}

void LanRoom::postNotice(const UnicodeString &text)
{
	notice(text);
}

Bool LanRoom::takeNotice(UnicodeString *text)
{
	if (s_notices.empty()) {
		return FALSE;
	}
	*text = s_notices.front();
	s_notices.erase(s_notices.begin());
	return TRUE;
}
