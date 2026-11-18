/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "Common/Fingerprint.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/networkutil.h"
#include "Common/GlobalData.h"
#include "Common/RandomValue.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"
#include "Common/version.h"
#include "gitinfo.h"
#include "GameClient/MapUtil.h"
#include "Common/UserPreferences.h"
#include "GameLogic/GameLogic.h"
#include "Common/GuardLink.h"
#include "GameNetwork/LanRoom.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/InGameUI.h"

#include <map>
#include "Common/LanClock.h"   // O75


static const UnsignedShort lobbyPort = 8086; ///< This is the UDP port used by all LANAPI communication

AsciiString GetMessageTypeString(UnsignedInt type);

const UnsignedInt LANAPI::s_resendDelta = 10 * 1000;	///< This is how often we announce ourselves to the world
/*
LANGame::LANGame()
{
	m_gameName = L"";

	int player;
	for (player = 0; player < MAX_SLOTS; ++player)
	{
		m_playerName[player] = L"";
		m_playerIP[player]= 0;
		m_playerAccepted[player] = false;
	}
	m_lastHeard = 0;
	m_inProgress = false;
	m_next = nullptr;
}
*/




LANAPI::LANAPI() : m_transport(nullptr)
{
	DEBUG_LOG(("LANAPI::LANAPI() - max game option size is %d, sizeof(LANMessage)=%d, MAX_LANAPI_PACKET_SIZE=%d",
		m_lanMaxOptionsLength, sizeof(LANMessage), MAX_LANAPI_PACKET_SIZE));

	m_lastResendTime = 0;
	//
	m_lobbyPlayers = nullptr;
	m_games = nullptr;
	m_name = L""; // safe default?
	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_localIP = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_watchMatchLeave = FALSE;   // O60
	m_convergeGame = nullptr;    // O70
	m_lastRoomBeat = 0;
	for (Int k = 0; k < MAX_SLOTS; ++k) m_seatJoinedMs[k] = 0;
	m_lastLoopMs = 0;     // O71
	m_lobbySelectedHost = 0;   // O84
	m_startSeenMs = 0;
	m_notListedSinceMs = 0;
	m_acceptWanted = FALSE;
	m_hostViewMs = 0;
	m_mapLastSent = 0;
	for (Int k = 0; k < CONVERGE_COUNT; ++k) m_optionPending[k] = FALSE;
	m_startChecking = FALSE;     // O62
	m_startDelivering = FALSE;   // O74
	m_startDigest = 0;
	m_startCheckSince = 0;
	m_lastStartCheck = 0;
	m_currentGame = nullptr;
	m_broadcastAddr = INADDR_BROADCAST;
	m_directConnectRemoteIP = 0;
	m_actionTimeout = 5000; // ms
	m_lastUpdate = 0;
	m_transport = new Transport;
	m_isActive = TRUE;
}

LANAPI::~LANAPI()
{
	reset();
	delete m_transport;
}

namespace
{
	// O30: GuardLink's way out to another player
	void sendGuardBytes(UnsignedInt ip, const UnsignedByte *data, Int length)
	{
		if (TheLAN != nullptr) {
			TheLAN->sendGuardData(ip, data, length);
		}
	}
}

void LANAPI::init()
{
	GuardLink::setSender(&sendGuardBytes);
	m_gameStartTime = 0;
	m_gameStartSeconds = 0;
	// O26 test switch: the lobby socket on a loopback address only (else it would bind every address on the first
	// visit, before SetLocalIP, and Windows would ask about the firewall)
	if (LanTest::loopbackSlot() >= 0) {
		m_localIP = LanTest::loopbackIP();
	}
	m_transport->reset();
	m_transport->init(m_localIP, lobbyPort);
	m_transport->allowBroadcasts(true);

	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_watchMatchLeave = FALSE;   // O60
	m_convergeGame = nullptr;    // O70
	m_lastRoomBeat = 0;
	for (Int k = 0; k < MAX_SLOTS; ++k) m_seatJoinedMs[k] = 0;
	m_lastLoopMs = 0;     // O71
	m_lobbySelectedHost = 0;   // O84
	m_startSeenMs = 0;
	m_notListedSinceMs = 0;
	m_acceptWanted = FALSE;
	m_hostViewMs = 0;
	m_mapLastSent = 0;
	for (Int k = 0; k < CONVERGE_COUNT; ++k) m_optionPending[k] = FALSE;
	m_startChecking = FALSE;     // O62
	m_startDelivering = FALSE;   // O74
	m_startDigest = 0;
	m_startCheckSince = 0;
	m_lastStartCheck = 0;
	m_currentGame = nullptr;
	m_directConnectRemoteIP = 0;

	m_lastGameopt = "";

#if TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
	char userName[UNLEN + 1];
	DWORD bufSize = ARRAY_SIZE(userName);
	if (GetUserNameA(userName, &bufSize))
	{
		m_userName.set(userName, bufSize - 1);
	}
	else
	{
		m_userName = "unknown";
	}

	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	bufSize = ARRAY_SIZE(computerName);
	if (GetComputerNameA(computerName, &bufSize))
	{
		m_hostName.set(computerName, bufSize - 1);
	}
	else
	{
		m_hostName = "unknown";
	}
#endif
}

void LANAPI::reset()
{
	if (m_inLobby)
	{
		LANMessage msg;
		fillInLANMessage( &msg );
		msg.messageType = LANMessage::MSG_REQUEST_LOBBY_LEAVE;
		sendMessage(&msg);
	}
	m_transport->update();

	LANGameInfo *theGame = m_games;
	LANGameInfo *deletableGame = nullptr;

	while (theGame)
	{
		deletableGame = theGame;
		theGame = theGame->getNext();
		delete deletableGame;
	}

	LANPlayer *thePlayer = m_lobbyPlayers;
	LANPlayer *deletablePlayer = nullptr;

	while (thePlayer)
	{
		deletablePlayer = thePlayer;
		thePlayer = thePlayer->getNext();
		delete deletablePlayer;
	}

	m_games = nullptr;
	m_lobbyPlayers = nullptr;
	m_directConnectRemoteIP = 0;
	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_currentGame = nullptr;

}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature O26 (row 26) Files fingerprints of the others, and the LAN screen's test switches.
namespace
{
	std::map<UnsignedInt, UnsignedInt> s_filesFingerprints;   ///< sender ip -> fingerprint
	Int s_loopbackSlot = -1;
	Bool s_sampleData = FALSE;
	// O45: the test loss and the join log
	Int s_lossPercent = -1;
	UnsignedInt s_lossSeed = 0x2545F491u;
	// O45: the join request, repeated every second while no answer came (the request and its accept are single UDP
	// packets; one lost made the join time out after 5 s). The host answers a repeat with the same slot.
	const UnsignedInt kJoinRepeatMs = 1000;
	LANMessage s_joinRequest;
	UnsignedInt s_joinRequestIP = 0;
	UnsignedInt s_joinSentMs = 0;
	Int s_joinTries = 0;
}

Bool LanFiles::fingerprintOf(UnsignedInt ip, UnsignedInt *fingerprint)
{
	std::map<UnsignedInt, UnsignedInt>::const_iterator it = s_filesFingerprints.find(ip);
	if (it == s_filesFingerprints.end() || it->second == 0) {
		return FALSE;
	}
	if (fingerprint != nullptr) {
		*fingerprint = it->second;
	}
	return TRUE;
}

UnsignedInt LanFiles::own()
{
	return Fingerprint::isComputed() ? Fingerprint::overall() : 0;
}

void LanTest::setLoopbackSlot(Int slot) { s_loopbackSlot = (slot >= 0 && slot < 8) ? slot : -1; }
Int LanTest::loopbackSlot() { return s_loopbackSlot; }
UnsignedInt LanTest::loopbackIP() { return s_loopbackSlot >= 0 ? ((127u << 24) | (UnsignedInt)(10 + s_loopbackSlot)) : 0; }
void LanTest::setSampleData(Bool on) { s_sampleData = on; }
Bool LanTest::sampleData() { return s_sampleData; }
void LanTest::setLossPercent(Int percent) { s_lossPercent = percent < 0 ? -1 : (percent > 90 ? 90 : percent); }
Int LanTest::lossPercent() { return s_lossPercent; }

Bool LanTest::dropForTest()
{
	if (s_lossPercent <= 0) {
		return FALSE;
	}
	s_lossSeed = s_lossSeed * 1103515245u + 12345u;
	return (Int)((s_lossSeed >> 16) % 100u) < s_lossPercent;
}

