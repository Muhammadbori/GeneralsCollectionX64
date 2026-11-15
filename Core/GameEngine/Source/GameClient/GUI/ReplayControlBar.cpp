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

// ReplayControlBar.cpp /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See ReplayControlBar.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/ReplayControlBar.h"

#include "Common/FramePacer.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Recorder.h"
#include "Common/ReplayFollow.h"
#include "Common/ReplayStream.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LocalizedFallback.h"
#include "GameLogic/GameLogic.h"

namespace
{
	enum { SPEED_COUNT = 5 };
	const Int kSpeeds[SPEED_COUNT] = { 1, 2, 4, 8, ReplayControl::SPEED_MAX };
	const char *const kSpeedNames[SPEED_COUNT] = {
		"ReplayControlBar.wnd:ButtonSpeed1", "ReplayControlBar.wnd:ButtonSpeed2", "ReplayControlBar.wnd:ButtonSpeed4",
		"ReplayControlBar.wnd:ButtonSpeed8", "ReplayControlBar.wnd:ButtonSpeedMax" };

	Bool s_barDisabled = FALSE;
	Int s_factor = 1;                 ///< 1, 2, 4 or 8 (Max is GlobalData::m_TiVOFastMode)
	UnsignedInt s_goToFrame = 0;      ///< 0: no "go to" running
	Int s_speedBeforeGoTo = 1;
	Bool s_speedChangedInGoTo = FALSE;

	// what the bar shows now, to set its texts only when they change
	Int s_shownSpeed = -1;
	Int s_shownPaused = -1;
	UnsignedInt s_shownSeconds = 0xFFFFFFFF;
	UnsignedInt s_shownTotal = 0xFFFFFFFF;
	Int s_shownDelay = -2;
	Int s_shownWidth = 0;
	Int s_shownHeight = 0;
	Int s_startSpeed = -1;            ///< -replaySpeed; -1: none
	Bool s_startSpeedApplied = FALSE;
	Bool s_barHasHours = FALSE;   ///< the time box was sized for h:mm:ss

	NameKeyType idOf(const char *name)
	{
		return TheNameKeyGenerator->nameToKey(AsciiString(name));
	}

	GameWindow *find(const char *name)
	{
		return TheWindowManager != nullptr ? TheWindowManager->winGetWindowFromId(nullptr, idOf(name)) : nullptr;
	}

	UnicodeString text(const char *label, const WideChar *english, const WideChar *russian)
	{
		return TheGameText->FETCH_OR_SUBSTITUTE(label, localizedFallback(english, russian));
	}

	UnicodeString pauseText()
	{
		return text("GUI:ReplayBarPause", L"Pause", L"\x041F\x0430\x0443\x0437\x0430");
	}

	UnicodeString playText()
	{
		return text("GUI:ReplayBarPlay", L"Play", L"\x041F\x0443\x0441\x043A");
	}

	UnicodeString speedText(Int speed)
	{
		if (speed == ReplayControl::SPEED_MAX) {
			return text("GUI:ReplayBarMax", L"Max", L"\x041C\x0430\x043A\x0441");
		}
		UnicodeString s;
		s.format(L"%dx", speed);
		return s;
	}

	Int measure(GameFont *font, const UnicodeString &str, Int *height = nullptr)
	{
		Int w = (Int)str.getLength() * 7, h = 12;
		if (font != nullptr && TheDisplayStringManager != nullptr)
		{
			DisplayString *ds = TheDisplayStringManager->newDisplayString();
			if (ds != nullptr)
			{
				ds->setFont(font);
				ds->setText(str);
				ds->getSize(&w, &h);
				TheDisplayStringManager->freeDisplayString(ds);
			}
		}
		if (height != nullptr) {
			*height = h;
		}
		return w;
	}

	Bool following()
	{
		return ReplayFollow::isOn() && ReplayFollow::writerAlive();
	}

	/// The replay's length in logic frames: the writer's newest frame while following, else the header's count.
	UnsignedInt totalFrames()
	{
		if (following()) {
			return ReplayFollow::newestFrame();
		}
		return TheRecorder != nullptr ? TheRecorder->getPlaybackFrameCount() : 0;
	}

