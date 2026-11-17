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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LANAPIHandlers.cpp
// Author: Matthew D. Campbell, October 2001
// Description: LAN callback handlers
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "Common/GlobalData.h"
#include "Common/QuotedPrintable.h"
#include "Common/UserPreferences.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "Common/GuardLink.h"
#include "GameNetwork/LanRoom.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MapUtil.h"

void LANAPI::handleRequestLocations( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANMessage reply;
		fillInLANMessage( &reply );
		reply.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;

		sendMessage(&reply);
		m_lastResendTime = timeGetTime();
	}
	else
	{
		// In game - are we a game host?
		if (m_currentGame)
		{
			if (m_currentGame->getHostIP() == m_localIP)
			{
				AsciiString gameOpts = GenerateGameOptionsString();
				if (!gameOpts.isEmpty())
				{
					LANMessage reply;
					fillInLANMessage( &reply );
					reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
					strlcpy(reply.GameInfo.options, gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
					wcslcpy(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName));
					reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
					reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

					sendMessage(&reply);
				}
			}
			else
			{
				// We're a joiner
			}
		}
	}
	// Add the player to the lobby player list
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	player->setName(UnicodeString(msg->name));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);

	OnNameChange(player->getIP(), player->getName());
}

Bool LANAPI::announceFromItsHost( LANMessage *msg, UnsignedInt senderIP )
{
	// TheSuperHackers @bugfix O62 (H5 #2) The host of a room is the address its announce comes from, never what the
	// sender writes: a room that is not running is announced only from the address its name starts with
	// (RequestGameCreate writes the creator's own); a running game is announced by one of its seats (its packet router),
	// which must be a seat of the row this lobby has already and of the slot list it sends. Anything else is dropped,
	// so nobody can list a room in another computer's name.
	const UnicodeString name(msg->GameInfo.gameName);
	if (!msg->GameInfo.inProgress)
	{
		if (LANGameInfo::hostIPOfName(name) == senderIP) {
			return TRUE;
		}
		LanTest::joinLog("lobby: announce of a room from %d.%d.%d.%d dropped: its name claims %08X",
			PRINTF_IP_AS_4_INTS(senderIP), LANGameInfo::hostIPOfName(name));
		return FALSE;
	}
	LANGameInfo sent;
	sent.setName(name);
	Bool seated = FALSE;
	if (ParseGameOptionsString(&sent, AsciiString(msg->GameInfo.options)))
	{
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			seated = seated || (sent.getConstLANSlot(i)->isHuman() && sent.getConstLANSlot(i)->getIP() == senderIP);
		}
	}
	LANGameInfo *known = LookupGame(name);
	if (seated && known != nullptr)
	{
		Bool knownSeat = FALSE;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			knownSeat = knownSeat || (known->getConstLANSlot(i)->isHuman() && known->getConstLANSlot(i)->getIP() == senderIP);
		}
		seated = knownSeat;
	}
	if (!seated) {
		LanTest::joinLog("lobby: announce of a running game from %d.%d.%d.%d dropped: not a seat of it", PRINTF_IP_AS_4_INTS(senderIP));
		return FALSE;
	}
	// TheSuperHackers @bugfix O64 (H6 #2) ... and the address its name claims is a seat of it too, unless its admin watches
	// from outside the seats (HO=1: he has none). A row made from a running game's announce is not host-verified
	// (handleGameAnnounce): it is listed and can be watched (its stream comes from its seats), never joined or observed,
	// and nothing host-only of its claimed host is taken for it.
	const UnsignedInt claimed = LANGameInfo::hostIPOfName(name);
	Bool claimedSeat = FALSE;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		claimedSeat = claimedSeat || (sent.getConstLANSlot(i)->isHuman() && sent.getConstLANSlot(i)->getIP() == claimed);
	}
	if (!claimedSeat && !sent.getHostObserves())
	{
		LanTest::joinLog("lobby: announce of a running game from %d.%d.%d.%d dropped: its name's address %08X is no seat",
			PRINTF_IP_AS_4_INTS(senderIP), claimed);
		return FALSE;
	}
	return TRUE;
}

