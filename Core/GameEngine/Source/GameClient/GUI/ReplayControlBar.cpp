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

#include "GameClient/OwnUiScale.h"
#include "GameClient/ReplayControlBar.h"

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
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
#include "GameClient/GameFont.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/LookPalette.h"   // O94
#include "GameLogic/GameLogic.h"
#include "GameNetwork/LanRoom.h"
#include "Common/FrameShots.h"

namespace
{
	enum { CYCLE_COUNT = 4 };
	const Int kCycle[CYCLE_COUNT] = { 1, 2, 4, 8 };   ///< O61: the one speed button goes round these
	const UnsignedInt kNone = 0xFFFFFFFF;
	const UnsignedInt kLiveSlack = 2 * LOGICFRAMES_PER_SECOND;   ///< this close to the live point is live
	const UnsignedInt kRewindGiveUpMs = 120000;                  ///< a restart that never plays is given up

	enum SeekKind { SEEK_NONE, SEEK_RUN, SEEK_REWIND, SEEK_LIVE };

	Bool s_barDisabled = FALSE;
	Int s_factor = 1;                 ///< 1, 2, 4 or 8 (Max is GlobalData::m_TiVOFastMode)
	Int s_startSpeed = -1;            ///< -replaySpeed; -1: none
	Bool s_startSpeedApplied = FALSE;

	// O61: the seek that runs
	SeekKind s_seekKind = SEEK_NONE;
	UnsignedInt s_seekTarget = 0;          ///< the frame a run goes to
	Int s_speedBeforeSeek = 1;
	Bool s_pauseAtTarget = FALSE;          ///< paused before the seek (or goToFrame): stays paused there
	Bool s_catchUp = FALSE;                ///< row 49: the first run of a followed replay to its live point
	UnsignedInt s_rewindRequest = kNone;   ///< a backward seek waits for pollRewind
	UnsignedInt s_rewindTarget = kNone;    ///< the restarted replay runs to this frame once its map is loaded
	UnsignedInt s_rewindSinceMs = 0;
	Bool s_reachedLive = FALSE;            ///< row 49: this followed replay was at its live point once
	UnsignedInt s_runLastFrame = 0;        ///< O64 (H6 #6): a run that stops moving is ended
	UnsignedInt s_runLastNewest = 0;       ///< O65: ... only while no new data comes and the game is not paused
	UnsignedInt s_runLastMoveMs = 0;
	const UnsignedInt kRunStallMs = 30000;
	const UnsignedInt kRestartFailMs = 60000;   ///< O64 (H6 #7): a restarted replay whose logic never runs
	UnsignedInt s_rewindLoadedMs = 0;           ///< O65: when the restarted replay's map load ended (its clock)

	// what the panel shows now, to set its texts only when they change
	Int s_shownSpeed = -2;
	Int s_shownPaused = -1;
	UnsignedInt s_shownSeconds = kNone;
	UnsignedInt s_shownTotal = kNone;
	Int s_shownDelay = -3;
	Int s_shownSeek = -1;
	Int s_shownWidth = 0;
	Int s_shownHeight = 0;
	Bool s_barHasHours = FALSE;   ///< the time button was sized for h:mm:ss
	Bool s_barFollow = FALSE;     ///< the panel was made with the delay button
	Int s_barHostDelay = 0;       ///< ... and with this host delay line (seconds; 0: none)

	// O61: the seek line under the panel
	Bool s_seekOpen = FALSE;
	Bool s_seekHeld = FALSE;
	UnsignedInt s_seekDrag = 0;
	DisplayString *s_seekValue = nullptr;
	GameFont *s_font = nullptr;
	Int s_textH = 12;

	const char *const kBar = "ReplayControlBar.wnd:Bar";
	const char *const kButtonPause = "ReplayControlBar.wnd:ButtonPause";
	const char *const kButtonSpeed = "ReplayControlBar.wnd:ButtonSpeed";
	const char *const kButtonTime = "ReplayControlBar.wnd:ButtonTime";
	const char *const kButtonDelay = "ReplayControlBar.wnd:ButtonDelay";
	const char *const kTextHostDelay = "ReplayControlBar.wnd:TextHostDelay";
	const char *const kSeekLine = "ReplayControlBar.wnd:SeekLine";
	const char *const kTextSeek = "ReplayControlBar.wnd:TextSeek";

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