void LanTest::joinLog(const char *format, ...)
{
	// O82 (H14): only with -lanTestLoss; at most 8 MB a run (a LAN peer can make a line per message)
	static UnsignedInt s_joinLogBytes = 0;
	const UnsignedInt kJoinLogMaxBytes = 8u << 20;
	if (s_lossPercent < 0 || TheGlobalData == nullptr || s_joinLogBytes > kJoinLogMaxBytes) {
		return;
	}
	char path[_MAX_PATH];
	snprintf(path, sizeof(path), "%sLanJoin.log", TheGlobalData->getPath_UserData().str());
	FILE *f = fopen(path, "a");
	if (f == nullptr) {
		return;
	}
	if (s_joinLogBytes + 512 > kJoinLogMaxBytes)
	{
		fprintf(f, "[%10u] log full (8 MB this run): nothing more is written\n", (unsigned)LanClock::now());
		fclose(f);
		s_joinLogBytes = kJoinLogMaxBytes + 1;
		return;
	}
	s_joinLogBytes += 160;   // about one line
	fprintf(f, "[%10u] ", (unsigned)LanClock::now());
	va_list args;
	va_start(args, format);
	vfprintf(f, format, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

void LANAPI::sendMessage(LANMessage *msg, UnsignedInt ip /* = 0 */)
{
	sendMessageOnly(msg, ip);
	// O26: every announce is followed by the files fingerprint, to the same addresses
	if (msg->messageType == LANMessage::MSG_GAME_ANNOUNCE || msg->messageType == LANMessage::MSG_LOBBY_ANNOUNCE)
	{
		LANMessage files;
		memset(&files, 0, sizeof(files));
		fillInLANMessage(&files);
		files.messageType = LANMessage::MSG_FILES_INFO;
		files.FilesInfo.magic = LANFilesInfoMagic;
		files.FilesInfo.fingerprint = LanFiles::own();
		files.FilesInfo.exeCRC = TheGlobalData != nullptr ? TheGlobalData->m_exeCRC : 0;
		files.FilesInfo.iniCRC = TheGlobalData != nullptr ? TheGlobalData->m_iniCRC : 0;
		sendMessageOnly(&files, ip);
	}
}

void LANAPI::sendMessageOnly(LANMessage *msg, UnsignedInt ip)
{
	if (ip != 0)
	{
		m_transport->queueSend(ip, lobbyPort, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
	}
	else if ((m_currentGame != nullptr) && (m_currentGame->getIsDirectConnect()))
	{
		Int localSlot = m_currentGame->getLocalSlotNum();
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			if (i != localSlot) {
				GameSlot *slot = m_currentGame->getSlot(i);
				if ((slot != nullptr) && (slot->isHuman())) {
					m_transport->queueSend(slot->getIP(), lobbyPort, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
				}
			}
		}
	}
	else if (LanTest::loopbackSlot() >= 0)
	{
		// O26 test switch: the "broadcast" goes to the other loopback instances one by one
		for (Int k = 0; k < 8; ++k)
		{
			const UnsignedInt peer = (127u << 24) | (UnsignedInt)(10 + k);
			if (peer != m_localIP) {
				m_transport->queueSend(peer, lobbyPort, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
			}
		}
	}
	else
	{
		m_transport->queueSend(m_broadcastAddr, lobbyPort, (unsigned char *)msg, sizeof(LANMessage) /*, 0, 0 */);
	}
}


AsciiString GetMessageTypeString(UnsignedInt type)
{
	AsciiString returnString;

	switch (type)
	{
		case LANMessage::MSG_REQUEST_LOCATIONS:
			returnString.format( "Request Locations (%d)",type);
			break;
		case LANMessage::MSG_GAME_ANNOUNCE:
			returnString.format("Game Announce (%d)",type);
			break;
		case LANMessage::MSG_LOBBY_ANNOUNCE:
			returnString.format("Lobby Announce (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_JOIN:
			returnString.format("Request Join (%d)",type);
			break;
		case LANMessage::MSG_JOIN_ACCEPT:
			returnString.format("Join Accept (%d)",type);
			break;
		case LANMessage::MSG_JOIN_DENY:
			returnString.format("Join Deny (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_GAME_LEAVE:
			returnString.format("Request Game Leave (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_LOBBY_LEAVE:
			returnString.format("Request Lobby Leave (%d)",type);
			break;
		case LANMessage::MSG_SET_ACCEPT:
			returnString.format("Set Accept(%d)",type);
			break;
		case LANMessage::MSG_CHAT:
			returnString.format("Chat (%d)",type);
			break;
		case LANMessage::MSG_GAME_START:
			returnString.format("Game Start (%d)",type);
			break;
		case LANMessage::MSG_GAME_START_TIMER:
			returnString.format("Game Start Timer (%d)",type);
			break;
		case LANMessage::MSG_GAME_OPTIONS:
			returnString.format("Game Options (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_GAME_INFO:
			returnString.format("Request GameInfo (%d)", type);
			break;
		case LANMessage::MSG_INACTIVE:
			returnString.format("Inactive (%d)", type);
			break;
		case LANMessage::MSG_FILES_INFO:
			returnString.format("Files Info (%d)", type);
			break;
		default:
			returnString.format("Unknown Message (%d)",type);
	}
	return returnString;
}


void LANAPI::checkMOTD()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_useLocalMOTD)
	{
		// for a playtest, let's log some play statistics, eh?
		if (TheGlobalData->m_playStats <= 0)
			TheWritableGlobalData->m_playStats = 30;

		static UnsignedInt oldMOTDCRC = 0;
		UnsignedInt newMOTDCRC = 0;
		AsciiString asciiMOTD;
		char buf[4096];
		FILE *fp = fopen(TheGlobalData->m_MOTDPath.str(), "r");
		Int len;
		if (fp)
		{
			while( (len = fread(buf, 1, 4096, fp)) > 0 )
			{
				buf[len] = 0;
				asciiMOTD.concat(buf);
			}
			fclose(fp);
			CRC crcObj;
			crcObj.computeCRC(asciiMOTD.str(), asciiMOTD.getLength());
			newMOTDCRC = crcObj.get();
		}

		if (oldMOTDCRC != newMOTDCRC)
		{
			// different MOTD... display it
			oldMOTDCRC = newMOTDCRC;
			AsciiString line;
			while (asciiMOTD.nextToken(&line, "\n"))
			{
				if (line.getCharAt(line.getLength()-1) == '\r')
					line.removeLastChar();	// there is a trailing '\r'

				if (line.isEmpty())
				{
					line = " ";
				}

				UnicodeString uniLine;
				uniLine.translate(line);
				OnChat( L"MOTD", 0, uniLine, LANCHAT_SYSTEM );
			}
		}
	}
#endif
}

extern Bool LANbuttonPushed;
extern Bool LANSocketErrorDetected;
void LANAPI::update()
{
	if(LANbuttonPushed)
		return;
	// O60 (row 46): the admin who watches his room's match leaves the room now (OnGameStart)
	if (m_watchMatchLeave)
	{
		OnWatchMatchLeave();
		return;
	}
	static const UnsignedInt LANAPIUpdateDelay = 200;
	UnsignedInt now = LanClock::now();

	// TheSuperHackers @bugfix O71 (H8) Time in which this computer's own loop did not run (a window dragged, a long
	// frame, a modal box) is not silence of the others: their packets wait in the socket and nobody's clock is read.
	// After a gap of more than 3 s every room clock starts again from now.
	if (!m_inLobby && m_currentGame != nullptr && !m_currentGame->isGameInProgress())
	{
		if (m_lastLoopMs != 0 && (Int)(now - m_lastLoopMs) > 3000)
		{
			LanTest::joinLog("room: this computer's loop did not run for %u ms: the room's silence clocks start again", now - m_lastLoopMs);
			m_currentGame->setLastHeard(now);
			for (Int p = 0; p < MAX_SLOTS; ++p)
			{
				if (m_currentGame->getIP(p) != 0) {
					m_currentGame->setPlayerLastHeard(p, now);
				}
				if (m_seatJoinedMs[p] != 0) {
					m_seatJoinedMs[p] = now;
				}
			}
		}
		m_lastLoopMs = now;   // never 0 (LanClock::now)
	}
	else
	{
		m_lastLoopMs = 0;
	}

	// O73: every time test below is a signed difference: right across timeGetTime's wrap (every 49.7 days)
	if( LanClock::due(now, m_lastUpdate, LANAPIUpdateDelay))   // O75: 0 = never: due (not a negative difference)
	{
		m_lastUpdate = now;
	}
	else
	{
		return;
	}

	// Let the UDP socket breathe
	if ((m_transport->update() == FALSE) && (LANSocketErrorDetected == FALSE)) {
		if (m_isInLANMenu == TRUE) {
			LANSocketErrorDetected = TRUE;
		}
	}

	// Handle any new messages
	for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer) && !LANbuttonPushed; ++i)
	{
		if (m_transport->m_inBuffer[i].length > 0)
		{
			// Process the new message
			UnsignedInt senderIP = m_transport->m_inBuffer[i].addr;
			if (senderIP == m_localIP)
			{
				m_transport->m_inBuffer[i].length = 0;
				continue;
			}

			// O45 (test): a lossy link (-lanTestLoss)
			if (LanTest::dropForTest())
			{
				m_transport->m_inBuffer[i].length = 0;
				continue;
			}

			LANMessage *msg = (LANMessage *)(m_transport->m_inBuffer[i].data);
			terminateStrings(msg);   // O33 (review 7): every text field of the message ends inside the message
			noteHeardFrom(senderIP, msg->messageType);   // O71: a seat's room message counts as heard (O73: not lobby-wide ones)
			// O84 (H16): a join request to this host's room: the requester is checked in the join class (its own room,
			// the oldest request leaves first), so a host that requires the anti-cheat can always check a stranger
			if (msg->messageType == LANMessage::MSG_REQUEST_JOIN && !m_inLobby && m_currentGame != nullptr
				&& m_currentGame->amIHost() && !m_currentGame->isGameInProgress()) {
				GuardLink::notePeer(senderIP, m_localIP, GuardLink::PEER_JOIN);
			}
			//DEBUG_LOG(("LAN message type %s from %ls (%s@%s)", GetMessageTypeString(msg->messageType).str(),
			//	msg->name, msg->userName, msg->hostName));
			switch (msg->messageType)
			{
				// Location specification
			case LANMessage::MSG_REQUEST_LOCATIONS:		// Hey, where is everybody?
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_LOCATIONS from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestLocations( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_ANNOUNCE:				// Here someone is, and here's his game info!
				DEBUG_LOG(("LANAPI::update - got a MSG_GAME_ANNOUNCE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleGameAnnounce( msg, senderIP );
				break;
			case LANMessage::MSG_LOBBY_ANNOUNCE:			// Hey, I'm in the lobby!
				DEBUG_LOG(("LANAPI::update - got a MSG_LOBBY_ANNOUNCE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleLobbyAnnounce( msg, senderIP );
				break;
			case LANMessage::MSG_REQUEST_GAME_INFO:
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_GAME_INFO from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestGameInfo( msg, senderIP );
				break;

				// Joining games
			case LANMessage::MSG_REQUEST_JOIN:				// Let me in!  Let me in!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_JOIN from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestJoin( msg, senderIP );
				break;
			case LANMessage::MSG_JOIN_ACCEPT:					// Okay, you can join.
				DEBUG_LOG(("LANAPI::update - got a MSG_JOIN_ACCEPT from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleJoinAccept( msg, senderIP );
				break;
			case LANMessage::MSG_JOIN_DENY:						// Go away!  We don't want any!
				DEBUG_LOG(("LANAPI::update - got a MSG_JOIN_DENY from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleJoinDeny( msg, senderIP );
				break;

				// Leaving games, lobby
			case LANMessage::MSG_REQUEST_GAME_LEAVE:				// I'm outa here!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_GAME_LEAVE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestGameLeave( msg, senderIP );
				break;
			case LANMessage::MSG_REQUEST_LOBBY_LEAVE:				// I'm outa here!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_LOBBY_LEAVE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestLobbyLeave( msg, senderIP );
				break;

				// Game options, chat, etc
			case LANMessage::MSG_SET_ACCEPT:					// I'm cool with everything as is.
				handleSetAccept( msg, senderIP );
				break;
			case LANMessage::MSG_MAP_AVAILABILITY:		// Map status
				handleHasMap( msg, senderIP );
				break;
			case LANMessage::MSG_CHAT:								// Just spouting my mouth off.
				handleChat( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_START:					// Hold on; we're starting!
				handleGameStart( msg, senderIP );
				break;
			case LANMessage::MSG_START_CHECK:   // O62
				handleStartCheck( msg, senderIP );
				break;
			case LANMessage::MSG_START_READY:   // O62
				handleStartReady( msg, senderIP );
				break;
			case LANMessage::MSG_START_ACK:     // O74
				handleStartAck( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_START_TIMER:
				handleGameStartTimer( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_OPTIONS:				// Here's some info about the game.
				DEBUG_LOG(("LANAPI::update - got a MSG_GAME_OPTIONS from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleGameOptions( msg, senderIP );
				break;
			case LANMessage::MSG_INACTIVE:		// someone is telling us that we're inactive.
				handleInActive( msg, senderIP );
				break;
			case LANMessage::MSG_FILES_INFO:	// O26: the sender's files fingerprint
				// O27: only from a packet that holds the whole message, and with the right magic
				if (m_transport->m_inBuffer[i].length >= (Int)sizeof(LANMessage) && msg->FilesInfo.magic == LANFilesInfoMagic) {
					s_filesFingerprints[senderIP] = msg->FilesInfo.fingerprint;
				}
				break;
			case LANMessage::MSG_ROOM:	// O31: the room's observers
				if (m_transport->m_inBuffer[i].length >= (Int)sizeof(LANMessage) && msg->Room.magic == LANRoomMagic) {
					LanRoom::onMessage(msg, senderIP, m_localIP);
				}
				break;
			case LANMessage::MSG_GUARD_DATA:	// O30: for this player's anti-cheat, passed on unread
				if (m_transport->m_inBuffer[i].length >= (Int)sizeof(LANMessage) && msg->GuardData.magic == LANGuardDataMagic
					&& msg->GuardData.length > 0 && msg->GuardData.length <= LANGuardDataMax) {
					GuardLink::onLanData(senderIP, m_localIP, msg->GuardData.data, msg->GuardData.length);
				}
				break;

			default:
				DEBUG_LOG(("Unknown LAN message type %d", msg->messageType));
			}

			// Mark it as read
			m_transport->m_inBuffer[i].length = 0;
		}
		else
		{
			break;
		}
	}
	if(LANbuttonPushed)
		return;
	{
		// O32: in the lobby the red mark, and one chat line with the player's name and the kind of failure (never a path)
		UnsignedInt failedIP = 0;
		while (GuardLink::takeNewFailure(&failedIP))
		{
			UnicodeString name;
			if (LANPlayer *player = LookupPlayer(failedIP)) {
				name = player->getName();
			}
			for (Int k = 0; name.isEmpty() && m_currentGame != nullptr && k < MAX_SLOTS; ++k)
			{
				const LANGameSlot *slot = m_currentGame->getLANSlot(k);
				if (slot != nullptr && slot->isHuman() && slot->getIP() == failedIP) {
					name = slot->getName();
				}
			}
			if (!name.isEmpty()) {
				OnChat(UnicodeString::TheEmptyString, 0, GuardLink::failedInLobbyText(name, failedIP), LANAPIInterface::LANCHAT_SYSTEM);
			}
		}
	}
	noteGuardPeers();   // O83: the anti-cheat asks the addresses this player's screens show, not every packet's source
	GuardLink::logNamedChanges();   // O87 (test exe): what is named, when it changes
	LanRoom::update(m_localIP);   // O31: observers (heartbeats, the host's list, the stream start)
	// O70: the room's keep-alive every 2 s: the host's slot list (the joiners take the host for alive by it), a joiner's
	// HELLO (the host takes the seat for alive by it)
	pumpRoomBeat(now);
	// Send out periodic I'm Here messages
	if (LanClock::due(now, m_lastResendTime, s_resendDelta))
	{
		m_lastResendTime = now;

		if (m_inLobby)
		{
			RequestSetName(m_name);
		}
		else if (m_currentGame && !m_currentGame->isGameInProgress())
		{
			if (AmIHost())
			{
				RequestGameOptions( GenerateGameOptionsString(), true );
				RequestGameAnnounce();
			}
			else
			{
#if TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
				AsciiString text;
				text.format("User=%s", m_userName.str());
				RequestGameOptions( text, true );
				text.format("Host=%s", m_hostName.str());
				RequestGameOptions( text, true );
#endif
				RequestGameOptions( "HELLO", false );
			}
		}
		else if (m_currentGame)
		{
			// game is in progress - RequestGameAnnounce will check if we should send it
			RequestGameAnnounce();
		}
	}

	Bool playerListChanged = false;
	Bool gameListChanged = false;

	// Weed out people we haven't heard from in a while
	LANPlayer *player = m_lobbyPlayers;
	while (player)
	{
		if (LanClock::silentFor(now, player->getLastHeard(), s_resendDelta*2))
		{
			// He's gone!
			removePlayer(player);
			LANPlayer *nextPlayer = player->getNext();
			delete player;
			player = nextPlayer;
			playerListChanged = true;
		}
		else
		{
			player = player->getNext();
		}
	}

	// Weed out people we haven't heard from in a while
	LANGameInfo *game = m_games;
	while (game)
	{
		if (game != m_currentGame && LanClock::silentFor(now, game->getLastHeard(), s_resendDelta*2))
		{
			// He's gone!
			removeGame(game);
			LANGameInfo *nextGame = game->getNext();
			delete game;
			game = nextGame;
			gameListChanged = true;
		}
		else
		{
			game = game->getNext();
		}
	}
	if ( m_currentGame && !m_currentGame->isGameInProgress() )
	{
		// O70/O71: 30 s without the host's slot list (it comes every 2 s now: 15 lost in a row) instead of 160 s; not
		// while a start is under way (the host's start check or countdown came in the last 30 s)
		const Bool startUnderWay = m_startSeenMs != 0 && (Int)(now - m_startSeenMs) < 30000;
		if ( !AmIHost() && !startUnderWay && LanClock::silentFor(now, m_currentGame->getLastHeard(), 30000) )
		{
			// We haven't heard from the host in a while.  Bail.
			// Actually, fake a host leaving message. :)
			LANMessage msg;
			fillInLANMessage( &msg );
			msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
			wcslcpy(msg.name, m_currentGame->getHostName().str(), ARRAY_SIZE(msg.name));   // O60
			handleRequestGameLeave(&msg, m_currentGame->getHostIP());
			UnicodeString text;
			text = TheGameText->fetch("LAN:HostNotResponding");
			OnChat(UnicodeString::TheEmptyString, m_localIP, text, LANCHAT_SYSTEM);
			LanRoom::postNotice(text);   // O71: the room is gone: the line shows in the lobby too
			LanTest::joinLog("player: the host was silent for 30 s: back to the lobby");
		}
		else if ( AmIHost() && !IsStartPending() )   // O71: not during the start check and the countdown
		{
			// Check each player for timeouts. O60: every seat but the admin's own (slot 0 holds a player while the admin
			// watches from outside the seats)
			for (int p=0; p<MAX_SLOTS; ++p)
			{
				if (m_currentGame->getIP(p) == 0) {
					m_seatJoinedMs[p] = 0;   // O71: an open seat keeps no join clock for its next player
					continue;
				}
				if (m_currentGame->getIP(p) == m_localIP)
					continue;
				// O70/O71: a joiner says HELLO every 2 s now: 30 s of silence (15 lost in a row; was 80 s), or a seat given at a
				// join and never heard from after it for 10 s (its accept was lost and the joiner gave up: S41 hs11), frees it
				const Bool silent = LanClock::silentFor(now, m_currentGame->getPlayerLastHeard(p), 30000);   // O75
				// O71 (addendum): signed: the seat is given inside this update, after its "now" was read (S41 ww30_01/_04:
				// now - joined wrapped round to about 4e9 and freed the seat 0-2 ms after its join)
				const Bool unconfirmed = m_seatJoinedMs[p] != 0 && (Int)(now - m_seatJoinedMs[p]) > 10000;
				if (m_currentGame->getIP(p) && (silent || unconfirmed))
				{
					m_seatJoinedMs[p] = 0;
					LanTest::joinLog("host: seat %d freed (%s)", p, unconfirmed ? "never heard from after its join" : "silent for 30 s");
					LANMessage msg;
					fillInLANMessage( &msg );
					UnicodeString theStr;
					theStr.format(TheGameText->fetch("LAN:PlayerDropped"), m_currentGame->getPlayerName(p).str());
					msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
					wcslcpy(msg.name, m_currentGame->getPlayerName(p).str(), ARRAY_SIZE(msg.name));
					const UnsignedInt droppedIP = m_currentGame->getIP(p);
					handleRequestGameLeave(&msg, droppedIP);   // sends it the list without its seat once
					// O71: and twice more, so that one lost packet does not leave it in a room it is not in (its own
					// 2 s keep-alive copies of the list come by broadcast as well)
					RequestGameOptions(GenerateGameOptionsString(), false, droppedIP);
					RequestGameOptions(GenerateGameOptionsString(), false, droppedIP);
					OnChat(UnicodeString::TheEmptyString, m_localIP, theStr, LANCHAT_SYSTEM);
				}
			}
		}
	}

	if (playerListChanged)
	{
		OnPlayerList(m_lobbyPlayers);
	}

	if (gameListChanged)
	{
		OnGameList(m_games);
	}

	// O45: a join with no answer yet asks again every second, until the accept, a denial or the 5 s timeout
	if (m_pendingAction == ACT_JOIN && s_joinTries > 0 && now - s_joinSentMs >= kJoinRepeatMs && (Int)(m_expiration - now) > 300)
	{
		sendMessage(&s_joinRequest, s_joinRequestIP);
		s_joinSentMs = now;
		++s_joinTries;
		LanTest::joinLog("joiner: join request again (try %d)", s_joinTries);
	}
	if (m_pendingAction != ACT_JOIN) {
		s_joinTries = 0;
	}

	// Time out old actions
	if (m_pendingAction != ACT_NONE && (Int)(now - m_expiration) > 0)
	{
		switch (m_pendingAction)
		{
		case ACT_JOIN:
			LanTest::joinLog("joiner: join TIMED OUT after %d tries", s_joinTries);
			s_joinTries = 0;
			{
				// O70: the host may have seated this computer while every accept was lost (S41 hs11: a seat held for
				// nobody, the room then "full"): it is told that nobody comes, so it frees the seat at once
				LANMessage leave;
				fillInLANMessage(&leave);
				leave.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
				wcslcpy(leave.GameToLeave.gameName, s_joinRequest.GameToJoin.gameName, ARRAY_SIZE(leave.GameToLeave.gameName));
				sendMessage(&leave, s_joinRequest.GameToJoin.gameIP);
			}
			OnGameJoin(RET_TIMEOUT, nullptr);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		case ACT_LEAVE:
			OnPlayerLeave(m_name);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		case ACT_JOINDIRECTCONNECT:
			OnGameJoin(RET_TIMEOUT, nullptr);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		default:
			m_pendingAction = ACT_NONE;
		}
	}

	pumpStartCheck();   // O62
	pumpStartDelivery();   // O74
	pumpConvergence();  // O70

	// send out "game starting" messages
	if ( m_gameStartTime && m_gameStartSeconds && (Int)(now - m_gameStartTime) >= 0 )
	{
		// m_gameStartTime is when the next message goes out
		// m_gameStartSeconds is how many seconds remain in the message

		RequestGameStartTimer( m_gameStartSeconds );
	}
	else if (m_gameStartTime && (Int)(now - m_gameStartTime) >= 0)
	{
//		DEBUG_LOG(("m_gameStartTime=%d, now=%d, m_gameStartSeconds=%d", m_gameStartTime, now, m_gameStartSeconds));
		ResetGameStartTimer();
		RequestGameStart();
	}

	// Check for an MOTD every few seconds
	static UnsignedInt lastMOTDCheck = 0;
	static const UnsignedInt motdInterval = 30000;
	if (LanClock::due(now, lastMOTDCheck, motdInterval))
	{
		checkMOTD();
		lastMOTDCheck = now;
	}
}

// Request functions generate network traffic
void LANAPI::RequestLocations()
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOCATIONS;
	fillInLANMessage( &msg );
	sendMessage(&msg);
}

void LANAPI::RequestGameJoin( LANGameInfo *game, UnsignedInt ip /* = 0 */ )
{
	if ((m_pendingAction != ACT_NONE) && (m_pendingAction != ACT_JOINDIRECTCONNECT))
	{
		OnGameJoin( RET_BUSY, nullptr );
		return;
	}

	if (!game)
	{
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}
	// O62: another version's room is not joined (the lobby says so first; this also covers a direct connect)
	// O64 (H6 #2): only a room its host himself announced (a row made from a running game's announce is never joined)
	if (!game->isHostVerified())
	{
		LanTest::joinLog("joiner: the room was never announced by its host: not joined");
		m_pendingAction = ACT_NONE;
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}
	if (!LanSameVersion(game))   // O63: the build identity too
	{
		LanTest::joinLog("joiner: the room is protocol %d build '%s', this one %d '%s': not joined", game->getLanProtocol(),
			game->getLanBuild().str(), LANProtocolVersion, LanBuildIdentity().str());
		m_pendingAction = ACT_NONE;
		OnGameJoin( RET_CRC_MISMATCH, nullptr );
		return;
	}

	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_JOIN;
	fillInLANMessage( &msg );
	msg.GameToJoin.gameIP = game->getHostIP();   // O60: the admin, in a seat or not
	wcslcpy(msg.GameToJoin.gameName, game->getName().str(), ARRAY_SIZE(msg.GameToJoin.gameName));   // O62
	msg.GameToJoin.protocolMagic = LANProtocolMagic;
	msg.GameToJoin.protocol = LANProtocolVersion;
	strlcpy(msg.GameToJoin.build, LanBuildIdentity().str(), ARRAY_SIZE(msg.GameToJoin.build));   // O63
	msg.GameToJoin.exeCRC = TheGlobalData->m_exeCRC;
	msg.GameToJoin.iniCRC = TheGlobalData->m_iniCRC;

	AsciiString s;
	GetStringFromRegistry("\\ergc", "", s);
	strlcpy(msg.GameToJoin.serial, s.str(), ARRAY_SIZE(msg.GameToJoin.serial));

	sendMessage(&msg, ip);

	m_pendingAction = ACT_JOIN;
	m_expiration = LanClock::stamp(LanClock::now() + m_actionTimeout);
	// O45: kept for the repeats (LANAPI::update)
	s_joinRequest = msg;
	s_joinRequestIP = ip;
	s_joinSentMs = LanClock::now();
	s_joinTries = 1;
	LanTest::joinLog("joiner: join request to %d.%d.%d.%d (try 1)", PRINTF_IP_AS_4_INTS(msg.GameToJoin.gameIP));
}

void LANAPI::RequestGameJoinDirectConnect(UnsignedInt ipaddress)
{
	if (m_pendingAction != ACT_NONE)
	{
		OnGameJoin( RET_BUSY, nullptr );
		return;
	}

	if (ipaddress == 0)
	{
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}

	m_directConnectRemoteIP = ipaddress;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_GAME_INFO;
	fillInLANMessage(&msg);
	msg.PlayerInfo.ip = GetLocalIP();
	wcslcpy(msg.PlayerInfo.playerName, m_name.str(), ARRAY_SIZE(msg.PlayerInfo.playerName));

	sendMessage(&msg, ipaddress);

	m_pendingAction = ACT_JOINDIRECTCONNECT;
	m_expiration = LanClock::stamp(LanClock::now() + m_actionTimeout);
}

void LANAPI::RequestGameLeave()
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
	fillInLANMessage( &msg );
	wcslcpy(msg.PlayerInfo.playerName, m_name.str(), ARRAY_SIZE(msg.PlayerInfo.playerName));
	sendMessage(&msg);
	m_transport->update();  // Send immediately, before OnPlayerLeave below resets everything.

	if (m_currentGame && m_currentGame->getHostIP() == m_localIP)
	{
		// Exit out immediately if we're hosting
		OnPlayerLeave(m_name);
		removeGame(m_currentGame);
		delete m_currentGame;
		m_currentGame = nullptr;
		m_inLobby = true;
	}
	else
	{
		m_pendingAction = ACT_LEAVE;
		m_expiration = LanClock::stamp(LanClock::now() + m_actionTimeout);
	}
}

void LANAPI::RequestGameAnnounce()
{
	// In game - are we a game host?
	if (m_currentGame && !(m_currentGame->getIsDirectConnect()))
	{
		if (m_currentGame->getHostIP() == m_localIP || (m_currentGame->isGameInProgress() && TheNetwork && TheNetwork->isPacketRouter())) // if we're in game we should reply if we're the packet router
		{
			AsciiString gameOpts = GameInfoToAsciiString(m_currentGame);
			if (gameOpts.isEmpty())
			{
				return;
			}

			LANMessage reply;
			fillInLANMessage( &reply );
			reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;

			strlcpy(reply.GameInfo.options,gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
			wcslcpy(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName));
			reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
			reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

			sendMessage(&reply);
		}
	}
}

void LANAPI::RequestAccept()
{
	if (m_inLobby || !m_currentGame)
		return;
	// O70: wanted until the host's slot list shows it (pumpConvergence), for the room as it is now
	if (!m_currentGame->amIHost())
	{
		if (m_convergeGame != m_currentGame)
		{
			for (Int j = 0; j < CONVERGE_COUNT; ++j) m_optionPending[j] = FALSE;
			m_mapLastSent = 0;
			m_hostViewMs = 0;
		}
		m_convergeGame = m_currentGame;
		m_acceptWanted = TRUE;
		m_acceptDigest = LanStartDigest(m_currentGame);
		m_acceptSince = LanClock::now();
		m_acceptLastSent = m_acceptSince;
	}
	sendAcceptOnly();
}

void LANAPI::sendAcceptOnly()
{
	if (m_inLobby || !m_currentGame)
		return;

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_SET_ACCEPT;
	msg.Accept.isAccepted = true;
	wcslcpy(msg.Accept.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.Accept.gameName));
	sendMessage(&msg);
}

void LANAPI::RequestHasMap()
{
	if (m_inLobby || !m_currentGame)
		return;

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_MAP_AVAILABILITY;
	// O60: the admin outside the seats has no slot (and needs no map of his own for the match)
	const GameSlot *mySlot = m_currentGame->getSlot(m_currentGame->getLocalSlotNum());
	if (mySlot == nullptr) {
		return;
	}
	msg.MapStatus.hasMap = mySlot->hasMap();
	wcslcpy(msg.MapStatus.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.MapStatus.gameName));
	CRC mapNameCRC;
//mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
	AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
	mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
	msg.MapStatus.mapCRC = mapNameCRC.get();
	sendMessage(&msg);

	if (!msg.MapStatus.hasMap)
	{
		UnicodeString text;
		UnicodeString mapDisplayName;
		const MapMetaData *mapData = TheMapCache->findMap( m_currentGame->getMap() );
		Bool willTransfer = TRUE;
		if (mapData)
		{
			mapDisplayName.format(L"%ls", mapData->m_displayName.str());
			if (mapData->m_isOfficial)
				willTransfer = FALSE;
		}
		else
		{
			mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(m_currentGame->getMap()).str());
			willTransfer = WouldMapTransfer(m_currentGame->getMap());
		}
		if (willTransfer)
			text.format(TheGameText->fetch("GUI:LocalPlayerNoMapWillTransfer"), mapDisplayName.str());
		else
			text.format(TheGameText->fetch("GUI:LocalPlayerNoMap"), mapDisplayName.str());
		OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
	}
}

void LANAPI::RequestChat( UnicodeString message, ChatType format )
{
	LANMessage msg;
	fillInLANMessage( &msg );
	wcslcpy(msg.Chat.gameName, (m_currentGame) ? m_currentGame->getName().str() : L"", ARRAY_SIZE(msg.Chat.gameName));
	msg.messageType = LANMessage::MSG_CHAT;
	msg.Chat.chatType = format;
	wcslcpy(msg.Chat.message, message.str(), ARRAY_SIZE(msg.Chat.message));
	sendMessage(&msg);

	OnChat(m_name, m_localIP, message, format);
}

AsciiString LanBuildIdentity()
{
	static AsciiString s_id;
	static Bool s_made = FALSE;
	if (!s_made && TheVersion != nullptr)
	{
		s_made = TRUE;
		// letters, digits and '~' only: it travels in the room's options ("BV=") and in the join
		const AsciiString full = TheVersion->getAsciiGitShortHash();
		for (const char *c = full.str(); GitHaveInfo && c != nullptr && *c != 0 && s_id.getLength() < 15; ++c) {
			if (isalnum((unsigned char)*c) || *c == '~') s_id.concat(*c);
		}
		// O65: a build made without git info has an explicit identity of its own (never an empty one): "nogit" and its
		// exe CRC, so two such builds meet only when their exes are the same
		if (s_id.isEmpty() || (s_id.getLength() == 1 && s_id.str()[0] == '~'))
		{
			s_id.format("nogit%08X", TheGlobalData != nullptr ? TheGlobalData->m_exeCRC : 0u);
		}
	}
	return s_id;
}

UnsignedInt LanStartDigest(LANGameInfo *game)
{
	if (game == nullptr) {
		return 0;
	}
	LANGameInfo canon;
	canon.setName(game->getName());
	if (ParseAsciiStringToGameInfo(&canon, GameInfoToAsciiString(game))) {
		return canon.startDigest();
	}
	return game->startDigest();
}

Bool LanSameVersion(const GameInfo *game)
{
	return game != nullptr && game->getLanProtocol() == LANProtocolVersion && game->getLanBuild().compare(LanBuildIdentity()) == 0;
}

void LANAPI::RequestGameStart()
{
	if (m_inLobby || !m_currentGame || m_currentGame->getHostIP() != m_localIP)
		return;

	// TheSuperHackers @bugfix O62 (H5 #1) START no longer overtakes the slot list (a slot list lost on the way left a
	// player starting from another order): the host sends the options and START_CHECK with the digest of its room
	// (GameInfo::startDigest) until every seated player answers READY with the same digest, and only then START
	// (finishStart); a player whose own copy differs answers nothing and does not start. No answer within
	// kStartCheckMs: the start is stopped with a line naming who did not answer.
	if (!m_startChecking)
	{
		m_startChecking = TRUE;
		m_startDigest = LanStartDigest(m_currentGame);
		m_startCheckSince = LanClock::now();
		m_lastStartCheck = 0;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			m_startReady[i] = FALSE;
			m_startCannot[i] = FALSE;   // O76
		}
		LanTest::joinLog("host: start check begins, digest %08X", m_startDigest);
	}
	pumpStartCheck();
}

void LANAPI::pumpStartCheck()
{
	if (!m_startChecking) {
		return;
	}
	if (m_inLobby || m_currentGame == nullptr || !m_currentGame->amIHost() || m_currentGame->isGameInProgress())
	{
		m_startChecking = FALSE;
		return;
	}
	const UnsignedInt now = LanClock::now();
	const UnsignedInt digest = LanStartDigest(m_currentGame);
	if (digest != m_startDigest)
	{
		// the room changed meanwhile: every answer again
		m_startDigest = digest;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			m_startReady[i] = FALSE;
			m_startCannot[i] = FALSE;   // O76
		}
		LanTest::joinLog("host: start check: the room changed, digest %08X", digest);
	}
	// TheSuperHackers @bugfix O76 A player that cannot start (no map, and none will be transferred) stops the start
	// before anybody loads: the original refused Start for such a player with GUI:PlayerNoMap ("... does not have the
	// map ..."); the same line names it
	{
		Bool cannot = FALSE;
		const MapMetaData *mapData = TheMapCache->findMap(m_currentGame->getMap());
		UnicodeString mapName;
		if (mapData != nullptr) {
			mapName.format(L"%ls", mapData->m_displayName.str());
		} else {
			mapName.format(L"%hs", m_currentGame->getMap().str());
		}
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
			if (m_startReady[i] && m_startCannot[i] && slot != nullptr && slot->isHuman())
			{
				cannot = TRUE;
				UnicodeString text;
				text.format(TheGameText->fetch("GUI:PlayerNoMap"), slot->getName().str(), mapName.str());
				OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
				LanTest::joinLog("host: start check: stopped, slot %d cannot start (no map)", i);
			}
		}
		if (cannot)
		{
			m_startChecking = FALSE;
			LANEnableStartButton(true);
			return;
		}
	}
	UnicodeString missing;
	Bool allReady = TRUE;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() != m_localIP && !m_startReady[i])
		{
			allReady = FALSE;
			if (!missing.isEmpty()) {
				missing.concat(L", ");
			}
			missing.concat(slot->getName());
		}
	}
	if (allReady)
	{
		m_startChecking = FALSE;
		LanTest::joinLog("host: start check: every player READY after %u ms", (unsigned)(now - m_startCheckSince));
		finishStart();
		return;
	}
	const UnsignedInt kStartCheckMs = 8000;
	if ((Int)(now - m_startCheckSince) > (Int)kStartCheckMs)
	{
		m_startChecking = FALSE;
		LanTest::joinLog("host: start check: stopped, no READY from %ls", missing.str());
		UnicodeString text;
		text.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:LanStartNotConfirmed", localizedFallback(
			L"Start stopped: the slot list did not reach %ls. Press Start again.",
			L"\x0421\x0442\x0430\x0440\x0442 \x043E\x0442\x043C\x0435\x043D\x0451\x043D: \x0441\x043F\x0438\x0441\x043E\x043A \x043C\x0435\x0441\x0442 \x043D\x0435 \x0434\x043E\x0448\x0451\x043B \x0434\x043E: %ls. \x041D\x0430\x0436\x043C\x0438\x0442\x0435 \x00AB\x0421\x0442\x0430\x0440\x0442\x00BB \x0435\x0449\x0451 \x0440\x0430\x0437.")).str(), missing.str());
		OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
		LANEnableStartButton(true);
		return;
	}
	if (LanClock::due(now, m_lastStartCheck, 250))
	{
		m_lastStartCheck = now;
		RequestGameOptions(GenerateGameOptionsString(), true);
		LANMessage msg;
		fillInLANMessage(&msg);
		msg.messageType = LANMessage::MSG_START_CHECK;
		msg.StartCheck.magic = LANStartMagic;
		msg.StartCheck.digest = digest;
		wcslcpy(msg.StartCheck.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.StartCheck.gameName));
		msg.StartCheck.revision = m_currentGame->getOptionsRevision();   // O64
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
			if (slot != nullptr && slot->isHuman() && slot->getIP() != m_localIP && !m_startReady[i]) {
				sendMessage(&msg, slot->getIP());
			}
		}
		m_transport->update(); // force a send
	}
}

void LANAPI::finishStart()
{
	// O31: the room's observers get the players' addresses and follow the stream; nobody waits for them
	LanRoom::hostStart(m_currentGame, m_localIP);
	// O40: "Play again" keeps the layout; O47: taken at the Start click (LanGameOptionsMenu), before the open places close

	// TheSuperHackers @bugfix O74 START was sent once (to everybody and to each seat): a seat that lost both copies never
	// started, while the host went on (S41 ww30_02: the watching admin waited for a stream, the player left the room
	// 30 s later; a playing host would have loaded alone). START is now repeated to every seat that has not
	// acknowledged it (MSG_START_ACK, sent before the player leaves for its match) every 200 ms, for at most
	// kStartDeliverMs; then the host starts in any case, as the players that have it are already loading.
	m_startDelivering = TRUE;
	m_startDeliverSince = LanClock::now();
	m_lastStartSend = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_startAcked[i] = FALSE;
		m_startCannot[i] = FALSE;
	}
	sendStartMessage(0);   // O62: to everybody once, then to each seat (pumpStartDelivery)
	pumpStartDelivery();
}

// O78 (H12) Known limits of the start, as in the original: (1) a custom map whose transfer fails after START (the
// player leaves inside OnGameStart while the host transfers and reads no LAN message); (2) a leave after another player
// acknowledged START (the seat stays so every list matches): in both, the others wait the 60 s load-complete timeout
// (PROGRESS_COMPLETE_TIMEOUT), then the match goes on without that player; (3) a player whose five acknowledgements
// were all lost when the start is then stopped: it loads alone, waits that timeout and ends on the original's
// disconnect screen.
void LANAPI::stopStartDelivery(const char *why)
{
	// O76: START is called back while nobody has acknowledged it (a player whose five acknowledgements were all lost has
	// started anyway: its load waits for the others, gives up after the 60 s progress timeout and leaves it in a match
	// without them, on the original's disconnect screen)
	m_startDelivering = FALSE;
	LanTest::joinLog("host: %s, nobody started yet: start stopped", why);
	OnChat(L"SYSTEM", m_localIP, TheGameText->FETCH_OR_SUBSTITUTE("GUI:LanStartRoomChanged", localizedFallback(
		L"The room changed during the start: start stopped. Press Start again.",
		L"\x041A\x043E\x043C\x043D\x0430\x0442\x0430 \x0438\x0437\x043C\x0435\x043D\x0438\x043B\x0430\x0441\x044C \x0432\x043E \x0432\x0440\x0435\x043C\x044F \x0441\x0442\x0430\x0440\x0442\x0430: \x0441\x0442\x0430\x0440\x0442 \x043E\x0442\x043C\x0435\x043D\x0451\x043D. \x041D\x0430\x0436\x043C\x0438\x0442\x0435 \x00AB\x0421\x0442\x0430\x0440\x0442\x00BB \x0435\x0449\x0451 \x0440\x0430\x0437.")), LANCHAT_SYSTEM);
	LANEnableStartButton(true);
}

void LANAPI::sendStartMessage(UnsignedInt ip)
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_GAME_START;
	fillInLANMessage( &msg );
	msg.StartCheck.magic = LANStartMagic;   // O62: the digest every player confirmed
	msg.StartCheck.digest = m_startDigest;
	wcslcpy(msg.StartCheck.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.StartCheck.gameName));
	msg.StartCheck.revision = m_currentGame->getOptionsRevision();   // O64
	sendMessage(&msg, ip);
}

