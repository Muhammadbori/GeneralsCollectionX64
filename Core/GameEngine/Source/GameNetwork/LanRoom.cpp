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

#include "GameNetwork/LanRoom.h"

#include "Common/GlobalData.h"
#include "Common/Recorder.h"
#include "Common/ReplayFollow.h"
#include "Common/ReplayStream.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"

#include <map>

namespace
{
	enum Kind { KIND_OBSERVE = 1, KIND_LEAVE = 2, KIND_LIST = 3, KIND_REMOVED = 4, KIND_CHAT = 5, KIND_START = 6 };
	enum { REASON_BY_HOST = 0, REASON_FULL = 1, REASON_NO_ROOM = 2 };
	const UnsignedInt kHeartbeatMs = 4000;
	const UnsignedInt kListMs = 2000;
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
	const Text kLeft = { "GUI:LanRoomLeft", L"You no longer observe that game.", L"\x0412\x044B \x0431\x043E\x043B\x044C\x0448\x0435 \x043D\x0435 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0435\x0442\x0435 \x0437\x0430 \x044D\x0442\x043E\x0439 \x0438\x0433\x0440\x043E\x0439." };

	struct Entry { UnsignedInt ip; UnicodeString name; UnsignedInt lastHeard; };

	// host
	std::vector<Entry> s_hosted;
	Bool s_listDirty = FALSE;
	Bool s_listSentNonEmpty = FALSE;
	UnsignedInt s_lastList = 0;
	// what the lobby saw
	struct Known { std::vector<LanRoom::Observer> list; UnsignedInt seen; };
	std::map<UnsignedInt, Known> s_known;
	// this game as an observer / viewer
	LanRoom::State s_state = LanRoom::STATE_NONE;
	UnsignedInt s_roomHost = 0;
	UnsignedInt s_lastBeat = 0;
	UnsignedInt s_stateSince = 0;
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

	void broadcastList(UnsignedInt localIP)
	{
		LANMessage msg;
		prepare(msg, KIND_LIST, localIP);
		Int n = 0;
		for (size_t i = 0; i < s_hosted.size() && n < LanRoom::MAX_OBSERVERS; ++i, ++n)
		{
			msg.Room.ips[n] = s_hosted[i].ip;
			wcslcpy(msg.Room.u.names[n], s_hosted[i].name.str(), g_lanPlayerNameLength + 1);
		}
		msg.Room.count = (UnsignedByte)n;
		send(msg, 0);
		s_lastList = timeGetTime();
		s_listDirty = FALSE;
		s_listSentNonEmpty = n > 0;
	}

	void setState(LanRoom::State st)
	{
		s_state = st;
		s_stateSince = timeGetTime();
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

	void beginStream(const UnsignedInt *ips, Int count, Int port, UnsignedInt localIP, Bool roomObserver, Int delaySeconds)
	{
		// the viewer relays its copy on its own LAN address (viewers feed viewers)
		ReplayStream::configureServeAddress(localIP);
		ReplayStream::configureServeDefault(LanRoom::STREAM_PORT);
		ReplayStream::configureWatchList(ips, count, port, roomObserver);
		ReplayFollow::configure(delaySeconds, 2.0f);
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
	const Int places = MAX_PEOPLE - humansInSlots(game);
	return places > MAX_OBSERVERS ? MAX_OBSERVERS : (places < 0 ? 0 : places);
}

void LanRoom::onMessage(const LANMessage *msg, UnsignedInt senderIP, UnsignedInt localIP)
{
	const UnsignedInt now = timeGetTime();
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
			WideChar sender[g_lanPlayerNameLength + 1];
			copyFromPacket(sender, msg->name, g_lanPlayerNameLength + 1);
			e.name = UnicodeString(sender);
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
			Known &k = s_known[senderIP];
			k.list.clear();
			k.seen = now;
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
			if (s_roomHost == senderIP && me && s_state == STATE_ASKED)
			{
				setState(STATE_IN_ROOM);
				notice(textOf(kObserving));
			}
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
			Bool fromObserver = FALSE;
			for (size_t i = 0; i < s_hosted.size(); ++i) {
				fromObserver = fromObserver || s_hosted[i].ip == senderIP;
			}
			if (game != nullptr && fromObserver)
			{
				// the host: the line with the observer's name, here and to everybody in the room
				UnicodeString line;
				WideChar sender[g_lanPlayerNameLength + 1];
				copyFromPacket(sender, msg->name, g_lanPlayerNameLength + 1);
				line.format(L"%ls (%ls): %ls", sender, textOf(kRole).str(), text);
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
				const Bool myRoom = (mine != nullptr && mine->getIP(0) == senderIP) || (s_roomHost == senderIP && s_state == STATE_IN_ROOM);
				if (myRoom) {
					showLine(UnicodeString(text));
				}
			}
			break;
		}
		case KIND_START:
		{
			if (s_roomHost != senderIP || (s_state != STATE_ASKED && s_state != STATE_IN_ROOM)) {
				return;
			}
			UnsignedInt ips[MAX_SLOTS];
			Int n = 0;
			for (Int i = 0; i < msg->Room.count && i < MAX_SLOTS; ++i) {
				ips[n++] = msg->Room.ips[i];
			}
			s_watcher = FALSE;
			notice(textOf(kStarting));
			beginStream(ips, n, msg->Room.port != 0 ? msg->Room.port : STREAM_PORT, localIP, TRUE, 0);
			break;
		}
		default:
			break;
	}
}