	Bool barWanted()
	{
		return !s_barDisabled && TheGameLogic != nullptr && TheGameLogic->isInReplayGame() && TheGameLogic->isInGame()
			&& !TheGameLogic->isLoadingMap() && TheRecorder != nullptr && TheRecorder->isPlaybackMode()
			&& TheWindowManager != nullptr && TheDisplay != nullptr;
	}

	//--- look ---------------------------------------------------------------------------------------
	const Color kText = GameMakeColor(255, 255, 255, 255);
	const Color kTextBorder = GameMakeColor(0, 0, 0, 255);
	const Color kFill = GameMakeColor(24, 28, 36, 200);
	const Color kButton = GameMakeColor(48, 56, 72, 230);
	const Color kButtonBorder = GameMakeColor(120, 130, 150, 255);
	const Color kButtonHilite = GameMakeColor(80, 92, 118, 240);
	const Color kChosen = GameMakeColor(150, 112, 30, 240);
	const Color kChosenBorder = GameMakeColor(255, 210, 90, 255);

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

	GameWindow *makeButton(GameWindow *parent, GameFont *font, Int x, Int y, Int w, Int h, const UnicodeString &str,
		const char *name, const UnicodeString &tip)
	{
		WinInstanceData inst;
		plainLook(inst, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK, font, kButton, kButtonBorder, kButtonHilite);
		inst.m_id = idOf(name);
		GameWindow *win = TheWindowManager->gogoGadgetPushButton(parent, WIN_STATUS_ENABLED, x, y, w, h, &inst, font, FALSE);
		if (win != nullptr)
		{
			win->winSetWindowId(inst.m_id);
			GadgetButtonSetText(win, str);
			if (!tip.isEmpty()) {
				win->winSetTooltip(tip);
			}
		}
		return win;
	}