void LANAPI::pumpStartDelivery()
{
	if (!m_startDelivering) {
		return;
	}
	if (m_inLobby || m_currentGame == nullptr || !m_currentGame->amIHost() || m_currentGame->isGameInProgress())
	{
		m_startDelivering = FALSE;
		return;
	}
	const UnsignedInt now = LanClock::now();
	// O75 (H10): the room changed during START (only the host's own option change can: requests, joins and leaves are
	// held meanwhile). Nobody has started yet: the start is stopped. Somebody has: it cannot be called back, the host
	// starts with the room as he has it, and says so.
	if (LanStartDigest(m_currentGame) != m_startDigest)
	{
		Bool anyAcked = FALSE;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			anyAcked = anyAcked || m_startAcked[i];
		}
		if (!anyAcked)
		{
			stopStartDelivery("the room changed during the start");
			return;
		}
		LanTest::joinLog("host: the room changed during START after a player started: starting as it is");
	}
	UnicodeString missing;
	Bool allAcked = TRUE;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() != m_localIP && !m_startAcked[i])
		{
			allAcked = FALSE;
			if (!missing.isEmpty()) {
				missing.concat(L", ");
			}
			missing.concat(slot->getName());
		}
	}
	const UnsignedInt kStartDeliverMs = 3000;
	if (allAcked || (Int)(now - m_startDeliverSince) > (Int)kStartDeliverMs)
	{
		m_startDelivering = FALSE;
		if (allAcked)
		{
			LanTest::joinLog("host: START acknowledged by every player after %u ms", (unsigned)(now - m_startDeliverSince));
		}
		else
		{
			LanTest::joinLog("host: START not acknowledged by %ls after %u ms: starting anyway", missing.str(), (unsigned)(now - m_startDeliverSince));
			UnicodeString text;
			text.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:LanStartNotAcknowledged", localizedFallback(
				L"START was not confirmed by: %ls. They probably started (their confirmation was lost); a player that did not is left out at the end of the map load.",
				L"\x0421\x0442\x0430\x0440\x0442 \x043D\x0435 \x043F\x043E\x0434\x0442\x0432\x0435\x0440\x0434\x0438\x043B\x0438: %ls. \x0421\x043A\x043E\x0440\x0435\x0435 \x0432\x0441\x0435\x0433\x043E, \x043E\x043D\x0438 \x043D\x0430\x0447\x0430\x043B\x0438 (\x043F\x043E\x0434\x0442\x0432\x0435\x0440\x0436\x0434\x0435\x043D\x0438\x0435 \x043F\x043E\x0442\x0435\x0440\x044F\x043B\x043E\x0441\x044C); \x043D\x0435 \x043D\x0430\x0447\x0430\x0432\x0448\x0438\x0439 \x0438\x0433\x0440\x043E\x043A \x043E\x0442\x043A\x043B\x044E\x0447\x0438\x0442\x0441\x044F \x0432 \x043A\x043E\x043D\x0446\x0435 \x0437\x0430\x0433\x0440\x0443\x0437\x043A\x0438 \x043A\x0430\x0440\x0442\x044B.")).str(), missing.str());
			OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
			LanRoom::postNotice(text);   // shown in the lobby after the match too
		}
		m_transport->update(); // force a send
		OnGameStart();
		return;
	}
	if (LanClock::due(now, m_lastStartSend, 200))
	{
		m_lastStartSend = now;
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
			if (slot != nullptr && slot->isHuman() && slot->getIP() != m_localIP && !m_startAcked[i]) {
				sendStartMessage(slot->getIP());
			}
		}
		m_transport->update(); // force a send
	}
}

