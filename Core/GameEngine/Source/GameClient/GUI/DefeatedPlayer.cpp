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

// TheSuperHackers @feature See DefeatedPlayer.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/DefeatedPlayer.h"

#include "Common/CommandLine.h"
#include "Common/GameUtility.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/OwnUiScale.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LanRoom.h"
#include "GameNetwork/NetworkInterface.h"

namespace
{
	Bool s_joined = FALSE;
	GameWindow *s_bar = nullptr;
	GameWindow *s_button = nullptr;

	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }
	const Text kJoin = { "GUI:DefeatedJoinObservers", L"Join observers", L"\x041F\x0435\x0440\x0435\x0439\x0442\x0438 \x0432 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438" };
	const Text kJoinTip = { "GUI:DefeatedJoinObserversTip", L"See the whole map as an observer. From then on your voice and messages go to the observers only, not to your team.", L"\x0421\x043C\x043E\x0442\x0440\x0435\x0442\x044C \x0432\x0441\x044E \x043A\x0430\x0440\x0442\x0443 \x043A\x0430\x043A \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C. \x041F\x043E\x0441\x043B\x0435 \x044D\x0442\x043E\x0433\x043E \x0432\x0430\x0448 \x0433\x043E\x043B\x043E\x0441 \x0438 \x0441\x043E\x043E\x0431\x0449\x0435\x043D\x0438\x044F \x0441\x043B\x044B\x0448\x0430\x0442 \x0438 \x0432\x0438\x0434\x044F\x0442 \x0442\x043E\x043B\x044C\x043A\x043E \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438, \x043D\x0435 \x0432\x0430\x0448\x0430 \x043A\x043E\x043C\x0430\x043D\x0434\x0430." };
	const Text kJoined = { "GUI:DefeatedJoinedObservers", L"You are an observer now: the whole map. Your voice and messages go to the observers only.", L"\x0422\x0435\x043F\x0435\x0440\x044C \x0432\x044B \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C: \x0432\x0441\x044F \x043A\x0430\x0440\x0442\x0430. \x0412\x0430\x0448 \x0433\x043E\x043B\x043E\x0441 \x0438 \x0441\x043E\x043E\x0431\x0449\x0435\x043D\x0438\x044F \x043F\x043E\x043B\x0443\x0447\x0430\x044E\x0442 \x0442\x043E\x043B\x044C\x043A\x043E \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438." };

	const Color kText = GameMakeColor(255, 255, 255, 255);
	const Color kTextBorder = GameMakeColor(0, 0, 0, 255);
	const Color kFill = GameMakeColor(24, 28, 36, 200);
	const Color kButton = GameMakeColor(48, 56, 72, 230);
	const Color kButtonBorder = GameMakeColor(120, 130, 150, 255);
	const Color kButtonHilite = GameMakeColor(80, 92, 118, 240);

	void plainLook(WinInstanceData &inst, UnsignedInt style, GameFont *font, Color fill, Color border, Color hilite)
	{
		inst.init();
		inst.m_style = style;
		inst.m_font = font;
		inst.m_enabledText.color = kText;
		inst.m_enabledText.borderColor = kTextBorder;
		inst.m_disabledText = inst.m_enabledText;
		inst.m_hiliteText = inst.m_enabledText;
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			inst.m_enabledDrawData[i].image = nullptr;
			inst.m_enabledDrawData[i].color = fill;
			inst.m_enabledDrawData[i].borderColor = border;
			inst.m_disabledDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i].color = hilite;
		}
	}

	void destroyBar()
	{
		if (s_bar != nullptr && TheWindowManager != nullptr) {
			TheWindowManager->winDestroy(s_bar);
		}
		s_bar = s_button = nullptr;
	}

	WindowMsgHandledType barSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
	{
		if (msg == GBM_SELECTED && (GameWindow *)mData1 == s_button && s_button != nullptr)
		{
			DefeatedPlayer::joinObservers();
			return MSG_HANDLED;
		}
		return MSG_IGNORED;
	}

	WindowMsgHandledType barInput(GameWindow *, UnsignedInt msg, WindowMsgData, WindowMsgData)
	{
		// the bar's background takes the mouse, so a click on it does not reach the battlefield
		switch (msg)
		{
			case GWM_LEFT_DOWN: case GWM_LEFT_UP: case GWM_RIGHT_DOWN: case GWM_RIGHT_UP:
				return MSG_HANDLED;
			default:
				return MSG_IGNORED;
		}
	}

	/// "Join observers" at the top of the screen, in the middle.
	void makeBar()
	{
		if (s_bar != nullptr || TheWindowManager == nullptr || TheDisplay == nullptr || TheFontLibrary == nullptr) {
			return;
		}
		GameFont *font = OwnUi::font(TheFontLibrary->getFont(AsciiString("Arial"),
			TheGlobalLanguageData != nullptr ? TheGlobalLanguageData->adjustFontSize(13) : 13, TRUE));
		const UnicodeString label = textOf(kJoin);
		Int tw = 120, th = 16;
		if (DisplayString *ds = TheDisplayStringManager != nullptr ? TheDisplayStringManager->newDisplayString() : nullptr)
		{
			ds->setFont(font);
			ds->setText(label);
			ds->getSize(&tw, &th);
			TheDisplayStringManager->freeDisplayString(ds);
		}
		const Int screenW = TheDisplay->getWidth();
		const Int screenH = TheDisplay->getHeight();
		const Int pad = max(4, screenH / 150);
		const Int buttonW = tw + pad * 6;
		const Int buttonH = th + pad * 2;
		const Int barW = buttonW + pad * 2;
		const Int barH = buttonH + pad * 2;
		const Int barX = (screenW - barW) / 2;
		const Int barY = max(2, screenH / 9);

		WinInstanceData inst;
		plainLook(inst, GWS_USER_WINDOW, font, kFill, kButtonBorder, kFill);
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString("DefeatedPlayer.wnd:Bar"));
		s_bar = TheWindowManager->winCreate(nullptr, WIN_STATUS_ENABLED, barX, barY, barW, barH, barSystem, &inst);
		if (s_bar == nullptr) {
			return;
		}
		s_bar->winSetWindowId(inst.m_id);
		s_bar->winSetInputFunc(barInput);

		WinInstanceData b;
		plainLook(b, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK, font, kButton, kButtonBorder, kButtonHilite);
		b.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString("DefeatedPlayer.wnd:ButtonJoinObservers"));
		s_button = TheWindowManager->gogoGadgetPushButton(s_bar, WIN_STATUS_ENABLED, pad, pad, buttonW, buttonH, &b, font, FALSE);
		if (s_button != nullptr)
		{
			s_button->winSetWindowId(b.m_id);
			GadgetButtonSetText(s_button, label);
			s_button->winSetTooltip(textOf(kJoinTip));
		}
	}

	Bool liveNetworkMatch()
	{
		return TheNetwork != nullptr && TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()
			&& !TheGameLogic->isInReplayGame() && !TheGameLogic->isLoadingMap() && ThePlayerList != nullptr && TheGameInfo != nullptr;
	}
}

