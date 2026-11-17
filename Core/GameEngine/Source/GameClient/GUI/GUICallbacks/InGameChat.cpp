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

// FILE: InGameChat.cpp ///////////////////////////////////////////////////////////////////////
// Author: Matthew D. Campbell - June 2002
// Desc: GUI callbacks for the in-game chat entry
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "GameClient/DefeatedPlayer.h"

#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LanguageFilter.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameClient/LocalizedFallback.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/LanRoom.h"

static GameWindow *chatWindow = nullptr;
static GameWindow *chatTextEntry = nullptr;
static GameWindow *chatTypeStaticText = nullptr;
static UnicodeString s_savedChat;
static InGameChatType inGameChatType;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ShowInGameChat( Bool immediate )
{
	// TheSuperHackers @feature O34 In a replay only a LAN room observer has a chat (to the room's other observers); an
	// outside viewer (or any other replay) has none.
	if ((TheGameLogic->isInReplayGame() || DefeatedPlayer::localJoinedObservers()) && !LanRoom::observerChannelOpen())
		return;

	if (TheInGameUI->isQuitMenuVisible())
		return;

	if (TheDisconnectMenu && TheDisconnectMenu->isScreenVisible())
		return;

	if (chatWindow)
	{
		chatWindow->winHide(FALSE);
		chatWindow->winEnable(TRUE);
		chatTextEntry->winHide(FALSE);
		chatTextEntry->winEnable(TRUE);
		GadgetTextEntrySetText( chatTextEntry, s_savedChat );
		s_savedChat.clear();
	}
	else
	{
		chatWindow = TheWindowManager->winCreateFromScript( "InGameChat.wnd" );

		static NameKeyType textEntryChatID = TheNameKeyGenerator->nameToKey( "InGameChat.wnd:TextEntryChat" );
		chatTextEntry = TheWindowManager->winGetWindowFromId( nullptr, textEntryChatID );
		GadgetTextEntrySetText( chatTextEntry, UnicodeString::TheEmptyString );

		static NameKeyType chatTypeStaticTextID = TheNameKeyGenerator->nameToKey( "InGameChat.wnd:StaticTextChatType" );
		chatTypeStaticText = TheWindowManager->winGetWindowFromId( nullptr, chatTypeStaticTextID );
	}
	TheWindowManager->winSetFocus( chatTextEntry );
	SetInGameChatType( INGAME_CHAT_EVERYONE );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ResetInGameChat()
{
	if(chatWindow)
		TheWindowManager->winDestroy( chatWindow );
	chatWindow = nullptr;
	chatTextEntry = nullptr;
	chatTypeStaticText = nullptr;
	s_savedChat.clear();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void HideInGameChat( Bool immediate )
{
	if (chatWindow)
	{
		s_savedChat = GadgetTextEntryGetText( chatTextEntry );
		chatWindow->winHide(TRUE);
		chatWindow->winEnable(FALSE);
		chatTextEntry->winHide(TRUE);
		chatTextEntry->winEnable(FALSE);
		TheWindowManager->winSetFocus( nullptr );
	}
	TheWindowManager->winSetFocus( nullptr );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void SetInGameChatType( InGameChatType chatType )
{
	inGameChatType = chatType;
	if (chatTypeStaticText)
	{
		if (TheGameLogic->isInReplayGame() || DefeatedPlayer::localJoinedObservers())
		{
			GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Observers") );   // O34, O59: the observers' channel
			return;
		}
		if (DefeatedPlayer::localStaysWithTeam())
		{
			GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Allies") );   // O59: a defeated player's team only
			return;
		}
		switch (inGameChatType)
		{
		case INGAME_CHAT_EVERYONE:
			if (ThePlayerList->getLocalPlayer()->isPlayerActive())
				GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Everyone") );
			else
				GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Observers") );
			break;
		case INGAME_CHAT_ALLIES:
			GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Allies") );
			break;
		case INGAME_CHAT_PLAYERS:
			GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Players") );
			break;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool IsInGameChatActive() {
	if (chatWindow != nullptr) {
		if (chatWindow->winIsHidden() == FALSE) {
			return TRUE;
		}
	}
	return FALSE;
}

// Slash commands -------------------------------------------------------------------------
extern "C" {
int getQR2HostingStatus();
}
extern int isThreadHosting;

Bool handleInGameSlashCommands(UnicodeString uText)
{
	AsciiString message;
	message.translate(uText);

	if (message.getCharAt(0) != '/')
	{
		return FALSE; // not a slash command
	}

	AsciiString remainder = message.str() + 1;
	AsciiString token;
	remainder.nextToken(&token);
	token.toLower();

	if (token == "host")
	{
		UnicodeString s;
		s.format(L"Hosting qr2:%d thread:%d", getQR2HostingStatus(), isThreadHosting);
		TheInGameUI->message(s);
		return TRUE; // was a slash command
	}

	return FALSE; // not a slash command
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ToggleInGameChat( Bool immediate )
{
	static Bool justHid = false;
	if (justHid)
	{
		justHid = false;
		return;
	}

	// O34: in a replay, only a LAN room observer's channel. O59: also a defeated player who joined the observers.
	const Bool observerChannel = TheGameLogic->isInReplayGame() || DefeatedPlayer::localJoinedObservers();
	if (observerChannel && !LanRoom::observerChannelOpen())
		return;

	if (!observerChannel && !TheGameInfo->isMultiPlayer() && TheGlobalData->m_netMinPlayers)
		return;

	if (chatWindow)
	{
		Bool show = chatWindow->winIsHidden();
		if (show)
			ShowInGameChat( immediate );
		else
		{
			if (chatTextEntry)
			{
				// Send what is there, clear it out, and hide the window
				UnicodeString msg = GadgetTextEntryGetText( chatTextEntry );
				msg.trim();
				if (observerChannel)
				{
					// O34: to the room's other observers only; nothing goes to the match
					if (!msg.isEmpty())
					{
						TheLanguageFilter->filterLine(msg);
						LanRoom::sendObserverChat(msg);
					}
				}
				else if (!msg.isEmpty() && !handleInGameSlashCommands(msg))
				{
					const Player *localPlayer = ThePlayerList->getLocalPlayer();
					Int playerMask = 0;
					// TheSuperHackers @feature A defeated player (a spectator from then on) or an observer sends no text to
					// team-mates (items 19, 20; the same rule as the voice chat): they are left out of every chat.
					// TheSuperHackers @feature O59 (row 47) In a match of several people a defeated player stays with his team: his
					// text goes to his team-mates only (whatever the chat type), never to enemies or observers.
					const Bool staysWithTeam = DefeatedPlayer::localStaysWithTeam();
					const Bool outOfPlay = !staysWithTeam && localPlayer != nullptr && (localPlayer->isPlayerObserver()
						|| (TheVictoryConditions != nullptr && TheVictoryConditions->hasSinglePlayerBeenDefeated(const_cast<Player *>(localPlayer))));

					for (Int i=0; i<MAX_SLOTS; ++i)
					{
						const Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
						if (player && localPlayer)
						{
							switch (inGameChatType)
							{
							case INGAME_CHAT_EVERYONE:
								if (!TheGameInfo->getConstSlot(i)->isMuted())
									playerMask |= (1<<i);
								break;
							case INGAME_CHAT_ALLIES:
								if ( (player->getRelationship(localPlayer->getDefaultTeam()) == ALLIES &&
									localPlayer->getRelationship(player->getDefaultTeam()) == ALLIES) || player==localPlayer )
									playerMask |= (1<<i);
								break;
							case INGAME_CHAT_PLAYERS:
								if ( player == localPlayer )
									playerMask |= (1<<i);
								break;
							}
						}
					}
					if (staysWithTeam && localPlayer != nullptr)
					{
						playerMask = 0;
						for (Int i=0; i<MAX_SLOTS; ++i)
						{
							const Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
							if (player && !player->isPlayerObserver() && DefeatedPlayer::allies(player, localPlayer)
								&& !TheGameInfo->getConstSlot(i)->isMuted())
							{
								playerMask |= (1<<i);
							}
						}
					}
					if (outOfPlay && localPlayer != nullptr)
					{
						const Int maskBefore = playerMask;
						for (Int i=0; i<MAX_SLOTS; ++i)
						{
							const Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
							if (player && player != localPlayer && player->getRelationship(localPlayer->getDefaultTeam()) == ALLIES
								&& localPlayer->getRelationship(player->getDefaultTeam()) == ALLIES)
							{
								playerMask &= ~(1<<i);
							}
						}
						if (playerMask != maskBefore || inGameChatType == INGAME_CHAT_ALLIES)
						{
							TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE("GUI:ChatNoTeamWhenOut",
								localizedFallback(L"You are out of play: your team-mates do not get your messages.",
									L"\x0412\x044B \x0432\x044B\x0431\x044B\x043B\x0438 \x0438\x0437 \x0438\x0433\x0440\x044B: \x0441\x043E\x044E\x0437\x043D\x0438\x043A\x0438 \x043D\x0435 \x043F\x043E\x043B\x0443\x0447\x0430\x044E\x0442 \x0432\x0430\x0448\x0438 \x0441\x043E\x043E\x0431\x0449\x0435\x043D\x0438\x044F.")));
						}
					}
					TheLanguageFilter->filterLine(msg);
					TheNetwork->sendChat(msg, playerMask);
				}
				GadgetTextEntrySetText( chatTextEntry, UnicodeString::TheEmptyString );
				HideInGameChat( immediate );
				justHid = true;
			}
		}
	}
	else
	{
		ShowInGameChat( immediate );
	}
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType InGameChatInput( GameWindow *window, UnsignedInt msg,
																			WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
//			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{
					HideInGameChat();
					return MSG_HANDLED;
					//return MSG_IGNORED;
				}

			}

			return MSG_HANDLED;

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType InGameChatSystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		//---------------------------------------------------------------------------------------------
		case GGM_FOCUS_CHANGE:
		{
//			Bool focus = (Bool) mData1;
			//if (focus)
				//TheWindowManager->winSetGrabWindow( chatTextEntry );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;
		}

		//---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
		{
			ToggleInGameChat();
			//HideInGameChat();

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			static NameKeyType buttonClearID = TheNameKeyGenerator->nameToKey( "InGameChat.wnd:ButtonClear" );
			if (control && control->winGetWindowId() == buttonClearID)
			{
				if (chatTextEntry)
					GadgetTextEntrySetText( chatTextEntry, UnicodeString::TheEmptyString );
				s_savedChat.clear();
			}
			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