void LANAPI::noteGuardPeers()
{
	// TheSuperHackers @bugfix O83 (H15) The shields this player's screens show, from his own state: in the lobby, every
	// listed game's host and seats (the game rows, the selected game's seats), the room observers of each listed game,
	// and the lobby players list; in a room, its seats and its observers. (In a match LANAPI::pumpGuard notes the
	// seats.) Join requests to his room are noted by handleRequestJoin. Nothing else enters the anti-cheat's table.
	// O84 (H16): in classes with room of their own (GuardLink::PeerClass). My room: its seats, host and observers. The
	// lobby: the host of each listed row, the seats and observers of the SELECTED row only (never every row's made-up
	// seats), and the lobby players. A host in his room also keeps the lobby players checked (the likely joiners), in the
	// lobby class.
	// O85 (H17): a listed row is one whose announce passed announceFromItsHost: a room not running was announced from its
	// own address (isHostVerified); a RUNNING game by one of its seats, of the row known and of the list sent, naming a
	// seat as its host (or HO=1, its admin watching). Both are named here, so a lobby opened during a match shows that
	// match's marks (the rows people select to watch). Naming is display only: a running row is still not host-verified,
	// so nothing host-only (join, observe, its host's messages) is taken for it (O64).
	if (m_currentGame != nullptr && !m_inLobby)
	{
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			const LANGameSlot *slot = m_currentGame->getConstLANSlot(k);
			if (slot != nullptr && slot->isHuman() && slot->getIP() != 0) {
				GuardLink::notePeer(slot->getIP(), m_localIP, GuardLink::PEER_ROOM);
			}
		}
		GuardLink::notePeer(m_currentGame->getHostIP(), m_localIP, GuardLink::PEER_ROOM);
		const std::vector<LanRoom::Observer> observers = LanRoom::observersOf(m_currentGame->getHostIP());
		for (size_t i = 0; i < observers.size(); ++i) {
			GuardLink::notePeer(observers[i].ip, m_localIP, GuardLink::PEER_ROOM);
		}
		if (m_currentGame->amIHost() && !m_currentGame->isGameInProgress()) {
			for (LANPlayer *player = m_lobbyPlayers; player != nullptr; player = player->getNext()) {
				GuardLink::notePeer(player->getIP(), m_localIP, GuardLink::PEER_LOBBY);
			}
		}
		return;
	}
	for (LANGameInfo *game = m_games; game != nullptr; game = game->getNext())
	{
		if (!game->isHostVerified() && !game->isGameInProgress()) {
			continue;   // O85: neither of the two rules above (every row comes through handleGameAnnounce, so none today)
		}
		GuardLink::notePeer(game->getHostIP(), m_localIP, GuardLink::PEER_LOBBY);
		if (game->getHostIP() != m_lobbySelectedHost) {
			continue;
		}
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			const LANGameSlot *slot = game->getConstLANSlot(k);
			if (slot != nullptr && slot->isHuman() && slot->getIP() != 0) {
				GuardLink::notePeer(slot->getIP(), m_localIP, GuardLink::PEER_LOBBY);
			}
		}
		const std::vector<LanRoom::Observer> observers = LanRoom::observersOf(game->getHostIP());
		for (size_t i = 0; i < observers.size(); ++i) {
			GuardLink::notePeer(observers[i].ip, m_localIP, GuardLink::PEER_LOBBY);
		}
	}
	for (LANPlayer *player = m_lobbyPlayers; player != nullptr; player = player->getNext()) {
		GuardLink::notePeer(player->getIP(), m_localIP, GuardLink::PEER_LOBBY);
	}
}

