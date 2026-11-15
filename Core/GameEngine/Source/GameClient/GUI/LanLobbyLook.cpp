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

// LanLobbyLook.cpp /////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See LanLobbyLook.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/LanLobbyLook.h"

#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "Common/PlayerTemplate.h"
#include "Common/UserPreferences.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameInfoWindow.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Image.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/PlayerTable.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"

#include <vector>

namespace
{
	//---------------------------------------------------------------------------------------------
	// Places, in the 800 x 600 frame of LanLobbyMenu.wnd (scaled to the screen). One table per variant: switching to B
	// (Options.ini "LanLobbyLayout = B") moves only these numbers.
	struct Rect { Int x0, y0, x1, y1; };
	struct Layout
	{
		Rect nameLabel, nameEntry;
		Rect addrLabel, addrBox, copyButton;
		Rect connectLabel, connectEntry, historyButton, connectButton;
		Rect gamesLabel, gamesHint, gamesTable;
		Rect panelLabel, panel;
		Rect lobbyLabel, lobbyList;
		Rect chatLabel, chatList, chatEntry, chatButton;
	};

	// A: games list left, the selected game's panel right, lobby and chat under the list (01_variant_A_...png)
	const Layout kLayoutA =
	{
		{ 52, 80, 208, 95 }, { 52, 96, 208, 120 },
		{ 216, 80, 472, 95 }, { 216, 96, 386, 120 }, { 390, 96, 472, 120 },
		{ 480, 80, 751, 95 }, { 480, 96, 636, 120 }, { 638, 96, 660, 120 }, { 664, 96, 751, 120 },
		{ 52, 128, 290, 143 }, { 290, 128, 472, 143 }, { 52, 145, 472, 320 },
		{ 480, 128, 751, 143 }, { 480, 145, 751, 462 },
		{ 52, 326, 200, 341 }, { 52, 343, 200, 462 },
		{ 208, 326, 472, 341 }, { 208, 343, 472, 436 }, { 208, 440, 446, 462 }, { 450, 440, 472, 462 },
	};

	// B: a wide games list and the chat under it on the left, the panel and the lobby (two columns) right
	const Layout kLayoutB =
	{
		{ 52, 80, 208, 95 }, { 52, 96, 208, 120 },
		{ 216, 80, 472, 95 }, { 216, 96, 386, 120 }, { 390, 96, 472, 120 },
		{ 480, 80, 751, 95 }, { 480, 96, 636, 120 }, { 638, 96, 660, 120 }, { 664, 96, 751, 120 },
		{ 52, 128, 290, 143 }, { 290, 128, 460, 143 }, { 52, 145, 460, 300 },
		{ 468, 128, 751, 143 }, { 468, 145, 751, 372 },
		{ 468, 378, 751, 393 }, { 468, 395, 751, 462 },
		{ 52, 306, 460, 321 }, { 52, 323, 460, 436 }, { 52, 440, 434, 462 }, { 438, 440, 460, 462 },
	};

