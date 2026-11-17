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
// FILE: LANAPICallbacks.cpp
// Author: Chris Huybregts, October 2001
// Description: LAN API Callbacks
///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "WWLib/strtok_r.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
#include "Common/RandomValue.h"
#include "Common/UserPreferences.h"
#include "GameClient/GameText.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "Common/GuardLink.h"
#include "GameNetwork/LanRoom.h"
#include "GameClient/LocalizedFallback.h"   // O71
#include "GameNetwork/networkutil.h"
#include "Common/LanClock.h"   // O75

LANAPI *TheLAN = nullptr;
extern Bool LANbuttonPushed;


//Colors used for the chat dialogs
const Color playerColor =  GameMakeColor(255,255,255,255);
const Color gameColor =  GameMakeColor(255,255,255,255);
const Color gameInProgressColor =  GameMakeColor(128,128,128,255);
const Color chatNormalColor =  GameMakeColor(50,215,230,255);
const Color chatActionColor =  GameMakeColor(255,0,255,255);
const Color chatLocalNormalColor =  GameMakeColor(255,128,0,255);
const Color chatLocalActionColor =  GameMakeColor(128,255,255,255);
const Color chatSystemColor =  GameMakeColor(255,255,255,255);
const Color acceptTrueColor =  GameMakeColor(0,255,0,255);
const Color acceptFalseColor =  GameMakeColor(255,0,0,255);


UnicodeString LANAPIInterface::getErrorStringFromReturnType( ReturnType ret )
{
	switch (ret)
	{
		case RET_OK:
			return TheGameText->fetch("LAN:OK");
		case RET_TIMEOUT:
			return TheGameText->fetch("LAN:ErrorTimeout");
		case RET_GAME_FULL:
			return TheGameText->fetch("LAN:ErrorGameFull");
		case RET_DUPLICATE_NAME:
			return TheGameText->fetch("LAN:ErrorDuplicateName");
		case RET_CRC_MISMATCH:
			return TheGameText->fetch("LAN:ErrorCRCMismatch");
		case RET_GAME_STARTED:
			return TheGameText->fetch("LAN:ErrorGameStarted");
		case RET_GAME_EXISTS:
			return TheGameText->fetch("LAN:ErrorGameExists");
		case RET_GAME_GONE:
			return TheGameText->fetch("LAN:ErrorGameGone");
		case RET_BUSY:
			return TheGameText->fetch("LAN:ErrorBusy");
		case RET_SERIAL_DUPE:
			return TheGameText->fetch("WOL:ChatErrorSerialDup");
		case RET_GUARD_REQUIRED:   // O30
			return GuardLink::joinRefusedText();
		default:
			return TheGameText->fetch("LAN:ErrorUnknown");
	}
}

// TheSuperHackers @feature arcticdolphin 08/08/2026 Adds /me emotes to LAN player chat, consistent with WOL
void LANAPI::RequestPlayerChat( UnicodeString message )
{
	if (message.startsWithNoCase(L"/me "))
	{
		RequestChat(UnicodeString(message.str() + 4), LANCHAT_EMOTE);
		return;
	}

	RequestChat(message, LANCHAT_NORMAL);
}

// On functions are (generally) the result of network traffic

void LANAPI::OnAccept( UnsignedInt playerIP, Bool status )
{
	if( AmIHost() )
	{
		Int i = 0;
		for (; i < MAX_SLOTS; i++)
		{
			if (m_currentGame->getIP(i) == playerIP)
			{
				if(status)
					m_currentGame->getLANSlot(i)->setAccept();
				else
					m_currentGame->getLANSlot(i)->unAccept();
				break;
			}
		}
		if (i != MAX_SLOTS )
		{
			RequestGameOptions( GenerateGameOptionsString(), false );
			lanUpdateSlotList();
		}
	}
	else
	{
		//i'm not the host but if the accept came from the host...
		if( m_currentGame->getHostIP() == playerIP )
		{
			UnicodeString text;
			text = TheGameText->fetch("GUI:HostWantsToStart");
			OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
		}
	}
}