void LANAPI::pumpRoomBeat(UnsignedInt now)
{
	// O70: the room's keep-alive every 2 s: the host's slot list (the joiners take the host for alive by it), a joiner's
	// HELLO (the host takes the seat for alive by it)
	if (!m_inLobby && m_currentGame != nullptr && !m_currentGame->isGameInProgress() && LanClock::due(now, m_lastRoomBeat, 2000))
	{
		m_lastRoomBeat = now;
		if (AmIHost()) {
			RequestGameOptions( GenerateGameOptionsString(), true );
		} else {
			RequestGameOptions( "HELLO", false, m_currentGame->getHostIP() );   // O71 (H8): to the host, not broadcast
		}
	}
}

void LANAPI::keepRoomAlive()
{
	// TheSuperHackers @bugfix O71 (addendum) A joiner that has its accept opens the room screen: update() does not run
	// until that screen's init (LANbuttonPushed, set by OnGameJoin) and while the shell animates, so its first HELLO
	// and the requests OnGameJoin queued waited for the screen. On a busy computer that took long enough for the host's
	// 10 s rule (S41 ww30_02). The keep-alive goes on meanwhile, and the queue is sent; nothing is received here
	// (update() does that, with the room's checks).
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->isGameInProgress() || m_transport == nullptr) {
		return;
	}
	pumpRoomBeat(LanClock::now());
	noteGuardPeers();   // O84: a screen change does not let the shown addresses go stale
	m_transport->doSend();
}