	//---------------------------------------------------------------------------------------------
	// Colours of the mock
	const Color kFrame = GameMakeColor(30, 140, 255, 255);
	const Color kFill = GameMakeColor(0, 0, 0, 170);
	const Color kRowSelected = GameMakeColor(20, 30, 110, 230);
	const Color kText = GameMakeColor(255, 255, 255, 255);
	const Color kTextDim = GameMakeColor(150, 156, 172, 255);
	const Color kTextHeader = GameMakeColor(220, 224, 236, 255);
	const Color kGreen = GameMakeColor(140, 255, 50, 255);
	const Color kRed = GameMakeColor(255, 70, 70, 255);
	const Color kYellow = GameMakeColor(255, 220, 60, 255);
	const Color kDrop = GameMakeColor(0, 0, 0, 255);
	const Color kShieldOff = GameMakeColor(110, 114, 126, 140);

	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }

	const Text kTitle = { "GUI:LanScreenName", L"LAN", L"\x041B\x041E\x041A\x0410\x041B\x042C\x041D\x0410\x042F \x0421\x0415\x0422\x042C" };
	const Text kYourName = { "GUI:LanYourName", L"Your name", L"\x0412\x0430\x0448\x0435 \x0438\x043C\x044F" };
	const Text kYourAddress = { "GUI:LanYourAddress", L"Your address (for friends over Radmin VPN)", L"\x0412\x0430\x0448 \x0430\x0434\x0440\x0435\x0441 (\x0434\x043B\x044F \x0434\x0440\x0443\x0437\x0435\x0439 \x043F\x043E Radmin VPN)" };
	const Text kCopy = { "GUI:LanCopy", L"Copy", L"\x041A\x043E\x043F\x0438\x0440\x043E\x0432\x0430\x0442\x044C" };
	const Text kCopied = { "GUI:LanCopied", L"Copied", L"\x0421\x043A\x043E\x043F\x0438\x0440\x043E\x0432\x0430\x043D\x043E" };
	const Text kConnectByAddress = { "GUI:LanConnectByAddress", L"Connect by address", L"\x041F\x043E\x0434\x043A\x043B\x044E\x0447\x0438\x0442\x044C\x0441\x044F \x043F\x043E \x0430\x0434\x0440\x0435\x0441\x0443" };
	const Text kConnect = { "GUI:LanConnect", L"Connect", L"\x0412\x043E\x0439\x0442\x0438" };
	const Text kOpenGames = { "GUI:LanOpenGames", L"OPEN GAMES", L"\x041E\x0422\x041A\x0420\x042B\x0422\x042B\x0415 \x0418\x0413\x0420\x042B" };
	const Text kHint = { "GUI:LanMarksHint", L"tick: files match", L"\x0433\x0430\x043B\x043E\x0447\x043A\x0430: \x0444\x0430\x0439\x043B\x044B \x0441\x043E\x0432\x043F\x0430\x0434\x0430\x044E\x0442" };
	const Text kHintShield = { "GUI:LanMarksHintShield", L"shield: anti-cheat   tick: files match", L"\x0449\x0438\x0442: \x0437\x0430\x0449\x0438\x0442\x0430 \x043E\x0442 \x0447\x0438\x0442\x043E\x0432   \x0433\x0430\x043B\x043E\x0447\x043A\x0430: \x0444\x0430\x0439\x043B\x044B \x0441\x043E\x0432\x043F\x0430\x0434\x0430\x044E\x0442" };
	const Text kColGame = { "GUI:LanColGame", L"Game", L"\x0418\x0433\x0440\x0430" };
	const Text kColMap = { "GUI:LanColMap", L"Map", L"\x041A\x0430\x0440\x0442\x0430" };
	const Text kColPlayers = { "GUI:LanColPlayers", L"Players", L"\x0418\x0433\x0440\x043E\x043A\x0438" };
	const Text kColObservers = { "GUI:LanColObservers", L"Obs.", L"\x041D\x0430\x0431\x043B." };
	const Text kColSpeed = { "GUI:LanColSpeed", L"Speed", L"\x0421\x043A\x043E\x0440." };
	const Text kColColor = { "GUI:LanColColor", L"Colour", L"\x0426\x0432\x0435\x0442" };
	const Text kColArmy = { "GUI:LanColArmy", L"Army", L"\x0410\x0440\x043C\x0438\x044F" };
	const Text kColTeam = { "GUI:LanColTeam", L"Team", L"\x041A\x043E\x043C\x0430\x043D\x0434\x0430" };
	const Text kNoGames = { "GUI:LanNoGames", L"No games yet: create one, or connect by address.", L"\x0418\x0433\x0440 \x043F\x043E\x043A\x0430 \x043D\x0435\x0442: \x0441\x043E\x0437\x0434\x0430\x0439\x0442\x0435 \x0441\x0432\x043E\x044E \x0438\x043B\x0438 \x043F\x043E\x0434\x043A\x043B\x044E\x0447\x0438\x0442\x0435\x0441\x044C \x043F\x043E \x0430\x0434\x0440\x0435\x0441\x0443." };
	const Text kChooseGame = { "GUI:LanChooseGame", L"Choose a game in the list.", L"\x0412\x044B\x0431\x0435\x0440\x0438\x0442\x0435 \x0438\x0433\x0440\x0443 \x0432 \x0441\x043F\x0438\x0441\x043A\x0435." };
	const Text kGame = { "GUI:LanGameTitle", L"GAME", L"\x0418\x0413\x0420\x0410" };
	const Text kHost = { "GUI:LanHost", L"(host)", L"(\x0445\x043E\x0437\x044F\x0438\x043D)" };
	const Text kOpen = { "GUI:LanSlotOpen", L"Open", L"\x041E\x0442\x043A\x0440\x044B\x0442\x043E" };
	const Text kClosed = { "GUI:LanSlotClosed", L"Closed", L"\x0417\x0430\x043A\x0440\x044B\x0442\x043E" };
	const Text kObservers = { "GUI:LanObservers", L"Observers", L"\x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438" };
	const Text kSummary = { "GUI:LanSummary", L"Players %d/%d   Observers %d   Money $%d", L"\x0418\x0433\x0440\x043E\x043A\x0438 %d/%d   \x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438 %d   \x0414\x0435\x043D\x044C\x0433\x0438 $%d" };
	const Text kInLobby = { "GUI:LanInLobby", L"IN LOBBY: %d", L"\x0412 \x041B\x041E\x0411\x0411\x0418: %d" };
	const Text kChat = { "GUI:LanChat", L"CHAT", L"\x0427\x0410\x0422" };
	const Text kBadAddress = { "GUI:LanBadAddress", L"Type an address like 26.12.45.78.", L"\x0412\x0432\x0435\x0434\x0438\x0442\x0435 \x0430\x0434\x0440\x0435\x0441 \x0432\x0438\x0434\x0430 26.12.45.78." };
	const Text kInProgress = { "GUI:LanInProgress", L"in game", L"\x0438\x0434\x0451\x0442 \x0438\x0433\x0440\x0430" };

	//---------------------------------------------------------------------------------------------
	// Sample data for pictures without a network (-lanSample)
	struct SampleSlot { const WideChar *name; Int state; Int color; Int army; Int team; Int files; };   // state: 0 open 1 closed 2 human 3 AI 4 observer; files: 1 tick, 0 cross, -1 none
	struct SampleGame { const WideChar *name; const WideChar *map; Int players, maxPlayers, observers; const WideChar *speed; Int files; SampleSlot slots[8]; };
	const SampleGame kSamples[] =
	{
		{ L"Vova", L"Tournament Desert", 3, 8, 1, L"1x", 1, {
			{ L"Vova", 2, 0, 0, 1, 1 }, { L"Alisher", 2, 1, 1, 1, 1 }, { L"Sardor_K", 2, -1, -1, 2, 1 }, { L"Timur", 4, -1, -1, -1, 0 },
			{ nullptr, 0, 0, 0, 0, -1 }, { nullptr, 0, 0, 0, 0, -1 }, { nullptr, 1, 0, 0, 0, -1 }, { nullptr, 0, 0, 0, 0, -1 } } },
		{ L"Radmin: evening", L"Alpine Assault", 5, 6, 0, L"1x", 1, {
			{ L"Bek", 2, 2, 2, 1, 1 }, { L"Lola", 2, 3, 0, 1, 1 }, { L"Hard AI", 3, 4, 1, 2, -1 }, { L"Rustam", 2, 5, 2, 2, 1 },
			{ L"Aziz", 2, 6, 0, -1, 1 }, { nullptr, 0, 0, 0, 0, -1 }, { nullptr, 1, 0, 0, 0, -1 }, { nullptr, 1, 0, 0, 0, -1 } } },
		{ L"Beginners", L"River Valley", 2, 4, 0, L"1x", 0, {
			{ L"Nodir", 2, 1, 1, -1, 0 }, { L"Islom", 2, 0, 2, -1, 1 }, { nullptr, 0, 0, 0, 0, -1 }, { nullptr, 0, 0, 0, 0, -1 },
			{ nullptr, 1, 0, 0, 0, -1 }, { nullptr, 1, 0, 0, 0, -1 }, { nullptr, 1, 0, 0, 0, -1 }, { nullptr, 1, 0, 0, 0, -1 } } },
		{ L"Quick", L"Storm Desert", 8, 8, 2, L"1x", -1, {
			{ L"A1", 2, 0, 0, 1, -1 }, { L"A2", 2, 1, 1, 1, -1 }, { L"A3", 2, 2, 2, 1, -1 }, { L"A4", 2, 3, 0, 1, -1 },
			{ L"B1", 2, 4, 1, 2, -1 }, { L"B2", 2, 5, 2, 2, -1 }, { L"B3", 2, 6, 0, 2, -1 }, { L"B4", 2, 7, 1, 2, -1 } } },
	};
	const WideChar *const kSampleLobby[] = { L"Vova", L"Alisher", L"Sardor_K", L"DESKTOP-IVQ416R", L"Timur", L"Lola", L"Bek" };
	const Int kSampleLobbyFiles[] = { 1, 1, 1, 1, 0, 1, -1 };

	//---------------------------------------------------------------------------------------------
	struct State
	{
		GameWindow *parent;
		const Layout *layout;
		// the old windows
		GameWindow *title;
		GameWindow *nameEntry;
		GameWindow *clearButton;
		GameWindow *listPlayers;
		GameWindow *listGames;
		GameWindow *gameInfo;
		GameWindow *chatList;
		GameWindow *chatEntry;
		GameWindow *chatButton;
		GameWindow *buttonLook;
		std::vector<GameWindow *> oldLabels;
		// made here
		std::vector<GameWindow *> made;
		GameWindow *addrBox;
		GameWindow *copyButton;
		GameWindow *connectEntry;
		GameWindow *historyButton;
		GameWindow *connectButton;
		GameWindow *gamesTable;
		GameWindow *panel;
		GameWindow *panelLabel;
		GameWindow *lobbyList;
		GameWindow *lobbyLabel;
		GameWindow *historyPopup;
		GameFont *font;
		Bool shown;
		Bool anticheat;
		Int scroll;
		Int sampleSelected;
		Int lastClickRow;
		UnsignedInt lastClickMs;
		UnsignedInt copiedUntil;
		std::vector<UnicodeString> history;
		UnsignedInt shownIP;
		Bool historyLone;          ///< the address list is the window manager's lone window (a click elsewhere closes it)
	};
	State s;

	// display strings of the drawn tables, kept by their text
	struct CachedText { UnicodeString text; GameFont *font; DisplayString *ds; };
	std::vector<CachedText> s_texts;

	DisplayString *textString(GameFont *font, const UnicodeString &text)
	{
		for (size_t i = 0; i < s_texts.size(); ++i) {
			if (s_texts[i].font == font && s_texts[i].text.compare(text) == 0) {
				return s_texts[i].ds;
			}
		}
		if (TheDisplayStringManager == nullptr) {
			return nullptr;
		}
		if (s_texts.size() > 400)
		{
			for (size_t i = 0; i < s_texts.size(); ++i) {
				TheDisplayStringManager->freeDisplayString(s_texts[i].ds);
			}
			s_texts.clear();
		}
		CachedText c;
		c.text = text;
		c.font = font;
		c.ds = TheDisplayStringManager->newDisplayString();
		if (c.ds == nullptr) {
			return nullptr;
		}
		c.ds->setFont(font);
		c.ds->setText(text);
		s_texts.push_back(c);
		return c.ds;
	}

	DisplayString *s_measure = nullptr;   ///< one string for measuring (not kept per text)

	void freeTexts()
	{
		if (TheDisplayStringManager != nullptr)
		{
			for (size_t i = 0; i < s_texts.size(); ++i) {
				TheDisplayStringManager->freeDisplayString(s_texts[i].ds);
			}
			if (s_measure != nullptr) {
				TheDisplayStringManager->freeDisplayString(s_measure);
			}
		}
		s_measure = nullptr;
		s_texts.clear();
	}

	Int textWidth(const UnicodeString &text)
	{
		if (s_measure == nullptr && TheDisplayStringManager != nullptr) {
			s_measure = TheDisplayStringManager->newDisplayString();
		}
		Int w = 0, h = 0;
		if (s_measure != nullptr && !text.isEmpty())
		{
			s_measure->setFont(s.font);
			s_measure->setText(text);
			s_measure->getSize(&w, &h);
		}
		return w;
	}

	Int fontHeight()
	{
		return s.font != nullptr && TheWindowManager != nullptr ? TheWindowManager->winFontHeight(s.font) : 12;
	}

	/// Text clipped to 'maxW' (an ellipsis when it is cut), drawn at x, y.
	void drawText(const UnicodeString &text, Int x, Int y, Int maxW, Color color)
	{
		if (text.isEmpty() || maxW <= 4) {
			return;
		}
		UnicodeString t = text;
		if (textWidth(t) > maxW)
		{
			while (t.getLength() > 1 && textWidth(t) + textWidth(UnicodeString(L"...")) > maxW) {
				t.removeLastChar();
			}
			t.concat(L"...");
		}
		DisplayString *ds = textString(s.font, t);
		if (ds != nullptr) {
			ds->draw(x, y, color, kDrop);
		}
	}

	void drawFrame(Int x, Int y, Int w, Int h)
	{
		TheDisplay->drawFillRect(x, y, w, h, kFill);
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, kFrame);
	}

	/// The files mark: a green tick (match), a red cross (differ); nothing when not known.
	void drawFilesMark(Int x, Int y, Int size, Int mark)
	{
		const Real t = size >= 16 ? 2.5f : 2.0f;
		if (mark > 0)
		{
			TheDisplay->drawLine(x + size / 6, y + size / 2, x + size * 5 / 12, y + size * 3 / 4, t, kGreen);
			TheDisplay->drawLine(x + size * 5 / 12, y + size * 3 / 4, x + size * 5 / 6, y + size / 5, t, kGreen);
		}
		else if (mark == 0)
		{
			TheDisplay->drawLine(x + size / 5, y + size / 5, x + size * 4 / 5, y + size * 4 / 5, t, kRed);
			TheDisplay->drawLine(x + size * 4 / 5, y + size / 5, x + size / 5, y + size * 4 / 5, t, kRed);
		}
	}

	/// The anti-cheat place: only with the switch on (item 15 not built yet), as a grey outline.
	void drawShieldPlace(Int x, Int y, Int size)
	{
		if (!s.anticheat) {
			return;
		}
		const Image *off = PlayerTable::picture(PlayerTable::PICTURE_ANTICHEAT_OFF);
		if (off != nullptr) {
			TheDisplay->drawImage(off, x, y, x + size, y + size);
		} else {
			TheDisplay->drawOpenRect(x + size / 6, y + size / 8, size * 2 / 3, size * 3 / 4, 1.0f, kShieldOff);
		}
	}

	Int filesMarkOfIP(UnsignedInt ip)
	{
		if (TheLAN != nullptr && ip == TheLAN->GetLocalIP()) {
			return 1;
		}
		UnsignedInt theirs = 0;
		const UnsignedInt mine = LanFiles::own();
		if (mine == 0 || !LanFiles::fingerprintOf(ip, &theirs)) {
			return -1;
		}
		return theirs == mine ? 1 : 0;
	}

	//---------------------------------------------------------------------------------------------
	Int sx(Int x) { Int w = 800, h = 600; if (s.parent) s.parent->winGetSize(&w, &h); return x * w / 800; }
	Int sy(Int y) { Int w = 800, h = 600; if (s.parent) s.parent->winGetSize(&w, &h); return y * h / 600; }

	void place(GameWindow *w, const Rect &r)
	{
		if (w == nullptr) {
			return;
		}
		w->winSetPosition(sx(r.x0), sy(r.y0));
		w->winSetSize(sx(r.x1) - sx(r.x0), sy(r.y1) - sy(r.y0));
	}

	GameWindow *findChild(const char *name)
	{
		AsciiString full("LanLobbyMenu.wnd:");
		full.concat(name);
		return TheWindowManager->winGetWindowFromId(s.parent, TheNameKeyGenerator->nameToKey(full));
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

	UnsignedInt imageStatusOf(GameWindow *look) { return look != nullptr ? (look->winGetStatus() & WIN_STATUS_IMAGE) : 0; }

	GameWindow *makeLabel(const Rect &r, const UnicodeString &text, Color color, Bool rightAligned)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_STATIC_TEXT;
		inst.m_font = s.font;
		inst.m_enabledText.color = color;
		inst.m_enabledText.borderColor = kDrop;
		inst.m_disabledText = inst.m_enabledText;
		inst.m_hiliteText = inst.m_enabledText;
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			inst.m_enabledDrawData[i].image = nullptr;
			inst.m_enabledDrawData[i].color = WIN_COLOR_UNDEFINED;
			inst.m_enabledDrawData[i].borderColor = WIN_COLOR_UNDEFINED;
			inst.m_disabledDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i] = inst.m_enabledDrawData[i];
		}
		TextData textData;
		memset(&textData, 0, sizeof(textData));
		textData.centeredVertically = TRUE;
		GameWindow *win = TheWindowManager->gogoGadgetStaticText(s.parent, WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT, sx(r.x0), sy(r.y0),
			sx(r.x1) - sx(r.x0), sy(r.y1) - sy(r.y0), &inst, &textData, s.font, FALSE);
		if (win != nullptr)
		{
			GadgetStaticTextSetText(win, text);
			if (rightAligned)
			{
				const Int tw = textWidth(text) + 4;
				const Int w = sx(r.x1) - sx(r.x0);
				if (tw < w)
				{
					win->winSetPosition(sx(r.x1) - tw, sy(r.y0));
					win->winSetSize(tw, sy(r.y1) - sy(r.y0));
				}
			}
			s.made.push_back(win);
		}
		return win;
	}

	GameWindow *makeButton(const Rect &r, const UnicodeString &text, const char *name)
	{
		WinInstanceData inst;
		copyLook(inst, s.buttonLook, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);
		inst.m_font = s.font;
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString(name));
		GameWindow *win = TheWindowManager->gogoGadgetPushButton(s.parent, WIN_STATUS_ENABLED | imageStatusOf(s.buttonLook), sx(r.x0), sy(r.y0),
			sx(r.x1) - sx(r.x0), sy(r.y1) - sy(r.y0), &inst, s.font, s.buttonLook == nullptr);
		if (win != nullptr)
		{
			win->winSetWindowId(inst.m_id);
			win->winSetOwner(s.parent);
			GadgetButtonSetText(win, text);
			s.made.push_back(win);
		}
		return win;
	}

	GameWindow *makeEntry(const Rect &r, const char *name, Bool readOnly)
	{
		WinInstanceData inst;
		copyLook(inst, s.nameEntry, GWS_ENTRY_FIELD | GWS_MOUSE_TRACK);
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString(name));
		EntryData data;
		memset(&data, 0, sizeof(data));
		data.maxTextLen = 40;
		UnsignedInt status = WIN_STATUS_ENABLED | imageStatusOf(s.nameEntry);
		if (readOnly) {
			status |= WIN_STATUS_NO_INPUT;
		}
		GameWindow *win = TheWindowManager->gogoGadgetTextEntry(s.parent, status, sx(r.x0), sy(r.y0), sx(r.x1) - sx(r.x0), sy(r.y1) - sy(r.y0),
			&inst, &data, inst.m_font, s.nameEntry == nullptr);
		if (win != nullptr)
		{
			win->winSetWindowId(inst.m_id);
			win->winSetOwner(s.parent);
			s.made.push_back(win);
		}
		return win;
	}

	GameWindow *makeDrawn(const Rect &r, GameWinDrawFunc draw, GameWinInputFunc input, const char *name)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_USER_WINDOW;
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString(name));   // a name for the test scripts (clickwin)
		GameWindow *win = TheWindowManager->winCreate(s.parent, WIN_STATUS_ENABLED, sx(r.x0), sy(r.y0), sx(r.x1) - sx(r.x0),
			sy(r.y1) - sy(r.y0), PassMessagesToParentSystem, &inst);
		if (win != nullptr)
		{
			win->winSetWindowId(inst.m_id);
			win->winSetDrawFunc(draw);
			if (input != nullptr) {
				win->winSetInputFunc(input);
			}
			s.made.push_back(win);
		}
		return win;
	}

	//---------------------------------------------------------------------------------------------
	// The games: what a row shows
	struct GameRow
	{
		UnicodeString name, map, speed;
		Int players, maxPlayers, observers;
		Int files;
		Bool inProgress;
		LANGameInfo *game;
		const SampleGame *sample;
	};

	UnicodeString mapNameOf(const AsciiString &map)
	{
		const MapMetaData *md = TheMapCache != nullptr ? TheMapCache->findMap(map) : nullptr;
		if (md != nullptr && !md->m_displayName.isEmpty()) {
			return md->m_displayName;
		}
		// the file name without folders and extension
		AsciiString stem = map;
		const char *slash = strrchr(map.str(), '\\');
		if (slash != nullptr) {
			stem = AsciiString(slash + 1);
		}
		if (stem.endsWithNoCase(".map")) {
			stem.truncateBy(4);
		}
		UnicodeString u;
		u.translate(stem);
		return u;
	}

	Bool isObserverSlot(const GameSlot *slot)
	{
		return slot != nullptr && slot->isHuman() && slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER;
	}

	void collectRows(std::vector<GameRow> &rows)
	{
		rows.clear();
		if (LanTest::sampleData())
		{
			for (Int i = 0; i < (Int)ARRAY_SIZE(kSamples); ++i)
			{
				const SampleGame &g = kSamples[i];
				GameRow r;
				r.name = UnicodeString(g.name);
				r.map = UnicodeString(g.map);
				r.speed = UnicodeString(g.speed);
				r.players = g.players;
				r.maxPlayers = g.maxPlayers;
				r.observers = g.observers;
				r.files = g.files;
				r.inProgress = FALSE;
				r.game = nullptr;
				r.sample = &g;
				rows.push_back(r);
			}
			return;
		}
		if (s.listGames == nullptr) {
			return;
		}
		const Int count = GadgetListBoxGetNumEntries(s.listGames);
		for (Int i = 0; i < count; ++i)
		{
			LANGameInfo *game = (LANGameInfo *)GadgetListBoxGetItemData(s.listGames, i, 0);
			if (game == nullptr) {
				continue;
			}
			GameRow r;
			r.game = game;
			r.sample = nullptr;
			r.name = game->getName();
			if (r.name.isEmpty()) {
				r.name = game->getPlayerName(0);
			}
			r.map = mapNameOf(game->getMap());
			r.speed = UnicodeString(L"1x");   // LAN games of this build have no speed setting: they run at the normal speed
			r.players = r.maxPlayers = r.observers = 0;
			for (Int k = 0; k < MAX_SLOTS; ++k)
			{
				const GameSlot *slot = game->getConstSlot(k);
				if (slot == nullptr || slot->getState() == SLOT_CLOSED) {
					continue;
				}
				if (isObserverSlot(slot)) {
					++r.observers;
					continue;
				}
				++r.maxPlayers;
				if (slot->isOccupied()) {
					++r.players;
				}
			}
			r.files = filesMarkOfIP(game->getHostIP());
			r.inProgress = game->isGameInProgress();
			rows.push_back(r);
		}
	}

	Int selectedRow()
	{
		if (LanTest::sampleData()) {
			return s.sampleSelected;
		}
		Int row = -1;
		if (s.listGames != nullptr) {
			GadgetListBoxGetSelected(s.listGames, &row);
		}
		return row;
	}

	/// The columns of the games table: left edges as parts of 1000 of the width.
	enum { COL_GAME, COL_MAP, COL_PLAYERS, COL_OBS, COL_SPEED, COL_SHIELD, COL_FILES, COL_COUNT };
	const Int kColStart[COL_COUNT + 1] = { 0, 300, 600, 720, 810, 890, 945, 1000 };

	Int rowHeight() { return fontHeight() + 6; }

	void drawGamesTable(GameWindow *window, WinInstanceData *)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		drawFrame(x, y, w, h);
		const Int pad = 4, rh = rowHeight();
		const Int inner = w - 2 * pad;
		Int colX[COL_COUNT + 1];
		for (Int c = 0; c <= COL_COUNT; ++c) {
			colX[c] = x + pad + inner * kColStart[c] / 1000;
		}
		const Text *heads[COL_COUNT] = { &kColGame, &kColMap, &kColPlayers, &kColObservers, &kColSpeed, nullptr, nullptr };
		for (Int c = 0; c < COL_COUNT; ++c) {
			if (heads[c] != nullptr) {
				drawText(textOf(*heads[c]), colX[c] + 2, y + 3, colX[c + 1] - colX[c] - 4, kTextHeader);
			}
		}
		TheDisplay->drawLine(x + 1, y + rh + 1, x + w - 2, y + rh + 1, 1.0f, kFrame);

		std::vector<GameRow> rows;
		collectRows(rows);
		const Int top = y + rh + 3;
		const Int visible = max(1, (h - rh - 6) / rh);
		const Int maxScroll = max(0, (Int)rows.size() - visible);
		s.scroll = s.scroll > maxScroll ? maxScroll : (s.scroll < 0 ? 0 : s.scroll);
		if (rows.empty())
		{
			drawText(textOf(kNoGames), x + pad * 2, top + rh / 2, w - pad * 4, kTextDim);
			return;
		}
		const Int selected = selectedRow();
		const Int mark = min(rh - 4, colX[COL_FILES + 1] - colX[COL_FILES] - 2);
		for (Int i = 0; i < visible && s.scroll + i < (Int)rows.size(); ++i)
		{
			const Int index = s.scroll + i;
			const GameRow &r = rows[index];
			const Int ry = top + i * rh;
			const Bool sel = index == selected;
			if (sel)
			{
				TheDisplay->drawFillRect(x + 2, ry, w - 4, rh, kRowSelected);
				TheDisplay->drawOpenRect(x + 2, ry, w - 4, rh, 1.0f, kFrame);
			}
			const Color c = r.inProgress ? kTextDim : (sel ? kGreen : kText);
			const Int ty = ry + (rh - fontHeight()) / 2;
			UnicodeString name = r.name;
			if (r.inProgress)
			{
				name.concat(L" (");
				name.concat(textOf(kInProgress));
				name.concat(L")");
			}
			drawText(name, colX[COL_GAME] + 2, ty, colX[COL_MAP] - colX[COL_GAME] - 6, c);
			drawText(r.map, colX[COL_MAP] + 2, ty, colX[COL_PLAYERS] - colX[COL_MAP] - 6, c);
			UnicodeString t;
			t.format(L"%d/%d", r.players, r.maxPlayers);
			drawText(t, colX[COL_PLAYERS] + 2, ty, colX[COL_OBS] - colX[COL_PLAYERS] - 4, (r.maxPlayers > 0 && r.players >= r.maxPlayers) ? kRed : c);
			t.format(L"%d", r.observers);
			drawText(t, colX[COL_OBS] + 2, ty, colX[COL_SPEED] - colX[COL_OBS] - 4, c);
			drawText(r.speed, colX[COL_SPEED] + 2, ty, colX[COL_SHIELD] - colX[COL_SPEED] - 4, c);
			drawShieldPlace(colX[COL_SHIELD] + 1, ry + 2, mark);
			drawFilesMark(colX[COL_FILES] + 1, ry + 2, mark, r.files);
		}
	}

	void joinRow(Int row)
	{
		if (LanTest::sampleData() || TheLAN == nullptr || s.listGames == nullptr) {
			return;
		}
		LANGameInfo *game = (LANGameInfo *)GadgetListBoxGetItemData(s.listGames, row, 0);
		if (game != nullptr) {
			TheLAN->RequestGameJoin(game);
		}
	}

	WindowMsgHandledType inputGamesTable(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
	{
		switch (msg)
		{
			case GWM_LEFT_DOWN:
				return MSG_HANDLED;
			case GWM_LEFT_UP:
			{
				Int x = 0, y = 0;
				window->winGetScreenPosition(&x, &y);
				const Int my = (Int)HILONGTOSHORT(mData1);
				const Int rh = rowHeight();
				const Int i = (my - (y + rh + 3)) / rh;
				std::vector<GameRow> rows;
				collectRows(rows);
				const Int row = s.scroll + i;
				if (my < y + rh + 3 || row < 0 || row >= (Int)rows.size()) {
					return MSG_HANDLED;
				}
				if (LanTest::sampleData()) {
					s.sampleSelected = row;
				} else if (s.listGames != nullptr) {
					GadgetListBoxSetSelected(s.listGames, row);
				}
				const UnsignedInt now = timeGetTime();
				if (row == s.lastClickRow && now - s.lastClickMs < 450) {
					joinRow(row);   // a double click joins, as on the old list
				}
				s.lastClickRow = row;
				s.lastClickMs = now;
				return MSG_HANDLED;
			}
			case GWM_WHEEL_UP:
				--s.scroll;
				return MSG_HANDLED;
			case GWM_WHEEL_DOWN:
				++s.scroll;
				return MSG_HANDLED;
		}
		return MSG_IGNORED;
	}

	//---------------------------------------------------------------------------------------------
	// The selected game
	void drawPanel(GameWindow *window, WinInstanceData *)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		drawFrame(x, y, w, h);
		const Int pad = 4, rh = rowHeight();
		std::vector<GameRow> rows;
		collectRows(rows);
		const Int sel = selectedRow();
		if (sel < 0 || sel >= (Int)rows.size())
		{
			drawText(textOf(kChooseGame), x + pad * 2, y + rh, w - pad * 4, kTextDim);
			return;
		}
		const GameRow &r = rows[sel];
		// columns: name | colour | army | team | shield | files
		const Int icon = rh - 4;
		const Int colFiles = x + w - pad - icon;
		const Int colShield = colFiles - icon - 4;
		const Int colTeam = colShield - icon - 10;
		const Int colArmy = colTeam - icon - 8;
		const Int colColor = colArmy - icon - 6;
		drawText(textOf(kColPlayers), x + pad + 2, y + 3, colColor - x - pad - 6, kTextHeader);
		drawText(textOf(kColColor), colColor - 2, y + 3, icon + 8, kTextDim);
		drawText(textOf(kColArmy), colArmy - 2, y + 3, icon + 8, kTextDim);
		drawText(textOf(kColTeam), colTeam - 2, y + 3, colShield - colTeam, kTextDim);
		TheDisplay->drawLine(x + 1, y + rh + 1, x + w - 2, y + rh + 1, 1.0f, kFrame);
		Int ry = y + rh + 3;
		Int observers = 0;
		struct Obs { UnicodeString name; Int files; };
		std::vector<Obs> obs;
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			UnicodeString name;
			Int state = 0, color = -1, army = -1, team = -1, files = -1;
			if (r.sample != nullptr)
			{
				const SampleSlot &ss = r.sample->slots[k];
				state = ss.state; color = ss.color; army = ss.army; team = ss.team; files = ss.files;
				name = ss.name != nullptr ? UnicodeString(ss.name) : UnicodeString::TheEmptyString;
			}
			else
			{
				const GameSlot *slot = r.game != nullptr ? r.game->getConstSlot(k) : nullptr;
				if (slot == nullptr) {
					continue;
				}
				state = slot->getState() == SLOT_OPEN ? 0 : (slot->getState() == SLOT_CLOSED ? 1 : (isObserverSlot(slot) ? 4 : (slot->isHuman() ? 2 : 3)));
				color = slot->getColor();
				army = slot->getPlayerTemplate();
				team = slot->getTeamNumber();
				name = slot->getName();
				if (slot->isHuman()) {
					files = filesMarkOfIP(slot->getIP());
				}
			}
			if (state == 4)
			{
				Obs o;
				o.name = name;
				o.files = files;
				obs.push_back(o);
				++observers;
				continue;
			}
			if (ry + rh > y + h - rh * 2) {
				break;
			}
			const Int ty = ry + (rh - fontHeight()) / 2;
			if (state == 0 || state == 1)
			{
				drawText(textOf(state == 0 ? kOpen : kClosed), x + pad + 2, ty, colColor - x - pad - 6, kTextDim);
				ry += rh;
				continue;
			}
			if (k == 0)
			{
				name.concat(L" ");
				name.concat(textOf(kHost));
			}
			drawText(name, x + pad + 2, ty, colColor - x - pad - 6, k == 0 ? kGreen : kText);
			Color tint = 0xFFFFFFFF;
			const Image *colorImage = PlayerTable::colorPictureOf(color, &tint);
			if (colorImage != nullptr) {
				TheDisplay->drawImage(colorImage, colColor, ry + 2, colColor + icon, ry + 2 + icon, tint);
			}
			const Image *armyImage = PlayerTable::armyPictureOf(army);
			if (armyImage != nullptr) {
				TheDisplay->drawImage(armyImage, colArmy, ry + 2, colArmy + icon, ry + 2 + icon);
			}
			if (team >= 0)
			{
				UnicodeString t;
				t.format(L"%d", team + 1);
				drawText(t, colTeam + 2, ty, icon + 6, kText);
			}
			else if (const Image *pirate = PlayerTable::picture(PlayerTable::PICTURE_NO_TEAM))
			{
				TheDisplay->drawImage(pirate, colTeam, ry + 2, colTeam + icon, ry + 2 + icon);
			}
			if (state == 2)
			{
				drawShieldPlace(colShield, ry + 2, icon);
				drawFilesMark(colFiles, ry + 2, icon, files);
			}
			ry += rh;
		}
		// the observers (row 21 brings real observer places)
		if (!obs.empty() && ry + rh * 2 < y + h - rh)
		{
			TheDisplay->drawLine(x + 1, ry + 2, x + w - 2, ry + 2, 1.0f, kFrame);
			drawText(textOf(kObservers), x + pad + 2, ry + 4, w - pad * 4, kTextDim);
			ry += rh + 2;
			for (size_t i = 0; i < obs.size() && ry + rh < y + h - rh; ++i)
			{
				drawText(obs[i].name, x + pad + 12, ry + (rh - fontHeight()) / 2, colShield - x - pad - 16, kText);
				drawFilesMark(colFiles, ry + 2, icon, obs[i].files);
				ry += rh;
			}
		}
		// the summary line at the bottom
		Int money = 0;
		if (r.game != nullptr) {
			money = (Int)r.game->getStartingCash().countMoney();
		} else if (TheMultiplayerSettings != nullptr) {
			money = (Int)TheMultiplayerSettings->getDefaultStartingMoney().countMoney();
		}
		UnicodeString summary;
		summary.format(textOf(kSummary).str(), r.players, r.maxPlayers, observers, money);
		drawText(summary, x + pad + 2, y + h - rh, w - pad * 4, kTextDim);
	}

	//---------------------------------------------------------------------------------------------
	// The lobby
	void drawLobbyList(GameWindow *window, WinInstanceData *)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		drawFrame(x, y, w, h);
		const Int pad = 4, rh = rowHeight();
		const Int icon = rh - 4;
		std::vector<UnicodeString> names;
		std::vector<Int> files;
		if (LanTest::sampleData())
		{
			for (Int i = 0; i < (Int)ARRAY_SIZE(kSampleLobby); ++i)
			{
				names.push_back(UnicodeString(kSampleLobby[i]));
				files.push_back(kSampleLobbyFiles[i]);
			}
		}
		else if (s.listPlayers != nullptr)
		{
			const Int count = GadgetListBoxGetNumEntries(s.listPlayers);
			for (Int i = 0; i < count; ++i)
			{
				names.push_back(GadgetListBoxGetText(s.listPlayers, i, 0));
				files.push_back(filesMarkOfIP((UnsignedInt)(uintptr_t)GadgetListBoxGetItemData(s.listPlayers, i, 0)));
			}
		}
		// one column, or two when the box is wide (variant B)
		const Int columns = (w > sx(240)) ? 2 : 1;
		const Int colW = (w - 2 * pad) / columns;
		const Int perColumn = max(1, (h - 2 * pad) / rh);
		for (size_t i = 0; i < names.size(); ++i)
		{
			const Int c = (Int)i / perColumn;
			if (c >= columns) {
				break;
			}
			const Int cx = x + pad + c * colW;
			const Int ry = y + pad + ((Int)i % perColumn) * rh;
			drawText(names[i], cx + 2, ry + (rh - fontHeight()) / 2, colW - icon * 2 - 10, files[i] == 0 ? kYellow : kText);
			drawShieldPlace(cx + colW - icon * 2 - 6, ry + 2, icon);
			drawFilesMark(cx + colW - icon - 2, ry + 2, icon, files[i]);
		}
	}

	//---------------------------------------------------------------------------------------------
	// Addresses
	UnicodeString ipText(UnsignedInt ip)
	{
		UnicodeString t;
		t.format(L"%d.%d.%d.%d", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
		return t;
	}

	/// "a.b.c.d" at the start of 'text' (a ":port" or "(name)" after it is ignored); 0 when it is not an address.
	UnsignedInt parseAddress(const UnicodeString &text)
	{
		Int a = 0, b = 0, c = 0, d = 0;
		if (swscanf(text.str(), L" %d.%d.%d.%d", &a, &b, &c, &d) != 4) {
			return 0;
		}
		if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255 || a == 0) {
			return 0;
		}
		return ((UnsignedInt)a << 24) | ((UnsignedInt)b << 16) | ((UnsignedInt)c << 8) | (UnsignedInt)d;
	}

	void loadHistory()
	{
		s.history.clear();
		LANPreferences prefs;
		const Int count = prefs.getNumRemoteIPs();
		for (Int i = 0; i < count && i < 10; ++i)
		{
			const UnicodeString entry = prefs.getRemoteIPEntry(i);
			if (parseAddress(entry) != 0) {
				s.history.push_back(entry);
			}
		}
	}

	/// The address goes first in Network.ini's RemoteIP list (the direct connect screen's list), without a copy of it.
	void rememberAddress(UnsignedInt ip)
	{
		LANPreferences prefs;
		std::vector<AsciiString> keep;
		AsciiString first;
		first.format("%d.%d.%d.%d", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
		keep.push_back(first);
		const Int count = prefs.getNumRemoteIPs();
		for (Int i = 0; i < count && (Int)keep.size() < 10; ++i)
		{
			AsciiString key;
			key.format("RemoteIP%d", i);
			LANPreferences::const_iterator it = prefs.find(key.str());
			if (it == prefs.end()) {
				continue;
			}
			UnicodeString u;
			u.translate(it->second);
			if (parseAddress(u) != ip) {
				keep.push_back(it->second);
			}
		}
		for (Int i = 0; i < (Int)keep.size(); ++i)
		{
			AsciiString key;
			key.format("RemoteIP%d", i);
			prefs[key.str()] = keep[i];
		}
		AsciiString n;
		n.format("%d", (Int)keep.size());
		prefs["NumRemoteIPs"] = n;
		prefs.write();
		loadHistory();
	}

	void copyToClipboard(const UnicodeString &text)
	{
		if (!OpenClipboard(nullptr)) {
			return;
		}
		EmptyClipboard();
		const size_t bytes = (text.getLength() + 1) * sizeof(WideChar);
		HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (mem != nullptr)
		{
			void *p = GlobalLock(mem);
			if (p != nullptr)
			{
				memcpy(p, text.str(), bytes);
				GlobalUnlock(mem);
				if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
					GlobalFree(mem);
				}
			}
			else
			{
				GlobalFree(mem);
			}
		}
		CloseClipboard();
	}

	void connectTo()
	{
		if (s.connectEntry == nullptr) {
			return;
		}
		const UnsignedInt ip = parseAddress(GadgetTextEntryGetText(s.connectEntry));
		if (ip == 0)
		{
			GameWindow *chat = s.chatList;
			if (chat != nullptr) {
				GadgetListBoxAddEntryText(chat, textOf(kBadAddress), chatSystemColor, -1, 0);
			}
			return;
		}
		rememberAddress(ip);
		if (TheLAN != nullptr && !LanTest::sampleData()) {
			TheLAN->RequestGameJoinDirectConnect(ip);
		}
	}

	// the list of the last addresses, opened under the field
	void drawHistory(GameWindow *window, WinInstanceData *)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		TheDisplay->drawFillRect(x, y, w, h, GameMakeColor(0, 0, 0, 235));
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, kFrame);
		const Int rh = rowHeight();
		const ICoord2D *mouse = TheMouse != nullptr ? &TheMouse->getMouseStatus()->pos : nullptr;
		for (size_t i = 0; i < s.history.size(); ++i)
		{
			const Int ry = y + 2 + (Int)i * rh;
			const Bool hover = mouse != nullptr && mouse->x >= x && mouse->x < x + w && mouse->y >= ry && mouse->y < ry + rh;
			if (hover) {
				TheDisplay->drawFillRect(x + 1, ry, w - 2, rh, kRowSelected);
			}
			drawText(s.history[i], x + 6, ry + (rh - fontHeight()) / 2, w - 12, hover ? kGreen : kText);
		}
	}

	void closeHistory()
	{
		if (s.historyPopup != nullptr) {
			s.historyPopup->winHide(TRUE);
		}
		if (s.historyLone && TheWindowManager != nullptr)
		{
			s.historyLone = FALSE;
			TheWindowManager->winSetLoneWindow(nullptr);
		}
	}

	/// The window manager closes the lone window (GGM_CLOSE) when a click lands elsewhere.
	WindowMsgHandledType systemHistory(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg == GGM_CLOSE)
		{
			window->winHide(TRUE);
			s.historyLone = FALSE;
			return MSG_HANDLED;
		}
		return PassMessagesToParentSystem(window, msg, mData1, mData2);
	}

	WindowMsgHandledType inputHistory(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
	{
		if (msg == GWM_LEFT_DOWN) {
			return MSG_HANDLED;
		}
		if (msg != GWM_LEFT_UP) {
			return MSG_IGNORED;
		}
		Int x = 0, y = 0;
		window->winGetScreenPosition(&x, &y);
		const Int i = ((Int)HILONGTOSHORT(mData1) - y - 2) / rowHeight();
		if (i >= 0 && i < (Int)s.history.size() && s.connectEntry != nullptr) {
			GadgetTextEntrySetText(s.connectEntry, s.history[i]);
		}
		closeHistory();
		return MSG_HANDLED;
	}

	void openHistory()
	{
		if (s.historyPopup == nullptr || s.history.empty()) {
			return;
		}
		if (!s.historyPopup->winIsHidden()) {
			closeHistory();
			return;
		}
		const Rect &r = s.layout->connectEntry;
		const Int h = (Int)s.history.size() * rowHeight() + 4;
		s.historyPopup->winSetPosition(sx(r.x0), sy(r.y1));
		s.historyPopup->winSetSize(sx(s.layout->historyButton.x1) - sx(r.x0), h);
		s.historyPopup->winHide(FALSE);
		s.historyPopup->winBringToTop();
		TheWindowManager->winSetLoneWindow(s.historyPopup);   // a click elsewhere closes it
		s.historyLone = TRUE;
	}

	//---------------------------------------------------------------------------------------------
	void setShown(Bool shown)
	{
		if (shown == s.shown) {
			return;
		}
		for (size_t i = 0; i < s.made.size(); ++i) {
			if (s.made[i] != s.historyPopup) {
				s.made[i]->winHide(!shown);
			}
		}
		if (!shown) {
			closeHistory();
		}
		s.shown = shown;
	}

	/// The old windows the new ones replace: hidden again every update (the enter fade shows the windows of its group).
	void hideOld()
	{
		for (size_t i = 0; i < s.oldLabels.size(); ++i) {
			if (!s.oldLabels[i]->winIsHidden()) s.oldLabels[i]->winHide(TRUE);
		}
		GameWindow *const old[] = { s.clearButton, s.listGames, s.listPlayers, s.gameInfo };
		for (Int i = 0; i < (Int)ARRAY_SIZE(old); ++i) {
			if (old[i] != nullptr && !old[i]->winIsHidden()) old[i]->winHide(TRUE);
		}
		HideGameInfoWindow(TRUE);
	}
}