void LANAPI::OnHasMap( UnsignedInt playerIP, Bool status )
{
	if( AmIHost() )
	{
		Int i = 0;
		for (; i < MAX_SLOTS; i++)
		{
			if (m_currentGame->getIP(i) == playerIP)
			{
				m_currentGame->getLANSlot(i)->setMapAvailability( status );
				break;
			}
		}
		if (i != MAX_SLOTS )
		{
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
				mapDisplayName.format(L"%hs", m_currentGame->getMap().str());
				willTransfer = WouldMapTransfer(m_currentGame->getMap());
			}
			if (!status)
			{
				UnicodeString text;
				if (willTransfer)
					text.format(TheGameText->fetch("GUI:PlayerNoMapWillTransfer"), m_currentGame->getLANSlot(i)->getName().str(), mapDisplayName.str());
				else
					text.format(TheGameText->fetch("GUI:PlayerNoMap"), m_currentGame->getLANSlot(i)->getName().str(), mapDisplayName.str());
				OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
			}
			lanUpdateSlotList();
		}
	}
}

void LANAPI::OnGameStartTimer( Int seconds )
{
	UnicodeString text;
	if (seconds == 1)
		text.format(TheGameText->fetch("LAN:GameStartTimerSingular"), seconds);
	else
		text.format(TheGameText->fetch("LAN:GameStartTimerPlural"), seconds);
	OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
}

void LANAPI::OnGameStart()
{
	//DEBUG_LOG(("Map is '%s', preview is '%s'", m_currentGame->getMap().str(), GetPreviewFromMap(m_currentGame->getMap()).str()));
	//DEBUG_LOG(("Map is '%s', INI is '%s'", m_currentGame->getMap().str(), GetINIFromMap(m_currentGame->getMap()).str()));

	if (m_currentGame)
	{
		LANPreferences pref;
		AsciiString option;
		// O60: the admin outside the seats keeps his saved army and colour
		if (const LANGameSlot *mine = m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() ))
		{
			option.format("%d", mine->getPlayerTemplate());
			pref["PlayerTemplate"] = option;
			option.format("%d", mine->getColor());
			pref["Color"] = option;
		}
		if (m_currentGame->amIHost())
    {
    	pref["Map"] = AsciiStringToQuotedPrintable(m_currentGame->getMap());
      pref.setSuperweaponRestricted( m_currentGame->getSuperweaponRestriction() > 0 );
      pref.setStartingCash( m_currentGame->getStartingCash() );
    }
		pref.write();

		// TheSuperHackers @feature O60 (row 46) The admin who watches from outside the seats is not in the match (no
		// network, no lockstep, no map transfer): LanRoom::hostStart began his stream. He leaves his room on the next
		// update without a word to the players (their match does not wait for him and goes on if his computer goes
		// away) and watches from the lobby.
		if (m_currentGame->amIHost() && m_currentGame->getHostObserves() && m_currentGame->getLocalSlotNum() < 0)
		{
			m_watchMatchLeave = TRUE;
			return;
		}

		m_isInLANMenu = FALSE;

		//m_currentGame->startGame(0);

		// O31: every player serves its replay on its LAN address (observers and outside viewers follow it)
		LanRoom::configureServing(m_localIP);

		// Set up the game network
		DEBUG_ASSERTCRASH(TheNetwork == nullptr, ("For some reason TheNetwork isn't null at the start of this game.  Better look into that."));

		delete TheNetwork;
		TheNetwork = nullptr;

		// Time to initialize TheNetwork for this game.
		TheNetwork = NetworkInterface::createNetwork();
		TheNetwork->init();
		TheNetwork->setLocalAddress(m_localIP, 8088);
		TheNetwork->initTransport();

		TheNetwork->parseUserList(m_currentGame);

		if (TheGameLogic->isInGame())
			TheGameLogic->clearGameData();

		Bool filesOk = DoAnyMapTransfers(m_currentGame);

		// see if we really have the map.  if not, back out.
		TheMapCache->updateCache();
		if (!filesOk || TheMapCache->findMap(m_currentGame->getMap()) == nullptr)
		{
			DEBUG_LOG(("After transfer, we didn't really have the map.  Bailing..."));
			OnPlayerLeave(m_name);
			removeGame(m_currentGame);
			delete m_currentGame;
			m_currentGame = nullptr;
			m_inLobby = TRUE;

			delete TheNetwork;
			TheNetwork = nullptr;

			OnChat(UnicodeString::TheEmptyString, 0, TheGameText->fetch("GUI:CouldNotTransferMap"), LANCHAT_SYSTEM);
			return;
		}

		m_currentGame->startGame(0);

		// shutdown the top, but do not pop it off the stack
		//TheShell->hideShell();
		// setup the Global Data with the Map and Seed
		TheWritableGlobalData->m_pendingFile = m_currentGame->getMap();

		// send a message to the logic for a new game
		GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
		msg->appendIntegerArgument(GAME_LAN);

		// TheSuperHackers @tweak The user's render fps limit applies here too (it used to be switched off for the rest of
		// the run). It is 30 or more, and network games run at most 30 logic frames per second (ConnectionManager), so it
		// cannot slow a game down.

		// Set the seeds
		InitRandom( m_currentGame->getSeed() );
		DEBUG_LOG(("InitRandom( %d )", m_currentGame->getSeed()));
	}
}