void LANAPI::handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	if (senderIP == m_localIP)
	{
		return; // Don't try to update own info
	}
	if (!announceFromItsHost(msg, senderIP))
	{
		return; // O62 (H5 #2)
	}
	else if (m_currentGame && m_currentGame->isGameInProgress())
	{
		return; // Don't care about games if we're playing
	}
	else if (senderIP == m_directConnectRemoteIP)
	{

		if (m_currentGame == nullptr)
		{
			LANGameInfo *game = LookupGame(UnicodeString(msg->GameInfo.gameName));
			if (!game)
			{
				game = NEW LANGameInfo;
				game->setName(UnicodeString(msg->GameInfo.gameName));
				addGame(game);
			}
			Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
			game->setGameInProgress(msg->GameInfo.inProgress);
			game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
		if (!msg->GameInfo.inProgress) {
			game->setHostDisplayName(UnicodeString(msg->name));   // O62: a room's announce comes from its host (checked)
			game->setHostVerified(TRUE);   // O64: announced by its host himself
		}
			game->setLastHeard(timeGetTime());
			if (!success)
			{
				// remove from list
				removeGame(game);
				delete game;
				return;
			}
			RequestGameJoin(game, m_directConnectRemoteIP);
		}
	}
	else
	{
		LANGameInfo *game = LookupGame(UnicodeString(msg->GameInfo.gameName));
		if (!game)
		{
			game = NEW LANGameInfo;
			game->setName(UnicodeString(msg->GameInfo.gameName));
			addGame(game);
		}
		Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
		game->setGameInProgress(msg->GameInfo.inProgress);
		game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
		if (!msg->GameInfo.inProgress) {
			game->setHostDisplayName(UnicodeString(msg->name));   // O62: a room's announce comes from its host (checked)
			game->setHostVerified(TRUE);   // O64: announced by its host himself
		}
		game->setLastHeard(timeGetTime());
		if (!success)
		{
			// remove from list
			removeGame(game);
			delete game;
			game = nullptr;
		}

		OnGameList( m_games );
	//	if (game == m_currentGame && !m_inLobby)
	//		OnSlotList(RET_OK, game);
	}
}

void LANAPI::handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	player->setName(UnicodeString(msg->name));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP )
{
	// In game - are we a game host?
	if (m_currentGame)
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

			sendMessage(&reply, senderIP);
		}
	}
}

static Bool IsInvalidCharForPlayerName(const WideChar c)
{
	return c < L' ' || // C0 control chars
		c == L',' || c == L':' || c == L';' || // chars used for strtok in ParseAsciiStringToGameInfo
		(c >= L'\x007f' && c <= L'\x009f') || // DEL + C1 control chars
		c == L'\x2028' || c == L'\x2029' || // line and paragraph separators
		(c >= L'\xdc00' && c <= L'\xdfff') || // low surrogate, for chars beyond the Unicode Basic Multilingual Plane
		(c >= L'\xd800' && c <= L'\xdbff'); // high surrogate, for chars beyond the BMP
}

static Bool IsSpaceCharacter(const WideChar c)
{
	return c == L' ' || // space
		c == L'\xA0' || // no-break space
		c == L'\x1680' || // ogham space mark
		(c >= L'\x2000' && c <= L'\x200A') || // en/em spaces, figure, punctuation, thin, hair
		c == L'\x202F' || // narrow no-break space
		c == L'\x205F' || // medium mathematical space
		c == L'\x3000'; // ideographic space
}

static Bool ContainsInvalidChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (IsInvalidCharForPlayerName(*playerName++))
			return true;
	}

	return false;
}

static Bool ContainsAnyReadableChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (!IsSpaceCharacter(*playerName++))
			return true;
	}

	return false;
}