void LANAPI::noteHeardFrom(UnsignedInt senderIP, Int messageType)
{
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->isGameInProgress() || senderIP == 0) {
		return;
	}
	// O73 (H9): the messages a player or host sends to its room; not the lobby-wide ones (announces, locations, files,
	// the room observers' MSG_ROOM), which another instance on the same address could send for a player that is gone
	switch (messageType)
	{
	case LANMessage::MSG_GAME_OPTIONS: case LANMessage::MSG_SET_ACCEPT: case LANMessage::MSG_MAP_AVAILABILITY:
	case LANMessage::MSG_CHAT: case LANMessage::MSG_START_READY: case LANMessage::MSG_REQUEST_JOIN:
	case LANMessage::MSG_JOIN_ACCEPT: case LANMessage::MSG_START_CHECK: case LANMessage::MSG_GAME_START_TIMER:
	case LANMessage::MSG_INACTIVE: case LANMessage::MSG_START_ACK: case LANMessage::MSG_GAME_START:
		break;
	default:
		return;
	}
	const UnsignedInt t = LanClock::now();
	if (m_currentGame->amIHost())
	{
		for (Int p = 0; p < MAX_SLOTS; ++p)
		{
			if (m_currentGame->getIP(p) == senderIP && senderIP != m_localIP)
			{
				m_currentGame->setPlayerLastHeard(p, t);
				m_seatJoinedMs[p] = 0;
			}
		}
	}
	else if (senderIP == m_currentGame->getHostIP())
	{
		m_currentGame->setLastHeard(t);
	}
}

void LANAPI::noteHostView(const AsciiString &options)
{
	if (m_currentGame == nullptr || m_currentGame->amIHost()) {
		return;
	}
	LANGameInfo view;
	view.setName(m_currentGame->getName());
	if (!ParseAsciiStringToGameInfo(&view, options)) {
		return;
	}
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot *slot = view.getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() == m_localIP)
		{
			if (m_convergeGame != m_currentGame)
			{
				m_convergeGame = m_currentGame;
				m_acceptWanted = FALSE;
				for (Int k = 0; k < CONVERGE_COUNT; ++k) m_optionPending[k] = FALSE;
				m_mapLastSent = 0;
			}
			m_hostViewMs = LanClock::now();
			m_hostSeesAccepted = slot->isAccepted();
			m_hostSeesMap = slot->hasMap();
			m_hostSeesValue[CONVERGE_COLOR] = slot->getColor();
			m_hostSeesValue[CONVERGE_TEMPLATE] = slot->getPlayerTemplate();
			m_hostSeesValue[CONVERGE_STARTPOS] = slot->getStartPos();
			m_hostSeesValue[CONVERGE_TEAM] = slot->getTeamNumber();
			return;
		}
	}
}

void LANAPI::pumpConvergence()
{
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->amIHost() || m_currentGame->isGameInProgress()
		|| m_convergeGame != m_currentGame || m_hostViewMs == 0) {
		return;
	}
	const Int local = m_currentGame->getLocalSlotNum();
	const LANGameSlot *mine = local >= 0 ? m_currentGame->getConstLANSlot(local) : nullptr;
	if (mine == nullptr) {
		return;
	}
	const UnsignedInt now = LanClock::now();
	// accept: again every 0.7 s until the host's list (newer than the last send) shows it; a change of the room by the
	// host resets every accept by design, and then this one is not wanted any more; at most 30 s
	if (m_acceptWanted)
	{
		if (m_hostSeesAccepted && (Int)(m_hostViewMs - m_acceptLastSent) > 0) {   // O73: the order of two stamps, signed
			m_acceptWanted = FALSE;
		} else if (LanStartDigest(m_currentGame) != m_acceptDigest || (Int)(now - m_acceptSince) > 30000) {
			m_acceptWanted = FALSE;
			// O71 (H8): said, not dropped silently; the room shows this player not accepted, the click accepts again
			LanTest::joinLog("player: accept no longer sent again (%s)", LanStartDigest(m_currentGame) != m_acceptDigest
				? "the host changed the room: every accept was reset" : "30 s without the host's list showing it");
		} else if ((Int)(now - m_acceptLastSent) >= 700) {
			m_acceptLastSent = now;
			LanTest::joinLog("player: accept again (the host's list does not show it)");
			sendAcceptOnly();
		}
	}
	// has the map: this computer's own check (GameInfo::setMapCRC/setMapSize) against what the host's list says
	if (mine->hasMap() != m_hostSeesMap && LanClock::due(now, m_mapLastSent, 1500) && (Int)(now - m_hostViewMs) < 5000)
	{
		m_mapLastSent = now;
		LanTest::joinLog("player: map status again (mine %d, the host's list %d)", mine->hasMap() ? 1 : 0, m_hostSeesMap ? 1 : 0);
		RequestHasMap();
	}
	// army, colour, team, start place: again every 0.7 s for 5 s while the host's list shows another value (a value the
	// host refuses, e.g. a colour taken meanwhile, stays refused after that)
	static const char *const kKeys[CONVERGE_COUNT] = { "Color", "PlayerTemplate", "StartPos", "Team" };
	for (Int k = 0; k < CONVERGE_COUNT; ++k)
	{
		if (!m_optionPending[k]) {
			continue;
		}
		if (m_hostSeesValue[k] == m_optionWanted[k] && (Int)(m_hostViewMs - m_optionLastSent[k]) > 0) {
			m_optionPending[k] = FALSE;
		} else if ((Int)(now - m_optionSince[k]) > 5000) {
			m_optionPending[k] = FALSE;
		} else if ((Int)(now - m_optionLastSent[k]) >= 700) {
			m_optionLastSent[k] = now;
			AsciiString request;
			request.format("%s=%d", kKeys[k], m_optionWanted[k]);
			LANMessage msg;
			fillInLANMessage(&msg);
			msg.messageType = LANMessage::MSG_GAME_OPTIONS;
			strlcpy(msg.GameOptions.options, request.str(), ARRAY_SIZE(msg.GameOptions.options));
			msg.GameOptions.revisionMagic = 0;
			msg.GameOptions.revision = 0;
			sendMessage(&msg, m_currentGame->getHostIP());
			LanTest::joinLog("player: %s again", request.str());
		}
	}
}

void LANAPI::ResetGameStartTimer()
{
	m_gameStartTime = 0;
	m_gameStartSeconds = 0;
	m_startChecking = FALSE;   // O62: a change of the room (resetAccepted) stops a start check too
}

void LANAPI::RequestGameStartTimer( Int seconds )
{
	if (m_inLobby || !m_currentGame || m_currentGame->getHostIP() != m_localIP)
		return;

	UnsignedInt now = LanClock::now();
	m_gameStartTime = LanClock::stamp(now + 1000);   // O75: 0 means "no countdown"
	m_gameStartSeconds = (seconds) ? seconds - 1 : 0;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_GAME_START_TIMER;
	msg.StartTimer.seconds = seconds;
	fillInLANMessage( &msg );
	sendMessage(&msg);
	m_transport->update(); // force a send

	OnGameStartTimer(seconds);
}

void LANAPI::RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip /* = 0 */ )
{
	DEBUG_ASSERTCRASH(gameOptions.getLength() <= m_lanMaxOptionsLength, ("Game options string is too long!"));

	if (!m_currentGame || gameOptions.isEmpty())
	{
		return;
	}
	// O70: a joiner's request of army, colour, team or start place: wanted until the host's slot list shows it
	if (!isPublic && !m_currentGame->amIHost())
	{
		static const char *const kKeys[CONVERGE_COUNT] = { "Color", "PlayerTemplate", "StartPos", "Team" };
		for (Int k = 0; k < CONVERGE_COUNT; ++k)
		{
			const Int keyLength = (Int)strlen(kKeys[k]);
			if (strncmp(gameOptions.str(), kKeys[k], keyLength) == 0 && gameOptions.str()[keyLength] == '=')
			{
				if (m_convergeGame != m_currentGame) {
					m_convergeGame = m_currentGame;
					m_acceptWanted = FALSE;
					for (Int j = 0; j < CONVERGE_COUNT; ++j) m_optionPending[j] = FALSE;
					m_mapLastSent = 0;
					m_hostViewMs = 0;
				}
				m_optionPending[k] = TRUE;
				m_optionWanted[k] = atoi(gameOptions.str() + keyLength + 1);
				m_optionSince[k] = LanClock::now();
				m_optionLastSent[k] = m_optionSince[k];
			}
		}
	}

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_GAME_OPTIONS;
	strlcpy(msg.GameOptions.options, gameOptions.str(), ARRAY_SIZE(msg.GameOptions.options));
	// TheSuperHackers @bugfix O64 (H6 #4) The host's slot list carries a revision that grows with every change; a player
	// ignores an older one that arrives late (it would roll its copy back after its READY)
	msg.GameOptions.revisionMagic = 0;
	msg.GameOptions.revision = 0;
	if (m_currentGame->amIHost())
	{
		if (gameOptions.compare(m_currentGame->getLastOptionsSent()) != 0)
		{
			m_currentGame->setOptionsRevision(m_currentGame->getOptionsRevision() + 1);
			m_currentGame->setLastOptionsSent(gameOptions);
		}
		msg.GameOptions.revisionMagic = LANProtocolMagic;
		msg.GameOptions.revision = m_currentGame->getOptionsRevision();
	}
	sendMessage(&msg, ip);

	m_lastGameopt = gameOptions;

	int player;
	for (player = 0; player<MAX_SLOTS; ++player)
	{
		if (m_currentGame->getIP(player) == m_localIP)
		{
			OnGameOptions(m_localIP, player, AsciiString(msg.GameOptions.options));
			break;
		}
	}

	// We can request game options (side, color, etc) while we don't have a slot yet.  Of course, we don't need to
	// call OnGameOptions for those, so it's okay to silently fail.
	//DEBUG_ASSERTCRASH(player != MAX_SLOTS, ("Requested game options, but we're not in slot list!");
}