//-------------------------------------------------------------------------------------------------
UnicodeString LanLobbyLook::screenName()
{
	return textOf(kTitle);
}

void LanLobbyLook::init(GameWindow *parent)
{
	shutdown();
	s.parent = parent;
	if (s.parent == nullptr) {
		return;
	}
	OptionPreferences pref;
	OptionPreferences::const_iterator it = pref.find("LanLobbyLayout");
	s.layout = (it != pref.end() && stricmp(it->second.str(), "B") == 0) ? &kLayoutB : &kLayoutA;
	it = pref.find("LanShowAntiCheat");
	s.anticheat = it != pref.end() && stricmp(it->second.str(), "yes") == 0;
	s.lastClickRow = -1;
	s.sampleSelected = 0;

	s.nameEntry = findChild("TextEntryPlayerName");
	s.clearButton = findChild("ButtonClear");
	s.listPlayers = findChild("ListboxPlayers");
	s.listGames = findChild("ListboxGames");
	s.gameInfo = findChild("StaticTextGameInfo");
	s.chatList = findChild("ListboxChatWindowLanLobby");
	s.chatEntry = findChild("TextEntryChat");
	s.chatButton = findChild("ButtonEmote");
	s.buttonLook = findChild("ButtonHost");
	s.font = s.listGames != nullptr ? s.listGames->winGetFont() : (s.nameEntry != nullptr ? s.nameEntry->winGetFont() : nullptr);

	// the old unnamed labels: the title (the highest) stays and gets the new name, the others go
	const NameKeyType unnamed = TheNameKeyGenerator->nameToKey("LanLobbyMenu.wnd:");
	Int titleY = 1000000;
	for (GameWindow *c = s.parent->winGetChild(); c != nullptr; c = c->winGetNext())
	{
		if ((c->winGetStyle() & GWS_STATIC_TEXT) == 0 || c->winGetWindowId() != (Int)unnamed) {
			continue;
		}
		Int x = 0, y = 0;
		c->winGetPosition(&x, &y);
		if (y < titleY)
		{
			if (s.title != nullptr) {
				s.oldLabels.push_back(s.title);
			}
			s.title = c;
			titleY = y;
		}
		else
		{
			s.oldLabels.push_back(c);
		}
	}
	if (s.title != nullptr) {
		GadgetStaticTextSetText(s.title, textOf(kTitle));
	}

	const Layout &L = *s.layout;
	// the old windows that stay, in their new places
	place(s.nameEntry, L.nameEntry);
	place(s.chatList, L.chatList);
	place(s.chatEntry, L.chatEntry);
	place(s.chatButton, L.chatButton);

	// labels
	makeLabel(L.nameLabel, textOf(kYourName), kTextDim, FALSE);
	makeLabel(L.addrLabel, textOf(kYourAddress), kTextDim, FALSE);
	makeLabel(L.connectLabel, textOf(kConnectByAddress), kTextDim, FALSE);
	makeLabel(L.gamesLabel, textOf(kOpenGames), kGreen, FALSE);
	makeLabel(L.gamesHint, textOf(s.anticheat ? kHintShield : kHint), kTextDim, TRUE);
	s.panelLabel = makeLabel(L.panelLabel, UnicodeString::TheEmptyString, kGreen, FALSE);
	UnicodeString lobby;
	lobby.format(textOf(kInLobby).str(), 0);
	s.lobbyLabel = makeLabel(L.lobbyLabel, lobby, kGreen, FALSE);
	makeLabel(L.chatLabel, textOf(kChat), kGreen, FALSE);

	// address, copy, connect
	s.addrBox = makeEntry(L.addrBox, "LanLobbyMenu.wnd:Look.Address", TRUE);
	if (s.addrBox != nullptr) {
		s.addrBox->winSetEnabledTextColors(kYellow, kDrop);
	}
	s.copyButton = makeButton(L.copyButton, textOf(kCopy), "LanLobbyMenu.wnd:Look.Copy");
	s.connectEntry = makeEntry(L.connectEntry, "LanLobbyMenu.wnd:Look.ConnectAddress", FALSE);
	s.historyButton = makeButton(L.historyButton, UnicodeString(L"v"), "LanLobbyMenu.wnd:Look.History");
	s.connectButton = makeButton(L.connectButton, textOf(kConnect), "LanLobbyMenu.wnd:Look.Connect");
	loadHistory();
	if (s.connectEntry != nullptr && !s.history.empty())
	{
		// the last address, without the name the direct connect screen keeps after it
		const UnsignedInt ip = parseAddress(s.history[0]);
		GadgetTextEntrySetText(s.connectEntry, ipText(ip));
	}

	// the drawn tables
	s.gamesTable = makeDrawn(L.gamesTable, drawGamesTable, inputGamesTable, "LanLobbyMenu.wnd:Look.Games");
	s.panel = makeDrawn(L.panel, drawPanel, nullptr, "LanLobbyMenu.wnd:Look.Panel");
	s.lobbyList = makeDrawn(L.lobbyList, drawLobbyList, nullptr, "LanLobbyMenu.wnd:Look.Lobby");
	{
		const Rect popup = { L.connectEntry.x0, L.connectEntry.y1, L.historyButton.x1, L.connectEntry.y1 + 40 };
		s.historyPopup = makeDrawn(popup, drawHistory, inputHistory, "LanLobbyMenu.wnd:Look.HistoryList");
		if (s.historyPopup != nullptr) {
			s.historyPopup->winSetSystemFunc(systemHistory);
		}
	}

	// everything made here waits for the end of the enter fade (row 27)
	s.shown = TRUE;
	setShown(FALSE);
	closeHistory();
	hideOld();
}

