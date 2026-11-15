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

#include "Common/FrameShots.h"
#include "Common/GuardLink.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "Common/PlayerTemplate.h"
#include "Common/QuotedPrintable.h"
#include "Common/UserPreferences.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/TickSlider.h"
#include "GameNetwork/LanRoom.h"
#include "GameClient/GameFont.h"
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
		Rect gamesLabel, gamesTable;   ///< O29: the marks' legend is the table's last line (no separate hint label)
		Rect panelLabel, panel;
		Rect lobbyLabel, lobbyList;
		Rect chatLabel, chatList, chatEntry, chatButton;
		Rect panelActions;             ///< O31: Observe / Watch and the delay slider, under the panel
	};

	// A: games list left, the selected game's panel right, lobby and chat under the list (01_variant_A_...png)
	const Layout kLayoutA =
	{
		{ 52, 80, 208, 95 }, { 52, 96, 208, 120 },
		{ 216, 80, 472, 95 }, { 216, 96, 386, 120 }, { 390, 96, 472, 120 },
		{ 480, 80, 751, 95 }, { 480, 96, 636, 120 }, { 638, 96, 660, 120 }, { 664, 96, 751, 120 },
		{ 52, 128, 472, 143 }, { 52, 145, 472, 320 },
		{ 480, 128, 751, 143 }, { 480, 145, 751, 436 },
		{ 52, 326, 200, 341 }, { 52, 343, 200, 462 },
		{ 208, 326, 472, 341 }, { 208, 343, 472, 436 }, { 208, 440, 446, 462 }, { 450, 440, 472, 462 },
		{ 480, 440, 751, 462 },
	};

	// B: a wide games list and the chat under it on the left, the panel and the lobby (two columns) right. O36: down to y 496
	// (the buttons start at 505): the panel shows the slots and the observers, the lobby list three rows (S26 B at 800 x 600)
	const Layout kLayoutB =
	{
		{ 52, 80, 208, 95 }, { 52, 96, 208, 120 },
		{ 216, 80, 472, 95 }, { 216, 96, 386, 120 }, { 390, 96, 472, 120 },
		{ 480, 80, 751, 95 }, { 480, 96, 636, 120 }, { 638, 96, 660, 120 }, { 664, 96, 751, 120 },
		{ 52, 128, 460, 143 }, { 52, 145, 460, 300 },
		{ 468, 128, 751, 143 }, { 468, 145, 751, 378 },
		{ 468, 404, 751, 419 }, { 468, 421, 751, 496 },
		{ 52, 306, 460, 321 }, { 52, 323, 460, 470 }, { 52, 474, 434, 496 }, { 438, 474, 460, 496 },
		{ 468, 380, 751, 400 },
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
	// O29
	const Text kColPlayersShort = { "GUI:LanColPlayersShort", L"P", L"\x0418" };
	const Text kColObserversShort = { "GUI:LanColObserversShort", L"O", L"\x041D" };
	const Text kColSpeedShort = { "GUI:LanColSpeedShort", L"S", L"\x0421" };
	const Text kColColorShort = { "GUI:LanColColorShort", L"C", L"\x0426" };
	const Text kColArmyShort = { "GUI:LanColArmyShort", L"A", L"\x0410" };
	const Text kColTeamShort = { "GUI:LanColTeamShort", L"T", L"\x041A" };
	const Text kSummaryPlayers = { "GUI:LanSummaryPlayers", L"Players %d/%d   Observers %d", L"\x0418\x0433\x0440\x043E\x043A\x0438 %d/%d   \x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x0438 %d" };
	const Text kSummaryMoney = { "GUI:LanSummaryMoney", L"Money $%d", L"\x0414\x0435\x043D\x044C\x0433\x0438 $%d" };
	const Text kGameFull = { "GUI:LanGameFull", L"This game is full (%d/%d). Choose another game, or wait for a free place.", L"\x042D\x0442\x0430 \x0438\x0433\x0440\x0430 \x0437\x0430\x043F\x043E\x043B\x043D\x0435\x043D\x0430 (%d/%d). \x0412\x044B\x0431\x0435\x0440\x0438\x0442\x0435 \x0434\x0440\x0443\x0433\x0443\x044E \x0438\x0433\x0440\x0443 \x0438\x043B\x0438 \x0434\x043E\x0436\x0434\x0438\x0442\x0435\x0441\x044C \x0441\x0432\x043E\x0431\x043E\x0434\x043D\x043E\x0433\x043E \x043C\x0435\x0441\x0442\x0430." };
	const Text kNoGamesPanel = { "GUI:LanNoGamesPanel", L"No open games on the network yet.", L"\x0412 \x0441\x0435\x0442\x0438 \x043F\x043E\x043A\x0430 \x043D\x0435\x0442 \x043E\x0442\x043A\x0440\x044B\x0442\x044B\x0445 \x0438\x0433\x0440." };
	// O31
	const Text kObserve = { "GUI:LanObserve", L"Observe", L"\x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x044C" };
	const Text kStopObserving = { "GUI:LanStopObserving", L"Stop observing", L"\x041D\x0435 \x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x044C" };
	const Text kWatch = { "GUI:LanWatch", L"Watch", L"\x0421\x043C\x043E\x0442\x0440\x0435\x0442\x044C" };
	const Text kWaiting = { "GUI:LanWaitingStream", L"Waiting...", L"\x041E\x0436\x0438\x0434\x0430\x043D\x0438\x0435..." };
	const Text kInProgress = { "GUI:LanInProgress", L"in game", L"\x0438\x0434\x0451\x0442 \x0438\x0433\x0440\x0430" };
	// O42: the host allows no outside viewers (its LIST, or its RULES during the match)
	const Text kNoViewersTag = { "GUI:LanNoViewersTag", L"no viewers", L"\x0431\x0435\x0437 \x0437\x0440\x0438\x0442\x0435\x043B\x0435\x0439" };
	const Text kNoViewersButton = { "GUI:LanNoViewersButton", L"No viewers", L"\x0411\x0435\x0437 \x0437\x0440\x0438\x0442\x0435\x043B\x0435\x0439" };

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
	/// O31: the outside viewers' delay choices (seconds) and their labels. O33: the players' servers keep the host's delay;
	/// the viewer's choice only adds to it.
	const Int kDelays[8] = { 0, 10, 20, 30, 60, 90, 120, 180 };
	const WideChar *const kDelayEnglish[8] = { L"+0", L"+10s", L"+20s", L"+30s", L"+1m", L"+1.5m", L"+2m", L"+3m" };
	const WideChar *const kDelayRussian[8] = { L"+0", L"+10\x0441", L"+20\x0441", L"+30\x0441", L"+1\x043C", L"+1,5\x043C", L"+2\x043C", L"+3\x043C" };

	const WideChar *const kSampleLobby[] = { L"Vova", L"Alisher", L"Sardor_K", L"DESKTOP-IVQ416R", L"Timur", L"Lola", L"Bek" };
	const Int kSampleLobbyFiles[] = { 1, 1, 1, 1, 0, 1, -1 };
	/// O29 (item 8): the address list with -lanSample when Network.ini has none (shown only, never written)
	const WideChar *const kSampleHistory[] = { L"26.12.45.10 (Bek)", L"26.31.7.200", L"25.4.118.3 (Lola)" };

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
		GameWindow *joinButton;
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
		GameWindow *actionButton;  ///< O31: Observe / Stop observing / Watch
		GameWindow *delaySlider;   ///< O31: the outside viewer's delay
		const Text *actionLabel;   ///< O31: the action button's text now
		GameFont *font;
		Bool shown;
		Bool anticheat;
		Int scroll;
		Int lobbyScroll;           ///< O29: the first shown name of the lobby list
		Int sampleSelected;
		Int lastClickRow;
		UnsignedInt lastClickMs;
		UnsignedInt copiedUntil;
		std::vector<UnicodeString> history;
		UnsignedInt shownIP;
		Bool historyLone;          ///< the address list is the window manager's lone window (a click elsewhere closes it)
		Bool ownFailureSaid;       ///< O30: the chat said once that this player's own anti-cheat check failed
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

	/// O29 (item 4): a header is drawn whole, never with an ellipsis (the columns are made wide enough for it).
	void drawHeader(const UnicodeString &text, Int x, Int y, Color color)
	{
		DisplayString *ds = text.isEmpty() ? nullptr : textString(s.font, text);
		if (ds != nullptr) {
			ds->draw(x, y, color, kDrop);
		}
	}

	/// 'text' cut with an ellipsis to fit 'maxW' (for a one-line label).
	UnicodeString fitLine(const UnicodeString &text, Int maxW)
	{
		UnicodeString t = text;
		if (textWidth(t) <= maxW) {
			return t;
		}
		while (t.getLength() > 1 && textWidth(t) + textWidth(UnicodeString(L"...")) > maxW) {
			t.removeLastChar();
		}
		t.concat(L"...");
		return t;
	}

	const Color kBarTrack = GameMakeColor(42, 46, 72, 255);
	const Int kBarW = 6;

	/// O29 (item 5): a thin scroll bar when not all rows are shown: the track and a thumb for the shown part.
	void drawScrollBar(Int barX, Int top, Int height, Int first, Int visible, Int total)
	{
		if (total <= visible || height <= 8) {
			return;
		}
		TheDisplay->drawFillRect(barX, top, kBarW, height, kBarTrack);
		const Int thumbH = max(8, height * visible / total);
		const Int thumbY = top + (height - thumbH) * first / max(1, total - visible);
		TheDisplay->drawFillRect(barX, thumbY, kBarW, thumbH, kFrame);
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

	/// TheSuperHackers @feature O30 (row 15) The anti-cheat shield, drawn (picture drafts come later): green and filled for
	/// a player the anti-cheat verified, a red outline with a cross for one that failed, nothing without an answer.
	void drawShieldShape(Int x, Int y, Int size, Color color, Bool filled)
	{
		// a shield: straight sides for the upper 55 %, then narrowing to a point at the bottom
		const Int w = max(6, size * 3 / 4);
		const Int left = x + (size - w) / 2;
		const Int top = y + size / 10;
		const Int h = size - size / 5;
		const Int straight = h * 55 / 100;
		Int prevL = left, prevR = left + w;
		for (Int r = 0; r < h; ++r)
		{
			Int inset = 0;
			if (r >= straight) {
				inset = (w / 2) * (r - straight) / max(1, h - straight);
			}
			const Int l = left + inset, rr = left + w - inset;
			if (filled) {
				TheDisplay->drawFillRect(l, top + r, max(1, rr - l), 1, color);
			} else if (r == 0) {
				TheDisplay->drawFillRect(l, top, rr - l, 1, color);
			} else {
				TheDisplay->drawLine(prevL, top + r - 1, l, top + r, 1.0f, color);
				TheDisplay->drawLine(prevR - 1, top + r - 1, rr - 1, top + r, 1.0f, color);
			}
			prevL = l;
			prevR = rr;
		}
	}

	void drawShieldVerdict(Int x, Int y, Int size, Int verdict)
	{
		if (verdict == GuardLink::VERDICT_VERIFIED)
		{
			drawShieldShape(x, y, size, kGreen, TRUE);
			drawShieldShape(x, y, size, GameMakeColor(40, 90, 20, 255), FALSE);
		}
		else if (verdict == GuardLink::VERDICT_FAILED)
		{
			drawShieldShape(x, y, size, kRed, FALSE);
			const Int c = size / 2, d = max(2, size / 5);
			TheDisplay->drawLine(x + c - d, y + c - d, x + c + d, y + c + d, 2.0f, kRed);
			TheDisplay->drawLine(x + c + d, y + c - d, x + c - d, y + c + d, 2.0f, kRed);
		}
	}

	void drawShield(Int x, Int y, Int size, Int verdict)
	{
		if (s.anticheat) {
			drawShieldVerdict(x, y, size, verdict);
		}
	}

	/// The anti-cheat verdict of a player: this player's own checks, or what this player's anti-cheat found about them.
	Int shieldOfIP(UnsignedInt ip)
	{
		if (TheLAN != nullptr && ip == TheLAN->GetLocalIP()) {
			return GuardLink::ownVerdict();
		}
		return GuardLink::verdictOf(ip);
	}

	/// -lanSample pictures: the sample's files mark stands in for the shield (tick: verified, cross: failed).
	Int sampleShield(Int files) { return files > 0 ? GuardLink::VERDICT_VERIFIED : (files == 0 ? GuardLink::VERDICT_FAILED : GuardLink::VERDICT_NONE); }

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

	/// O29 (item 2): 'r' is in the frame of the screen's parent; a window inside a sub-window (the old windows are all in
	/// LanLobbyMenu.wnd:GadgetParent, 41 pixels in) gets the place relative to its own parent.
	void place(GameWindow *w, const Rect &r)
	{
		if (w == nullptr) {
			return;
		}
		Int px = 0, py = 0, ox = 0, oy = 0;
		if (s.parent != nullptr) {
			s.parent->winGetScreenPosition(&px, &py);
		}
		if (GameWindow *up = w->winGetParent()) {
			up->winGetScreenPosition(&ox, &oy);
		}
		w->winSetPosition(px + sx(r.x0) - ox, py + sy(r.y0) - oy);
		w->winSetSize(sx(r.x1) - sx(r.x0), sy(r.y1) - sy(r.y0));
	}

	/// O29 (item 2): every unnamed static text under 'w', at any depth (the old labels are in a sub-window).
	void collectUnnamedLabels(GameWindow *w, Int unnamed, std::vector<GameWindow *> &out)
	{
		for (GameWindow *c = w->winGetChild(); c != nullptr; c = c->winGetNext())
		{
			if ((c->winGetStyle() & GWS_STATIC_TEXT) != 0 && c->winGetWindowId() == unnamed) {
				out.push_back(c);
			}
			collectUnnamedLabels(c, unnamed, out);
		}
	}

	/// O29 (item 6): the tables' font at the size the screen needs: about 2 % of the screen height (12 pixels at 600, 16 at
	/// 800, 30 at 1504), never smaller than the old list's own font.
	GameFont *screenFont(GameFont *font)
	{
		if (font == nullptr || TheFontLibrary == nullptr || TheDisplay == nullptr || TheWindowManager == nullptr) {
			return font;
		}
		// TheSuperHackers @tweak O36 A tall window (over 1000 lines) gets a larger share (1/44 instead of 1/50), nearer the
		// menu's own buttons and titles (S26 c03 at 2256 x 1504); 800 x 600 and 1280 x 800 stay as they were
		const Int screenH = (Int)TheDisplay->getHeight();
		const Int want = screenH > 1000 ? screenH / 44 : screenH / 50;
		const Int have = TheWindowManager->winFontHeight(font);
		if (have <= 0 || have >= want - 1) {
			return font;
		}
		const Int points = max(font->pointSize + 1, font->pointSize * want / have);
		GameFont *bigger = TheFontLibrary->getFont(font->nameString, points, font->bold);
		return bigger != nullptr ? bigger : font;
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
		Int shield;                ///< O30: the host's anti-cheat verdict
		Bool inProgress;
		Bool noViewers;            ///< O42: its host allows no outside viewers
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
				r.shield = sampleShield(g.files);
				r.inProgress = FALSE;
				r.noViewers = FALSE;
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
			// O29 (item 7): the game's own name is an id (host address and seed in hex, then the name cut to 16
			// characters); the old list showed the host's name, and so does the table
			r.name = game->getPlayerName(0);
			if (r.name.isEmpty()) {
				r.name = game->getName();
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
			r.shield = shieldOfIP(game->getHostIP());
			r.inProgress = game->isGameInProgress();
			{
				Int hostDelay = 60;
				Bool outsideAllowed = TRUE;
				r.noViewers = LanRoom::viewerRulesOf(game->getHostIP(), &hostDelay, &outsideAllowed) && !outsideAllowed;
			}
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

	/// The columns of the games table.
	enum { COL_GAME, COL_MAP, COL_PLAYERS, COL_OBS, COL_SPEED, COL_SHIELD, COL_FILES, COL_COUNT };

	Int rowHeight() { return fontHeight() + 6; }

	/// O29 (item 4): the number columns are as wide as their header and their values (no header is cut); the name and the
	/// map share the rest. One-letter headers only when the full ones would leave name and map less than 45 %.
	struct GameCols { Int x[COL_COUNT + 1]; Bool shortHeads; };

	const Text *gameHead(Int c, Bool shortHeads)
	{
		switch (c)
		{
			case COL_GAME: return &kColGame;
			case COL_MAP: return &kColMap;
			case COL_PLAYERS: return shortHeads ? &kColPlayersShort : &kColPlayers;
			case COL_OBS: return shortHeads ? &kColObserversShort : &kColObservers;
			case COL_SPEED: return shortHeads ? &kColSpeedShort : &kColSpeed;
		}
		return nullptr;
	}

	GameCols gameColumns(Int left, Int inner, Int mark)
	{
		GameCols g;
		for (Int pass = 0; pass < 2; ++pass)
		{
			g.shortHeads = pass == 1;
			const Int wPlayers = max(textWidth(textOf(*gameHead(COL_PLAYERS, g.shortHeads))), textWidth(UnicodeString(L"8/8"))) + 12;
			const Int wObs = max(textWidth(textOf(*gameHead(COL_OBS, g.shortHeads))), textWidth(UnicodeString(L"88"))) + 12;
			const Int wSpeed = max(textWidth(textOf(*gameHead(COL_SPEED, g.shortHeads))), textWidth(UnicodeString(L"1,5x"))) + 12;
			const Int wShield = s.anticheat ? mark + 6 : 0;
			const Int wFiles = mark + 6;
			const Int rest = inner - wPlayers - wObs - wSpeed - wShield - wFiles;
			g.x[COL_GAME] = left;
			g.x[COL_MAP] = left + rest * 55 / 100;
			g.x[COL_PLAYERS] = left + rest;
			g.x[COL_OBS] = g.x[COL_PLAYERS] + wPlayers;
			g.x[COL_SPEED] = g.x[COL_OBS] + wObs;
			g.x[COL_SHIELD] = g.x[COL_SPEED] + wSpeed;
			g.x[COL_FILES] = g.x[COL_SHIELD] + wShield;
			g.x[COL_COUNT] = left + inner;
			if (rest >= inner * 45 / 100) {
				break;
			}
		}
		return g;
	}

	/// The table's rows: under the header line, above the legend line (the marks' meaning, item 2).
	struct TableGeo { Int x, y, w, h, rh, top, visible, legendY, barX; };

	TableGeo gamesGeo(GameWindow *window)
	{
		TableGeo g;
		window->winGetScreenPosition(&g.x, &g.y);
		window->winGetSize(&g.w, &g.h);
		g.rh = rowHeight();
		g.top = g.y + g.rh + 3;
		g.legendY = g.y + g.h - g.rh - 2;
		g.visible = max(1, (g.legendY - g.top - 2) / g.rh);
		g.barX = g.x + g.w - kBarW - 3;
		return g;
	}

	void drawGamesTable(GameWindow *window, WinInstanceData *)
	{
		const TableGeo g = gamesGeo(window);
		const Int x = g.x, y = g.y, w = g.w, rh = g.rh, pad = 4;
		drawFrame(x, y, w, g.h);
		std::vector<GameRow> rows;
		collectRows(rows);
		const Bool overflow = (Int)rows.size() > g.visible;
		const Int inner = w - 2 * pad - (overflow ? kBarW + 4 : 0);
		const Int mark = rh - 4;
		const GameCols cols = gameColumns(x + pad, inner, mark);
		for (Int c = COL_GAME; c <= COL_SPEED; ++c) {
			drawHeader(textOf(*gameHead(c, cols.shortHeads)), cols.x[c] + 2, y + 3, kTextHeader);
		}
		TheDisplay->drawLine(x + 1, y + rh + 1, x + w - 2, y + rh + 1, 1.0f, kFrame);
		// the legend of the marks, one line at the bottom (it was a label that wrapped into two cut lines)
		TheDisplay->drawLine(x + 1, g.legendY, x + w - 2, g.legendY, 1.0f, kFrame);
		drawText(textOf(s.anticheat ? kHintShield : kHint), x + pad * 2, g.legendY + 1 + (rh - fontHeight()) / 2, w - pad * 4, kTextDim);

		const Int maxScroll = max(0, (Int)rows.size() - g.visible);
		s.scroll = s.scroll > maxScroll ? maxScroll : (s.scroll < 0 ? 0 : s.scroll);
		if (rows.empty())
		{
			drawText(textOf(kNoGames), x + pad * 2, g.top + rh / 2, w - pad * 4, kTextDim);
			return;
		}
		const Int selected = selectedRow();
		for (Int i = 0; i < g.visible && s.scroll + i < (Int)rows.size(); ++i)
		{
			const Int index = s.scroll + i;
			const GameRow &r = rows[index];
			const Int ry = g.top + i * rh;
			const Bool sel = index == selected;
			if (sel)
			{
				TheDisplay->drawFillRect(x + 2, ry, inner + pad * 2 - 4, rh, kRowSelected);
				TheDisplay->drawOpenRect(x + 2, ry, inner + pad * 2 - 4, rh, 1.0f, kFrame);
			}
			const Color c = r.inProgress ? kTextDim : (sel ? kGreen : kText);
			const Int ty = ry + (rh - fontHeight()) / 2;
			// O44: the state in brackets is drawn whole; the host's name is cut instead ("(без з..." was cut in S42). When
			// both marks do not fit next to a few letters of the name, "in game" goes (the row is dimmed for it already).
			const Int gameW = cols.x[COL_MAP] - cols.x[COL_GAME] - 6;
			UnicodeString suffix;
			for (Int pass = 0; pass < 2; ++pass)
			{
				const Bool withInGame = r.inProgress && (pass == 0 || !r.noViewers);
				suffix.clear();
				if (withInGame || r.noViewers)
				{
					suffix.concat(L" (");
					if (withInGame) {
						suffix.concat(textOf(kInProgress));
					}
					if (r.noViewers)
					{
						// O42: the host allows no outside viewers
						if (withInGame) {
							suffix.concat(L", ");
						}
						suffix.concat(textOf(kNoViewersTag));
					}
					suffix.concat(L")");
				}
				if (suffix.isEmpty() || textWidth(suffix) + textWidth(UnicodeString(L"W...")) <= gameW) {
					break;
				}
			}
			UnicodeString name = r.name;
			const Int suffixW = textWidth(suffix);
			if (!suffix.isEmpty() && textWidth(name) + suffixW > gameW)
			{
				while (name.getLength() > 1 && textWidth(name) + textWidth(UnicodeString(L"...")) + suffixW > gameW) {
					name.removeLastChar();
				}
				name.concat(L"...");
			}
			name.concat(suffix);
			drawText(name, cols.x[COL_GAME] + 2, ty, gameW, c);
			drawText(r.map, cols.x[COL_MAP] + 2, ty, cols.x[COL_PLAYERS] - cols.x[COL_MAP] - 6, c);
			UnicodeString t;
			t.format(L"%d/%d", r.players, r.maxPlayers);
			drawText(t, cols.x[COL_PLAYERS] + 2, ty, cols.x[COL_OBS] - cols.x[COL_PLAYERS] - 4, (r.maxPlayers > 0 && r.players >= r.maxPlayers) ? kRed : c);
			t.format(L"%d", r.observers);
			drawText(t, cols.x[COL_OBS] + 2, ty, cols.x[COL_SPEED] - cols.x[COL_OBS] - 4, c);
			drawText(r.speed, cols.x[COL_SPEED] + 2, ty, cols.x[COL_SHIELD] - cols.x[COL_SPEED] - 4, c);
			drawShield(cols.x[COL_SHIELD] + 1, ry + 2, mark, r.shield);
			drawFilesMark(cols.x[COL_FILES] + 1, ry + 2, mark, r.files);
		}
		drawScrollBar(g.barX, g.top, g.visible * rh, s.scroll, g.visible, (Int)rows.size());
	}

	/// O29 (item 9): a full game is not joined; the chat says why (a clear text, not the host's silent refusal).
	Bool refuseFull(Int row)
	{
		std::vector<GameRow> rows;
		collectRows(rows);
		if (row < 0 || row >= (Int)rows.size()) {
			return FALSE;
		}
		const GameRow &r = rows[row];
		if (r.maxPlayers <= 0 || r.players < r.maxPlayers) {
			return FALSE;
		}
		if (s.chatList != nullptr)
		{
			// TheSuperHackers @tweak O36 Once per game and a few seconds: Join and a double click on the same full game (or
			// the click pair of a double click) gave the line twice (S26 c05b/c05c)
			static const void *s_lastGame = nullptr;
			static Int s_lastRow = -1;
			static UnsignedInt s_lastMs = 0;
			const UnsignedInt now = timeGetTime();
			if (s_lastGame != (const void *)r.game || s_lastRow != row || now - s_lastMs > 5000)
			{
				UnicodeString t;
				t.format(textOf(kGameFull).str(), r.players, r.maxPlayers);
				GadgetListBoxAddEntryText(s.chatList, t, chatSystemColor, -1, 0);
			}
			s_lastGame = (const void *)r.game;
			s_lastRow = row;
			s_lastMs = now;
		}
		return TRUE;
	}

	void joinRow(Int row)
	{
		if (refuseFull(row) || LanTest::sampleData() || TheLAN == nullptr || s.listGames == nullptr) {
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
				const TableGeo g = gamesGeo(window);
				const Int mx = (Int)LOLONGTOSHORT(mData1);
				const Int my = (Int)HILONGTOSHORT(mData1);
				std::vector<GameRow> rows;
				collectRows(rows);
				// O29: a click on the scroll bar moves a page
				if ((Int)rows.size() > g.visible && mx >= g.barX - 3 && my >= g.top && my < g.top + g.visible * g.rh)
				{
					s.scroll += (my < g.top + g.visible * g.rh / 2) ? -g.visible : g.visible;
					return MSG_HANDLED;
				}
				const Int i = (my - g.top) / g.rh;
				const Int row = s.scroll + i;
				if (my < g.top || i >= g.visible || row < 0 || row >= (Int)rows.size()) {
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
		if (rows.empty())
		{
			// O29 (item 10): no games: the panel says so (no title, nothing left from a game that went away)
			drawText(textOf(kNoGamesPanel), x + pad * 2, y + rh, w - pad * 4, kTextDim);
			return;
		}
		if (sel < 0 || sel >= (Int)rows.size())
		{
			drawText(textOf(kChooseGame), x + pad * 2, y + rh, w - pad * 4, kTextDim);
			return;
		}
		const GameRow &r = rows[sel];
		// columns: name | colour | army | team | shield | files. O29 (item 4): the colour, army and team columns are as
		// wide as their headers, drawn whole; one-letter headers when the full ones would leave the names less than 40 %.
		const Int icon = rh - 4;
		const Int colFiles = x + w - pad - icon;
		const Int colShield = colFiles - (s.anticheat ? icon + 4 : 4);
		const Text *heads[3] = { &kColColor, &kColArmy, &kColTeam };
		Int widths[3] = { 0, 0, 0 };
		for (Int pass = 0; pass < 2; ++pass)
		{
			if (pass == 1)
			{
				heads[0] = &kColColorShort;
				heads[1] = &kColArmyShort;
				heads[2] = &kColTeamShort;
			}
			Int sum = 0;
			for (Int i = 0; i < 3; ++i)
			{
				widths[i] = max(icon, textWidth(textOf(*heads[i]))) + 8;
				sum += widths[i];
			}
			if (colShield - sum - x - pad >= w * 40 / 100) {
				break;
			}
		}
		const Int colTeam = colShield - widths[2];
		const Int colArmy = colTeam - widths[1];
		const Int colColor = colArmy - widths[0];
		const Int colStart[3] = { colColor, colArmy, colTeam };
		drawText(textOf(kColPlayers), x + pad + 2, y + 3, colColor - x - pad - 6, kTextHeader);
		for (Int i = 0; i < 3; ++i)
		{
			const UnicodeString head = textOf(*heads[i]);
			drawHeader(head, colStart[i] + (widths[i] - textWidth(head)) / 2, y + 3, kTextDim);
		}
		// the icons centred under their headers
		const Int iconColor = colColor + (widths[0] - icon) / 2;
		const Int iconArmy = colArmy + (widths[1] - icon) / 2;
		const Int iconTeam = colTeam + (widths[2] - icon) / 2;
		TheDisplay->drawLine(x + 1, y + rh + 1, x + w - 2, y + rh + 1, 1.0f, kFrame);
		// O29 (item 5): the summary on one line, or on two when the panel is narrow (800 x 600)
		Int money = 0;
		if (r.game != nullptr) {
			money = (Int)r.game->getStartingCash().countMoney();
		} else if (TheMultiplayerSettings != nullptr) {
			money = (Int)TheMultiplayerSettings->getDefaultStartingMoney().countMoney();
		}
		Int observerCount = 0;
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			if (r.sample != nullptr) {
				observerCount += r.sample->slots[k].state == 4 ? 1 : 0;
			} else if (r.game != nullptr) {
				observerCount += isObserverSlot(r.game->getConstSlot(k)) ? 1 : 0;
			}
		}
		UnicodeString summary1, summary2;
		summary1.format(textOf(kSummary).str(), r.players, r.maxPlayers, observerCount, money);
		Int summaryLines = 1;
		if (textWidth(summary1) > w - pad * 4)
		{
			summaryLines = 2;
			summary1.format(textOf(kSummaryPlayers).str(), r.players, r.maxPlayers, observerCount);
			summary2.format(textOf(kSummaryMoney).str(), money);
		}
		const Int bottom = y + h - rh * summaryLines - 2;   // the slots and observers stay above the summary
		Int ry = y + rh + 3;
		Int observers = 0;
		struct Obs { UnicodeString name; Int files; Int shield; };
		std::vector<Obs> obs;
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			UnicodeString name;
			Int state = 0, color = -1, army = -1, team = -1, files = -1, shield = GuardLink::VERDICT_NONE;
			if (r.sample != nullptr)
			{
				const SampleSlot &ss = r.sample->slots[k];
				state = ss.state; color = ss.color; army = ss.army; team = ss.team; files = ss.files;
				shield = sampleShield(ss.files);
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
				if (slot->isHuman())
				{
					files = filesMarkOfIP(slot->getIP());
					shield = shieldOfIP(slot->getIP());
				}
			}
			if (state == 4)
			{
				Obs o;
				o.name = name;
				o.files = files;
				o.shield = shield;
				obs.push_back(o);
				++observers;
				continue;
			}
			if (ry + rh > bottom) {
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
				TheDisplay->drawImage(colorImage, iconColor, ry + 2, iconColor + icon, ry + 2 + icon, tint);
			}
			const Image *armyImage = PlayerTable::armyPictureOf(army);
			if (armyImage != nullptr) {
				TheDisplay->drawImage(armyImage, iconArmy, ry + 2, iconArmy + icon, ry + 2 + icon);
			}
			if (team >= 0)
			{
				UnicodeString t;
				t.format(L"%d", team + 1);
				drawHeader(t, colTeam + (widths[2] - textWidth(t)) / 2, ty, kText);
			}
			else if (const Image *pirate = PlayerTable::picture(PlayerTable::PICTURE_NO_TEAM))
			{
				TheDisplay->drawImage(pirate, iconTeam, ry + 2, iconTeam + icon, ry + 2 + icon);
			}
			if (state == 2)
			{
				drawShield(colShield, ry + 2, icon, shield);
				drawFilesMark(colFiles, ry + 2, icon, files);
			}
			ry += rh;
		}
		// O31: the room's observers (LanRoom), with their files mark and shield
		if (r.game != nullptr)
		{
			const std::vector<LanRoom::Observer> room = LanRoom::observersOf(r.game->getHostIP());
			for (size_t i = 0; i < room.size(); ++i)
			{
				Obs o;
				o.name = room[i].name;
				o.files = filesMarkOfIP(room[i].ip);
				o.shield = shieldOfIP(room[i].ip);
				obs.push_back(o);
			}
		}
		// the observers
		if (!obs.empty() && ry + rh * 2 < bottom)
		{
			TheDisplay->drawLine(x + 1, ry + 2, x + w - 2, ry + 2, 1.0f, kFrame);
			drawText(textOf(kObservers), x + pad + 2, ry + 4, w - pad * 4, kTextDim);
			ry += rh + 2;
			for (size_t i = 0; i < obs.size() && ry + rh < bottom; ++i)
			{
				drawText(obs[i].name, x + pad + 12, ry + (rh - fontHeight()) / 2, colShield - x - pad - 16, kText);
				drawShield(colShield, ry + 2, icon, obs[i].shield);
				drawFilesMark(colFiles, ry + 2, icon, obs[i].files);
				ry += rh;
			}
		}
		// the summary at the bottom
		(void)observers;
		drawText(summary1, x + pad + 2, bottom + 2, w - pad * 4, kTextDim);
		if (summaryLines == 2) {
			drawText(summary2, x + pad + 2, bottom + 2 + rh, w - pad * 4, kTextDim);
		}
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
		std::vector<Int> shields;
		if (LanTest::sampleData())
		{
			for (Int i = 0; i < (Int)ARRAY_SIZE(kSampleLobby); ++i)
			{
				names.push_back(UnicodeString(kSampleLobby[i]));
				files.push_back(kSampleLobbyFiles[i]);
				shields.push_back(sampleShield(kSampleLobbyFiles[i]));
			}
		}
		else if (s.listPlayers != nullptr)
		{
			const Int count = GadgetListBoxGetNumEntries(s.listPlayers);
			for (Int i = 0; i < count; ++i)
			{
				const UnsignedInt ip = (UnsignedInt)(uintptr_t)GadgetListBoxGetItemData(s.listPlayers, i, 0);
				names.push_back(GadgetListBoxGetText(s.listPlayers, i, 0));
				files.push_back(filesMarkOfIP(ip));
				shields.push_back(shieldOfIP(ip));
			}
		}
		// one column, or two when the box is wide (variant B). O29 (item 5): more names than fit scroll (wheel, or a click
		// on the thin bar at the right), and the bar shows that there are more.
		const Bool twoColumns = w > sx(240);
		const Int columns = twoColumns ? 2 : 1;
		const Int perColumn = max(1, (h - 2 * pad) / rh);
		const Int fit = perColumn * columns;
		const Bool overflow = (Int)names.size() > fit;
		const Int colW = (w - 2 * pad - (overflow ? kBarW + 4 : 0)) / columns;
		const Int maxScroll = max(0, (Int)names.size() - fit);
		s.lobbyScroll = s.lobbyScroll > maxScroll ? maxScroll : (s.lobbyScroll < 0 ? 0 : s.lobbyScroll);
		for (Int k = 0; k < fit && s.lobbyScroll + k < (Int)names.size(); ++k)
		{
			const Int i = s.lobbyScroll + k;
			const Int c = k / perColumn;
			const Int cx = x + pad + c * colW;
			const Int ry = y + pad + (k % perColumn) * rh;
			drawText(names[i], cx + 2, ry + (rh - fontHeight()) / 2, colW - icon * 2 - 10, files[i] == 0 ? kYellow : kText);
			drawShield(cx + colW - icon * 2 - 6, ry + 2, icon, shields[i]);
			drawFilesMark(cx + colW - icon - 2, ry + 2, icon, files[i]);
		}
		drawScrollBar(x + w - kBarW - 3, y + pad, perColumn * rh, s.lobbyScroll, fit, (Int)names.size());
	}

	Int lobbyCount()
	{
		if (LanTest::sampleData()) {
			return (Int)ARRAY_SIZE(kSampleLobby);
		}
		return s.listPlayers != nullptr ? GadgetListBoxGetNumEntries(s.listPlayers) : 0;
	}

	WindowMsgHandledType inputLobbyList(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
	{
		switch (msg)
		{
			case GWM_WHEEL_UP: --s.lobbyScroll; return MSG_HANDLED;
			case GWM_WHEEL_DOWN: ++s.lobbyScroll; return MSG_HANDLED;
			case GWM_LEFT_DOWN: return MSG_HANDLED;
			case GWM_LEFT_UP:
			{
				Int x = 0, y = 0, w = 0, h = 0;
				window->winGetScreenPosition(&x, &y);
				window->winGetSize(&w, &h);
				const Int mx = (Int)LOLONGTOSHORT(mData1);
				const Int my = (Int)HILONGTOSHORT(mData1);
				const Int rh = rowHeight();
				const Int perColumn = max(1, (h - 8) / rh);
				const Int fit = perColumn * (w > sx(240) ? 2 : 1);
				if (lobbyCount() > fit && mx >= x + w - kBarW - 6) {
					s.lobbyScroll += (my < y + h / 2) ? -fit : fit;
				}
				return MSG_HANDLED;
			}
		}
		return MSG_IGNORED;
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

	/// The last addresses from Network.ini ("RemoteIP<n> = a.b.c.d[:name]", as the direct connect screen keeps them).
	/// O29: read here (getRemoteIPEntry steps past the end of an entry without ':'); with -lanSample and no entries,
	/// three sample addresses (shown only, never written).
	void loadHistory()
	{
		s.history.clear();
		LANPreferences prefs;
		const Int count = prefs.getNumRemoteIPs();
		for (Int i = 0; i < count && i < 10; ++i)
		{
			AsciiString key;
			key.format("RemoteIP%d", i);
			LANPreferences::const_iterator it = prefs.find(key.str());
			if (it == prefs.end()) {
				continue;
			}
			AsciiString rest = it->second;
			AsciiString ip;
			rest.nextToken(&ip, ":");
			UnicodeString entry;
			entry.translate(ip);
			if (rest.getLength() > 1)
			{
				entry.concat(L" (");
				entry.concat(QuotedPrintableToUnicodeString(AsciiString(rest.str() + 1)));
				entry.concat(L")");
			}
			if (parseAddress(entry) != 0) {
				s.history.push_back(entry);
			}
		}
		if (s.history.empty() && LanTest::sampleData())
		{
			for (Int i = 0; i < (Int)ARRAY_SIZE(kSampleHistory); ++i) {
				s.history.push_back(UnicodeString(kSampleHistory[i]));
			}
		}
	}

	/// The address goes first in Network.ini's RemoteIP list (the direct connect screen's list), without a copy of it.
	void rememberAddress(UnsignedInt ip)
	{
		LANPreferences prefs;
		std::vector<AsciiString> keep;
		AsciiString first;
		// "a.b.c.d:" (no name): the direct connect screen's reader expects the ':'
		first.format("%d.%d.%d.%d:", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
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

namespace
{
	/// O31: the chat line of an observer in a room goes to that room; FALSE when the line is empty.
	Bool sendRoomChat()
	{
		if (s.chatEntry == nullptr) {
			return FALSE;
		}
		UnicodeString text = GadgetTextEntryGetText(s.chatEntry);
		text.trim();
		if (text.isEmpty()) {
			return FALSE;
		}
		LanRoom::sendChat(text);
		GadgetTextEntrySetText(s.chatEntry, UnicodeString::TheEmptyString);
		return TRUE;
	}

	/// O29 (item 9): the screen point of row 'row' of a drawn list, for a -uiScript (clickrow). The row is scrolled into
	/// view first. Games table, lobby list and the open address list.
	Bool scriptRowPoint(GameWindow *window, Int row, Int *px, Int *py)
	{
		if (s.parent == nullptr || window == nullptr || row < 0) {
			return FALSE;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		const Int rh = rowHeight();
		if (window == s.gamesTable)
		{
			const TableGeo g = gamesGeo(window);
			if (row < s.scroll || row >= s.scroll + g.visible) {
				s.scroll = row;
			}
			*px = x + w / 4;
			*py = g.top + (row - s.scroll) * rh + rh / 2;
			return TRUE;
		}
		if (window == s.lobbyList)
		{
			const Int perColumn = max(1, (h - 8) / rh);
			const Int fit = perColumn * (w > sx(240) ? 2 : 1);
			if (row < s.lobbyScroll || row >= s.lobbyScroll + fit) {
				s.lobbyScroll = row;
			}
			const Int k = row - s.lobbyScroll;
			*px = x + 4 + (k / perColumn) * (w / 2) + 20;
			*py = y + 4 + (k % perColumn) * rh + rh / 2;
			return TRUE;
		}
		if (window == s.historyPopup && row < (Int)s.history.size())
		{
			*px = x + w / 3;
			*py = y + 2 + row * rh + rh / 2;
			return TRUE;
		}
		return FALSE;
	}
}

//-------------------------------------------------------------------------------------------------
UnicodeString LanLobbyLook::screenName()
{
	return textOf(kTitle);
}

void LanLobbyLook::init(GameWindow *parent)
{
	teardown();
	LanRoom::resetViewer();   // O31: back from following a stream: follow mode off, no mirror
	s.parent = parent;
	if (s.parent == nullptr) {
		return;
	}
	OptionPreferences pref;
	OptionPreferences::const_iterator it = pref.find("LanLobbyLayout");
	s.layout = (it != pref.end() && stricmp(it->second.str(), "B") == 0) ? &kLayoutB : &kLayoutA;
	it = pref.find("LanShowAntiCheat");
	// O30: the anti-cheat exists now: the shield column shows unless Options.ini says LanShowAntiCheat = no
	s.anticheat = it == pref.end() || stricmp(it->second.str(), "no") != 0;
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
	s.joinButton = findChild("ButtonJoin");
	s.font = screenFont(s.listGames != nullptr ? s.listGames->winGetFont() : (s.nameEntry != nullptr ? s.nameEntry->winGetFont() : nullptr));

	// the old unnamed labels: the title (the highest) stays and gets the new name, the others go. O29 (items 1, 2): they
	// are all inside LanLobbyMenu.wnd:GadgetParent, so every depth is searched (the first pass looked at the parent's
	// own children only and found none: the old title "LAN LOBBY" and the old labels stayed)
	const NameKeyType unnamed = TheNameKeyGenerator->nameToKey("LanLobbyMenu.wnd:");
	std::vector<GameWindow *> labels;
	collectUnnamedLabels(s.parent, (Int)unnamed, labels);
	Int titleY = 1000000;
	for (size_t i = 0; i < labels.size(); ++i)
	{
		GameWindow *c = labels[i];
		Int x = 0, y = 0;
		c->winGetScreenPosition(&x, &y);
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
	s.lobbyList = makeDrawn(L.lobbyList, drawLobbyList, inputLobbyList, "LanLobbyMenu.wnd:Look.Lobby");
	{
		const Rect popup = { L.connectEntry.x0, L.connectEntry.y1, L.historyButton.x1, L.connectEntry.y1 + 40 };
		s.historyPopup = makeDrawn(popup, drawHistory, inputHistory, "LanLobbyMenu.wnd:Look.HistoryList");
		if (s.historyPopup != nullptr) {
			s.historyPopup->winSetSystemFunc(systemHistory);
		}
	}

	FrameShots::setRowHook(&scriptRowPoint);   // O29 (item 9): clickrow / dclickrow of a -uiScript

	// O31: Observe (a room before its start) / Watch (a running game) and the watch delay
	{
		const Rect &a = L.panelActions;
		// O44: the button is as wide as its longest label needs ("Без зрителей", "Не наблюдать"), at least 40 % and at
		// most 60 % of the row; the delay slider takes the rest
		Int widest = 0;
		{
			const Text *labels[] = { &kObserve, &kStopObserving, &kWatch, &kWaiting, &kNoViewersButton };
			for (Int k = 0; k < (Int)ARRAY_SIZE(labels); ++k) {
				widest = max(widest, textWidth(textOf(*labels[k])));
			}
		}
		// the text is measured in screen pixels, the layout is in 800-wide design units
		const Int screen800 = max(1, sx(800));
		const Int widestDesign = (widest + 28) * 800 / screen800;
		const Int rowW = a.x1 - a.x0;
		const Int buttonW = min(rowW * 60 / 100, max(rowW * 40 / 100, widestDesign));
		const Int mid = a.x0 + buttonW;
		const Rect buttonRect = { a.x0, a.y0, mid - 4, a.y1 };
		s.actionButton = makeButton(buttonRect, textOf(kObserve), "LanLobbyMenu.wnd:Look.Observe");
		s.actionLabel = &kObserve;
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_HORZ_SLIDER | GWS_MOUSE_TRACK;
		inst.m_font = s.font;
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString("LanLobbyMenu.wnd:Look.WatchDelay"));
		SliderData data;
		memset(&data, 0, sizeof(data));
		data.minVal = 0;
		data.maxVal = 7;
		s.delaySlider = TheWindowManager->gogoGadgetSlider(s.parent, WIN_STATUS_ENABLED, sx(mid + 4), sy(a.y0), sx(a.x1) - sx(mid + 4),
			sy(a.y1) - sy(a.y0), &inst, &data, s.font, TRUE);
		if (s.delaySlider != nullptr)
		{
			s.delaySlider->winSetWindowId(inst.m_id);
			s.delaySlider->winSetOwner(s.parent);
			TickSlider::attach(s.delaySlider, s.font, FALSE);
			UnicodeString labels[8];
			for (Int i = 0; i < 8; ++i) {
				labels[i] = localizedFallback(kDelayEnglish[i], kDelayRussian[i]);
			}
			TickSlider::setLabels(s.delaySlider, labels, 8);
			s.made.push_back(s.delaySlider);
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
	// O40: "Play again" on the score screen: the room again, with the same layout (LanRoom::applyRematchLayout)
	if (TheLAN != nullptr && TheLAN->GetMyGame() == nullptr && !LanTest::sampleData() && LanRoom::takeRematchRequest()) {
		TheLAN->RequestGameCreate(UnicodeString::TheEmptyString, FALSE);
	}
	// O31: the room's notices in the chat; the action row follows the selected game
	{
		UnicodeString line;
		while (LanRoom::takeNotice(&line)) {
			if (s.chatList != nullptr) GadgetListBoxAddEntryText(s.chatList, line, chatSystemColor, -1, 0);
		}
		std::vector<GameRow> rows;
		collectRows(rows);
		const Int sel = selectedRow();
		const Bool have = sel >= 0 && sel < (Int)rows.size();
		const Bool running = have && rows[sel].inProgress;
		const LanRoom::State st = LanRoom::state();
		const Bool busy = st == LanRoom::STATE_STARTING || st == LanRoom::STATE_PLAYING;
		const UnsignedInt host = have && rows[sel].game != nullptr ? rows[sel].game->getHostIP() : 0;
		const Bool observingThis = host != 0 && LanRoom::observedHost() == host && (st == LanRoom::STATE_ASKED || st == LanRoom::STATE_IN_ROOM);
		// O33: not one's own room; a running game whose host forbids outside viewers cannot be watched
		const Bool ownRoom = host != 0 && TheLAN != nullptr && host == TheLAN->GetLocalIP();
		Int hostDelay = 60;
		Bool outsideAllowed = TRUE;
		LanRoom::viewerRulesOf(host, &hostDelay, &outsideAllowed);
		const Bool noViewers = running && !outsideAllowed;
		const Bool blocked = ownRoom || noViewers;
		if (s.actionButton != nullptr)
		{
			// O42: a running match whose host allows no outside viewers: the button says so, disabled
			const Text &label = busy ? kWaiting : (noViewers ? kNoViewersButton : (running ? kWatch : (observingThis ? kStopObserving : kObserve)));
			if (&label != s.actionLabel)
			{
				GadgetButtonSetText(s.actionButton, textOf(label));
				s.actionLabel = &label;
			}
			s.actionButton->winHide(!have || ownRoom);
			s.actionButton->winEnable(have && !busy && !blocked);
		}
		if (s.delaySlider != nullptr) {
			s.delaySlider->winHide(!running || busy || blocked);
		}
	}
	// O30: this player's own anti-cheat check failed: said once in the chat (others see the red mark)
	// O32: the line names the file, module or program, and comes again when the finding changes
	{
		UnicodeString own;
		if (s.chatList != nullptr && GuardLink::takeOwnFailureNotice(&own))
		{
			GadgetListBoxAddEntryText(s.chatList, own, chatSystemColor, -1, 0);
			s.ownFailureSaid = TRUE;
		}
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
		if (sel >= 0 && sel < (Int)rows.size())
		{
			t.format(L"%ls \x00AB%ls\x00BB   %ls   %ls", textOf(kGame).str(), rows[sel].name.str(), rows[sel].map.str(), rows[sel].speed.str());
			// O29: one line (a static text wraps at its width - 10)
			Int lw = 0, lh = 0;
			s.panelLabel->winGetSize(&lw, &lh);
			t = fitLine(t, lw - 14);
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
	// O31: leaving the LAN screen (main menu, or into a slot) ends observing a room; following a started stream goes on.
	// O33: a stream whose replay has not started yet is given up (it would start a replay over another screen).
	LanRoom::stopObserving();
	LanRoom::cancelStarting();
	teardown();
}

void LanLobbyLook::teardown()
{
	FrameShots::setRowHook(nullptr);
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
	s.joinButton = nullptr;
	s.lobbyScroll = 0;
	s.ownFailureSaid = FALSE;
	s.addrBox = s.copyButton = s.connectEntry = s.historyButton = s.connectButton = nullptr;
	s.gamesTable = s.panel = s.panelLabel = s.lobbyList = s.lobbyLabel = s.historyPopup = nullptr;
	s.actionButton = s.delaySlider = nullptr;
	s.actionLabel = nullptr;
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
			// O29: the address shown (with -lanSample the sample address, not the loopback one)
			const UnsignedInt ip = s.shownIP != 0 ? s.shownIP : (TheLAN != nullptr ? TheLAN->GetLocalIP() : 0);
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
		if (control == s.joinButton && refuseFull(selectedRow())) {
			return TRUE;   // O29 (item 9): the menu does not ask the host
		}
		if (control == s.actionButton)
		{
			// O31: observe a room before its start, stop it, or watch a running game with the chosen delay
			std::vector<GameRow> rows;
			collectRows(rows);
			const Int sel = selectedRow();
			if (sel >= 0 && sel < (Int)rows.size() && rows[sel].game != nullptr && !LanTest::sampleData())
			{
				LANGameInfo *game = rows[sel].game;
				const UnsignedInt host = game->getHostIP();
				const LanRoom::State st = LanRoom::state();
				if (rows[sel].inProgress)
				{
					const Int step = s.delaySlider != nullptr ? GadgetSliderGetPosition(s.delaySlider) : 0;
					LanRoom::startWatching(game, kDelays[step < 0 ? 0 : (step > 7 ? 7 : step)]);
				}
				else if (LanRoom::observedHost() == host && (st == LanRoom::STATE_ASKED || st == LanRoom::STATE_IN_ROOM))
				{
					LanRoom::stopObserving();
				}
				else
				{
					LanRoom::startObserving(host);
				}
			}
			return TRUE;
		}
		if (control == s.chatButton && LanRoom::state() == LanRoom::STATE_IN_ROOM && sendRoomChat()) {
			return TRUE;
		}
	}
	if (msg == GEM_EDIT_DONE && control == s.chatEntry && LanRoom::state() == LanRoom::STATE_IN_ROOM && sendRoomChat()) {
		return TRUE;   // O31: an observer's chat goes to its room (the host shows it to the room)
	}
	if (msg == GEM_EDIT_DONE && control == s.connectEntry)
	{
		connectTo();   // Enter in the address field
		return TRUE;
	}
	return FALSE;
}

// O31: a player's anti-cheat shield and files mark side by side (each 'size' wide), for the game room's observers list
void LanLobbyLook::drawMarks(Int x, Int y, Int size, UnsignedInt ip)
{
	drawShieldVerdict(x, y, size, shieldOfIP(ip));
	drawFilesMark(x + size + 2, y, size, filesMarkOfIP(ip));
}