void LANAPI::handleRequestJoin( LANMessage *msg, UnsignedInt senderIP )
{
	UnsignedInt responseIP = senderIP;	// need this cause the player may or may not be
																			// in the player list at the sendMessage.

	if (msg->GameToJoin.gameIP != m_localIP)
	{
		return; // Not us.  Ignore it.
	}
	// TheSuperHackers @feature O62 Another version's join (no protocol number, or another one) is refused with the
	// version line (an older build shows its own "version" text for RET_CRC_MISMATCH), never seated
	msg->GameToJoin.build[ARRAY_SIZE(msg->GameToJoin.build) - 1] = 0;
	if (msg->GameToJoin.protocolMagic != LANProtocolMagic || msg->GameToJoin.protocol != LANProtocolVersion
		|| LanBuildIdentity().compare(msg->GameToJoin.build) != 0)   // O63: another build of the same number too
	{
		LanTest::joinLog("host: join request from %d.%d.%d.%d of another protocol: refused", PRINTF_IP_AS_4_INTS(senderIP));
		LANMessage deny;
		fillInLANMessage(&deny);
		deny.messageType = LANMessage::MSG_JOIN_DENY;
		deny.GameNotJoined.gameName[0] = 0;
		if (m_currentGame != nullptr) {
			wcslcpy(deny.GameNotJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(deny.GameNotJoined.gameName));
		}
		deny.GameNotJoined.reason = LANAPIInterface::RET_CRC_MISMATCH;
		deny.GameNotJoined.gameIP = m_localIP;
		deny.GameNotJoined.playerIP = senderIP;
		sendMessage(&deny, senderIP);
		return;
	}
	// TheSuperHackers @bugfix O62 (H5 #2) ... and the room it asks for is this host's room (a joiner of a row that names
	// this computer falsely is not let into the real room)
	if (m_currentGame != nullptr && m_currentGame->getName().compare(msg->GameToJoin.gameName) != 0)
	{
		LanTest::joinLog("host: join request from %d.%d.%d.%d for another room: ignored", PRINTF_IP_AS_4_INTS(senderIP));
		return;
	}
	LANMessage reply;
	fillInLANMessage( &reply );
	if (!m_inLobby && m_currentGame && m_currentGame->getHostIP() == m_localIP)
	{
		if (m_currentGame->isGameInProgress())
		{
			reply.messageType = LANMessage::MSG_JOIN_DENY;
			reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_STARTED;
			reply.GameNotJoined.gameIP = m_localIP;
			reply.GameNotJoined.playerIP = senderIP;
			DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game already started."));
		}
		else
		{
			int player;
			Bool canJoin = true;

			// TheSuperHackers @bugfix O45 The same player asking again (its accept was lost on the way: the joiner repeats
			// its request every second): the same answer with the slot it has, never a second slot ("duplicate name"
			// before)
			// O62 (H5 #4): from slot 0 (a player sits there while the admin watches from outside the seats)
			for (player = 0; player < MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				if (slot->isHuman() && slot->getIP() == senderIP && slot->getName().compare(msg->name) == 0)
				{
					reply.messageType = LANMessage::MSG_JOIN_ACCEPT;
					wcslcpy(reply.GameJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameJoined.gameName));
					reply.GameJoined.slotPosition = player;
					reply.GameJoined.gameIP = m_localIP;
					reply.GameJoined.playerIP = senderIP;
					reply.GameJoined.protocolMagic = LANProtocolMagic;   // O62
					reply.GameJoined.protocol = LANProtocolVersion;
					strlcpy(reply.GameJoined.build, LanBuildIdentity().str(), ARRAY_SIZE(reply.GameJoined.build));   // O63
					LanTest::joinLog("host: join request again from %d.%d.%d.%d: accepted again, slot %d", PRINTF_IP_AS_4_INTS(senderIP), player);
					sendMessage(&reply, 0);   // as the first accept
					RequestGameOptions(GenerateGameOptionsString(), true);
					return;
				}
			}

			// see if the CRCs match
#if defined(RTS_DEBUG)
			if (TheGlobalData->m_netMinPlayers > 0) {
#endif
// TheSuperHackers @todo Enable CRC checks!
#if !RTS_ZEROHOUR
			if (msg->GameToJoin.iniCRC != TheGlobalData->m_iniCRC ||
					msg->GameToJoin.exeCRC != TheGlobalData->m_exeCRC)
			{
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of CRC mismatch. CRCs are them/us INI:%X/%X exe:%X/%X",
					msg->GameToJoin.iniCRC, TheGlobalData->m_iniCRC,
					msg->GameToJoin.exeCRC, TheGlobalData->m_exeCRC));
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				reply.GameNotJoined.reason = LANAPIInterface::RET_CRC_MISMATCH;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				canJoin = false;
			}
#endif
#if defined(RTS_DEBUG)
			}
#endif

			// TheSuperHackers @feature O30 (row 15) The host's anti-cheat rule (on by default): only a player whose
			// anti-cheat this host's anti-cheat has verified gets in; the player is checked at once for the next try.
			if (canJoin && GuardLink::hostRequiresGuard() && GuardLink::verdictOf(senderIP) != GuardLink::VERDICT_VERIFIED)
			{
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied: no verified anti-cheat"));
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				reply.GameNotJoined.reason = LANAPIInterface::RET_GUARD_REQUIRED;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				canJoin = false;
			}