void LanLobbyLook::update(Bool screenReady)
{
	if (s.parent == nullptr) {
		return;
	}
	hideOld();
	setShown(screenReady);
	if (!screenReady) {
		return;
	}
	// own address (copy button feedback for two seconds)
	const UnsignedInt ip = LanTest::sampleData() ? ((26u << 24) | (12u << 16) | (45u << 8) | 78u) : (TheLAN != nullptr ? TheLAN->GetLocalIP() : 0);
	if (s.addrBox != nullptr && ip != s.shownIP)
	{
		GadgetTextEntrySetText(s.addrBox, ipText(ip));
		s.shownIP = ip;
	}
	if (s.copyButton != nullptr && s.copiedUntil != 0 && timeGetTime() > s.copiedUntil)
	{
		GadgetButtonSetText(s.copyButton, textOf(kCopy));
		s.copiedUntil = 0;
	}
	// the panel's title: the selected game, its map and speed
	if (s.panelLabel != nullptr)
	{
		std::vector<GameRow> rows;
		collectRows(rows);
		const Int sel = selectedRow();
		UnicodeString t;
		if (sel >= 0 && sel < (Int)rows.size()) {
			t.format(L"%ls \x00AB%ls\x00BB   %ls   %ls", textOf(kGame).str(), rows[sel].name.str(), rows[sel].map.str(), rows[sel].speed.str());
		}
		if (t.compare(GadgetStaticTextGetText(s.panelLabel)) != 0) {
			GadgetStaticTextSetText(s.panelLabel, t);
		}
	}
	if (s.lobbyLabel != nullptr)
	{
		const Int count = LanTest::sampleData() ? (Int)ARRAY_SIZE(kSampleLobby) : (s.listPlayers != nullptr ? GadgetListBoxGetNumEntries(s.listPlayers) : 0);
		UnicodeString t;
		t.format(textOf(kInLobby).str(), count);
		if (t.compare(GadgetStaticTextGetText(s.lobbyLabel)) != 0) {
			GadgetStaticTextSetText(s.lobbyLabel, t);
		}
	}
}