void LANAPI::RequestGameCreate( UnicodeString gameName, Bool isDirectConnect )
{
	LanRoom::resetHostDelay();   // O56 (row 43): a new room starts from Options.ini's delay for outside viewers
	// No games of the same name should exist...  Ignore that for now.
	/// @todo: make sure LAN games with identical names don't crash things like in RA2.

	if ((!m_inLobby || m_currentGame) && !isDirectConnect)
	{
		DEBUG_ASSERTCRASH(m_inLobby && m_currentGame, ("Can't create a game while in one!"));
		OnGameCreate(LANAPIInterface::RET_BUSY);
		return;
	}

	if (m_pendingAction != ACT_NONE)
	{
		OnGameCreate(LANAPIInterface::RET_BUSY);
		return;
	}

	// Create the local game object
	m_inLobby = false;
	for (Int k = 0; k < MAX_SLOTS; ++k) m_seatJoinedMs[k] = 0;   // O71: nothing of an earlier room (Play again)
	m_lastRoomBeat = 0;
	m_lastLoopMs = 0;
	m_startSeenMs = 0;
	LANGameInfo *myGame = NEW LANGameInfo;

	myGame->setSeed(GetTickCount());

//	myGame->setInProgress(false);
	myGame->enterGame();
	myGame->setLanProtocol(LANProtocolVersion);   // O62: this build's rooms carry its protocol
	myGame->setLanBuild(LanBuildIdentity());       // O63: and its build identity
	myGame->setHostVerified(TRUE);                 // O64
	UnicodeString s;
	s.format(L"%8.8X%8.8X", m_localIP, myGame->getSeed());
	if (gameName.isEmpty())
		s.concat(m_name);
	else
		s.concat(gameName);

	s.truncateTo(g_lanGameNameLength);

	DEBUG_LOG(("Setting local game name to '%ls'", s.str()));

	myGame->setName(s);

	LANGameSlot newSlot;
	newSlot.setState(SLOT_PLAYER, m_name);
	newSlot.setIP(m_localIP);
	newSlot.setPort(NETWORK_BASE_PORT_NUMBER); // LAN game, everyone has a unique IP, so it's ok to use the same port.
	newSlot.setLastHeard(0);
	newSlot.setLogin(m_userName);
	newSlot.setHost(m_hostName);

	myGame->setSlot(0,newSlot);
	myGame->setNext(nullptr);
	LANPreferences pref;

	AsciiString mapName = pref.getPreferredMap();

	myGame->setMap(mapName);
	myGame->setIsDirectConnect(isDirectConnect);

	myGame->setLastHeard(LanClock::now());
	m_currentGame = myGame;

/// @todo: Need to initialize the players elsewere.
/*	for (int player = 1; player < MAX_SLOTS; ++player)
	{
		myGame->setPlayerName(player, L"");
		myGame->setIP(player, 0);
		myGame->setAccepted(player, false);
	}*/

	// Add the game to the local game list
	addGame(myGame);

	// Send an announcement
	//RequestSlotList();
/*
	LANMessage msg;
	wcslcpy(msg.name, m_name.str(), ARRAY_SIZE(msg.name));
	wcscpy(msg.GameInfo.gameName, myGame->getName().str());
	for (player=0; player<MAX_SLOTS; ++player)
	{
		wcscpy(msg.GameInfo.name[player], myGame->getPlayerName(player).str());
		msg.GameInfo.ip[player] = myGame->getIP(player);
		msg.GameInfo.playerAccepted[player] = myGame->getAccepted(player);
	}
	msg.messageType = LANMessage::MSG_GAME_ANNOUNCE;
*/
	OnGameCreate(LANAPIInterface::RET_OK);
}


/*static const char slotListID		= 'S';
static const char gameOptionsID	= 'G';
static const char acceptID			= 'A';
static const char wannaStartID	= 'W';

AsciiString LANAPI::createSlotString()
{
	AsciiString slotList;
	slotList.concat(slotListID);
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		LANGameSlot *slot = GetMyGame()->getLANSlot(i);
		AsciiString str;
		if (slot->isHuman())
		{
			str = "H";
			LANPlayer *user = slot->getUser();
			DEBUG_ASSERTCRASH(user, ("Human player has no User*!"));
			AsciiString name;
			name.translate(user->getName());
			str.concat(name);
			str.concat(',');
		}
		else if (slot->isAI())
		{
			if (slot->getState() == SLOT_EASY_AI)
				str = "CE,";
			if (slot->getState() == SLOT_MED_AI)
				str = "CM,";
			else
				str = "CB,";
		}
		else if (slot->getState() == SLOT_OPEN)
		{
			str = "O,";
		}
		else if (slot->getState() == SLOT_CLOSED)
		{
			str = "X,";
		}
		else
		{
			DEBUG_CRASH(("Bad slot type"));
			str = "X,";
		}

		slotList.concat(str);
	}
	return slotList;
}
*/
/*
void LANAPI::RequestSlotList()
{

	LANMessage reply;
	reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
	wcslcpy(reply.name, m_name.str(), ARRAY_SIZE(reply.name));
	int player;
	for (player = 0; player < MAX_SLOTS; ++player)
	{
		wcslcpy(reply.GameInfo.name[player], m_currentGame->getPlayerName(player).str(), ARRAY_SIZE(reply.GameInfo.name[player]));
		reply.GameInfo.ip[player] = m_currentGame->getIP(player);
		reply.GameInfo.playerAccepted[player] = m_currentGame->getSlot(player)->isAccepted();
	}
	wcslcpy(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName));
	reply.GameInfo.inProgress = m_currentGame->isGameInProgress();

	sendMessage(&reply);

	OnSlotList(LANAPIInterface::RET_OK, m_currentGame);
}
*/
void LANAPI::RequestSetName( UnicodeString newName )
{
	newName.trim();
	if (m_pendingAction != ACT_NONE)
	{
		// Can't change name while joining games
		OnNameChange(m_localIP, newName);
		return;
	}

	// Set up timer
	m_lastResendTime = LanClock::now();

	if (m_inLobby && m_pendingAction == ACT_NONE)
	{
		m_name = newName;
		LANMessage msg;
		fillInLANMessage( &msg );
		msg.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;
		sendMessage(&msg);

		// Update the interface
		LANPlayer *player = LookupPlayer(m_localIP);
		if (!player)
		{
			player = NEW LANPlayer;
			player->setIP(m_localIP);
		}
		else
		{
			removePlayer(player);
		}
		player->setName(m_name);
		player->setHost(m_hostName);
		player->setLogin(m_userName);
		player->setLastHeard(LanClock::now());

		addPlayer(player);

		OnNameChange(player->getIP(), player->getName());
	}
}

void LANAPI::fillInLANMessage( LANMessage *msg )
{
	if (!msg)
		return;

	wcslcpy(msg->name, m_name.str(), ARRAY_SIZE(msg->name));
	strlcpy(msg->userName, m_userName.str(), ARRAY_SIZE(msg->userName));
	strlcpy(msg->hostName, m_hostName.str(), ARRAY_SIZE(msg->hostName));
}

void LANAPI::RequestLobbyLeave( Bool forced )
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOBBY_LEAVE;
	fillInLANMessage( &msg );
	sendMessage(&msg);

	if (forced)
		m_transport->update();
}

// Misc utility functions
LANGameInfo * LANAPI::LookupGame( UnicodeString gameName )
{
	LANGameInfo *theGame = m_games;

	while (theGame && theGame->getName() != gameName)
	{
		theGame = theGame->getNext();
	}

	return theGame; // null means we didn't find anything.
}

LANGameInfo * LANAPI::LookupGameByListOffset( Int offset )
{
	LANGameInfo *theGame = m_games;

	if (offset < 0)
		return nullptr;

	while (offset-- && theGame)
	{
		theGame = theGame->getNext();
	}

	return theGame; // null means we didn't find anything.
}

LANGameInfo* LANAPI::LookupGameByHost(UnsignedInt hostIP)
{
	LANGameInfo* lastGame = nullptr;
	UnsignedInt lastHeard = 0;

	// O65: a row its host announced himself (host-verified) before one made from a running game's announce
	for (Int pass = 0; pass < 2 && lastGame == nullptr; ++pass)
	{
		for (LANGameInfo* game = m_games; game; game = game->getNext())
		{
			if (game->getHostIP() == hostIP && (pass == 1 || game->isHostVerified()) && game->getLastHeard() >= lastHeard)
			{
				lastGame = game;
				lastHeard = game->getLastHeard();
			}
		}
	}

	return lastGame;
}

void LANAPI::removeGame( LANGameInfo *game )
{
	LANGameInfo *g = m_games;
	if (!game)
	{
		return;
	}
	else if (m_games == game)
	{
		m_games = m_games->getNext();
	}
	else
	{
		while (g->getNext() && g->getNext() != game)
		{
			g = g->getNext();
		}
		if (g->getNext() == game)
		{
			g->setNext(game->getNext());
		}
		else
		{
			// Odd.  We went the whole way without finding it in the list.
			DEBUG_CRASH(("LANGameInfo wasn't in the list"));
		}
	}
}

LANPlayer * LANAPI::LookupPlayer( UnsignedInt playerIP )
{
	LANPlayer *thePlayer = m_lobbyPlayers;

	while (thePlayer && thePlayer->getIP() != playerIP)
	{
		thePlayer = thePlayer->getNext();
	}

	return thePlayer; // null means we didn't find anything.
}

void LANAPI::removePlayer( LANPlayer *player )
{
	LANPlayer *p = m_lobbyPlayers;
	if (!player)
	{
		return;
	}
	else if (m_lobbyPlayers == player)
	{
		m_lobbyPlayers = m_lobbyPlayers->getNext();
	}
	else
	{
		while (p->getNext() && p->getNext() != player)
		{
			p = p->getNext();
		}
		if (p->getNext() == player)
		{
			p->setNext(player->getNext());
		}
		else
		{
			// Odd.  We went the whole way without finding it in the list.
			DEBUG_CRASH(("LANPlayer wasn't in the list"));
		}
	}
}