// TheSuperHackers @tweak Disables the duplicate serial check
#if 0
			// check for a duplicate serial
			AsciiString s;
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				s.clear();
				if (player == 0)
				{
					GetStringFromRegistry("\\ergc", "", s);
				}
				else if (slot->isHuman())
				{
					s = slot->getSerial();
					if (s.isEmpty())
						s = "<Munkee>";
				}

				if (s.isNotEmpty())
				{
					DEBUG_LOG(("Checking serial '%s' in slot %d", s.str(), player));

					if (!strncmp(s.str(), msg->GameToJoin.serial, g_maxSerialLength))
					{
						// serials match!  kick the punk!
						reply.messageType = LANMessage::MSG_JOIN_DENY;
						reply.GameNotJoined.reason = LANAPIInterface::RET_SERIAL_DUPE;
						reply.GameNotJoined.gameIP = m_localIP;
						reply.GameNotJoined.playerIP = senderIP;
						canJoin = false;

						DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate serial # (%s).", s.str()));
						break;
					}
				}
			}
#endif

			// TheSuperHackers @bugfix slurmlord 18/09/2025 need to validate the name of the connecting player before
			// allowing them to join to prevent messing up the format of game state string. Commas, colons, semicolons etc.
			// should not be in a player name. It should also not consist of only space characters.
			if (canJoin)
			{
				if (ContainsInvalidChars(msg->name) || !ContainsAnyReadableChars(msg->name))
				{
					// Just deny with a duplicate name reason, for backwards compatibility with retail
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of illegal characters in the player name."));
				}
			}	

			// Then see if the player has a duplicate name
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				if (slot->isHuman() && slot->getName().compare(msg->name) == 0)
				{
					// just deny duplicates
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate names."));
					break;
				}
			}

			// TheSuperHackers @bugfix Stubbjax 26/09/2025 Players can now join open slots regardless of starting spots on the map.
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				if (m_currentGame->getLANSlot(player)->isOpen())
				{
					// OK, add him in.
					reply.messageType = LANMessage::MSG_JOIN_ACCEPT;
					wcslcpy(reply.GameJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameJoined.gameName));
					reply.GameJoined.slotPosition = player;
					reply.GameJoined.gameIP = m_localIP;
					reply.GameJoined.playerIP = senderIP;
					reply.GameJoined.protocolMagic = LANProtocolMagic;   // O62
					reply.GameJoined.protocol = LANProtocolVersion;
					strlcpy(reply.GameJoined.build, LanBuildIdentity().str(), ARRAY_SIZE(reply.GameJoined.build));   // O63

					LANGameSlot newSlot;
					newSlot.setState(SLOT_PLAYER, UnicodeString(msg->name));
					newSlot.setIP(senderIP);
					newSlot.setPort(NETWORK_BASE_PORT_NUMBER);
					newSlot.setLastHeard(timeGetTime());
					newSlot.setSerial(msg->GameToJoin.serial);
					m_currentGame->setSlot(player,newSlot);
					DEBUG_LOG(("LANAPI::handleRequestJoin - added player %ls at ip 0x%08x to the game", msg->name, senderIP));

					OnPlayerJoin(player, UnicodeString(msg->name));
					LanRoom::hostDropJoined(senderIP);   // O31: an observer who took a slot leaves the list
					LanTest::joinLog("host: join request from %d.%d.%d.%d: accepted, slot %d", PRINTF_IP_AS_4_INTS(senderIP), player);
					responseIP = 0;

					break;
				}
			}

			if (canJoin && player == MAX_SLOTS)
			{
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				wcslcpy(reply.GameNotJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameNotJoined.gameName));
				reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_FULL;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game is full."));
			}
		}
	}
	else
	{
		reply.messageType = LANMessage::MSG_JOIN_DENY;
		reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_GONE;
		reply.GameNotJoined.gameIP = m_localIP;
		reply.GameNotJoined.playerIP = senderIP;
	}
	sendMessage(&reply, responseIP);
	RequestGameOptions(GenerateGameOptionsString(), true);
}