void LanLobbyLook::shutdown()
{
	if (TheWindowManager != nullptr)
	{
		if (s.historyLone)
		{
			s.historyLone = FALSE;
			TheWindowManager->winSetLoneWindow(nullptr);
		}
		for (size_t i = 0; i < s.made.size(); ++i) {
			TheWindowManager->winDestroy(s.made[i]);
		}
	}
	freeTexts();
	s.made.clear();
	s.oldLabels.clear();
	s.history.clear();
	s.parent = nullptr;
	s.layout = nullptr;
	s.title = s.nameEntry = s.clearButton = s.listPlayers = s.listGames = s.gameInfo = s.chatList = s.chatEntry = s.chatButton = s.buttonLook = nullptr;
	s.addrBox = s.copyButton = s.connectEntry = s.historyButton = s.connectButton = nullptr;
	s.gamesTable = s.panel = s.panelLabel = s.lobbyList = s.lobbyLabel = s.historyPopup = nullptr;
	s.font = nullptr;
	s.shown = FALSE;
	s.scroll = 0;
	s.copiedUntil = 0;
	s.shownIP = 0;
}

Bool LanLobbyLook::onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
{
	if (s.parent == nullptr) {
		return FALSE;
	}
	GameWindow *control = (GameWindow *)mData1;
	if (control == nullptr) {
		return FALSE;
	}
	if (msg == GBM_SELECTED)
	{
		if (control == s.copyButton)
		{
			const UnsignedInt ip = TheLAN != nullptr ? TheLAN->GetLocalIP() : 0;
			if (ip != 0) {
				copyToClipboard(ipText(ip));
			}
			GadgetButtonSetText(s.copyButton, textOf(kCopied));
			s.copiedUntil = timeGetTime() + 2000;
			return TRUE;
		}
		if (control == s.connectButton)
		{
			closeHistory();
			connectTo();
			return TRUE;
		}
		if (control == s.historyButton)
		{
			openHistory();
			return TRUE;
		}
	}
	if (msg == GEM_EDIT_DONE && control == s.connectEntry)
	{
		connectTo();   // Enter in the address field
		return TRUE;
	}
	return FALSE;
}