void LANAPI::addGame( LANGameInfo *game )
{
	if (!m_games)
	{
		m_games = game;
		game->setNext(nullptr);
		return;
	}
	else
	{
		if (game->getName().compareNoCase(m_games->getName()) < 0)
		{
			game->setNext(m_games);
			m_games = game;
			return;
		}
		else
		{
			LANGameInfo *g = m_games;
			while (g->getNext() && g->getNext()->getName().compareNoCase(game->getName()) > 0)
			{
				g = g->getNext();
			}
			game->setNext(g->getNext());
			g->setNext(game);
			return;
		}
	}
}

void LANAPI::addPlayer( LANPlayer *player )
{
	if (!m_lobbyPlayers)
	{
		m_lobbyPlayers = player;
		player->setNext(nullptr);
		return;
	}
	else
	{
		if (player->getName().compareNoCase(m_lobbyPlayers->getName()) < 0)
		{
			player->setNext(m_lobbyPlayers);
			m_lobbyPlayers = player;
			return;
		}
		else
		{
			LANPlayer *p = m_lobbyPlayers;
			while (p->getNext() && p->getNext()->getName().compareNoCase(player->getName()) > 0)
			{
				p = p->getNext();
			}
			player->setNext(p->getNext());
			p->setNext(player);
			return;
		}
	}
}

Bool LANAPI::SetLocalIP( UnsignedInt localIP )
{
	// TheSuperHackers @bugfix O64 (H6 #5) Never while this computer hosts a room: its name holds the address it was made
	// on, and every lobby drops an announce from another address. The LAN screen sets the address only when it opens
	// (LanLobbyMenuInit), when no room is open; this keeps it so.
	if (m_currentGame != nullptr && m_currentGame->amIHost() && localIP != m_localIP)
	{
		DEBUG_LOG(("LANAPI::SetLocalIP - not while hosting a room"));
		return TRUE;
	}
	Bool retval = TRUE;
	m_localIP = (LanTest::loopbackSlot() >= 0) ? LanTest::loopbackIP() : localIP;   // O26 test switch

	m_transport->reset();
	retval = m_transport->init(m_localIP, lobbyPort);
	m_transport->allowBroadcasts(true);

	return retval;
}

void LANAPI::SetLocalIP( AsciiString localIP )
{
	UnsignedInt resolvedIP = ResolveIP(localIP);
	SetLocalIP(resolvedIP);
}

Bool LANAPI::AmIHost()
{
	return m_currentGame && m_currentGame->getHostIP() == m_localIP;
}

void LANAPI::setIsActive(Bool isActive) {
	DEBUG_LOG(("LANAPI::setIsActive - entering"));
	if (isActive != m_isActive) {
		DEBUG_LOG(("LANAPI::setIsActive - m_isActive changed to %s", isActive ? "TRUE" : "FALSE"));
		if (isActive == FALSE) {
			if ((m_inLobby == FALSE) && (m_currentGame != nullptr)) {
				LANMessage msg;
				fillInLANMessage( &msg );
				msg.messageType = LANMessage::MSG_INACTIVE;
				sendMessage(&msg);
				DEBUG_LOG(("LANAPI::setIsActive - sent an IsActive message"));
			}
		}
	}
	m_isActive = isActive;
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature O30 (row 15) The anti-cheat's messages (GuardLink.h)
void LANAPI::sendGuardData(UnsignedInt ip, const UnsignedByte *data, Int length)
{
	if (ip == 0 || data == nullptr || length <= 0 || length > LANGuardDataMax) {
		return;
	}
	LANMessage msg;
	memset(&msg, 0, sizeof(msg));
	fillInLANMessage(&msg);
	msg.messageType = LANMessage::MSG_GUARD_DATA;
	msg.GuardData.magic = LANGuardDataMagic;
	msg.GuardData.length = (UnsignedShort)length;
	memcpy(msg.GuardData.data, data, length);
	sendMessageOnly(&msg, ip);
}

/// TheSuperHackers @bugfix O33 (review 7) A text field of a received message may lack its terminator; the handlers copy
/// them with wcslcpy/strlcpy or build strings from them, which read up to the terminator. Each field of the message's
/// type gets one at its last character (the message buffer holds a whole LANMessage). The Room fields are copied with
/// bounded copies (LanRoom.cpp) and are left as they are.
void LANAPI::terminateStrings(LANMessage *msg)
{
	msg->name[ARRAY_SIZE(msg->name) - 1] = 0;
	msg->userName[ARRAY_SIZE(msg->userName) - 1] = 0;
	msg->hostName[ARRAY_SIZE(msg->hostName) - 1] = 0;
	switch (msg->messageType)
	{
		case LANMessage::MSG_GAME_ANNOUNCE:
			msg->GameInfo.gameName[ARRAY_SIZE(msg->GameInfo.gameName) - 1] = 0;
			msg->GameInfo.options[ARRAY_SIZE(msg->GameInfo.options) - 1] = 0;
			break;
		case LANMessage::MSG_REQUEST_GAME_LEAVE:
			msg->GameToLeave.gameName[ARRAY_SIZE(msg->GameToLeave.gameName) - 1] = 0;
			break;
		case LANMessage::MSG_REQUEST_GAME_INFO:
			msg->PlayerInfo.playerName[ARRAY_SIZE(msg->PlayerInfo.playerName) - 1] = 0;
			break;
		case LANMessage::MSG_REQUEST_JOIN:
			msg->GameToJoin.serial[ARRAY_SIZE(msg->GameToJoin.serial) - 1] = 0;
			msg->GameToJoin.gameName[ARRAY_SIZE(msg->GameToJoin.gameName) - 1] = 0;   // O65
			msg->GameToJoin.build[ARRAY_SIZE(msg->GameToJoin.build) - 1] = 0;
			break;
		case LANMessage::MSG_JOIN_ACCEPT:
			msg->GameJoined.gameName[ARRAY_SIZE(msg->GameJoined.gameName) - 1] = 0;
			msg->GameJoined.build[ARRAY_SIZE(msg->GameJoined.build) - 1] = 0;   // O65
			break;
		case LANMessage::MSG_GAME_START:   // O65: the start handshake's room name
		case LANMessage::MSG_START_CHECK:
		case LANMessage::MSG_START_READY:
		case LANMessage::MSG_START_ACK:   // O74
			msg->StartCheck.gameName[ARRAY_SIZE(msg->StartCheck.gameName) - 1] = 0;
			break;
		case LANMessage::MSG_JOIN_DENY:
			msg->GameNotJoined.gameName[ARRAY_SIZE(msg->GameNotJoined.gameName) - 1] = 0;
			break;
		case LANMessage::MSG_SET_ACCEPT:
			msg->Accept.gameName[ARRAY_SIZE(msg->Accept.gameName) - 1] = 0;
			break;
		case LANMessage::MSG_MAP_AVAILABILITY:
			msg->MapStatus.gameName[ARRAY_SIZE(msg->MapStatus.gameName) - 1] = 0;
			break;
		case LANMessage::MSG_CHAT:
			msg->Chat.gameName[ARRAY_SIZE(msg->Chat.gameName) - 1] = 0;
			msg->Chat.message[ARRAY_SIZE(msg->Chat.message) - 1] = 0;
			break;
		case LANMessage::MSG_GAME_OPTIONS:
			msg->GameOptions.options[ARRAY_SIZE(msg->GameOptions.options) - 1] = 0;
			break;
		default:
			break;
	}
}

void LANAPI::pumpGuard()
{
	static UnsignedInt s_lastPump = 0;
	const UnsignedInt now = LanClock::now();
	if (now - s_lastPump < 200) {
		return;
	}
	s_lastPump = now;
	m_transport->update();   // sends what is queued, reads what came
	for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer); ++i)
	{
		if (m_transport->m_inBuffer[i].length <= 0) {
			continue;
		}
		const UnsignedInt senderIP = m_transport->m_inBuffer[i].addr;
		const LANMessage *msg = (const LANMessage *)(m_transport->m_inBuffer[i].data);
		// O33 (review 5): during the match only the match's players are heard here (an observer or anybody else is not)
		// TheSuperHackers @bugfix O87 ... except the anti-cheat's questions. The lobby's own update also runs during a
		// match (LanLobbyMenuUpdate stays on the shell's stack and announces the running game), and the two read the same
		// socket every 200 ms: a question that landed here was dropped. The phase of a lobby's 500 ms question pass against
		// these two is fixed, so a lobby that opened after the start could lose every question (no host mark, no seat
		// marks) while one that knew the match before kept its marks. GuardLink::onLanData bounds what an untracked
		// address may send (questions only, rate-limited).
		Bool fromPlayer = FALSE;
		for (Int k = 0; m_currentGame != nullptr && k < MAX_SLOTS && !fromPlayer; ++k)
		{
			const LANGameSlot *slot = m_currentGame->getLANSlot(k);
			fromPlayer = slot != nullptr && slot->isHuman() && slot->getIP() == senderIP;
		}
		if (senderIP != m_localIP && m_transport->m_inBuffer[i].length >= (Int)sizeof(LANMessage)
			&& msg->messageType == LANMessage::MSG_GUARD_DATA && msg->GuardData.magic == LANGuardDataMagic
			&& msg->GuardData.length > 0 && msg->GuardData.length <= LANGuardDataMax)
		{
			GuardLink::onLanData(senderIP, m_localIP, msg->GuardData.data, msg->GuardData.length);
		}
		if (fromPlayer && senderIP != m_localIP && m_transport->m_inBuffer[i].length >= (Int)sizeof(LANMessage)
			&& msg->messageType == LANMessage::MSG_ROOM && msg->Room.magic == LANRoomMagic)
		{
			LanRoom::onMatchMessage(msg, senderIP);   // O33: the other players' viewer counts (for the HUD)
		}
		m_transport->m_inBuffer[i].length = 0;   // lobby traffic during a match is stale: the lobby asks again later
	}
	// the match's players stay checked; one that fails is named for everyone who checks it
	if (m_currentGame != nullptr)
	{
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			const LANGameSlot *slot = m_currentGame->getLANSlot(k);
			if (slot != nullptr && slot->isHuman() && slot->getIP() != m_localIP) {
				GuardLink::notePeer(slot->getIP(), m_localIP);
			}
		}
	}
	GuardLink::logNamedChanges();   // O87 (test exe)
	LanRoom::matchUpdate(m_localIP);   // O33: this player's viewer count to the other players
	// O32: this player's own new finding, named, during the match too
	{
		UnicodeString own;
		if (GuardLink::takeOwnFailureNotice(&own) && TheInGameUI != nullptr) {
			TheInGameUI->messageNoFormat(own);
		}
	}
	UnsignedInt failedIP = 0;
	while (GuardLink::takeNewFailure(&failedIP))
	{
		UnicodeString name;
		for (Int k = 0; m_currentGame != nullptr && k < MAX_SLOTS; ++k)
		{
			const LANGameSlot *slot = m_currentGame->getLANSlot(k);
			if (slot != nullptr && slot->isHuman() && slot->getIP() == failedIP) {
				name = slot->getName();
			}
		}
		if (!name.isEmpty() && TheInGameUI != nullptr) {
			TheInGameUI->messageNoFormat(GuardLink::failedInMatchText(name, failedIP));
		}
	}
}

// TheSuperHackers @feature O31 (row 21, step 3) LanRoom's messages
void LANAPI::sendRoomMessage(LANMessage *msg, UnsignedInt ip)
{
	if (msg == nullptr) {
		return;
	}
	fillInLANMessage(msg);
	msg->messageType = LANMessage::MSG_ROOM;
	sendMessageOnly(msg, ip);
}