void LANAPI::handleJoinAccept( LANMessage *msg, UnsignedInt senderIP )
{
	if (msg->GameJoined.playerIP == m_localIP) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// O62 (H5 #8): only the host of the room asked for accepts (its address, as its announce came)
			LANGameInfo *joined = LookupGame(UnicodeString(msg->GameJoined.gameName));
			if (joined != nullptr && joined->getHostIP() != senderIP)
			{
				LanTest::joinLog("joiner: an accept from %d.%d.%d.%d, not the room's host: ignored", PRINTF_IP_AS_4_INTS(senderIP));
				return;
			}
			// O62: an accept of another protocol (an older host) is no join
			msg->GameJoined.build[ARRAY_SIZE(msg->GameJoined.build) - 1] = 0;
			if (msg->GameJoined.protocolMagic != LANProtocolMagic || msg->GameJoined.protocol != LANProtocolVersion
				|| LanBuildIdentity().compare(msg->GameJoined.build) != 0)   // O63
			{
				LanTest::joinLog("joiner: an accept of another protocol: not joined");
				m_pendingAction = ACT_NONE;
				OnGameJoin(RET_CRC_MISMATCH, nullptr);
				return;
			}
			m_currentGame = joined;
			LanTest::joinLog("joiner: accepted, slot %d", (Int)msg->GameJoined.slotPosition);

			if (!m_currentGame)
			{
				DEBUG_CRASH(("Could not find game to join!"));
				OnGameJoin(RET_UNKNOWN, nullptr);
			}
			else
			{
				m_inLobby = false;
				AsciiString options = GameInfoToAsciiString(m_currentGame);
				m_currentGame->enterGame();
				ParseAsciiStringToGameInfo(m_currentGame, options);

				Int pos = msg->GameJoined.slotPosition;

				LANGameSlot slot;
				slot.setState(SLOT_PLAYER, m_name);
				slot.setIP(m_localIP);
				slot.setPort(NETWORK_BASE_PORT_NUMBER);
				slot.setLastHeard(0);
				slot.setLogin(m_userName);
				slot.setHost(m_hostName);
				m_currentGame->setSlot(pos, slot);

				// O60: the admin's login and machine, on his seat (none while he watches from outside the seats)
				for (Int s = 0; s < MAX_SLOTS; ++s)
				{
					if (m_currentGame->getLANSlot(s)->isHuman() && m_currentGame->getIP(s) == m_currentGame->getHostIP())
					{
						m_currentGame->getLANSlot(s)->setHost(msg->hostName);
						m_currentGame->getLANSlot(s)->setLogin(msg->userName);
					}
				}

				LANPreferences prefs;
				AsciiString entry;
				entry.format("%d.%d.%d.%d:%s", PRINTF_IP_AS_4_INTS(senderIP), UnicodeStringToQuotedPrintable(m_currentGame->getHostName()).str());
				prefs["RemoteIP0"] = entry;
				prefs.write();

				OnGameJoin(RET_OK, m_currentGame);
				//DEBUG_CRASH(("setting host to %ls@%ls", m_currentGame->getLANSlot(0)->getUser()->getLogin().str(),
				//	m_currentGame->getLANSlot(0)->getUser()->getHost().str()));
			}
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

void LANAPI::handleJoinDeny( LANMessage *msg, UnsignedInt senderIP )
{
	if (msg->GameJoined.playerIP == m_localIP) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			LanTest::joinLog("joiner: denied, reason %d", (Int)msg->GameNotJoined.reason);
			OnGameJoin(msg->GameNotJoined.reason, LookupGame(UnicodeString(msg->GameNotJoined.gameName)));
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

void LANAPI::handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		// TheSuperHackers @bugfix O62 The host leaving is the host's address, not slot 0: while the admin watches from
		// outside the seats the player in slot 0 leaving is a player leaving, and the admin (no seat) leaving closes the
		// room (he is matched at the last slot when he has none)
		const Bool hostLeaves = senderIP == m_currentGame->getHostIP();
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP || (hostLeaves && player == MAX_SLOTS - 1))
			{
				if (hostLeaves)
				{
					OnHostLeave();
					removeGame(m_currentGame);
					delete m_currentGame;
					m_currentGame = nullptr;

					/// @todo re-add myself to lobby?  Or just keep me there all the time?  If we send a LOBBY_ANNOUNCE things'll work out...
					LANPlayer *lanPlayer = LookupPlayer(m_localIP);
					if (!lanPlayer)
					{
						lanPlayer = NEW LANPlayer;
						lanPlayer->setIP(m_localIP);
					}
					else
					{
						removePlayer(lanPlayer);
					}
					lanPlayer->setName(UnicodeString(m_name));
					lanPlayer->setHost(m_hostName);
					lanPlayer->setLogin(m_userName);
					lanPlayer->setLastHeard(timeGetTime());
					addPlayer(lanPlayer);

				}
				else
				{
					if (AmIHost())
					{
						// remove the deadbeat
						LANGameSlot slot;
						slot.setState(SLOT_OPEN);
						m_currentGame->setSlot( player, slot );
					}
					OnPlayerLeave(UnicodeString(msg->name));
					m_currentGame->getLANSlot(player)->setState(SLOT_OPEN);
					m_currentGame->resetAccepted();
					RequestGameOptions(GenerateGameOptionsString(), false, senderIP);
					//m_currentGame->endGame();
				}
				break;
			}
			DEBUG_ASSERTCRASH(player < MAX_SLOTS, ("Didn't find player!"));
		}
	}
	else if (m_inLobby)
	{
		// Look for dissappearing games
		LANGameInfo *game = m_games;
		while (game)
		{
			// O62 (H5 #2): only its host takes a room off the lobby's list (a player leaving it does not)
			if (game->getName().compare(msg->GameToLeave.gameName) == 0 && LANGameInfo::hostIPOfName(game->getName()) == senderIP)
			{
				removeGame(game);
				delete game;
				OnGameList(m_games);
				break;
			}
			game = game->getNext();
		}
	}
}