	WindowMsgHandledType barInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		// the bar's own background takes the mouse, so a click on it does not reach the battlefield
		switch (msg)
		{
		case GWM_LEFT_DOWN: case GWM_LEFT_UP: case GWM_LEFT_DOUBLE_CLICK: case GWM_LEFT_DRAG:
		case GWM_MIDDLE_DOWN: case GWM_MIDDLE_UP: case GWM_MIDDLE_DOUBLE_CLICK: case GWM_MIDDLE_DRAG:
		case GWM_RIGHT_DOWN: case GWM_RIGHT_UP: case GWM_RIGHT_DOUBLE_CLICK: case GWM_RIGHT_DRAG:
			return MSG_HANDLED;
		default:
			return MSG_IGNORED;
		}
	}

	void goToEntryTime()
	{
		GameWindow *entry = find("ReplayControlBar.wnd:EntryTime");
		if (entry == nullptr) {
			return;
		}
		UnsignedInt frame = 0;
		if (!ReplayControl::parseTime(GadgetTextEntryGetText(entry), &frame))
		{
			TheInGameUI->message(text("GUI:ReplayBarTimeFormat", L"Enter a time as mm:ss",
				L"\x0412\x0432\x0435\x0434\x0438\x0442\x0435 \x0432\x0440\x0435\x043C\x044F \x043A\x0430\x043A \x043C\x043C:\x0441\x0441"));
			return;
		}
		ReplayControl::goToFrame(frame);
		TheWindowManager->winSetFocus(nullptr);
	}

	WindowMsgHandledType barSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		switch (msg)
		{
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			const Int id = control != nullptr ? control->winGetWindowId() : 0;
			if (id == idOf("ReplayControlBar.wnd:ButtonPause"))
			{
				TheMessageStream->appendMessage(GameMessage::MSG_META_TOGGLE_PAUSE);
			}
			else if (id == idOf("ReplayControlBar.wnd:ButtonStep"))
			{
				TheMessageStream->appendMessage(GameMessage::MSG_META_STEP_FRAME);
			}
			else if (id == idOf("ReplayControlBar.wnd:ButtonGoTo"))
			{
				goToEntryTime();
			}
			else
			{
				for (Int i = 0; i < SPEED_COUNT; ++i)
				{
					if (id == idOf(kSpeedNames[i]))
					{
						ReplayControl::setSpeed(kSpeeds[i]);
						TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ReplayBarSpeed",
							localizedFallback(L"Replay speed %ls",
								L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x0432\x043E\x0441\x043F\x0440\x043E\x0438\x0437\x0432\x0435\x0434\x0435\x043D\x0438\x044F %ls"),
							speedText(kSpeeds[i]).str()));
						break;
					}
				}
			}
			return MSG_HANDLED;
		}
		case GEM_EDIT_DONE:
			goToEntryTime();
			return MSG_HANDLED;
		default:
			return MSG_IGNORED;
		}
	}

	UnicodeString timeLine(UnsignedInt frame, UnsignedInt total, Int delaySeconds)
	{
		UnicodeString line = ReplayControl::timeText(frame);
		if (total > 0)
		{
			line.concat(L" / ");
			line.concat(ReplayControl::timeText(total));
		}
		if (delaySeconds >= 0)
		{
			line.concat(L"   ");
			line.concat(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ReplayBarBehind",
				localizedFallback(L"delay %ls", L"\x0437\x0430\x0434\x0435\x0440\x0436\x043A\x0430 %ls"),
				ReplayControl::timeText((UnsignedInt)delaySeconds * LOGICFRAMES_PER_SECOND).str()));
		}
		return line;
	}

	void destroyBar()
	{
		GameWindow *bar = find("ReplayControlBar.wnd:Bar");
		if (bar != nullptr) {
			TheWindowManager->winDestroy(bar);
		}
		s_shownSpeed = -1;
		s_shownPaused = -1;
		s_shownSeconds = 0xFFFFFFFF;
		s_shownTotal = 0xFFFFFFFF;
		s_shownDelay = -2;
	}

	/// Builds the bar for the current screen size: the buttons sized from their texts, the time box from its widest
	/// text, all in one row at the top centre.
	void createBar()
	{
		const Int screenW = TheDisplay->getWidth();
		const Int screenH = TheDisplay->getHeight();
		const Real scale = min((Real)screenW / 800.0f, (Real)screenH / 600.0f);
		GameFont *font = TheFontLibrary != nullptr
			? TheFontLibrary->getFont(AsciiString("Arial"), TheGlobalLanguageData != nullptr ? TheGlobalLanguageData->adjustFontSize(12) : 12, TRUE)
			: nullptr;

		Int textH = 12;
		measure(font, UnicodeString(L"0:00"), &textH);
		Int pad = max(4, REAL_TO_INT(6.0f * scale));
		Int gap = max(2, REAL_TO_INT(3.0f * scale));
		Int groupGap = max(6, REAL_TO_INT(10.0f * scale));
		const Int rowH = textH + pad;
		const Int margin = max(2, REAL_TO_INT(3.0f * scale));

		const Int pauseTextW = max(measure(font, pauseText()), measure(font, playText()));
		const UnicodeString stepStr = text("GUI:ReplayBarStep", L"Step", L"\x0428\x0430\x0433");
		const Int stepTextW = measure(font, stepStr);
		Int speedTextW[SPEED_COUNT];
		for (Int i = 0; i < SPEED_COUNT; ++i)
			speedTextW[i] = measure(font, speedText(kSpeeds[i]));
		const Bool follow = ReplayFollow::isOn();
		// TheSuperHackers @tweak (O18, D4) The time box is as wide as the times need: m:ss up to an hour (the bar is made
		// again when a time passes an hour), so the bar stays narrow at 800x600.
		const UnsignedInt hourFrames = 3600 * LOGICFRAMES_PER_SECOND;
		s_barHasHours = totalFrames() >= hourFrames || TheGameLogic->getFrame() >= hourFrames;
		const UnsignedInt widest = s_barHasHours ? 10 * hourFrames - 1 : hourFrames - 1;
		const Int timeTextW = measure(font, timeLine(widest, widest, follow ? 3599 : -1));
		const Int entryTextW = measure(font, UnicodeString(L"0:00:00"));
		const UnicodeString goStr = text("GUI:ReplayBarGoTo", L"Go to", L"\x041F\x0435\x0440\x0435\x0439\x0442\x0438");
		const Int goTextW = measure(font, goStr);

		// TheSuperHackers @bugfix (O18, D4) The top left belongs to the game's messages (the replay hints, the speed
		// messages): the bar stays right of the left 30 % of the screen, centred when it fits there.
		// TheSuperHackers @bugfix (O18, S18) ... and right of the widest of those messages as the message font draws it
		// (at 800x600 "Press F to toggle Fast Forward" is wider than 30 %). When the bar does not fit beside it, its
		// paddings and gaps shrink.
		Int keepLeft = screenW * 3 / 10;
		if (TheInGameUI != nullptr && TheFontLibrary != nullptr && TheGlobalLanguageData != nullptr)
		{
			GameFont *messageFont = TheFontLibrary->getFont(TheInGameUI->getMessageFontName(),
				TheGlobalLanguageData->adjustFontSize(TheInGameUI->getMessagePointSize()), TheInGameUI->isMessageBold());
			Int hintW = measure(messageFont, FastForwardText::instructions());
			hintW = max(hintW, measure(messageFont, FastForwardText::on()));
			hintW = max(hintW, measure(messageFont, FastForwardText::off()));
			UnicodeString speedMessage;
			speedMessage.format(text("GUI:ReplayBarSpeed", L"Replay speed %ls",
				L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x0432\x043E\x0441\x043F\x0440\x043E\x0438\x0437\x0432\x0435\x0434\x0435\x043D\x0438\x044F %ls").str(),
				speedText(ReplayControl::SPEED_MAX).str());
			hintW = max(hintW, measure(messageFont, speedMessage));
			keepLeft = max(keepLeft, TheInGameUI->getMessagePosition().x + hintW + groupGap);
		}

		Int pauseW = 0, stepW = 0, speedsW = 0, timeW = 0, entryW = 0, goW = 0, innerW = 0;
		Int speedW[SPEED_COUNT];
		for (Int attempt = 0; attempt < 2; ++attempt)
		{
			if (attempt == 1)
			{
				if (innerW + 2 * margin <= screenW - keepLeft - margin)
					break;
				pad = max(2, pad / 2);
				gap = max(1, gap / 2);
				groupGap = max(3, groupGap / 2);
			}
			pauseW = pauseTextW + 2 * pad;
			stepW = stepTextW + 2 * pad;
			speedsW = 0;
			for (Int i = 0; i < SPEED_COUNT; ++i)
			{
				speedW[i] = max(speedTextW[i] + 2 * pad, rowH + pad);
				speedsW += speedW[i] + (i > 0 ? gap : 0);
			}
			timeW = timeTextW + 2 * pad;
			entryW = entryTextW + 2 * pad;
			goW = goTextW + 2 * pad;
			innerW = pauseW + gap + stepW + groupGap + speedsW + groupGap + timeW + groupGap + entryW + gap + goW;
		}
		const Int barW = min(innerW + 2 * margin, screenW);
		const Int barH = rowH + 2 * margin;
		Int barX = (screenW - barW) / 2;
		if (barX < keepLeft) {
			barX = max(0, min(keepLeft, screenW - barW - margin));
		}
		const Int barY = max(2, REAL_TO_INT(4.0f * scale));

		WinInstanceData inst;
		plainLook(inst, GWS_USER_WINDOW, font, kFill, kButtonBorder, kFill);
		inst.m_id = idOf("ReplayControlBar.wnd:Bar");
		GameWindow *bar = TheWindowManager->winCreate(nullptr, WIN_STATUS_ENABLED, barX, barY, barW, barH, barSystem, &inst);
		if (bar == nullptr) {
			return;
		}
		bar->winSetWindowId(inst.m_id);
		bar->winSetInputFunc(barInput);
		bar->winSetFont(font);

		Int x = margin;
		const Int y = margin;
		makeButton(bar, font, x, y, pauseW, rowH, pauseText(), "ReplayControlBar.wnd:ButtonPause",
			text("GUI:ReplayBarPauseTip", L"Pause or resume the replay", L"\x041F\x0430\x0443\x0437\x0430 \x0438\x043B\x0438 \x043F\x0440\x043E\x0434\x043E\x043B\x0436\x0435\x043D\x0438\x0435"));
		x += pauseW + gap;
		makeButton(bar, font, x, y, stepW, rowH, stepStr, "ReplayControlBar.wnd:ButtonStep",
			text("GUI:ReplayBarStepTip", L"One logic frame, then pause", L"\x041E\x0434\x0438\x043D \x043A\x0430\x0434\x0440 \x043B\x043E\x0433\x0438\x043A\x0438, \x0437\x0430\x0442\x0435\x043C \x043F\x0430\x0443\x0437\x0430"));
		x += stepW + groupGap;
		for (Int i = 0; i < SPEED_COUNT; ++i)
		{
			makeButton(bar, font, x, y, speedW[i], rowH, speedText(kSpeeds[i]), kSpeedNames[i], UnicodeString::TheEmptyString);
			x += speedW[i] + gap;
		}
		x += groupGap - gap;

		{
			WinInstanceData textInst;
			plainLook(textInst, GWS_STATIC_TEXT, font, GameMakeColor(0, 0, 0, 0), GameMakeColor(0, 0, 0, 0), GameMakeColor(0, 0, 0, 0));
			textInst.m_id = idOf("ReplayControlBar.wnd:TextTime");
			TextData textData;
			memset(&textData, 0, sizeof(textData));
			textData.centered = TRUE;
			textData.centeredVertically = TRUE;
			GameWindow *timeWin = TheWindowManager->gogoGadgetStaticText(bar, WIN_STATUS_ENABLED, x, y, timeW, rowH, &textInst, &textData, font, FALSE);
			if (timeWin != nullptr) {
				timeWin->winSetWindowId(textInst.m_id);
			}
		}
		x += timeW + groupGap;

		{
			WinInstanceData entryInst;
			plainLook(entryInst, GWS_ENTRY_FIELD | GWS_MOUSE_TRACK, font, GameMakeColor(8, 10, 14, 240), kButtonBorder, GameMakeColor(8, 10, 14, 240));
			entryInst.m_id = idOf("ReplayControlBar.wnd:EntryTime");
			EntryData entryData;
			memset(&entryData, 0, sizeof(entryData));
			entryData.maxTextLen = 8;
			entryData.aSCIIOnly = TRUE;
			GameWindow *entry = TheWindowManager->gogoGadgetTextEntry(bar, WIN_STATUS_ENABLED, x, y, entryW, rowH, &entryInst, &entryData, font, FALSE);
			if (entry != nullptr)
			{
				entry->winSetWindowId(entryInst.m_id);
				entry->winSetTooltip(text("GUI:ReplayBarEntryTip", L"The time to go to, mm:ss",
					L"\x0412\x0440\x0435\x043C\x044F \x043F\x0435\x0440\x0435\x0445\x043E\x0434\x0430, \x043C\x043C:\x0441\x0441"));
			}
		}
		x += entryW + gap;
		makeButton(bar, font, x, y, goW, rowH, goStr, "ReplayControlBar.wnd:ButtonGoTo",
			text("GUI:ReplayBarGoToTip", L"Run to that time and pause", L"\x0414\x043E\x0439\x0442\x0438 \x0434\x043E \x044D\x0442\x043E\x0433\x043E \x0432\x0440\x0435\x043C\x0435\x043D\x0438 \x0438 \x043F\x0430\x0443\x0437\x0430"));

		s_shownWidth = screenW;
		s_shownHeight = screenH;
	}

	void refreshBar()
	{
		const Int speed = ReplayControl::speed();
		if (speed != s_shownSpeed)
		{
			s_shownSpeed = speed;
			for (Int i = 0; i < SPEED_COUNT; ++i)
			{
				GameWindow *button = find(kSpeedNames[i]);
				if (button == nullptr) {
					continue;
				}
				const Bool chosen = kSpeeds[i] == speed;
				for (Int k = 0; k < MAX_DRAW_DATA; ++k)
				{
					button->winSetEnabledColor(k, chosen ? kChosen : kButton);
					button->winSetEnabledBorderColor(k, chosen ? kChosenBorder : kButtonBorder);
					button->winSetHiliteColor(k, chosen ? kChosen : kButtonHilite);
				}
			}
		}

		const Int paused = TheGameLogic->isGamePaused() ? 1 : 0;
		if (paused != s_shownPaused)
		{
			s_shownPaused = paused;
			GameWindow *button = find("ReplayControlBar.wnd:ButtonPause");
			if (button != nullptr) {
				GadgetButtonSetText(button, paused ? playText() : pauseText());
			}
		}

		const UnsignedInt frame = TheGameLogic->getFrame();
		const UnsignedInt seconds = frame / LOGICFRAMES_PER_SECOND;
		const UnsignedInt total = totalFrames();
		Int delay = -1;
		if (following())
		{
			const UnsignedInt newest = ReplayFollow::newestFrame();
			delay = newest > frame ? (Int)((newest - frame) / LOGICFRAMES_PER_SECOND) : 0;
		}
		if (!s_barHasHours && (total >= 3600u * LOGICFRAMES_PER_SECOND || frame >= 3600u * LOGICFRAMES_PER_SECOND))
		{
			destroyBar();   // made again, wider, at the next update
			return;
		}
		if (seconds != s_shownSeconds || total / LOGICFRAMES_PER_SECOND != s_shownTotal || delay != s_shownDelay)
		{
			s_shownSeconds = seconds;
			s_shownTotal = total / LOGICFRAMES_PER_SECOND;
			s_shownDelay = delay;
			GameWindow *timeWin = find("ReplayControlBar.wnd:TextTime");
			if (timeWin != nullptr) {
				GadgetStaticTextSetText(timeWin, timeLine(frame, total, delay));
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void ReplayControl::disableBar()
{
	s_barDisabled = TRUE;
}

void ReplayControl::configureStartSpeed(Int speed)
{
	s_startSpeed = speed;
}

Int ReplayControl::speed()
{
	if (TheGlobalData != nullptr && TheGlobalData->m_TiVOFastMode) {
		return SPEED_MAX;
	}
	return s_factor;
}

void ReplayControl::setSpeed(Int newSpeed)
{
	if (newSpeed != SPEED_MAX && newSpeed != 1 && newSpeed != 2 && newSpeed != 4 && newSpeed != 8) {
		newSpeed = 1;
	}
	if (s_goToFrame != 0) {
		s_speedChangedInGoTo = TRUE;
	}
	s_factor = newSpeed == SPEED_MAX ? 1 : newSpeed;
	if (TheWritableGlobalData != nullptr) {
		TheWritableGlobalData->m_TiVOFastMode = newSpeed == SPEED_MAX;
	}
	if (TheFramePacer != nullptr) {
		TheFramePacer->setReplaySpeedFactor(s_factor);
	}
}

void ReplayControl::toggleFastForward()
{
	setSpeed(speed() == 1 ? SPEED_MAX : 1);
	TheInGameUI->messageNoFormat(TheGlobalData->m_TiVOFastMode
		? FastForwardText::on()
		: FastForwardText::off());
}

Bool ReplayControl::goToFrame(UnsignedInt frame)
{
	if (TheGameLogic == nullptr || !TheGameLogic->isInReplayGame()) {
		return FALSE;
	}
	const UnsignedInt now = TheGameLogic->getFrame();
	const UnsignedInt total = following() ? 0 : totalFrames();
	if (total > 0 && frame > total) {
		frame = total;
	}
	if (frame <= now)
	{
		TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ReplayBarNoBack",
			localizedFallback(L"A replay cannot go back (now %ls)",
				L"\x041D\x0430\x0437\x0430\x0434 \x043F\x0435\x0440\x0435\x0439\x0442\x0438 \x043D\x0435\x043B\x044C\x0437\x044F (\x0441\x0435\x0439\x0447\x0430\x0441 %ls)"),
			timeText(now).str()));
		return FALSE;
	}
	if (s_goToFrame == 0) {
		s_speedBeforeGoTo = speed();
	}
	setSpeed(SPEED_MAX);
	s_goToFrame = frame;
	s_speedChangedInGoTo = FALSE;
	if (TheGameLogic->isGamePaused()) {
		TheGameLogic->setGamePaused(FALSE);
	}
	TheGameLogic->setGamePausedInFrame(frame, FALSE);
	TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ReplayBarGoing",
		localizedFallback(L"Going to %ls", L"\x041F\x0435\x0440\x0435\x0445\x043E\x0434 \x043A %ls"), timeText(frame).str()));
	return TRUE;
}

