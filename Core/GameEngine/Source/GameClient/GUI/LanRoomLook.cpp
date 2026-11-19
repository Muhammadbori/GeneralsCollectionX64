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

#include "GameClient/OwnUiScale.h"
#include "GameClient/LanRoomLook.h"

#include "Common/NameKeyGenerator.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameSpeedControl.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/LanLobbyLook.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/PlayerTable.h"
#include "GameClient/LookPalette.h"   // O94
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/LanRoom.h"

#include <math.h>
#include <vector>

namespace
{
	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }
	const Text kObservers = { "GUI:LanRoomObservers", L"Observers %d/%d", L"\x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438 %d/%d" };
	const Text kNobody = { "GUI:LanRoomNobody", L"Nobody observes.", L"\x041D\x0435\x0442 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x0439." };
	// O56 (row 43): the host's delay for outside viewers, beside the room's rules
	const Text kDelayEntry = { "GUI:LanRoomViewerDelayEntry", L"Viewers: %d s", L"\x0417\x0440\x0438\x0442\x0435\x043B\x0438: %d \x0441" };
	const Text kDelayTip = { "GUI:LanRoomViewerDelayTip", L"Delay for outside viewers of this match (the host chooses; they can always watch)", L"\x0417\x0430\x0434\x0435\x0440\x0436\x043A\x0430 \x0434\x043B\x044F \x043F\x043E\x0441\x0442\x043E\x0440\x043E\x043D\x043D\x0438\x0445 \x0437\x0440\x0438\x0442\x0435\x043B\x0435\x0439 \x044D\x0442\x043E\x0433\x043E \x043C\x0430\x0442\x0447\x0430 (\x0432\x044B\x0431\x0438\x0440\x0430\x0435\x0442 \x0445\x043E\x0441\x0442; \x0441\x043C\x043E\x0442\x0440\x0435\x0442\x044C \x043C\x043E\x0436\x043D\x043E \x0432\x0441\x0435\x0433\x0434\x0430)" };
	const Int kDelays[] = { 10, 20, 30, 45, 60, 90, 120, 180 };
	// O60 (row 46): the admin outside the 8 seats
	const Text kHostWatch = { "GUI:LanRoomHostWatch", L"Watch", L"\x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x044C" };
	const Text kHostWatchTip = { "GUI:LanRoomHostWatchTip", L"Watch the match from outside the 8 seats: your seat opens for a player, and you stay the host.", L"\x0421\x043C\x043E\x0442\x0440\x0435\x0442\x044C \x043C\x0430\x0442\x0447 \x0432\x043D\x0435 8 \x043C\x0435\x0441\x0442: \x0432\x0430\x0448\x0435 \x043C\x0435\x0441\x0442\x043E \x043E\x0441\x0432\x043E\x0431\x043E\x0434\x0438\x0442\x0441\x044F \x0434\x043B\x044F \x0438\x0433\x0440\x043E\x043A\x0430, \x0430 \x0432\x044B \x043E\x0441\x0442\x0430\x043D\x0435\x0442\x0435\x0441\x044C \x0445\x043E\x0441\x0442\x043E\x043C." };
	// TheSuperHackers @tweak O90 (row 50) "Take a slot", not a second "Play" beside the Start button
	const Text kHostPlay = { "GUI:LanRoomHostPlay", L"Take a slot", L"\x0417\x0430\x043D\x044F\x0442\x044C \x0441\x043B\x043E\x0442" };
	const Text kHostPlayTip = { "GUI:LanRoomHostPlayTip", L"Take a slot again (the first one if it is free, else the first free slot).", L"\x0421\x043D\x043E\x0432\x0430 \x0437\x0430\x043D\x044F\x0442\x044C \x0441\x043B\x043E\x0442 (\x043F\x0435\x0440\x0432\x044B\x0439, \x0435\x0441\x043B\x0438 \x043E\x043D \x0441\x0432\x043E\x0431\x043E\x0434\x0435\x043D, \x0438\x043D\x0430\x0447\x0435 \x043F\x0435\x0440\x0432\x044B\x0439 \x0441\x0432\x043E\x0431\x043E\x0434\x043D\x044B\x0439)." };
	const Text kHostNoSeatTip = { "GUI:LanRoomHostNoSeatTip", L"No free seat.", L"\x041D\x0435\x0442 \x0441\x0432\x043E\x0431\x043E\x0434\x043D\x043E\x0433\x043E \x043C\x0435\x0441\x0442\x0430." };
	const Text kAdminRow = { "GUI:LanRoomAdminRow", L"%ls - observer (host)", L"%ls - \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C (\x0445\x043E\x0441\x0442)" };
	const Color kAdminGold = GameMakeColor(255, 210, 90, 255);
	const Text kObserverPlayTip = { "GUI:LanRoomObserverPlayTip", L"You observe from the room. Take a free player place (you leave the observers list).", L"\x0412\x044B \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0435\x0442\x0435 \x0438\x0437 \x043A\x043E\x043C\x043D\x0430\x0442\x044B. \x0417\x0430\x043D\x044F\x0442\x044C \x0441\x0432\x043E\x0431\x043E\x0434\x043D\x043E\x0435 \x043C\x0435\x0441\x0442\x043E \x0438\x0433\x0440\x043E\x043A\x0430 (\x0432\x044B \x043F\x043E\x043A\x0438\x043D\x0435\x0442\x0435 \x0441\x043F\x0438\x0441\x043E\x043A \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x0439)." };   // O98
	const Text kBecome = { "GUI:LanRoomBecomeObserver", L"Become observer", L"\x0421\x0442\x0430\x0442\x044C \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0435\x043C" };

	const Color kFrame = LookPalette::kFrame;   // O94: gold in the base game
	const Color kFill = GameMakeColor(0, 0, 0, 170);
	const Color kText = GameMakeColor(255, 255, 255, 255);
	const Color kDim = LookPalette::kTextDim;   // O94
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
		GameWindow *hostToggle; ///< O60: the admin's "Watch" / "Play"
		Int hostToggleShown;    ///< -1 none yet, 0 "Watch", 1 "Play"
		Int becomeSeatless;     ///< O98: -1 none yet, 1 "Take a slot" (a room observer)
		GameWindow *adminRow;   ///< O60: the admin above the table while he watches from outside the seats
		GameWindow *adminMark;  ///< O60: the star on the admin's own seat while he plays
		GameWindow *delay;      ///< O56 (row 43)
		Int delayShown;         ///< the seconds the box shows (-1: none yet)
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
		return g != nullptr ? g->getHostIP() : 0;   // O60
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
		// O66: the admin watching from outside the seats is one of the observers here as in the lobby's column (he was left
		// out of both numbers: "0/9" here and "1/10" in the lobby for the same room)
		const Int admin = (game != nullptr && game->getHostObserves()) ? 1 : 0;
		head.format(textOf(kObservers).str(), (Int)list.size() + admin, LanRoom::capacity(game) + admin);
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

	/// O60: a five-pointed star (the admin's mark), lines only
	void drawStar(Int cx, Int cy, Int r, Color)
	{
		PlayerTable::drawAdminStar(cx, cy, r);   // O60 (S25): the same filled gold star everywhere
	}

	LANGameInfo *roomGame()
	{
		return TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
	}

	/// O60: the admin watching from outside the seats, above the table (every client draws it)
	void drawAdminRow(GameWindow *window, WinInstanceData *)
	{
		LANGameInfo *game = roomGame();
		if (game == nullptr || !game->getHostObserves()) {
			return;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		TheDisplay->drawFillRect(x, y, w, h, kFill);
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, kAdminGold);
		const Int r = max(4, h / 3);
		drawStar(x + 4 + r, y + h / 2, r, kAdminGold);
		UnicodeString text;
		text.format(textOf(kAdminRow).str(), game->getHostName().str());
		drawLine(text, x + 10 + 2 * r, y + (h - rowHeight() + 6) / 2, kText);
		LanLobbyLook::drawMarks(x + w - 4 - (rowHeight() - 4) * 2, y + (h - rowHeight() + 4) / 2 + 2, rowHeight() - 4, game->getHostIP());
	}

	/// O60: the star on the admin's own seat (where its accept light would be) while he plays
	void drawAdminMark(GameWindow *window, WinInstanceData *)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		drawStar(x + w / 2, y + h / 2, max(3, min(w, h) / 2 - 1), kAdminGold);
	}

	void copyDrawData(GameWindow *to, GameWindow *from)
	{
		if (to == nullptr || from == nullptr) {
			return;
		}
		WinInstanceData *t = to->winGetInstanceData();
		const WinInstanceData *f = from->winGetInstanceData();
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			t->m_enabledDrawData[i] = f->m_enabledDrawData[i];
			t->m_disabledDrawData[i] = f->m_disabledDrawData[i];
			t->m_hiliteDrawData[i] = f->m_hiliteDrawData[i];
		}
		to->winSetDrawFunc(from->winGetDrawFunc());
	}

	/// O56 (row 43): a drop-down list in the look of the room's own lists.
	GameWindow *makeCombo(GameWindow *parent, GameWindow *look, Int x, Int y, Int w, Int h, const char *name)
	{
		WinInstanceData inst;
		copyLook(inst, look, GWS_COMBO_BOX | GWS_MOUSE_TRACK);
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString(name));
		ComboBoxData data;
		memset(&data, 0, sizeof(data));
		data.isEditable = FALSE;
		data.maxDisplay = 8;
		data.maxChars = 64;
		data.entryData = NEW EntryData;
		memset(data.entryData, 0, sizeof(EntryData));
		data.entryData->maxTextLen = data.maxChars;
		data.listboxData = NEW ListboxData;
		memset(data.listboxData, 0, sizeof(ListboxData));
		data.listboxData->listLength = 10;
		data.listboxData->scrollBar = 1;
		data.listboxData->forceSelect = 1;
		data.listboxData->columns = 1;
		GameWindow *combo = TheWindowManager->gogoGadgetComboBox(parent, WIN_STATUS_ENABLED | (look != nullptr ? (look->winGetStatus() & WIN_STATUS_IMAGE) : 0),
			x, y, w, h, &inst, &data, inst.m_font, look == nullptr);
		if (combo == nullptr) {
			return nullptr;
		}
		combo->winSetWindowId(inst.m_id);
		combo->winSetOwner(s.parent);
		if (look != nullptr)
		{
			copyDrawData(GadgetComboBoxGetDropDownButton(combo), GadgetComboBoxGetDropDownButton(look));
			copyDrawData(GadgetComboBoxGetEditBox(combo), GadgetComboBoxGetEditBox(look));
			GameWindow *list = GadgetComboBoxGetListBox(combo);
			GameWindow *lookList = GadgetComboBoxGetListBox(look);
			copyDrawData(list, lookList);
			if (list != nullptr && lookList != nullptr)
			{
				copyDrawData(GadgetListBoxGetUpButton(list), GadgetListBoxGetUpButton(lookList));
				copyDrawData(GadgetListBoxGetDownButton(list), GadgetListBoxGetDownButton(lookList));
				GameWindow *bar = GadgetListBoxGetSlider(list);
				GameWindow *lookBar = GadgetListBoxGetSlider(lookList);
				copyDrawData(bar, lookBar);
				if (bar != nullptr && lookBar != nullptr) {
					copyDrawData(bar->winGetChild(), lookBar->winGetChild());
				}
			}
		}
		return combo;
	}

	/// O56 (row 43): the delay the room's host set: the host's own choice, else the host's LIST.
	Int roomDelay()
	{
		Int delay = 60;
		Bool outside = TRUE;
		if (iAmHost()) {
			LanRoom::hostViewerRules(&delay, &outside);
		} else if (!LanRoom::viewerRulesOf(roomHost(), &delay, &outside)) {
			return -1;
		}
		return delay;
	}

	/// O56 (row 43): the box shows 'seconds' (the nearest step; a value between steps from an older host is added).
	void showDelay(Int seconds)
	{
		if (s.delay == nullptr || seconds == s.delayShown) {
			return;
		}
		GadgetComboBoxReset(s.delay);
		Int chosen = -1;
		Int count = 0;
		for (Int i = 0; i < (Int)ARRAY_SIZE(kDelays); ++i)
		{
			if (seconds > 0 && chosen < 0 && seconds < kDelays[i])
			{
				// a value no step has: an entry of its own, so the box shows the truth
				UnicodeString text;
				text.format(textOf(kDelayEntry).str(), seconds);
				GadgetComboBoxAddEntry(s.delay, text, GameMakeColor(255, 255, 255, 255));
				GadgetComboBoxSetItemData(s.delay, count, (void *)(intptr_t)seconds);
				chosen = count++;
			}
			UnicodeString text;
			text.format(textOf(kDelayEntry).str(), kDelays[i]);
			GadgetComboBoxAddEntry(s.delay, text, GameMakeColor(255, 255, 255, 255));
			GadgetComboBoxSetItemData(s.delay, count, (void *)(intptr_t)kDelays[i]);
			if (kDelays[i] == seconds) {
				chosen = count;
			}
			++count;
		}
		if (chosen >= 0) {
			GadgetComboBoxSetSelectedPos(s.delay, chosen);
		}
		s.delayShown = seconds;
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
		s.parent = s.chatList = s.startButton = s.backButton = s.list = s.become = s.delay = nullptr;
		s.hostToggle = s.adminRow = s.adminMark = nullptr;
		s.hostToggleShown = -1;
		s.becomeSeatless = -1;
		s.delayShown = -1;
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
	s.font = OwnUi::font(s.chatList != nullptr ? s.chatList->winGetFont() : nullptr);   // O53: the observers box
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
	// O60 (row 46): the admin's row above the table and the star on his seat; both take no input
	{
		GameWindow *title = find("StaticTextTitle");
		GameWindow *frame = title != nullptr ? title->winGetParent() : nullptr;
		if (title != nullptr && frame != nullptr)
		{
			Int tx = 0, ty = 0, tw = 0, th = 0, fw = 0, fh = 0;
			title->winGetPosition(&tx, &ty);
			title->winGetSize(&tw, &th);
			frame->winGetSize(&fw, &fh);
			const Int gap = 8 * pw / 800;
			const Int rowH = min(th, rowHeight() + 8);
			const Int left = tx + tw + gap;
			const Int right = fw - gap;
			if (right - left > 80)
			{
				WinInstanceData inst;
				inst.init();
				inst.m_style = GWS_USER_WINDOW;
				s.adminRow = TheWindowManager->winCreate(frame, 0, left, ty + (th - rowH) / 2, right - left, rowH, PassMessagesToParentSystem, &inst);
				if (s.adminRow != nullptr)
				{
					s.adminRow->winSetDrawFunc(drawAdminRow);
					s.adminRow->winSetTooltip(PlayerTable::adminTip());
					s.made.push_back(s.adminRow);
				}
			}
			WinInstanceData markInst;
			markInst.init();
			markInst.m_style = GWS_USER_WINDOW;
			markInst.m_style |= GWS_MOUSE_TRACK;
			s.adminMark = TheWindowManager->winCreate(frame, WIN_STATUS_ENABLED, 0, 0, 16, 16, PassMessagesToParentSystem, &markInst);
			if (s.adminMark != nullptr)
			{
				s.adminMark->winSetDrawFunc(drawAdminMark);
				s.adminMark->winSetTooltip(PlayerTable::adminTip());
				s.adminMark->winHide(TRUE);
				s.made.push_back(s.adminMark);
			}
		}
	}
	// O60: the admin's "Watch" (outside the seats) / "Play", where a player's "Become observer" is
	if (iAmHost() && s.startButton != nullptr && s.backButton != nullptr && s.startButton->winGetParent() == s.backButton->winGetParent())
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
			inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString("LanGameOptionsMenu.wnd:Room.HostWatchOrPlay"));
			s.hostToggle = TheWindowManager->gogoGadgetPushButton(s.backButton->winGetParent(),
				WIN_STATUS_ENABLED | (s.backButton->winGetStatus() & WIN_STATUS_IMAGE), left, by, right - left, bh, &inst, inst.m_font, FALSE);
			if (s.hostToggle != nullptr)
			{
				s.hostToggle->winSetWindowId(inst.m_id);
				s.hostToggle->winSetOwner(s.parent);
				s.made.push_back(s.hostToggle);
			}
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
	// TheSuperHackers @feature O56 (row 43) The delay for outside viewers, chosen by the host in the room (everybody sees it:
	// the host's LIST carries it), beside "Limit superweapons"; the base game's room has no such row: above the observers
	// list, which gives up one row for it.
	{
		GameWindow *look = find("ComboBoxStartingCash");
		if (look == nullptr) {
			look = find("ComboBoxColor0");
		}
		GameWindow *anchor = find("CheckboxLimitSuperweapons");
		GameWindow *frame = anchor != nullptr ? anchor->winGetParent() : (s.chatList != nullptr ? s.chatList->winGetParent() : nullptr);
		Int x = 0, y = 0, w = 0, h = 0;
		Bool place = FALSE;
		if (anchor != nullptr && frame != nullptr && look != nullptr)
		{
			Int ax = 0, ay = 0, aw = 0, ah = 0, fx = 0, fy = 0, fw = 0, fh = 0;
			anchor->winGetPosition(&ax, &ay);
			anchor->winGetSize(&aw, &ah);
			frame->winGetSize(&fw, &fh);
			(void)fx; (void)fy;
			const Int gap = 6 * pw / 800;
			x = ax + aw + gap;
			w = fw - x - 8 * pw / 800;
			Int lookW = 0;
			look->winGetSize(&lookW, &h);
			y = ay + (ah - h) / 2;
			place = w > 60;
		}
		else if (s.list != nullptr && look != nullptr)
		{
			Int lx = 0, ly = 0, lw = 0, lh = 0;
			s.list->winGetPosition(&lx, &ly);
			s.list->winGetSize(&lw, &lh);
			Int dummy = 0;
			look->winGetSize(&dummy, &h);
			x = lx;
			y = ly;
			w = lw;
			s.list->winSetPosition(lx, ly + h + 2);
			s.list->winSetSize(lw, lh - h - 2);
			place = TRUE;
		}
		if (place && frame != nullptr)
		{
			s.delay = makeCombo(frame, look, x, y, w, h, "LanGameOptionsMenu.wnd:Room.ViewerDelay");
			if (s.delay != nullptr)
			{
				s.delay->winSetTooltip(textOf(kDelayTip));
				s.delay->winEnable(iAmHost());
				s.made.push_back(s.delay);
				showDelay(roomDelay());
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
	// O60 (row 46): the admin's button and his star
	LANGameInfo *game = roomGame();
	if (s.hostToggle != nullptr && game != nullptr)
	{
		const Int want = game->getHostObserves() ? 1 : 0;
		if (want != s.hostToggleShown)
		{
			GadgetButtonSetText(s.hostToggle, textOf(want ? kHostPlay : kHostWatch));
			s.hostToggleShown = want;
		}
		const Bool canPlay = LanRoom::hostHasFreeSeat(game);
		s.hostToggle->winEnable((!want || canPlay) && !TheLAN->IsStartPending());   // O62 (H5 #7): locked while a start is under way
		s.hostToggle->winSetTooltip(textOf(want ? (canPlay ? kHostPlayTip : kHostNoSeatTip) : kHostWatchTip));
	}
	// TheSuperHackers @feature O98 (P5-b) A room observer in the room: "Take a slot" where a player's "Become observer" is
	// (enabled while a seat is open), and no Accept of his own
	if (TheLAN != nullptr && TheLAN->IsSeatlessObserver() && game != nullptr)
	{
		if (s.become != nullptr)
		{
			if (s.becomeSeatless != 1)
			{
				GadgetButtonSetText(s.become, textOf(kHostPlay));
				s.becomeSeatless = 1;
			}
			const Bool canPlay = LanRoom::hostHasFreeSeat(game);
			s.become->winEnable(canPlay);
			s.become->winSetTooltip(textOf(canPlay ? kObserverPlayTip : kHostNoSeatTip));
		}
		if (s.startButton != nullptr) {
			s.startButton->winHide(TRUE);
		}
	}
	if (s.adminMark != nullptr && game != nullptr)
	{
		// on the accept light's place of the admin's seat while he plays
		Int seat = -1;
		for (Int i = 0; i < MAX_SLOTS && !game->getHostObserves(); ++i) {
			if (game->getConstSlot(i) != nullptr && game->getConstSlot(i)->isHuman() && game->getConstSlot(i)->getIP() == game->getHostIP()) seat = i;
		}
		GameWindow *light = nullptr;
		if (seat >= 0)
		{
			AsciiString name;
			name.format("ButtonAccept%d", seat);
			light = find(name.str());
		}
		if (light != nullptr && light->winGetParent() == s.adminMark->winGetParent())
		{
			Int lx = 0, ly = 0, lw = 0, lh = 0;
			light->winGetPosition(&lx, &ly);
			light->winGetSize(&lw, &lh);
			s.adminMark->winSetPosition(lx, ly);
			s.adminMark->winSetSize(lw, lh);
			s.adminMark->winHide(!screenReady);
		}
		else
		{
			s.adminMark->winHide(TRUE);
		}
	}
	// O56 (row 43): a player's box follows the host's LIST; the host's follows its own choice
	if (s.delay != nullptr)
	{
		const Int delay = roomDelay();
		if (delay > 0) {
			showDelay(delay);
		}
		s.delay->winEnable(iAmHost());
	}
}

void LanRoomLook::shutdown()
{
	teardown();
}

Bool LanRoomLook::onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
{
	// O56 (row 43): the host picks the delay for outside viewers
	if (s.parent != nullptr && msg == GCM_SELECTED && s.delay != nullptr && (GameWindow *)mData1 == s.delay)
	{
		Int pos = -1;
		GadgetComboBoxGetSelectedPos(s.delay, &pos);
		if (iAmHost() && pos >= 0)
		{
			const Int seconds = (Int)(intptr_t)GadgetComboBoxGetItemData(s.delay, pos);
			if (seconds > 0)
			{
				LanRoom::setHostDelay(seconds);
				s.delayShown = seconds;
			}
		}
		return TRUE;
	}
	// O60 (row 46): the admin watches from outside the seats, or takes a seat again
	if (s.parent != nullptr && msg == GBM_SELECTED && s.hostToggle != nullptr && (GameWindow *)mData1 == s.hostToggle)
	{
		LANGameInfo *game = roomGame();
		if (game != nullptr) {
			LanRoom::hostSetObserves(game, !game->getHostObserves());
		}
		return TRUE;
	}
	if (s.parent == nullptr || msg != GBM_SELECTED || s.become == nullptr || (GameWindow *)mData1 != s.become) {
		return FALSE;
	}
	// O98: a room observer takes a seat: off the observer list, and a normal join from the lobby
	if (TheLAN != nullptr && TheLAN->IsSeatlessObserver())
	{
		TheLAN->TakeSeatFromObserver();
		return TRUE;
	}
	// leave the slot the normal way (the menu's own Back), then ask this room for an observer place from the lobby
	LanRoom::requestBecomeObserver(roomHost());
	if (s.backButton != nullptr) {
		TheWindowManager->winSendSystemMsg(s.parent, GBM_SELECTED, (WindowMsgData)s.backButton, s.backButton->winGetWindowId());
	}
	return TRUE;
}