void LANAPI::OnWatchMatchLeave()
{
	m_watchMatchLeave = FALSE;
	ResetGameStartTimer();
	if (m_currentGame != nullptr)
	{
		removeGame(m_currentGame);
		delete m_currentGame;
		m_currentGame = nullptr;
	}
	m_inLobby = TRUE;
	LANbuttonPushed = true;
	DEBUG_LOG(("OnWatchMatchLeave: the admin watches his room's match from the lobby"));
	TheShell->pop();
}

void LANAPI::OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options )
{
	if (!m_currentGame)
		return;

	// O60 (row 46): the host by identity (in a seat, or watching from outside them with no slot)
	const Bool fromHost = playerIP != 0 && playerIP == m_currentGame->getHostIP();
	if (!fromHost && (playerSlot < 0 || playerSlot >= MAX_SLOTS || m_currentGame->getIP(playerSlot) != playerIP))
		return; // He's not in our game?!?


	if (m_currentGame->isGameInProgress())
		return; // we don't want to process any game options while in game.

	if (fromHost && !m_currentGame->amIHost())
	{
		m_currentGame->setLastHeard(LanClock::now());
		AsciiString oldOptions = GameInfoToAsciiString(m_currentGame); // save these off for if we get booted
		if(ParseGameOptionsString(m_currentGame,options))
		{
			lanUpdateSlotList();
			updateGameOptions();
		}
		Bool booted = true;
		for(Int player = 0; player< MAX_SLOTS; player++)   // O60: slot 0 is a player's while the admin watches
		{
			if(m_currentGame->getIP(player) == m_localIP)
			{
				booted = false;
				break;
			}
		}
		if (!booted) {
			m_notListedSinceMs = 0;   // O71
		}
		if(booted)
		{
			// restore the options with us in so we can save prefs
			ParseGameOptionsString(m_currentGame, oldOptions);
			// TheSuperHackers @bugfix O71 A list without this computer that comes within 3 s of the first one may be older
			// than its seat (sent before the join, arrived after the accept): the room stays as it was meanwhile. After
			// 3 s of such lists (the host sends one every 2 s, and three at a drop) this computer has been removed.
			const UnsignedInt t = LanClock::now();
			if (m_notListedSinceMs == 0) {
				m_notListedSinceMs = t;
				LanTest::joinLog("player: the host's list does not have this computer: waiting 3 s");
				lanUpdateSlotList();
				return;
			}
			if ((Int)(t - m_notListedSinceMs) < 3000) {
				lanUpdateSlotList();
				return;
			}
			m_notListedSinceMs = 0;
			// TheSuperHackers @bugfix O71 (addendum) Said, not a silent return to the lobby
			LanTest::joinLog("player: the host's list no longer has this computer: back to the lobby");
			LanRoom::postNotice(TheGameText->FETCH_OR_SUBSTITUTE("GUI:LanRemovedFromRoom", localizedFallback(
				L"You are no longer in the room: the host removed this computer (a kick, or it did not hear from it).",
				L"\x0412\x044B \x0431\x043E\x043B\x044C\x0448\x0435 \x043D\x0435 \x0432 \x043A\x043E\x043C\x043D\x0430\x0442\x0435: \x0445\x043E\x0441\x0442 \x0443\x0431\x0440\x0430\x043B \x044D\x0442\x043E\x0442 \x043A\x043E\x043C\x043F\x044C\x044E\x0442\x0435\x0440 (\x0438\x0441\x043A\x043B\x044E\x0447\x0438\x043B \x0438\x043B\x0438 \x043D\x0435 \x043F\x043E\x043B\x0443\x0447\x0430\x043B \x043E\x0442 \x043D\x0435\x0433\x043E \x043E\x0442\x0432\x0435\x0442\x0430).")));
			OnPlayerLeave(m_name);
		}

	}
	else
	{
		if (playerSlot < 0 || playerSlot >= MAX_SLOTS) {
			return;   // O62 (H5 #9): a host without a seat sends only as the host (the branch above)
		}
		// Check for user/host updates
		{
			AsciiString key;
			AsciiString munkee = options;
			munkee.nextToken(&key, "=");
			//DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d", key.str(), munkee.str(), playerSlot));

			LANGameSlot *slot = m_currentGame->getLANSlot(playerSlot);
			if (!slot)
				return;

			if (key == "User")
			{
				slot->setLogin(munkee.str()+1);
				return;
			}
			else if (key == "Host")
			{
				slot->setHost(munkee.str()+1);
				return;
			}
		}

		// O75 (H10): while START is out, a player's request (army, colour, team ...) would change the room the players
		// started with: not taken (the request's sender is in the match by then or is told by the room it stays)
		if (AmIHost() && m_localIP != playerIP && m_startDelivering && options.compare("HELLO") != 0)
		{
			m_currentGame->setPlayerLastHeard(playerSlot, LanClock::now());
			LanTest::joinLog("host: a request from slot %d during START: not taken", playerSlot);
			return;
		}
		// Parse player requests (side, color, etc)
		if( AmIHost() && m_localIP != playerIP)
		{
			m_seatJoinedMs[playerSlot] = 0;   // O70: heard from: the seat is confirmed
			if (options.compare("HELLO") == 0)
			{
				m_currentGame->setPlayerLastHeard(playerSlot, LanClock::now());
			}
			else
			{
				m_currentGame->setPlayerLastHeard(playerSlot, LanClock::now());
				Bool change = false;
				Bool shouldUnaccept = false;
				AsciiString key;
				options.nextToken(&key, "=");
				Int val = atoi(options.str()+1);
				DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d", key.str(), options.str(), playerSlot));

				LANGameSlot *slot = m_currentGame->getLANSlot(playerSlot);
				if (!slot)
					return;

				if (key == "Color")
				{
					if (val >= -1 && val < TheMultiplayerSettings->getNumColors() && val != slot->getColor() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
					{
						Bool colorAvailable = TRUE;
						if(val != -1 )
						{
							for(Int i=0; i <MAX_SLOTS; i++)
							{
								LANGameSlot *checkSlot = m_currentGame->getLANSlot(i);
								if(val == checkSlot->getColor() && slot != checkSlot)
								{
									colorAvailable = FALSE;
									break;
								}
							}
						}
						if(colorAvailable)
							slot->setColor(val);
						change = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid color %d", val));
					}
				}
				else if (key == "PlayerTemplate")
				{
					if (val >= PLAYERTEMPLATE_MIN && val < ThePlayerTemplateStore->getPlayerTemplateCount() && val != slot->getPlayerTemplate())
					{
						const Bool wasObserver = slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER;
						slot->setPlayerTemplate(val);
						if (val == PLAYERTEMPLATE_OBSERVER)
						{
							slot->setColor(-1);
							slot->setStartPos(-1);
							slot->setTeamNumber(-1);
						}
						// O56 (row 41): an observer frees the player place it held (and takes it back when it plays again)
						if (wasObserver != (val == PLAYERTEMPLATE_OBSERVER)) {
							m_currentGame->balanceObserverPlaces();
						}
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid PlayerTemplate %d", val));
					}
				}
				else if (key == "StartPos" && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
				{

					if (val >= -1 && val < MAX_SLOTS && val != slot->getStartPos())
					{
						Bool startPosAvailable = TRUE;
						if(val != -1)
							for(Int i=0; i <MAX_SLOTS; i++)
							{
								LANGameSlot *checkSlot = m_currentGame->getLANSlot(i);
								if(val == checkSlot->getStartPos() && slot != checkSlot)
								{
									startPosAvailable = FALSE;
									break;
								}
							}
						if(startPosAvailable)
							slot->setStartPos(val);
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid startPos %d", val));
					}
				}
				else if (key == "Team")
				{
					if (val >= -1 && val < MAX_SLOTS/2 && val != slot->getTeamNumber() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
					{
						slot->setTeamNumber(val);
						change = true;
						shouldUnaccept = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid team %d", val));
					}
				}
				else if (key == "NAT")
				{
					if ((val >= FirewallHelperClass::FIREWALL_TYPE_SIMPLE) &&
							(val <= FirewallHelperClass::FIREWALL_TYPE_DESTINATION_PORT_DELTA))
					{
						slot->setNATBehavior((FirewallHelperClass::FirewallBehaviorType)val);
						DEBUG_LOG(("NAT behavior set to %d for player %d", val, playerSlot));
						change = true;
					}
					else
					{
						DEBUG_LOG(("Rejecting invalid NAT behavior %d", (Int)val));
					}
				}

				if (change)
				{
					if (shouldUnaccept)
						m_currentGame->resetAccepted();
					RequestGameOptions(GenerateGameOptionsString(), true);
					lanUpdateSlotList();
					DEBUG_LOG(("Slot value is color=%d, PlayerTemplate=%d, startPos=%d, team=%d",
						slot->getColor(), slot->getPlayerTemplate(), slot->getStartPos(), slot->getTeamNumber()));
					DEBUG_LOG(("Slot list updated to %s", GenerateGameOptionsString().str()));
				}
			}
		}
	}
}


/*
void LANAPI::OnSlotList( ReturnType ret, LANGameInfo *theGame )
{
	if (!theGame || theGame != m_currentGame)
		return;

	Bool foundMe = false;
	for (int player = 0; player < MAX_SLOTS; ++player)
	{
		if (m_currentGame->getIP(player) == m_localIP)
		{
			foundMe = true;
			break;
		}
	}
	if (!foundMe)
	{
		// I've been kicked - back to the lobby for me!
		// We're counting on the fact that OnPlayerLeave winds up calling reset on TheLAN.
		OnPlayerLeave(m_name);
		return;
	}

	lanUpdateSlotList();
}
*/
void LANAPI::OnPlayerJoin( Int slot, UnicodeString playerName )
{
	if (m_currentGame && m_currentGame->getHostIP() == m_localIP)
	{
		// Someone New Joined.. lets reset the accepts
		m_currentGame->resetAccepted();

		// Send out the game options
		RequestGameOptions(GenerateGameOptionsString(), true);
	}

	lanUpdateSlotList();
}

void LANAPI::OnGameJoin( ReturnType ret, LANGameInfo *theGame )
{
	if (ret == RET_OK)
	{
		LANbuttonPushed = true;
		TheShell->push( "Menus/LanGameOptionsMenu.wnd" );
		//lanUpdateSlotList();

		LANPreferences pref;
		AsciiString options;
		options.format("PlayerTemplate=%d", pref.getPreferredFaction());
		RequestGameOptions(options, true);
		options.format("Color=%d", pref.getPreferredColor());
		RequestGameOptions(options, true);
		options.format("User=%s", m_userName.str());
		RequestGameOptions( options, true );
		options.format("Host=%s", m_hostName.str());
		RequestGameOptions( options, true );
		options.format("NAT=%d", FirewallHelperClass::FIREWALL_TYPE_SIMPLE); // BGC: This is a LAN game, so there is no firewall.
		RequestGameOptions( options, true );
	}
	else if (ret != RET_BUSY)
	{
		/// @todo: re-enable lobby controls?  Error msgs?
		UnicodeString title, body;
		title = TheGameText->fetch("LAN:JoinFailed");
		body = getErrorStringFromReturnType(ret);
		MessageBoxOk(title, body, nullptr);
	}
}

void LANAPI::OnHostLeave()
{
	DEBUG_ASSERTCRASH(!m_inLobby && m_currentGame, ("Game info is gone!"));
	if (m_inLobby || !m_currentGame)
		return;
	LANbuttonPushed = true;
	DEBUG_LOG(("Host left - popping to lobby"));
	TheShell->pop();
}

void LANAPI::OnPlayerLeave( UnicodeString player )
{
	DEBUG_ASSERTCRASH(!m_inLobby && m_currentGame, ("Game info is gone!"));
	if (m_inLobby || !m_currentGame || m_currentGame->isGameInProgress())
		return;

	if (m_name.compare(player) == 0)
	{
		// We're leaving.  Save options and Pop the shell up a screen.
		//DEBUG_CRASH(("Slot is %d", m_currentGame->getLocalSlotNum()));
		if (m_currentGame && m_currentGame->isInGame() && m_currentGame->getLocalSlotNum() >= 0)
		{
			LANPreferences pref;
			AsciiString option;
			option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getPlayerTemplate());
			pref["PlayerTemplate"] = option;
			option.format("%d", m_currentGame->getLANSlot( m_currentGame->getLocalSlotNum() )->getColor());
			pref["Color"] = option;
			if (m_currentGame->amIHost())
				pref["Map"] = AsciiStringToQuotedPrintable(m_currentGame->getMap());
			pref.write();
		}
		LANbuttonPushed = true;
		DEBUG_LOG(("OnPlayerLeave says we're leaving!  pop away!"));
		TheShell->pop();
	}
	else
	{
		if (m_currentGame && m_currentGame->getHostIP() == m_localIP)
		{
			// Force a new slotlist send
			m_lastResendTime = 0;

			lanUpdateSlotList();
			RequestGameOptions( GenerateGameOptionsString(), true );

		}
	}
}

void LANAPI::OnGameList( LANGameInfo *gameList )
{

	if (m_inLobby)
	{
		LANDisplayGameList(listboxGames, gameList);
	}
}

void LANAPI::OnGameCreate( ReturnType ret )
{
	if (ret == RET_OK)
	{

		LANbuttonPushed = true;
		TheShell->push( "Menus/LanGameOptionsMenu.wnd" );

		RequestLobbyLeave( false );
		//RequestGameAnnounce(); // can't do this here, since we don't have a map set
	}
	else
	{
		if(m_inLobby)
		{
			switch( ret )
			{
			case RET_GAME_EXISTS:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorGameExists"), chatSystemColor, -1, -1);
				break;
			case RET_BUSY:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorBusy"), chatSystemColor, -1, -1);
				break;
			default:
				GadgetListBoxAddEntryText(listboxChatWindow, TheGameText->fetch("LAN:ErrorUnknown"), chatSystemColor, -1, -1);
			}
		}
	}

}

void LANAPI::OnPlayerList( LANPlayer *playerList )
{
	if (m_inLobby)
	{

		UnsignedInt selectedIP = 0;
		Int selectedIndex = -1;
		Int indexToSelect = -1;
		GadgetListBoxGetSelected(listboxPlayers, &selectedIndex);

		if (selectedIndex != -1 )
			selectedIP = (UnsignedInt)(uintptr_t)GadgetListBoxGetItemData(listboxPlayers, selectedIndex, 0);

		GadgetListBoxReset(listboxPlayers);

		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			Int addedIndex = GadgetListBoxAddEntryText(listboxPlayers, player->getName(), playerColor, -1, -1);
			GadgetListBoxSetItemData(listboxPlayers, (void *)player->getIP(),addedIndex, 0 );

			if (selectedIP == player->getIP())
				indexToSelect = addedIndex;

			player = player->getNext();
		}

		if (indexToSelect >= 0)
			GadgetListBoxSetSelected(listboxPlayers, indexToSelect);
	}
}

void LANAPI::OnNameChange( UnsignedInt IP, UnicodeString newName )
{
	OnPlayerList(m_lobbyPlayers);
}

void LANAPI::OnInActive(UnsignedInt IP) {

}

void LANAPI::OnChat( UnicodeString player, UnsignedInt ip, UnicodeString message, ChatType format )
{
	GameWindow *chatWindow = nullptr;

	if (m_inLobby)
	{
		chatWindow = listboxChatWindow;
	}
	else if( m_currentGame && m_currentGame->isGameInProgress() && TheShell->isShellActive())
	{
		chatWindow = listboxChatWindowScoreScreen;
	}
	else if( m_currentGame && !m_currentGame->isGameInProgress())
	{
		chatWindow = listboxChatWindowLanGame;
	}
	if (chatWindow == nullptr)
		return;
	Int index = -1;
	UnicodeString unicodeChat;
	switch (format)
	{
		case LANAPIInterface::LANCHAT_SYSTEM:
			unicodeChat = message;
			index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatSystemColor, -1, -1);
			break;
		case LANAPIInterface::LANCHAT_EMOTE:
			unicodeChat = player;
			unicodeChat.concat(L' ');
			unicodeChat.concat(message);
			if (ip == m_localIP)
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatLocalActionColor, -1, -1);
			else
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatActionColor, -1, -1);
			break;
		case LANAPIInterface::LANCHAT_NORMAL:
		default:
		{
			// Do the language filtering.
			TheLanguageFilter->filterLine(message);

			Color chatColor = GameMakeColor(255, 255, 255, 255);
			if (m_currentGame)
			{
				Int slotNum = m_currentGame->getSlotNum(player);
				// it'll be -1 if its invalid.
				if (slotNum >= 0) {
					GameSlot *gs = m_currentGame->getSlot(slotNum);
					if (gs) {
						Int colorIndex = gs->getColor();
						MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(colorIndex);
						if (def)
							chatColor = def->getColor();
					}
				}
			}

			unicodeChat = L"[";
			unicodeChat.concat(player);
			unicodeChat.concat(L"] ");
			unicodeChat.concat(message);
			if (ip == m_localIP)
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatColor, -1, -1);
			else
				index =GadgetListBoxAddEntryText(chatWindow, unicodeChat, chatColor, -1, -1);
			break;
		}
	}
	GadgetListBoxSetItemData(chatWindow, (void *)-1, index);
}