Bool ReplayControl::parseTime(const UnicodeString &str, UnsignedInt *frame)
{
	Int parts[3] = { 0, 0, 0 };
	Int count = 0;
	Bool digit = FALSE;
	for (const WideChar *p = str.str(); *p != 0; ++p)
	{
		if (*p >= L'0' && *p <= L'9')
		{
			if (count == 0) {
				count = 1;
			}
			parts[count - 1] = parts[count - 1] * 10 + (*p - L'0');
			if (parts[count - 1] > 100000) {
				return FALSE;
			}
			digit = TRUE;
		}
		else if (*p == L':' || *p == L'.')
		{
			if (count == 0 || count == 3) {
				return FALSE;
			}
			++count;
		}
		else if (*p != L' ')
		{
			return FALSE;
		}
	}
	if (!digit || count == 0) {
		return FALSE;
	}
	Int seconds = 0;
	if (count == 1) {
		seconds = parts[0] * 60;                                   // "m"
	} else if (count == 2) {
		seconds = parts[0] * 60 + parts[1];                        // "mm:ss"
	} else {
		seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];      // "h:mm:ss"
	}
	*frame = (UnsignedInt)seconds * LOGICFRAMES_PER_SECOND;
	return TRUE;
}

UnicodeString ReplayControl::timeText(UnsignedInt frame)
{
	const UnsignedInt seconds = frame / LOGICFRAMES_PER_SECOND;
	UnicodeString s;
	if (seconds >= 3600) {
		s.format(L"%u:%02u:%02u", seconds / 3600, (seconds / 60) % 60, seconds % 60);
	} else {
		s.format(L"%u:%02u", seconds / 60, seconds % 60);
	}
	return s;
}