	UnicodeString liveText()
	{
		return text("GUI:ReplayBarLive", L"LIVE", L"\x042D\x0424\x0418\x0420");
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

	/// O61 (point 4): the host's delay of an outside viewer in words: "1 \x043C\x0438\x043D", "45 \x0441", "1 \x043C\x0438\x043D 30 \x0441".
	UnicodeString hostDelayText(Int seconds)
	{
		UnicodeString amount;
		if (seconds < 60) {
			amount.format(text("GUI:ReplayBarSeconds", L"%d s", L"%d \x0441").str(), seconds);
		} else if (seconds % 60 == 0) {
			amount.format(text("GUI:ReplayBarMinutes", L"%d min", L"%d \x043C\x0438\x043D").str(), seconds / 60);
		} else {
			amount.format(text("GUI:ReplayBarMinutesSeconds", L"%d min %d s", L"%d \x043C\x0438\x043D %d \x0441").str(), seconds / 60, seconds % 60);
		}
		UnicodeString line;
		line.format(text("GUI:ReplayBarHostDelay", L"Host delay: %ls", L"\x0417\x0430\x0434\x0435\x0440\x0436\x043A\x0430 \x0445\x043E\x0441\x0442\x0430: %ls").str(), amount.str());
		return line;
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

	/// The end of the time line in logic frames: while following, the live point (the newest frame this viewer may play,
	/// O61); a followed stream that ended: its last frame; else the header's count.
	UnsignedInt totalFrames()
	{
		if (ReplayFollow::isOn())
		{
			if (following()) {
				return ReplayFollow::livePointFrame();
			}
			const UnsignedInt newest = ReplayFollow::newestFrame();
			if (newest > 0) {
				return newest;
			}
		}
		return TheRecorder != nullptr ? TheRecorder->getPlaybackFrameCount() : 0;
	}

	Bool barWanted()
	{
		return !s_barDisabled && TheGameLogic != nullptr && TheGameLogic->isInReplayGame() && TheGameLogic->isInGame()
			&& !TheGameLogic->isLoadingMap() && TheRecorder != nullptr && TheRecorder->isPlaybackMode()
			&& TheWindowManager != nullptr && TheDisplay != nullptr;
	}

	Bool seekBusy()
	{
		return s_seekKind != SEEK_NONE || s_rewindRequest != kNone || s_rewindTarget != kNone;
	}

	/// O68: while a -uiScript plays, each seek's start and end with the play state, in <user data>ReplayBar.log
	void seekLog(const char *format, ...)
	{
		if (!FrameShots::isUiScriptRunning() || TheGlobalData == nullptr) {
			return;
		}
		char path[_MAX_PATH];
		snprintf(path, sizeof(path), "%sReplayBar.log", TheGlobalData->getPath_UserData().str());
		FILE *f = fopen(path, "a");
		if (f == nullptr) {
			return;
		}
		fprintf(f, "[%10u] ", (unsigned)timeGetTime());
		va_list args;
		va_start(args, format);
		vfprintf(f, format, args);
		va_end(args);
		fputc('\n', f);
		fclose(f);
	}

	//--- seeking ------------------------------------------------------------------------------------
	/// Runs at Max to 'frame' (the logic pauses itself there; finishSeek decides what comes after).
	void beginRun(UnsignedInt frame)
	{
		ReplayControl::setSpeed(ReplayControl::SPEED_MAX);
		s_seekTarget = frame;
		s_runLastFrame = TheGameLogic->getFrame();
		s_runLastNewest = ReplayFollow::newestFrame();
		s_runLastMoveMs = timeGetTime();
		if (TheGameLogic->isGamePaused()) {
			TheGameLogic->setGamePaused(FALSE);
		}
		TheGameLogic->setGamePausedInFrame(frame, FALSE);
	}

	void finishSeek()
	{
		// TheSuperHackers @bugfix O97 (P5-e) The follower's catch-up pace (ReplayFollow::mayRunFrame: faster while more than
		// two seconds behind the live point) ran every seek on to the live point at 2x, after a backward seek's restart
		// and after a forward run alike. A seek to a time behind the live point now stops following it; "back to live"
		// (and the first catch-up of an outside viewer, row 49) follows it again.
		if (following())
		{
			const UnsignedInt live = ReplayFollow::livePointFrame();
			const Bool atLive = s_seekKind == SEEK_LIVE || s_catchUp
				|| (live > 0 && TheGameLogic->getFrame() + kLiveSlack >= live);
			ReplayFollow::setFollowLive(atLive);
		}
		const Bool pause = s_pauseAtTarget;
		seekLog("seek end at frame %u: %s", TheGameLogic->getFrame(), pause ? "paused (as before the seek)" : "playing (as before the seek)");
		s_seekKind = SEEK_NONE;
		s_seekTarget = 0;
		s_catchUp = FALSE;
		ReplayControl::setSpeed(s_speedBeforeSeek);
		if (pause && !TheGameLogic->isGamePaused()) {
			TheGameLogic->setGamePaused(TRUE, FALSE, FALSE);
		} else if (!pause && TheGameLogic->isGamePaused()) {
			TheGameLogic->setGamePaused(FALSE);
		}
	}

	/// Forward: a run at Max. Backward: the replay restarts from its first frame (pollRewind), then runs. 'live': back to
	/// the live point (it plays on there, also when paused before).
	Bool startSeek(UnsignedInt frame, Bool pauseAtTarget, Bool live)
	{
		if (TheGameLogic == nullptr || !TheGameLogic->isInReplayGame() || TheGameLogic->isLoadingMap() || seekBusy()) {
			return FALSE;
		}
		const UnsignedInt now = TheGameLogic->getFrame();
		const UnsignedInt total = totalFrames();
		// O64 (H6 #6): a followed replay without data yet has no end to clamp to: no seek then
		if (following() && total == 0) {
			return FALSE;
		}
		if (total > 0 && frame > total && frame > now) {
			frame = total > now ? total : now;
		}
		if (frame == now) {
			return TRUE;
		}
		s_speedBeforeSeek = ReplayControl::speed();
		s_pauseAtTarget = !live && (pauseAtTarget || TheGameLogic->isGamePaused());
		seekLog("seek from frame %u to %u (%s): before it %s", now, frame, live ? "live" : (frame > now ? "forward" : "backward"),
			TheGameLogic->isGamePaused() ? "paused" : "playing");
		if (frame > now)
		{
			s_seekKind = live ? SEEK_LIVE : SEEK_RUN;
			beginRun(frame);
		}
		else
		{
			s_seekKind = SEEK_REWIND;
			s_rewindRequest = frame;
		}
		return TRUE;
	}

	//--- look ---------------------------------------------------------------------------------------
	const Color kText = GameMakeColor(255, 255, 255, 255);
	const Color kTextBorder = GameMakeColor(0, 0, 0, 255);
	const Color kFill = GameMakeColor(24, 28, 36, 200);
	const Color kButton = GameMakeColor(48, 56, 72, 230);
	const Color kButtonBorder = GameMakeColor(120, 130, 150, 255);
	const Color kButtonHilite = GameMakeColor(80, 92, 118, 240);
	const Color kLive = GameMakeColor(255, 80, 64, 255);
	// the seek line: the colours of the ticked slider (TickSlider.cpp)
	const Color kTrack = LookPalette::kTrack;   // O94: gold in the base game
	const Color kTrackFill = LookPalette::kFrame;
	const Color kHandle = GameMakeColor(255, 255, 255, 255);
	const Color kHeld = GameMakeColor(140, 255, 50, 255);

	void plainLook(WinInstanceData &inst, UnsignedInt style, GameFont *font, Color fill, Color border, Color hilite)
	{
		inst.init();
		inst.m_style = style;
		inst.m_font = font;
		inst.m_enabledText.color = kText;
		inst.m_enabledText.borderColor = kTextBorder;
		inst.m_disabledText = inst.m_enabledText;
		inst.m_disabledText.color = GameMakeColor(150, 150, 150, 255);
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

	GameWindow *makeText(GameWindow *parent, GameFont *font, Int x, Int y, Int w, Int h, const char *name, Color fill)
	{
		WinInstanceData inst;
		plainLook(inst, GWS_STATIC_TEXT, font, fill, fill, fill);
		inst.m_id = idOf(name);
		TextData textData;
		memset(&textData, 0, sizeof(textData));
		textData.centered = TRUE;
		textData.centeredVertically = TRUE;
		GameWindow *win = TheWindowManager->gogoGadgetStaticText(parent, WIN_STATUS_ENABLED, x, y, w, h, &inst, &textData, font, FALSE);
		if (win != nullptr) {
			win->winSetWindowId(inst.m_id);
		}
		return win;
	}

	WindowMsgHandledType eatMouse(UnsignedInt msg)
	{
		// the panel's own background takes the mouse, so a click on it does not reach the battlefield
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

	WindowMsgHandledType barInput(GameWindow *, UnsignedInt msg, WindowMsgData, WindowMsgData)
	{
		return eatMouse(msg);
	}

	UnicodeString timeLine(UnsignedInt frame, UnsignedInt total)
	{
		UnicodeString line = ReplayControl::timeText(frame);
		if (total > 0)
		{
			line.concat(L" / ");
			line.concat(ReplayControl::timeText(total));
		}
		return line;
	}

	UnicodeString delayLine(Int lagFrames)
	{
		UnicodeString s(L"-");
		s.concat(ReplayControl::timeText((UnsignedInt)(lagFrames > 0 ? lagFrames : 0)));   // O64: never below 0
		return s;
	}

	//--- the seek line ------------------------------------------------------------------------------
	struct SeekGeo { Int trackL, trackR, trackY, r, thick, valueY; };

	SeekGeo seekGeo(GameWindow *window)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		SeekGeo g;
		g.r = max(4, s_textH / 2);
		g.thick = max(3, g.r / 2);
		g.valueY = y + 2;
		g.trackY = y + 2 + s_textH + 2 + g.r;
		g.trackL = x + g.r + 4;
		g.trackR = x + w - g.r - 4;
		return g;
	}

	UnsignedInt frameAtX(GameWindow *window, Int mx)
	{
		const SeekGeo g = seekGeo(window);
		const UnsignedInt total = max(totalFrames(), TheGameLogic->getFrame());
		if (g.trackR <= g.trackL || total == 0) {
			return 0;
		}
		Real t = (Real)(mx - g.trackL) / (Real)(g.trackR - g.trackL);
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		return (UnsignedInt)(t * (Real)total + 0.5f);
	}

	void fillDisc(Int cx, Int cy, Int r, Color c)
	{
		for (Int dy = -r; dy <= r; ++dy)
		{
			const Int hw = (Int)sqrtf((Real)(r * r - dy * dy));
			TheDisplay->drawFillRect(cx - hw, cy + dy, 2 * hw + 1, 1, c);
		}
	}

	void drawSeekLine(GameWindow *window, WinInstanceData *)
	{
		if (TheDisplay == nullptr || TheGameLogic == nullptr) {
			return;
		}
		// a release the line did not get (the mouse went up elsewhere): nothing is chosen
		if (s_seekHeld && TheWindowManager->winGetGrabWindow() != window) {
			s_seekHeld = FALSE;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		TheDisplay->drawFillRect(x, y, w, h, kFill);
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, kButtonBorder);
		const SeekGeo g = seekGeo(window);
		const UnsignedInt now = TheGameLogic->getFrame();
		const UnsignedInt total = max(totalFrames(), now);
		TheDisplay->drawFillRect(g.trackL, g.trackY - g.thick / 2, g.trackR - g.trackL, g.thick, kTrack);
		if (total == 0) {
			return;
		}
		const Int span = g.trackR - g.trackL;
		const Int nowX = g.trackL + (Int)((Real)span * (Real)now / (Real)total);
		if (nowX > g.trackL) {
			TheDisplay->drawFillRect(g.trackL, g.trackY - g.thick / 2, nowX - g.trackL, g.thick, kTrackFill);
		}
		// the handle: where the mouse holds it, else where the seek goes, else the time shown
		const UnsignedInt at = s_seekHeld ? s_seekDrag : (s_seekKind == SEEK_RUN || s_seekKind == SEEK_LIVE ? s_seekTarget
			: (s_rewindTarget != kNone ? s_rewindTarget : (s_rewindRequest != kNone ? s_rewindRequest : now)));
		const Int hx = g.trackL + (Int)((Real)span * (Real)min(at, total) / (Real)total);
		fillDisc(hx, g.trackY, g.r, s_seekHeld ? kHeld : kTrackFill);
		fillDisc(hx, g.trackY, max(1, g.r - 2), kHandle);
		if (s_seekValue == nullptr && TheDisplayStringManager != nullptr)
		{
			s_seekValue = TheDisplayStringManager->newDisplayString();
			if (s_seekValue != nullptr) {
				s_seekValue->setFont(s_font);
			}
		}
		if (s_seekValue != nullptr)
		{
			s_seekValue->setText(ReplayControl::timeText(at));
			Int tw = 0, th = 0;
			s_seekValue->getSize(&tw, &th);
			Int tx = hx - tw / 2;
			tx = tx + tw > x + w - 2 ? x + w - 2 - tw : tx;
			tx = tx < x + 2 ? x + 2 : tx;
			s_seekValue->draw(tx, g.valueY, s_seekHeld ? kHeld : kText, kTextBorder);
		}
	}

	WindowMsgHandledType seekInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData)
	{
		switch (msg)
		{
		case GWM_LEFT_DOWN:
			if (!seekBusy())
			{
				s_seekHeld = TRUE;
				s_seekDrag = frameAtX(window, (Int)LOLONGTOSHORT(mData1));
			}
			return MSG_HANDLED;
		case GWM_LEFT_DRAG:
			if (s_seekHeld) {
				s_seekDrag = frameAtX(window, (Int)LOLONGTOSHORT(mData1));
			}
			return MSG_HANDLED;
		case GWM_LEFT_UP:
			if (s_seekHeld)
			{
				s_seekHeld = FALSE;
				ReplayControl::seekTo(s_seekDrag);
			}
			return MSG_HANDLED;
		default:
			return eatMouse(msg);
		}
	}

	void showSeekLine(Bool show)
	{
		s_seekOpen = show;
		s_seekHeld = FALSE;
		GameWindow *line = find(kSeekLine);
		if (line != nullptr) {
			line->winHide(!show);
		}
	}

	//--- the panel ----------------------------------------------------------------------------------
	WindowMsgHandledType barSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg != GBM_SELECTED) {
			return MSG_IGNORED;
		}
		if (seekBusy()) {
			return MSG_HANDLED;   // a seek runs: the panel waits
		}
		GameWindow *control = (GameWindow *)mData1;
		const Int id = control != nullptr ? control->winGetWindowId() : 0;
		if (id == idOf(kButtonPause))
		{
			TheMessageStream->appendMessage(GameMessage::MSG_META_TOGGLE_PAUSE);
		}
		else if (id == idOf(kButtonSpeed))
		{
			// O61: one button, round 1x -> 2x -> 4x -> 8x -> 1x (from Max: 1x)
			const Int now = ReplayControl::speed();
			Int next = 1;
			for (Int i = 0; i < CYCLE_COUNT; ++i) {
				if (kCycle[i] == now) next = kCycle[(i + 1) % CYCLE_COUNT];
			}
			ReplayControl::setSpeed(next);
			TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ReplayBarSpeed",
				localizedFallback(L"Replay speed %ls", L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x0432\x043E\x0441\x043F\x0440\x043E\x0438\x0437\x0432\x0435\x0434\x0435\x043D\x0438\x044F %ls"), speedText(next).str()));
		}
		else if (id == idOf(kButtonTime))
		{
			showSeekLine(!s_seekOpen);
		}
		else if (id == idOf(kButtonDelay))
		{
			// O61: back to the live point (an outside viewer's is the host's delay behind the match); at it: nothing
			const UnsignedInt live = following() ? ReplayFollow::livePointFrame() : 0;
			if (live > TheGameLogic->getFrame() + kLiveSlack) {
				startSeek(live, FALSE, TRUE);
			}
		}
		return MSG_HANDLED;
	}

	void destroyBar()
	{
		const char *const names[] = { kBar, kSeekLine, kTextSeek };
		for (Int i = 0; i < (Int)ARRAY_SIZE(names); ++i)
		{
			GameWindow *win = find(names[i]);
			if (win != nullptr) {
				TheWindowManager->winDestroy(win);
			}
		}
		if (s_seekValue != nullptr && TheDisplayStringManager != nullptr) {
			TheDisplayStringManager->freeDisplayString(s_seekValue);
		}
		s_seekValue = nullptr;
		s_seekHeld = FALSE;
		s_shownSpeed = -2;
		s_shownPaused = -1;
		s_shownSeconds = kNone;
		s_shownTotal = kNone;
		s_shownDelay = -3;
		s_shownSeek = -1;
	}

	/// O61: the panel at the right edge under the top counters: the time and the delay, then Pause and the speed, then
	/// (outside viewer) the host's delay; the seek line under it and the seek message at the top middle, both hidden.
	void createBar()
	{
		const Int screenW = TheDisplay->getWidth();
		const Int screenH = TheDisplay->getHeight();
		const Real scale = min((Real)screenW / 800.0f, (Real)screenH / 600.0f);
		GameFont *font = OwnUi::font(TheFontLibrary != nullptr   // O53
			? TheFontLibrary->getFont(AsciiString("Arial"), TheGlobalLanguageData != nullptr ? TheGlobalLanguageData->adjustFontSize(12) : 12, TRUE)
			: nullptr);
		s_font = font;

		Int textH = 12;
		measure(font, UnicodeString(L"0:00"), &textH);
		s_textH = textH;
		const Int pad = max(3, textH / 3);
		const Int gap = max(2, textH / 5);
		const Int margin = max(2, textH / 5);
		const Int rowH = textH + pad;

		const Bool follow = ReplayFollow::isOn();
		const Int hostDelay = LanRoom::upstreamDelaySeconds();
		const UnsignedInt hourFrames = 3600 * LOGICFRAMES_PER_SECOND;
		s_barHasHours = totalFrames() >= hourFrames || TheGameLogic->getFrame() >= hourFrames;
		const UnsignedInt widest = s_barHasHours ? 10 * hourFrames - 1 : hourFrames - 1;
		const Int timeW = measure(font, timeLine(widest, widest)) + 2 * pad;
		const Int delayW = follow ? max(measure(font, delayLine(widest)), measure(font, liveText())) + 2 * pad : 0;
		const Int pauseW = max(measure(font, pauseText()), measure(font, playText())) + 2 * pad;
		Int speedW = measure(font, speedText(ReplayControl::SPEED_MAX));
		for (Int i = 0; i < CYCLE_COUNT; ++i) {
			speedW = max(speedW, measure(font, speedText(kCycle[i])));
		}
		speedW += 2 * pad;
		const UnicodeString hostLine = hostDelay > 0 ? hostDelayText(hostDelay) : UnicodeString::TheEmptyString;
		const Int hostW = hostDelay > 0 ? measure(font, hostLine) + 2 * pad : 0;

		const Int row1W = timeW + (follow ? gap + delayW : 0);
		const Int row2W = pauseW + gap + speedW;
		const Int innerW = max(max(row1W, row2W), hostW);
		const Int rows = hostDelay > 0 ? 3 : 2;
		const Int barW = min(innerW + 2 * margin, screenW);
		const Int barH = rows * rowH + (rows - 1) * gap + 2 * margin;
		const Int edge = max(2, REAL_TO_INT(4.0f * scale));
		const Int barX = max(0, screenW - barW - edge);
		// under the top counters (game time, fps, clock: one line at the top right)
		const Int barY = max(textH + 6, REAL_TO_INT(24.0f * scale));

		WinInstanceData inst;
		plainLook(inst, GWS_USER_WINDOW, font, kFill, kButtonBorder, kFill);
		inst.m_id = idOf(kBar);
		GameWindow *bar = TheWindowManager->winCreate(nullptr, WIN_STATUS_ENABLED, barX, barY, barW, barH, barSystem, &inst);
		if (bar == nullptr) {
			return;
		}
		bar->winSetWindowId(inst.m_id);
		bar->winSetInputFunc(barInput);
		bar->winSetFont(font);

		// right-aligned rows
		const Int right = barW - margin;
		Int y = margin;
		Int x = right;
		if (follow)
		{
			x -= delayW;
			makeButton(bar, font, x, y, delayW, rowH, UnicodeString::TheEmptyString, kButtonDelay,
				text("GUI:ReplayBarDelayTip", L"Behind live. Click: back to live", L"\x041E\x0442\x0441\x0442\x0430\x0432\x0430\x043D\x0438\x0435 \x043E\x0442 \x044D\x0444\x0438\x0440\x0430. \x041D\x0430\x0436\x043C\x0438\x0442\x0435: \x043D\x0430\x0437\x0430\x0434 \x0432 \x044D\x0444\x0438\x0440"));
			x -= gap;
		}
		x -= timeW;
		makeButton(bar, font, x, y, timeW, rowH, UnicodeString::TheEmptyString, kButtonTime,
			text("GUI:ReplayBarTimeTip", L"Click: the seek line", L"\x041D\x0430\x0436\x043C\x0438\x0442\x0435: \x043B\x0438\x043D\x0438\x044F \x043F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0438"));
		y += rowH + gap;
		x = right - speedW;
		makeButton(bar, font, x, y, speedW, rowH, speedText(1), kButtonSpeed,
			text("GUI:ReplayBarSpeedTip", L"Speed in turn: 1x, 2x, 4x, 8x", L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x043F\x043E \x043A\x0440\x0443\x0433\x0443: 1x, 2x, 4x, 8x"));
		x -= gap + pauseW;
		makeButton(bar, font, x, y, pauseW, rowH, pauseText(), kButtonPause,
			text("GUI:ReplayBarPauseTip", L"Pause or resume the replay", L"\x041F\x0430\x0443\x0437\x0430 \x0438\x043B\x0438 \x043F\x0440\x043E\x0434\x043E\x043B\x0436\x0435\x043D\x0438\x0435"));
		if (hostDelay > 0)
		{
			y += rowH + gap;
			GameWindow *host = makeText(bar, font, right - hostW, y, hostW, rowH, kTextHostDelay, GameMakeColor(0, 0, 0, 0));
			if (host != nullptr) {
				GadgetStaticTextSetText(host, hostLine);
			}
		}

		// the seek line under the panel, right-aligned with it
		{
			const Int r = max(4, textH / 2);
			const Int lineW = min(screenW - 2 * edge, max(barW, screenW * 2 / 5));
			const Int lineH = 2 + textH + 2 + 2 * r + 4;
			WinInstanceData lineInst;
			lineInst.init();
			lineInst.m_style = GWS_USER_WINDOW | GWS_MOUSE_TRACK;
			lineInst.m_id = idOf(kSeekLine);
			GameWindow *line = TheWindowManager->winCreate(nullptr, WIN_STATUS_ENABLED, screenW - edge - lineW, barY + barH + gap,
				lineW, lineH, PassMessagesToParentSystem, &lineInst);
			if (line != nullptr)
			{
				line->winSetWindowId(lineInst.m_id);
				line->winSetDrawFunc(drawSeekLine);
				line->winSetInputFunc(seekInput);
				line->winHide(!s_seekOpen);
			}
		}
		// the seek message at the top middle
		{
			const Int msgW = min(screenW - 2 * edge, max(barW * 2, screenW * 2 / 5));
			GameWindow *msg = makeText(nullptr, font, (screenW - msgW) / 2, barY, msgW, rowH + pad, kTextSeek, kFill);
			if (msg != nullptr)
			{
				msg->winSetInputFunc(barInput);
				msg->winHide(TRUE);
			}
		}

		s_barFollow = follow;
		s_barHostDelay = hostDelay;
		s_shownWidth = screenW;
		s_shownHeight = screenH;
	}

	void setButtonText(const char *name, const UnicodeString &str, Color color)
	{
		GameWindow *button = find(name);
		if (button != nullptr)
		{
			GadgetButtonSetText(button, str);
			button->winSetEnabledTextColors(color, kTextBorder);
			button->winSetHiliteTextColors(color, kTextBorder);
		}
	}

	void refreshBar()
	{
		const Bool busy = seekBusy();
		if ((Int)busy != s_shownSeek)
		{
			s_shownSeek = busy ? 1 : 0;
			const char *const buttons[] = { kButtonPause, kButtonSpeed, kButtonTime, kButtonDelay };
			for (Int i = 0; i < (Int)ARRAY_SIZE(buttons); ++i)
			{
				GameWindow *button = find(buttons[i]);
				if (button != nullptr) {
					button->winEnable(!busy);
				}
			}
			GameWindow *msg = find(kTextSeek);
			if (msg != nullptr) {
				msg->winHide(!busy);
			}
		}

		const Int speed = ReplayControl::speed();
		if (speed != s_shownSpeed)
		{
			s_shownSpeed = speed;
			setButtonText(kButtonSpeed, speedText(speed), kText);
		}

		const Int paused = TheGameLogic->isGamePaused() ? 1 : 0;
		if (paused != s_shownPaused)
		{
			s_shownPaused = paused;
			setButtonText(kButtonPause, paused ? playText() : pauseText(), kText);
		}

		const UnsignedInt frame = TheGameLogic->getFrame();
		const UnsignedInt total = max(totalFrames(), frame);
		if (!s_barHasHours && total >= 3600u * LOGICFRAMES_PER_SECOND)
		{
			destroyBar();   // made again, wider, at the next update
			return;
		}
		if (frame / LOGICFRAMES_PER_SECOND != s_shownSeconds || total / LOGICFRAMES_PER_SECOND != s_shownTotal)
		{
			s_shownSeconds = frame / LOGICFRAMES_PER_SECOND;
			s_shownTotal = total / LOGICFRAMES_PER_SECOND;
			setButtonText(kButtonTime, timeLine(frame, total), kText);
		}

		// the delay behind the live point: "-0:10", or the live mark; a stream that ended: nothing
		Int delay = -1;
		if (following())
		{
			const UnsignedInt live = ReplayFollow::livePointFrame();
			delay = live > frame + kLiveSlack ? (Int)((live - frame) / LOGICFRAMES_PER_SECOND) : 0;
		}
		if (delay != s_shownDelay && s_barFollow)
		{
			s_shownDelay = delay;
			if (delay < 0) {
				setButtonText(kButtonDelay, UnicodeString::TheEmptyString, kText);
			} else if (delay == 0) {
				setButtonText(kButtonDelay, liveText(), kLive);
			} else {
				setButtonText(kButtonDelay, delayLine((UnsignedInt)delay * LOGICFRAMES_PER_SECOND), kText);
			}
		}

		if (busy)
		{
			GameWindow *msg = find(kTextSeek);
			if (msg != nullptr)
			{
				const UnsignedInt target = s_seekKind == SEEK_REWIND
					? (s_rewindTarget != kNone ? s_rewindTarget : s_rewindRequest) : s_seekTarget;
				UnicodeString line;
				if (s_catchUp || s_seekKind == SEEK_LIVE) {
					line.format(text("GUI:ReplayBarCatchingUp", L"Catching up with live... %ls / %ls", L"\x0414\x043E\x0433\x043E\x043D\x044F\x0435\x043C \x044D\x0444\x0438\x0440... %ls / %ls").str(),
						ReplayControl::timeText(frame).str(), ReplayControl::timeText(target != kNone ? target : 0).str());
				} else {
					line.format(text("GUI:ReplayBarSeeking", L"Seeking... %ls / %ls", L"\x041F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0430... %ls / %ls").str(),
						ReplayControl::timeText(frame).str(), ReplayControl::timeText(target != kNone ? target : 0).str());
				}
				if (line.compare(GadgetStaticTextGetText(msg)) != 0) {
					GadgetStaticTextSetText(msg, line);
				}
			}
		}
	}

	void resetForNewReplay()
	{
		s_seekKind = SEEK_NONE;
		s_seekTarget = 0;
		s_catchUp = FALSE;
		s_rewindRequest = kNone;
		s_rewindTarget = kNone;
		s_reachedLive = FALSE;
		s_seekOpen = FALSE;
		s_seekHeld = FALSE;
		ReplayFollow::setRestarting(FALSE);
		ReplayFollow::setFollowLive(TRUE);   // O97 (P5-e): a new watch follows its live point
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
	return startSeek(frame, TRUE, FALSE);
}

Bool ReplayControl::seekTo(UnsignedInt frame)
{
	return startSeek(frame, FALSE, FALSE);
}

Bool ReplayControl::isSeeking()
{
	return seekBusy();
}

void ReplayControl::pollRewind()
{
	if (s_rewindRequest == kNone) {
		return;
	}
	const UnsignedInt target = s_rewindRequest;
	s_rewindRequest = kNone;
	if (TheGameLogic == nullptr || !TheGameLogic->isInReplayGame() || TheRecorder == nullptr || TheGameEngine == nullptr)
	{
		s_seekKind = SEEK_NONE;
		return;
	}
	const AsciiString file = TheRecorder->getCurrentReplayFilename();
	if (file.isEmpty())
	{
		s_seekKind = SEEK_NONE;
		return;
	}
	// O61: as the quit menu's restart of a replay (QuitMenu.cpp): end this playback and play the file again from its
	// first frame; a followed replay's mirror and relay go on meanwhile (LanRoom::pollStart waits, ReplayFollow::restarting)
	ReplayFollow::setRestarting(TRUE);
	s_rewindTarget = target;
	s_rewindSinceMs = timeGetTime();
	TheGameLogic->clearGameData(FALSE);
	TheGameEngine->setQuitting(FALSE);
	if (!TheRecorder->playbackFile(file))
	{
		s_rewindTarget = kNone;
		s_seekKind = SEEK_NONE;
		ReplayFollow::setRestarting(FALSE);
	}
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
	const Bool loading = inReplay && TheGameLogic->isLoadingMap();

	// TheSuperHackers @feature (S16) A watched game whose stream is lost: the watcher is told once.
	if (ReplayStream::takeLostNotice() && TheInGameUI != nullptr)
	{
		TheInGameUI->message(text("GUI:ReplayStreamLost", L"The game's stream was lost: the replay plays what it has to the end.",
			L"\x0422\x0440\x0430\x043D\x0441\x043B\x044F\x0446\x0438\x044F \x0438\x0433\x0440\x044B \x043F\x043E\x0442\x0435\x0440\x044F\x043D\x0430: \x0437\x0430\x043F\x0438\x0441\x044C \x043F\x0440\x043E\x0438\x0433\x0440\x044B\x0432\x0430\x0435\x0442\x0441\x044F \x0434\x043E \x043A\x043E\x043D\x0446\x0430 \x0442\x043E\x0433\x043E, \x0447\x0442\x043E \x043F\x043E\x043B\x0443\x0447\x0435\x043D\x043E."));
	}

	// TheSuperHackers @bugfix (O18, S18) -replaySpeed waits until the replay's map is loaded: the replay mode is set one
	// engine update before GameLogic::startNewGame runs, and that turns the fast forward (Max) off again; 2x..8x lived
	// through it only because their factor is not reset there.
	if (inReplay && s_startSpeed >= 0 && !s_startSpeedApplied && !loading)
	{
		s_startSpeedApplied = TRUE;
		setSpeed(s_startSpeed);
	}

	if (!inReplay)
	{
		const Bool restarting = s_rewindTarget != kNone && timeGetTime() - s_rewindSinceMs < kRewindGiveUpMs;
		if (!restarting)
		{
			// nothing of the panel acts outside replays (a rewind's restart is not outside: its replay comes back)
			if (s_factor != 1 || s_seekKind != SEEK_NONE || s_rewindTarget != kNone || s_reachedLive)
			{
				s_factor = 1;
				if (TheFramePacer != nullptr) {
					TheFramePacer->setReplaySpeedFactor(1);
				}
			}
			resetForNewReplay();
		}
	}
	else if (!loading && TheGameLogic->isInGame() && TheGameLogic->getFrame() > 0)
	{
		// (the replay mode is set one engine update before GameLogic::startNewGame runs, which turns Max off and resets
		// the frame: a seek begins only once the replay's logic runs)
		const UnsignedInt now = TheGameLogic->getFrame();
		if (s_rewindTarget != kNone)
		{
			// O61: the restarted replay is loaded: run at Max to the frame the seek asked for
			const UnsignedInt target = s_rewindTarget;
			s_rewindTarget = kNone;
			ReplayFollow::setRestarting(FALSE);
			s_seekKind = SEEK_RUN;
			if (target > now) {
				beginRun(target);
			} else {
				finishSeek();
			}
		}
		// TheSuperHackers @bugfix O68 Arrived only once the logic has paused itself at the target (GameLogic::preUpdate does
		// it at the start of the NEXT update after the frame is reached): before, the panel resumed first and the logic then
		// paused, so a seek started while playing ended paused
		else if ((s_seekKind == SEEK_RUN || s_seekKind == SEEK_LIVE) && now >= s_seekTarget
			&& (TheGameLogic->isGamePaused() || now > s_seekTarget))
		{
			Bool done = TRUE;
			if (s_seekKind == SEEK_LIVE && following())
			{
				// the live point moved on during the run: on to it
				const UnsignedInt live = ReplayFollow::livePointFrame();
				if (live > now + kLiveSlack)
				{
					beginRun(live);
					done = FALSE;
				}
				else
				{
					s_reachedLive = TRUE;
				}
			}
			if (done) {
				finishSeek();
			}
		}
		else if (s_seekKind == SEEK_RUN || s_seekKind == SEEK_LIVE)
		{
			// O64 (H6 #6): a run whose frame stops moving (a followed stream without more data) ends, and the keys and
			// the panel are free again
			// O65: a slow stream (a source hop takes 5-8 s) or a pause is no stall: only no new frame AND no new data
			const UnsignedInt newest = ReplayFollow::newestFrame();
			if (now != s_runLastFrame || newest != s_runLastNewest || TheGameLogic->isGamePaused())
			{
				s_runLastFrame = now;
				s_runLastNewest = newest;
				s_runLastMoveMs = timeGetTime();
			}
			else if (timeGetTime() - s_runLastMoveMs > kRunStallMs)
			{
				finishSeek();
				TheInGameUI->message(text("GUI:ReplayBarSeekStopped", L"Seek stopped: there is no more data.", L"\x041F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0430 \x043E\x0441\x0442\x0430\x043D\x043E\x0432\x043B\x0435\x043D\x0430: \x0434\x0430\x043B\x044C\x0448\x0435 \x0434\x0430\x043D\x043D\x044B\x0445 \x043D\x0435\x0442."));
			}
		}
		else if (s_seekKind == SEEK_NONE && !s_reachedLive && following())
		{
			// O61 (row 49): a followed replay that starts behind its live point (an outside viewer coming into a running
			// match) runs at Max to it first ("\x0414\x043E\x0433\x043E\x043D\x044F\x0435\x043C \x044D\x0444\x0438\x0440"), once; at the live point the viewer's own choices stand
			const UnsignedInt live = ReplayFollow::livePointFrame();
			if (live > now + 3 * LOGICFRAMES_PER_SECOND)
			{
				if (startSeek(live, FALSE, TRUE)) {
					s_catchUp = TRUE;
				}
			}
			else if (live > 0)
			{
				s_reachedLive = TRUE;
			}
		}
	}

	// TheSuperHackers @tweak O97 (P5-e, router) A pause of a followed replay (key or button; not the logic's own pause at a
	// seek's target) leaves the live point as a seek does: after Play it goes on at the chosen speed from the paused time.
	// "Back to live" follows again (finishSeek).
	if (inReplay && !loading && following() && !seekBusy() && TheGameLogic->isGamePaused()) {
		ReplayFollow::setFollowLive(FALSE);
	}

	// O64 (H6 #7): a restarted replay whose logic never gets going: the seek is given up and the input is free again.
	// O65: the clock starts when its map load has ended (a long load on a slow PC is no failure)
	if (s_rewindTarget == kNone || !inReplay) {
		s_rewindLoadedMs = 0;
	} else if (s_rewindLoadedMs == 0 && !loading && TheGameLogic->isInGame()) {
		s_rewindLoadedMs = timeGetTime();
	}
	if (inReplay && s_rewindTarget != kNone && s_rewindLoadedMs != 0 && timeGetTime() - s_rewindLoadedMs > kRestartFailMs
		&& TheGameLogic->getFrame() == 0)
	{
		s_rewindTarget = kNone;
		s_seekKind = SEEK_NONE;
		ReplayFollow::setRestarting(FALSE);
		setSpeed(s_speedBeforeSeek);
		TheInGameUI->message(text("GUI:ReplayBarSeekFailed", L"The seek failed: the replay did not start again.", L"\x041F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0430 \x043D\x0435 \x0443\x0434\x0430\x043B\x0430\x0441\x044C: \x0437\x0430\x043F\x0438\x0441\x044C \x043D\x0435 \x0437\x0430\x043F\x0443\x0441\x0442\x0438\x043B\x0430\x0441\x044C \x0441\x043D\x043E\x0432\x0430."));
	}

	if (!barWanted())
	{
		if (TheWindowManager != nullptr && (find(kBar) != nullptr || find(kSeekLine) != nullptr || find(kTextSeek) != nullptr)) {
			destroyBar();
		}
		return;
	}

	GameWindow *bar = find(kBar);
	if (bar != nullptr && (TheDisplay->getWidth() != s_shownWidth || TheDisplay->getHeight() != s_shownHeight
		|| ReplayFollow::isOn() != s_barFollow || LanRoom::upstreamDelaySeconds() != s_barHostDelay))
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