void LanRoom::update(UnsignedInt localIP)
{
	const UnsignedInt now = timeGetTime();
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
		if (s_listDirty || (!s_hosted.empty() && now - s_lastList >= kListMs)) {
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
		if (now - s_lastBeat >= kHeartbeatMs)
		{
			s_lastBeat = now;
			sendSimple(KIND_OBSERVE, s_roomHost, s_roomHost, 0);
		}
		if (TheLAN != nullptr && TheLAN->LookupGameByHost(s_roomHost) == nullptr && now - s_stateSince > 10000)
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
	// the stream: play it once its header is there
	if (s_state == STATE_STARTING)
	{
		if (ReplayStream::headerReady() && TheRecorder != nullptr && TheWritableGlobalData != nullptr)
		{
			setState(STATE_PLAYING);
			TheWritableGlobalData->m_loadReplayGame = s_mirror;
			TheRecorder->loadQueuedReplay();   // the mirror, in the follow mode (ReplayStream::isWatching)
		}
		else if (ReplayStream::takeLostNotice() || now - s_stateSince > kStreamWaitMs)
		{
			notice(textOf(kLost));
			resetViewer();
		}
	}
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
	if (s_hosted.empty() || game == nullptr) {
		return;
	}
	LANMessage msg;
	prepare(msg, KIND_START, localIP);
	UnsignedInt ips[MAX_SLOTS];
	const Int n = playerAddresses(game, ips, MAX_SLOTS);
	for (Int i = 0; i < n; ++i) {
		msg.Room.ips[i] = ips[i];
	}
	msg.Room.count = (UnsignedByte)n;
	msg.Room.port = STREAM_PORT;
	for (size_t i = 0; i < s_hosted.size(); ++i) {
		send(msg, s_hosted[i].ip);
	}
	s_hosted.clear();   // they follow the stream now; nobody waits for them
}

void LanRoom::configureServing(UnsignedInt localIP)
{
	// every player serves its replay on its LAN address (a -serveReplay port of the command line stays)
	ReplayStream::configureServeAddress(localIP);
	ReplayStream::configureServeDefault(STREAM_PORT);   // outside viewers per source: 4, or -serveReplayOutsideMax
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
	if (it != s_known.end() && timeGetTime() - it->second.seen < kKnownMs) {
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
	if (s_roomHost != 0 && s_roomHost != hostIP) {
		stopObserving();
	}
	s_roomHost = hostIP;
	s_watcher = FALSE;
	setState(STATE_ASKED);
	s_lastBeat = timeGetTime();
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

void LanRoom::startWatching(LANGameInfo *game, Int delaySeconds)
{
	if (game == nullptr || TheLAN == nullptr || s_state == STATE_STARTING || s_state == STATE_PLAYING) {
		return;
	}
	stopObserving();
	UnsignedInt ips[MAX_SLOTS];
	const Int n = playerAddresses(game, ips, MAX_SLOTS);
	if (n == 0) {
		return;
	}
	delaySeconds = delaySeconds < 10 ? 10 : (delaySeconds > 180 ? 180 : delaySeconds);
	s_watcher = TRUE;
	s_roomHost = game->getHostIP();
	UnicodeString t;
	t.format(textOf(kWatching).str(), delaySeconds);
	notice(t);
	beginStream(ips, n, STREAM_PORT, TheLAN->GetLocalIP(), FALSE, delaySeconds);
}

void LanRoom::resetViewer()
{
	if (s_state == STATE_STARTING || s_state == STATE_PLAYING)
	{
		ReplayStream::stopWatching();
		ReplayFollow::turnOff();
		setState(STATE_NONE);
		s_roomHost = 0;
		s_watcher = FALSE;
	}
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