void ReplayControl::update()
{
	const Bool inReplay = TheGameLogic != nullptr && TheGameLogic->isInReplayGame();

	// TheSuperHackers @feature (S16) A watched game whose stream is lost: the watcher is told once.
	if (ReplayStream::takeLostNotice() && TheInGameUI != nullptr)
	{
		TheInGameUI->message(text("GUI:ReplayStreamLost", L"The game's stream was lost: the replay plays what it has to the end.",
			L"\x0422\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x044F \x0438\x0433\x0440\x044B \x043F\x043E\x0442\x0435\x0440\x044F\x043D\x0430: \x0437\x0430\x043F\x0438\x0441\x044C \x043F\x0440\x043E\x0438\x0433\x0440\x044B\x0432\x0430\x0435\x0442\x0441\x044F \x0434\x043E \x043A\x043E\x043D\x0446\x0430 \x0442\x043E\x0433\x043E, \x0447\x0442\x043E \x043F\x043E\x043B\x0443\x0447\x0435\x043D\x043E."));
	}

	// TheSuperHackers @bugfix (O18, S18) -replaySpeed waits until the replay's map is loaded: the replay mode is set one
	// engine update before GameLogic::startNewGame runs, and that turns the fast forward (Max) off again; 2x..8x lived
	// through it only because their factor is not reset there.
	if (inReplay && s_startSpeed >= 0 && !s_startSpeedApplied && !TheGameLogic->isLoadingMap())
	{
		s_startSpeedApplied = TRUE;
		setSpeed(s_startSpeed);
	}
	if (!inReplay)
	{
		// nothing of the bar acts outside replays
		if (s_factor != 1 || s_goToFrame != 0)
		{
			s_factor = 1;
			s_goToFrame = 0;
			if (TheFramePacer != nullptr) {
				TheFramePacer->setReplaySpeedFactor(1);
			}
		}
	}
	else if (s_goToFrame != 0 && TheGameLogic->getFrame() >= s_goToFrame)
	{
		// arrived (the logic paused itself there): back to the speed before, unless it was changed meanwhile
		s_goToFrame = 0;
		if (!s_speedChangedInGoTo) {
			setSpeed(s_speedBeforeGoTo);
		}
	}

	if (!barWanted())
	{
		if (TheWindowManager != nullptr && find("ReplayControlBar.wnd:Bar") != nullptr) {
			destroyBar();
		}
		return;
	}

	GameWindow *bar = find("ReplayControlBar.wnd:Bar");
	if (bar != nullptr && (TheDisplay->getWidth() != s_shownWidth || TheDisplay->getHeight() != s_shownHeight))
	{
		destroyBar();
		bar = nullptr;
	}
	if (bar == nullptr)
	{
		destroyBar();
		createBar();
	}
	refreshBar();
}