void LANAPI::handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			if (player->getIP() == senderIP)
			{
				removePlayer(player);
				delete player;
				OnPlayerList(m_lobbyPlayers);
				break;
			}
			player = player->getNext();
		}
	}
}

void LANAPI::handleSetAccept( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnAccept(senderIP, msg->Accept.isAccepted);
				break;
			}
		}
	}
}

void LANAPI::handleHasMap( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame)
	{
		CRC mapNameCRC;
//	mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
		AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
		mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
		if (mapNameCRC.get() != msg->MapStatus.mapCRC)
		{
			return;
		}

		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnHasMap(senderIP, msg->MapStatus.hasMap);
				break;
			}
		}
	}
}

void LANAPI::handleChat( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player;
		if((player=LookupPlayer(senderIP)) != nullptr)
		{
			OnChat(UnicodeString(player->getName()), player->getIP(), UnicodeString(msg->Chat.message), msg->Chat.chatType);
			player->setLastHeard(timeGetTime());
		}
	}
	else
	{
		if (LookupGame(UnicodeString(msg->Chat.gameName)) != m_currentGame)
		{
			DEBUG_LOG(("Game '%ls' is not my game", msg->Chat.gameName));
			if (m_currentGame)
			{
				DEBUG_LOG(("Current game is '%ls'", m_currentGame->getName().str()));
			}
			return;
		}

		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame && m_currentGame->getIP(player) == senderIP)
			{
				OnChat(UnicodeString(msg->name), m_currentGame->getIP(player), UnicodeString(msg->Chat.message), msg->Chat.chatType);
				break;
			}
		}
	}
}

void LANAPI::handleGameStart( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getHostIP() == senderIP && !m_currentGame->isGameInProgress())
	{
		// TheSuperHackers @bugfix O62 (H5 #1) START names the room and the digest every player confirmed: a copy of the
		// room that differs does not start (another slot order would desynchronise the match)
		if (msg->StartCheck.magic != LANStartMagic || m_currentGame->getName().compare(msg->StartCheck.gameName) != 0) {
			return;
		}
		const UnsignedInt mine = m_currentGame->startDigest();
		if (mine != msg->StartCheck.digest)
		{
			LanTest::joinLog("player: START with digest %08X, mine %08X: not started", msg->StartCheck.digest, mine);
			OnChat(L"SYSTEM", m_localIP, TheGameText->FETCH_OR_SUBSTITUTE("GUI:LanStartOtherList", localizedFallback(
				L"The host started with another slot list than this computer has: this computer did not start.",
				L"\x0425\x043E\x0441\x0442 \x043D\x0430\x0447\x0430\x043B \x0441 \x0434\x0440\x0443\x0433\x0438\x043C \x0441\x043F\x0438\x0441\x043A\x043E\x043C \x043C\x0435\x0441\x0442, \x0447\x0435\x043C \x0443 \x044D\x0442\x043E\x0433\x043E \x043A\x043E\x043C\x043F\x044C\x044E\x0442\x0435\x0440\x0430: \x044D\x0442\x043E\x0442 \x043A\x043E\x043C\x043F\x044C\x044E\x0442\x0435\x0440 \x043D\x0435 \x043D\x0430\x0447\x0430\x043B \x0438\x0433\x0440\x0443.")), LANCHAT_SYSTEM);
			return;
		}
		LanTest::joinLog("player: START with digest %08X: started", mine);
		OnGameStart();
	}
}

