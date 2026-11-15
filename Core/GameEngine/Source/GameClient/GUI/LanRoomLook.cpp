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

// LanRoomLook.cpp //////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O31 (row 21, step 3) See LanRoomLook.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/LanRoomLook.h"

#include "Common/NameKeyGenerator.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameSpeedControl.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/LanLobbyLook.h"
#include "GameClient/LocalizedFallback.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/LanRoom.h"

#include <vector>

namespace
{
	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }
	const Text kObservers = { "GUI:LanRoomObservers", L"Observers %d/%d", L"\x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438 %d/%d" };
	const Text kNobody = { "GUI:LanRoomNobody", L"Nobody observes.", L"\x041D\x0435\x0442 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x0439." };
	const Text kBecome = { "GUI:LanRoomBecomeObserver", L"Become observer", L"\x0421\x0442\x0430\x0442\x044C \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x043C" };

	const Color kFrame = GameMakeColor(30, 140, 255, 255);
	const Color kFill = GameMakeColor(0, 0, 0, 170);
	const Color kText = GameMakeColor(255, 255, 255, 255);
	const Color kDim = GameMakeColor(150, 156, 172, 255);
	const Color kHeader = GameMakeColor(140, 255, 50, 255);
	const Color kRemove = GameMakeColor(255, 90, 90, 255);
	const Color kDrop = GameMakeColor(0, 0, 0, 255);

	struct State
	{
		GameWindow *parent;
		GameWindow *chatList;
		GameWindow *startButton;
		GameWindow *backButton;
		GameWindow *list;
		GameWindow *become;
		std::vector<GameWindow *> made;
		GameFont *font;
		Bool shown;
	};
	State s;

	// the room chat keeps its .wnd width the first time; the list takes its right part (also after the layout came back)
	GameWindow *s_shrunkChat = nullptr;
	Int s_chatFullWidth = 0;

	std::vector<DisplayString *> s_strings;

	void drawLine(const UnicodeString &text, Int x, Int y, Color color)
	{
		if (TheDisplayStringManager == nullptr || text.isEmpty()) {
			return;
		}
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return;
		}
		ds->setFont(s.font);
		ds->setText(text);
		ds->draw(x, y, color, kDrop);
		s_strings.push_back(ds);
	}

	void freeStrings()
	{
		if (TheDisplayStringManager != nullptr) {
			for (size_t i = 0; i < s_strings.size(); ++i) {
				TheDisplayStringManager->freeDisplayString(s_strings[i]);
			}
		}
		s_strings.clear();
	}

	Int rowHeight()
	{
		const Int fh = s.font != nullptr && TheWindowManager != nullptr ? TheWindowManager->winFontHeight(s.font) : 12;
		return fh + 6;
	}

	GameWindow *find(const char *name)
	{
		AsciiString full("LanGameOptionsMenu.wnd:");
		full.concat(name);
		return TheWindowManager->winGetWindowFromId(s.parent, TheNameKeyGenerator->nameToKey(full));
	}

	UnsignedInt roomHost()
	{
		LANGameInfo *g = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
		return g != nullptr ? g->getIP(0) : 0;
	}

	Bool iAmHost()
	{
		LANGameInfo *g = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
		return g != nullptr && g->amIHost();
	}

	void drawList(GameWindow *window, WinInstanceData *)
	{
		freeStrings();   // last frame's strings (the list is short and changes rarely)
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		TheDisplay->drawFillRect(x, y, w, h, kFill);
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, kFrame);
		const Int rh = rowHeight();
		LANGameInfo *game = TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
		const std::vector<LanRoom::Observer> list = LanRoom::observersOf(roomHost());
		UnicodeString head;
		head.format(textOf(kObservers).str(), (Int)list.size(), LanRoom::capacity(game));
		drawLine(head, x + 4, y + 3, kHeader);
		TheDisplay->drawLine(x + 1, y + rh + 1, x + w - 2, y + rh + 1, 1.0f, kFrame);
		if (list.empty())
		{
			drawLine(textOf(kNobody), x + 4, y + rh + 4, kDim);
			return;
		}
		const Int icon = rh - 4;
		const Bool host = iAmHost();
		for (size_t i = 0; i < list.size(); ++i)
		{
			const Int ry = y + rh + 3 + (Int)i * rh;
			if (ry + rh > y + h) {
				break;
			}
			drawLine(list[i].name, x + 4, ry + 3, kText);
			const Int marksX = x + w - 4 - icon * 2 - (host ? icon + 4 : 0);
			LanLobbyLook::drawMarks(marksX, ry + 2, icon, list[i].ip);
			if (host)
			{
				// the host's remove: an x at the right
				const Int cx = x + w - 4 - icon, cy = ry + 2, d = icon / 4;
				TheDisplay->drawLine(cx + d, cy + d, cx + icon - d, cy + icon - d, 2.0f, kRemove);
				TheDisplay->drawLine(cx + icon - d, cy + d, cx + d, cy + icon - d, 2.0f, kRemove);
			}
		}
	}

	WindowMsgHandledType inputList(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
	{
		if (msg == GWM_LEFT_DOWN) {
			return MSG_HANDLED;
		}
		if (msg != GWM_LEFT_UP || !iAmHost()) {
			return MSG_IGNORED;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		const Int mx = (Int)LOLONGTOSHORT(mData1), my = (Int)HILONGTOSHORT(mData1);
		const Int rh = rowHeight();
		const Int i = (my - (y + rh + 3)) / rh;
		const std::vector<LanRoom::Observer> list = LanRoom::observersOf(roomHost());
		if (my >= y + rh + 3 && i >= 0 && i < (Int)list.size() && mx >= x + w - 4 - (rh - 4) - 4) {
			LanRoom::hostRemove(list[i].ip);
		}
		return MSG_HANDLED;
	}

	void copyLook(WinInstanceData &inst, GameWindow *look, UnsignedInt style)
	{
		inst.init();
		inst.m_style = style;
		if (look == nullptr) {
			return;
		}
		const WinInstanceData *from = look->winGetInstanceData();
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			inst.m_enabledDrawData[i] = from->m_enabledDrawData[i];
			inst.m_disabledDrawData[i] = from->m_disabledDrawData[i];
			inst.m_hiliteDrawData[i] = from->m_hiliteDrawData[i];
		}
		inst.m_enabledText = from->m_enabledText;
		inst.m_disabledText = from->m_disabledText;
		inst.m_hiliteText = from->m_hiliteText;
		inst.m_imageOffset = from->m_imageOffset;
		inst.m_font = from->m_font;
	}

	void setShown(Bool shown)
	{
		if (shown == s.shown) {
			return;
		}
		for (size_t i = 0; i < s.made.size(); ++i) {
			s.made[i]->winHide(!shown);
		}
		s.shown = shown;
	}

	void teardown()
	{
		if (TheWindowManager != nullptr) {
			for (size_t i = 0; i < s.made.size(); ++i) {
				TheWindowManager->winDestroy(s.made[i]);
			}
		}
		freeStrings();
		s.made.clear();
		s.parent = s.chatList = s.startButton = s.backButton = s.list = s.become = nullptr;
		s.font = nullptr;
		s.shown = FALSE;
	}
}