Bool DefeatedPlayer::teamMatch()
{
	return TheGameInfo != nullptr && TheGameInfo->hasSeveralHumanPlayers();
}

Bool DefeatedPlayer::isDefeatedInTeamMatch(const Player *player)
{
	return player != nullptr && !player->isPlayerObserver() && TheVictoryConditions != nullptr && teamMatch()
		&& TheVictoryConditions->hasSinglePlayerBeenDefeated(const_cast<Player *>(player));
}

Bool DefeatedPlayer::localStaysWithTeam()
{
	return !s_joined && liveNetworkMatch() && isDefeatedInTeamMatch(ThePlayerList->getLocalPlayer());
}

Bool DefeatedPlayer::localJoinedObservers()
{
	return s_joined;
}

Bool DefeatedPlayer::viewsAsActive(const Player *player)
{
	if (player == nullptr) {
		return FALSE;
	}
	if (player->isPlayerActive()) {
		return TRUE;
	}
	return ThePlayerList != nullptr && player == ThePlayerList->getLocalPlayer() && localStaysWithTeam();
}

Bool DefeatedPlayer::allies(const Player *a, const Player *b)
{
	return a != nullptr && b != nullptr && (a == b
		|| (a->getRelationship(b->getDefaultTeam()) == ALLIES && b->getRelationship(a->getDefaultTeam()) == ALLIES));
}

void DefeatedPlayer::joinObservers()
{
	if (!localStaysWithTeam()) {
		return;
	}
	s_joined = TRUE;
	destroyBar();
	// the observers' channel: the room's observers of this match (their games take him in as a defeated player of it)
	if (TheLAN != nullptr) {
		LanRoom::openDefeatedObserverChannel(TheLAN->GetLocalIP());
	}
	// the whole map, as an observer: display only
	rts::setDisplayAsObserver(TRUE);
	if (TheControlBar != nullptr) {
		TheControlBar->populateObserverList();
	}
	SetInGameChatType(INGAME_CHAT_EVERYONE);
	if (TheInGameUI != nullptr) {
		TheInGameUI->messageNoFormat(textOf(kJoined));
	}
}

namespace
{
	UnsignedInt s_testDefeatFrame = 0;   ///< O68: -testDefeatAtFrame (0: none)
	Bool s_testDefeatSent = FALSE;
}

void DefeatedPlayer::configureTestDefeat(UnsignedInt frame)
{
	s_testDefeatFrame = frame;
}

void DefeatedPlayer::update()
{
	// O68 (test): the surrender at the given frame, once per run, through the normal network message
	if (s_testDefeatFrame > 0 && !s_testDefeatSent && liveNetworkMatch() && CommandLine::isTestExe() && TheGameLogic != nullptr
		&& TheGameLogic->getFrame() >= s_testDefeatFrame)
	{
		s_testDefeatSent = TRUE;
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
		msg->appendBooleanArgument(TRUE);
		DEBUG_LOG(("DefeatedPlayer: test surrender at frame %u", TheGameLogic->getFrame()));
	}
	if (!liveNetworkMatch())
	{
		if (s_joined)
		{
			s_joined = FALSE;
			LanRoom::closeDefeatedObserverChannel();
		}
		if (rts::isDisplayAsObserver()) {
			rts::setDisplayAsObserver(FALSE);
		}
		destroyBar();
		return;
	}
	if (localStaysWithTeam())
	{
		makeBar();
	}
	else
	{
		destroyBar();
	}
}