void LANAPI::handleStartCheck( LANMessage *msg, UnsignedInt senderIP )
{
	// O62 (H5 #1): the host asks whether this computer's copy of the room is the one it starts; READY only when it is
	if (m_inLobby || m_currentGame == nullptr || m_currentGame->isGameInProgress() || m_currentGame->getHostIP() != senderIP
		|| msg->StartCheck.magic != LANStartMagic || m_currentGame->getName().compare(msg->StartCheck.gameName) != 0) {
		return;
	}
	const UnsignedInt mine = m_currentGame->startDigest();
	if (mine != msg->StartCheck.digest)
	{
		LanTest::joinLog("player: start check digest %08X, mine %08X: no READY yet", msg->StartCheck.digest, mine);
		return;   // the options the host sends with every check bring this copy up to date
	}
	LANMessage reply;
	fillInLANMessage(&reply);
	reply.messageType = LANMessage::MSG_START_READY;
	reply.StartCheck.magic = LANStartMagic;
	reply.StartCheck.digest = mine;
	wcslcpy(reply.StartCheck.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.StartCheck.gameName));
	sendMessage(&reply, senderIP);
}

void LANAPI::handleStartReady( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_startChecking || m_currentGame == nullptr || !m_currentGame->amIHost() || msg->StartCheck.magic != LANStartMagic
		|| msg->StartCheck.digest != m_startDigest || m_currentGame->getName().compare(msg->StartCheck.gameName) != 0) {
		return;
	}
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const LANGameSlot *slot = m_currentGame->getConstLANSlot(i);
		if (slot != nullptr && slot->isHuman() && slot->getIP() == senderIP && !m_startReady[i])
		{
			m_startReady[i] = TRUE;
			LanTest::joinLog("host: READY from slot %d (%d.%d.%d.%d)", i, PRINTF_IP_AS_4_INTS(senderIP));
		}
	}
}

void LANAPI::handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getHostIP() == senderIP && !m_currentGame->isGameInProgress())
	{
		OnGameStartTimer(msg->StartTimer.seconds);
	}
}

void LANAPI::handleGameOptions( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnGameOptions(senderIP, player, AsciiString(msg->GameOptions.options));
				break;
			}
		}
		// O62: the host's name, from his own options (he may have no seat)
		if (senderIP != 0 && senderIP == m_currentGame->getHostIP()) {
			m_currentGame->setHostDisplayName(UnicodeString(msg->name));
		}
		// O60 (row 46): the admin watching from outside the seats has no slot
		if (player == MAX_SLOTS && senderIP != 0 && senderIP == m_currentGame->getHostIP()) {
			OnGameOptions(senderIP, -1, AsciiString(msg->GameOptions.options));
		}
	}
}

void LANAPI::handleInActive(LANMessage *msg, UnsignedInt senderIP) {
	if (m_inLobby || (m_currentGame == nullptr) || (m_currentGame->isGameInProgress())) {
		return;
	}

	// check to see if we are the host of this game.
	if (m_currentGame->amIHost() == FALSE) {
		return;
	}

	UnicodeString playerName;
	playerName = msg->name;

	Int slotNum = m_currentGame->getSlotNum(playerName);
	if (slotNum < 0)
		return;
	GameSlot *slot = m_currentGame->getSlot(slotNum);
	if (slot == nullptr) {
		return;
	}

	if (senderIP != slot->getIP()) {
		return;
	}

	// don't want to unaccept the host, that's silly.  They can't hit start alt-tabbed anyways.
	if (senderIP == GetLocalIP()) {
		return;
	}

	// only unaccept if the timer hasn't started yet.
	if (m_gameStartTime != 0) {
		return;
	}

	slot->unAccept();
	AsciiString options = GenerateGameOptionsString();
	RequestGameOptions(options, FALSE);
	lanUpdateSlotList();
}