void LanRoomLook::init(GameWindow *parent)
{
	teardown();
	s.parent = parent;
	if (parent == nullptr || TheLAN == nullptr) {
		return;
	}
	s.chatList = find("ListboxChatWindowLanGame");
	s.startButton = find("ButtonStart");
	s.backButton = find("ButtonBack");
	s.font = s.chatList != nullptr ? s.chatList->winGetFont() : nullptr;
	Int pw = 800, ph = 600;
	parent->winGetSize(&pw, &ph);

	// TheSuperHackers @tweak O36 "Starting cash" on one line, as in the skirmish menu: the label as wide as its text (a
	// static text wraps at its width - 10), ending just left of the list, on the list's line (S26: two lines, the second cut)
	{
		GameWindow *cash = find("ComboBoxStartingCash");
		GameWindow *cashLabel = cash != nullptr ? GameSpeed::findStaticText(parent, TheGameText->fetch("GUI:StartingMoney")) : nullptr;
		if (cashLabel != nullptr)
		{
			Int cx = 0, cy = 0, cw = 0, ch = 0;
			cash->winGetScreenPosition(&cx, &cy);
			cash->winGetSize(&cw, &ch);
			const Int labelW = GameSpeed::textWidthOf(cashLabel) + 14;
			GameSpeed::placeOnScreen(cashLabel, cx - 6 * pw / 800 - labelW, cy, labelW, ch);
		}
	}

	// the observers list in the right part of the room chat
	if (s.chatList != nullptr)
	{
		Int cx = 0, cy = 0, cw = 0, chh = 0;
		s.chatList->winGetPosition(&cx, &cy);
		s.chatList->winGetSize(&cw, &chh);
		if (s_shrunkChat != s.chatList) {
			s_chatFullWidth = cw;
		}
		const Int listW = 160 * pw / 800;
		const Int gap = 6 * pw / 800;
		const Int chatW = s_chatFullWidth - listW - gap;
		s.chatList->winSetSize(chatW, chh);
		s_shrunkChat = s.chatList;
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_USER_WINDOW;
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString("LanGameOptionsMenu.wnd:Room.Observers"));
		GameWindow *up = s.chatList->winGetParent();
		s.list = TheWindowManager->winCreate(up, WIN_STATUS_ENABLED, cx + chatW + gap, cy, listW, chh, PassMessagesToParentSystem, &inst);
		if (s.list != nullptr)
		{
			s.list->winSetWindowId(inst.m_id);
			s.list->winSetDrawFunc(drawList);
			s.list->winSetInputFunc(inputList);
			s.made.push_back(s.list);
		}
	}
	// "Become observer" between Start and Back, for a player in a slot (the host's leaving ends the room)
	if (!iAmHost() && s.startButton != nullptr && s.backButton != nullptr && s.startButton->winGetParent() == s.backButton->winGetParent())
	{
		Int sx0 = 0, sy0 = 0, sw = 0, sh = 0, bx = 0, by = 0, bw = 0, bh = 0;
		s.startButton->winGetPosition(&sx0, &sy0);
		s.startButton->winGetSize(&sw, &sh);
		s.backButton->winGetPosition(&bx, &by);
		s.backButton->winGetSize(&bw, &bh);
		const Int gap = 8 * pw / 800;
		const Int left = sx0 + sw + gap, right = bx - gap;
		if (right - left > 40)
		{
			WinInstanceData inst;
			copyLook(inst, s.backButton, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);
			inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString("LanGameOptionsMenu.wnd:Room.BecomeObserver"));
			s.become = TheWindowManager->gogoGadgetPushButton(s.backButton->winGetParent(),
				WIN_STATUS_ENABLED | (s.backButton->winGetStatus() & WIN_STATUS_IMAGE), left, by, right - left, bh, &inst, inst.m_font, FALSE);
			if (s.become != nullptr)
			{
				s.become->winSetWindowId(inst.m_id);
				s.become->winSetOwner(s.parent);
				GadgetButtonSetText(s.become, textOf(kBecome));
				s.made.push_back(s.become);
			}
		}
	}
	s.shown = TRUE;
	setShown(FALSE);   // shown when the menu's enter transition is over
}

void LanRoomLook::update(Bool screenReady)
{
	if (s.parent == nullptr) {
		return;
	}
	setShown(screenReady);
}

void LanRoomLook::shutdown()
{
	teardown();
}

Bool LanRoomLook::onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
{
	if (s.parent == nullptr || msg != GBM_SELECTED || s.become == nullptr || (GameWindow *)mData1 != s.become) {
		return FALSE;
	}
	// leave the slot the normal way (the menu's own Back), then ask this room for an observer place from the lobby
	LanRoom::requestBecomeObserver(roomHost());
	if (s.backButton != nullptr) {
		TheWindowManager->winSendSystemMsg(s.parent, GBM_SELECTED, (WindowMsgData)s.backButton, s.backButton->winGetWindowId());
	}
	return TRUE;
}
