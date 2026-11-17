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

#include "PreRTS.h"

#include "Common/FrameShots.h"
#include "Common/FrameTiming.h"

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Recorder.h"

#include "GameClient/Display.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/TickSlider.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/View.h"

#include "Common/MessageStream.h"

#include "GameLogic/GameLogic.h"

#include <algorithm>
#include <vector>


Bool FrameShots::s_enabled = FALSE;
Int FrameShots::s_failureExitCode = 0;

namespace
{
	std::vector<UnsignedInt> s_frames; ///< Listed logic frames, ascending and unique.
	std::vector<UnsignedInt> s_saveFrames; ///< Logic frames of the test saves (-saveFrames), ascending and unique.
	size_t s_nextSaveIndex = 0; ///< First listed save frame that the logic has not reached yet.
	FILE *s_savesCsv = nullptr;
	size_t s_nextFrameIndex = 0; ///< First listed frame that the logic has not reached yet.
	std::vector<UnsignedInt> s_pendingFrames; ///< Listed frames reached by the logic, waiting for the next render.
	UnsignedInt s_renderCountAtLogicUpdate = 0; ///< Rendered frame count when the last pending frame was reached.

	AsciiString s_directory;
	Bool s_exitWhenDone = FALSE;
	Bool s_outputReady = FALSE;
	Bool s_outputFailed = FALSE;
	FILE *s_csv = nullptr;

	Bool s_gameStarted = FALSE;
	UnsignedInt s_lastLogicFrame = 0;
	Int64 s_gameStartTicks = 0;
	UnsignedInt s_engineUpdatesSinceGameStart = 0;
	Bool s_exitRequested = FALSE;
	Bool s_noImage = FALSE;

	struct TestInputSpec
	{
		enum Kind { KIND_KEY, KIND_RMB, KIND_EDGE };
		Kind kind;
		Int key; ///< KEY_* for KIND_KEY
		Int dirX, dirY; ///< -1, 0, 1 for KIND_EDGE; the drag vector in pixels for KIND_RMB
		UnsignedInt from, to;
		Bool active;
		Bool pendingRelease; ///< KIND_EDGE: the forced screen edge mode is released one update after the pointer left the edge
	};
	std::vector<TestInputSpec> s_testInputs;
	Int s_testLogicFps = 0;

	//---------------------------------------------------------------------------------------------------------------
	// -uiScript: the steps. One step per line, '#' starts a comment, words separated by spaces:
	//   scale W H              the coordinates of the next steps are for a W x H client area, scaled to the real one
	//   timeout MS             the whole script ends with exit code 5 after MS milliseconds (default 300000)
	//   wait MS                wait MS milliseconds
	//   waitframes N           wait N engine updates
	//   waitshell              wait until the shell menu is up (a shell screen is shown and its animation has ended)
	//   waitgame               wait until a match (not the shell map) runs
	//   move X Y               move the pointer to X Y (it stays there for the next steps)
	//   click X Y [HOVER HOLD] move to X Y, wait HOVER ms (default 500), left button down, HOLD ms (default 200), up
	//   rclick X Y [HOVER HOLD] the same with the right button
	//   clickwin NAME [FX] [FY] click the window NAME (for example OptionsMenu.wnd:Sections.TextPack) at FX/1000 of its
	//                          width and FY/1000 of its height (default 500 500: the middle); skipped with a log line
	//                          when not found. An open combo box list is part of its combo box window.
	//   key NAME               press and release a key: ESC TAB ENTER SPACE BACKSPACE UP DOWN LEFT RIGHT F1..F12 A..Z 0..9
	//   text WORDS             type the rest of the line into the window that has the keyboard focus
	//   shot NAME              save <shot folder>\NAME.png after the next drawn frame (the -shotDir folder)
	//   loadshot NAME [PERCENT] save <shot folder>\NAME.png from the loading screen, at its first draw at PERCENT (default 0)
	//                          or more. The step only arms the picture and goes on at once: put it before the click that
	//                          starts the game (the steps do not run while a map loads). Several may be armed.
	//   exit [CODE]            end the game through the normal quit path, after the pictures are written
	//   down X Y               (O25) move there and press the left button, and keep it pressed
	//   downwin NAME [FX FY]   (O25) the same at a point of window NAME (FX, FY in thousandths of its size, default middle)
	//   drag X Y [MS]          (O25) move the pressed pointer there over MS milliseconds (default 500), a drag event every
	//                          update, as the real mouse sends them
	//   dragwin NAME FX FY [MS] (O25) the same to a point of window NAME
	//   up                     (O25) release the left button where the pointer is
	//   downslider NAME [POS]  (O28) press on the ticked slider NAME at slider position POS (a decimal, e.g. 2.6: between
	//                          step 2 and 3, past the middle), on the track's line; no POS: on the handle where it is
	//   dragslider NAME POS [MS] (O28) drag the pressed pointer to slider position POS of NAME (as drag)
	//   clickrow NAME ROW      (O29) click row ROW (0 = first) of the drawn list NAME (FrameShots::setRowHook), scrolled
	//                          into view first; a quick click (150 ms hover, 60 ms held)
	//   dclickrow NAME ROW     (O29) the same twice, 80 ms apart: a double click
	//   waitwin NAME [MS]      (O29) wait until window NAME exists and is shown (default at most 20000 ms; then a log
	//                          line and the next step)
	//   exitmatch              (O43) in a match: what the quit menu's Exit and its Yes do (in a network match this
	//                          player surrenders and goes to the score screen; the others play on). It does not need the
	//                          game window to be the active one, which the quit menu itself does (ESC opens it only in
	//                          the active window). Use "waitgame" and "wait" before it.
	// A step on a named window that is hidden (a settings section or page not shown) first asks the screen to show it
	// (FrameShots::setRevealHook); a window still hidden is skipped with a log line, never clicked blind.
	struct UiStep
	{
		enum Op { OP_SCALE, OP_TIMEOUT, OP_WAIT, OP_WAITFRAMES, OP_WAITSHELL, OP_WAITGAME, OP_MOVE, OP_CLICK, OP_RCLICK, OP_CLICKWIN, OP_KEY,
			OP_TEXT, OP_SHOT, OP_LOADSHOT, OP_EXIT, OP_DOWN, OP_DOWNWIN, OP_DRAG, OP_DRAGWIN, OP_UP, OP_DOWNSLIDER, OP_DRAGSLIDER,
			OP_CLICKROW, OP_WAITWIN, OP_EXITMATCH };
		Op op;
		Int a, b, c, d;
		AsciiString text;
		Int line;
	};
	std::vector<UiStep> s_uiSteps;
	size_t s_uiNext = 0;
	Bool s_uiStepStarted = FALSE;
	UnsignedInt s_uiStepStartMs = 0;
	UnsignedInt s_uiStepStartUpdate = 0;
	UnsignedInt s_uiUpdates = 0;
	UnsignedInt s_uiStartMs = 0;
	UnsignedInt s_uiTimeoutMs = 300000;
	Int s_uiPhase = 0;
	ICoord2D s_uiPointer = { 0, 0 };
	Bool s_uiButtonDown = FALSE;   ///< O67: a script's mouse button is held (between its DOWN and UP)
	Int s_uiScaleW = 0, s_uiScaleH = 0;
	FILE *s_uiLog = nullptr;

	struct LoadShot { AsciiString name; Int percent; };
	std::vector<LoadShot> s_loadShots;   ///< armed by "loadshot", taken by FrameShots::onLoadScreenDrawn

	struct UiKey { const char *name; Int key; };
	const UiKey s_uiKeys[] =
	{
		{ "ESC", KEY_ESC }, { "TAB", KEY_TAB }, { "ENTER", KEY_ENTER }, { "SPACE", KEY_SPACE }, { "BACKSPACE", KEY_BACKSPACE },
		{ "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT },
		{ "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 }, { "F4", KEY_F4 }, { "F5", KEY_F5 }, { "F6", KEY_F6 },
		{ "F7", KEY_F7 }, { "F8", KEY_F8 }, { "F9", KEY_F9 }, { "F10", KEY_F10 }, { "F11", KEY_F11 }, { "F12", KEY_F12 },
		{ "A", KEY_A }, { "B", KEY_B }, { "C", KEY_C }, { "D", KEY_D }, { "E", KEY_E }, { "F", KEY_F }, { "G", KEY_G },
		{ "H", KEY_H }, { "I", KEY_I }, { "J", KEY_J }, { "K", KEY_K }, { "L", KEY_L }, { "M", KEY_M }, { "N", KEY_N },
		{ "O", KEY_O }, { "P", KEY_P }, { "Q", KEY_Q }, { "R", KEY_R }, { "S", KEY_S }, { "T", KEY_T }, { "U", KEY_U },
		{ "V", KEY_V }, { "W", KEY_W }, { "X", KEY_X }, { "Y", KEY_Y }, { "Z", KEY_Z },
		{ "0", KEY_0 }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 }, { "4", KEY_4 }, { "5", KEY_5 }, { "6", KEY_6 },
		{ "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 },
	};

	void uiLog(const char *format, ...)
	{
		if (s_uiLog == nullptr)
			return;
		va_list args;
		va_start(args, format);
		fprintf(s_uiLog, "[%8.3fs] ", (timeGetTime() - s_uiStartMs) / 1000.0);
		vfprintf(s_uiLog, format, args);
		fprintf(s_uiLog, "\n");
		fflush(s_uiLog);
		va_end(args);
	}

	ICoord2D uiPoint(Int x, Int y)
	{
		ICoord2D p = { x, y };
		if (s_uiScaleW > 0 && s_uiScaleH > 0 && TheDisplay != nullptr)
		{
			p.x = x * (Int)TheDisplay->getWidth() / s_uiScaleW;
			p.y = y * (Int)TheDisplay->getHeight() / s_uiScaleH;
		}
		return p;
	}

	FrameShots::RevealFunc s_uiReveal = nullptr;   ///< O28: shows the hidden page of a step's window
	FrameShots::RowFunc s_uiRow = nullptr;         ///< O29: the screen point of a row of a drawn list

	Bool uiShownOnScreen(GameWindow *win)
	{
		for (GameWindow *w = win; w != nullptr; w = w->winGetParent()) {
			if (w->winIsHidden()) {
				return FALSE;
			}
		}
		return TRUE;
	}

	/// O28 (item 8): the window a step names, shown first when it is on a hidden page; nullptr (with a log line) when it
	/// does not exist or stays hidden.
	GameWindow *uiFindWindow(const AsciiString &name, const char *what)
	{
		GameWindow *win = (TheWindowManager != nullptr && TheNameKeyGenerator != nullptr)
			? TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(name)) : nullptr;
		if (win == nullptr)
		{
			uiLog("%s: no window %s", what, name.str());
			return nullptr;
		}
		if (!uiShownOnScreen(win) && s_uiReveal != nullptr && s_uiReveal(win)) {
			uiLog("%s: showed the page of %s", what, name.str());
		}
		if (!uiShownOnScreen(win))
		{
			uiLog("%s: window %s is hidden, step skipped", what, name.str());
			return nullptr;
		}
		return win;
	}

	ICoord2D s_uiDragFrom = { 0, 0 };   ///< O25: a drag step's start and end (screen pixels)
	ICoord2D s_uiDragTo = { 0, 0 };

	/// O25: a point of a window, in thousandths of its size (as clickwin), in screen pixels; FALSE when it does not exist
	/// (O28: or stays hidden).
	Bool uiWindowPoint(const AsciiString &name, Int fx, Int fy, ICoord2D &p, const char *what)
	{
		GameWindow *win = uiFindWindow(name, what);
		if (win == nullptr) {
			return FALSE;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		win->winGetScreenPosition(&x, &y);
		win->winGetSize(&w, &h);
		fx = fx < 0 ? 0 : (fx > 1000 ? 1000 : fx);
		fy = fy < 0 ? 0 : (fy > 1000 ? 1000 : fy);
		p.x = x + (w > 1 ? (w - 1) * fx / 1000 : 0);
		p.y = y + (h > 1 ? (h - 1) * fy / 1000 : 0);
		return TRUE;
	}

	void uiMouse(GameMessage::Type type)
	{
		if (type == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN || type == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN) {
			s_uiButtonDown = TRUE;
		} else if (type == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP || type == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP) {
			s_uiButtonDown = FALSE;
		}
		GameMessage *msg = TheMessageStream->appendMessage(type);
		msg->appendPixelArgument(s_uiPointer);
		msg->appendIntegerArgument(0);
		if (type != GameMessage::MSG_RAW_MOUSE_POSITION)
			msg->appendIntegerArgument((Int)timeGetTime());
	}

	GameWindow *s_uiFocusAtKeyDown = nullptr;   ///< O39: the focused window when a script key went down

	void uiKey(Int key, Bool down)
	{
		GameMessage *msg = TheMessageStream->appendMessage(down ? GameMessage::MSG_RAW_KEY_DOWN : GameMessage::MSG_RAW_KEY_UP);
		msg->appendIntegerArgument(key);
		msg->appendIntegerArgument(down ? KEY_STATE_DOWN : KEY_STATE_UP);
	}

	Int64 queryTicks()
	{
		LARGE_INTEGER ticks;
		QueryPerformanceCounter(&ticks);
		return ticks.QuadPart;
	}

	double ticksToMilliseconds(Int64 ticks)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		return (double)ticks * 1000.0 / (double)freq.QuadPart;
	}

	// Reads "300,900,1800" into an ascending list without duplicates.
	void parseFrameList(const char *commaSeparatedFrames, std::vector<UnsignedInt> &frames)
	{
		frames.clear();
		const char *p = commaSeparatedFrames;
		while (p != nullptr && *p != '\0')
		{
			char *end = nullptr;
			const unsigned long value = strtoul(p, &end, 10);
			if (end == p)
			{
				++p; // skip a separator
				continue;
			}
			frames.push_back((UnsignedInt)value);
			p = end;
		}
		std::sort(frames.begin(), frames.end());
		frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
	}

	// Creates each folder of the path in turn. Existing folders are fine.
	void createDirectories(const AsciiString &path)
	{
		char buffer[_MAX_PATH];
		strlcpy(buffer, path.str(), ARRAY_SIZE(buffer));
		for (char *p = buffer; *p != '\0'; ++p)
		{
			if ((*p == '\\' || *p == '/') && p != buffer && *(p - 1) != ':')
			{
				const char c = *p;
				*p = '\0';
				CreateDirectory(buffer, nullptr);
				*p = c;
			}
		}
		CreateDirectory(buffer, nullptr);
	}
}

void FrameShots::setFrames(const char *commaSeparatedFrames)
{
	parseFrameList(commaSeparatedFrames, s_frames);
	s_nextFrameIndex = 0;
	updateEnabled();
}

void FrameShots::setSaveFrames(const char *commaSeparatedFrames)
{
	parseFrameList(commaSeparatedFrames, s_saveFrames);
	s_nextSaveIndex = 0;
	updateEnabled();
}

void FrameShots::updateEnabled()
{
	s_enabled = !s_frames.empty() || !s_saveFrames.empty() || !s_uiSteps.empty();
}

void FrameShots::setRevealHook(RevealFunc func)
{
	s_uiReveal = func;
}

void FrameShots::setRowHook(RowFunc func)
{
	s_uiRow = func;
}

Bool FrameShots::uiScriptMouse(ICoord2D *pos, Bool *buttonDown)
{
	if (!isUiScriptRunning()) {
		return FALSE;
	}
	*pos = s_uiPointer;
	*buttonDown = s_uiButtonDown;
	return TRUE;
}

Bool FrameShots::isUiScriptRunning()
{
	return !s_uiSteps.empty() && s_uiNext < s_uiSteps.size();
}

void FrameShots::setUiScript(const char *path)
{
	s_uiSteps.clear();
	FILE *file = fopen(path, "r");
	if (file == nullptr)
	{
		// an unattended run must not wait behind a message box: the script is one step, exit with code 7
		DEBUG_LOG(("-uiScript: cannot open %s", path));
		UiStep step;
		step.op = UiStep::OP_EXIT;
		step.a = 7;
		step.b = step.c = step.d = 0;
		step.line = 0;
		s_uiSteps.push_back(step);
		updateEnabled();
		return;
	}
	char line[1024];
	Int lineNo = 0;
	while (fgets(line, sizeof(line), file) != nullptr)
	{
		++lineNo;
		char *hash = strchr(line, '#');
		if (hash != nullptr)
			*hash = '\0';
		char word[64] = {};
		Int consumed = 0;
		if (sscanf(line, " %63s%n", word, &consumed) != 1)
			continue;
		UiStep step;
		step.a = step.b = step.c = step.d = 0;
		step.line = lineNo;
		const char *rest = line + consumed;
		Int a = 0, b = 0, c = 500, d = 200;
		const Int n = sscanf(rest, "%d %d %d %d", &a, &b, &c, &d);
		Bool ok = TRUE;
		if (stricmp(word, "scale") == 0) { step.op = UiStep::OP_SCALE; ok = n >= 2; }
		else if (stricmp(word, "timeout") == 0) { step.op = UiStep::OP_TIMEOUT; ok = n >= 1; }
		else if (stricmp(word, "wait") == 0) { step.op = UiStep::OP_WAIT; ok = n >= 1; }
		else if (stricmp(word, "waitframes") == 0) { step.op = UiStep::OP_WAITFRAMES; ok = n >= 1; }
		else if (stricmp(word, "waitshell") == 0) { step.op = UiStep::OP_WAITSHELL; }
		else if (stricmp(word, "waitgame") == 0) { step.op = UiStep::OP_WAITGAME; }
		else if (stricmp(word, "exitmatch") == 0) { step.op = UiStep::OP_EXITMATCH; }
		else if (stricmp(word, "move") == 0) { step.op = UiStep::OP_MOVE; ok = n >= 2; }
		else if (stricmp(word, "click") == 0) { step.op = UiStep::OP_CLICK; ok = n >= 2; }
		else if (stricmp(word, "rclick") == 0) { step.op = UiStep::OP_RCLICK; ok = n >= 2; }
		else if (stricmp(word, "exit") == 0) { step.op = UiStep::OP_EXIT; if (n < 1) a = 0; }
		else if (stricmp(word, "down") == 0) { step.op = UiStep::OP_DOWN; ok = n >= 2; }
		else if (stricmp(word, "drag") == 0) { step.op = UiStep::OP_DRAG; ok = n >= 2; if (n < 3) c = 500; }
		else if (stricmp(word, "up") == 0) { step.op = UiStep::OP_UP; }
		else if (stricmp(word, "downwin") == 0 || stricmp(word, "dragwin") == 0)
		{
			const Bool isDrag = stricmp(word, "dragwin") == 0;
			step.op = isDrag ? UiStep::OP_DRAGWIN : UiStep::OP_DOWNWIN;
			char name[256] = {};
			Int fx = 500, fy = 500, ms = 500;
			const Int got = sscanf(rest, " %255s %d %d %d", name, &fx, &fy, &ms);
			ok = got >= (isDrag ? 3 : 1);
			step.text = name;
			a = fx; b = fy; c = ms; d = 0;
		}
		else if (stricmp(word, "downslider") == 0 || stricmp(word, "dragslider") == 0)
		{
			// O28: the position is a decimal slider position, kept in thousandths; b = 1 when it was given
			const Bool isDrag = stricmp(word, "dragslider") == 0;
			step.op = isDrag ? UiStep::OP_DRAGSLIDER : UiStep::OP_DOWNSLIDER;
			char name[256] = {};
			float pos = 0.0f;
			Int ms = 500;
			const Int got = sscanf(rest, " %255s %f %d", name, &pos, &ms);
			ok = got >= (isDrag ? 2 : 1);
			step.text = name;
			a = (Int)floorf(pos * 1000.0f + 0.5f); b = got >= 2 ? 1 : 0; c = ms; d = 0;
		}
		else if (stricmp(word, "clickrow") == 0 || stricmp(word, "dclickrow") == 0)
		{
			step.op = UiStep::OP_CLICKROW;
			char name[256] = {};
			Int row = 0;
			const Int got = sscanf(rest, " %255s %d", name, &row);
			ok = got >= 2;
			step.text = name;
			a = row; b = 0; c = 0; d = stricmp(word, "dclickrow") == 0 ? 2 : 1;
		}
		else if (stricmp(word, "waitwin") == 0)
		{
			step.op = UiStep::OP_WAITWIN;
			char name[256] = {};
			Int ms = 20000;
			const Int got = sscanf(rest, " %255s %d", name, &ms);
			ok = got >= 1;
			step.text = name;
			a = got >= 2 ? ms : 20000; b = 0; c = 0; d = 0;
		}
		else if (stricmp(word, "loadshot") == 0)
		{
			step.op = UiStep::OP_LOADSHOT;
			char name[256] = {};
			Int percent = 0;
			const Int got = sscanf(rest, " %255s %d", name, &percent);
			ok = got >= 1;
			step.text = name;
			a = percent; b = 0; c = 500; d = 200;
		}
		else if (stricmp(word, "clickwin") == 0)
		{
			step.op = UiStep::OP_CLICKWIN;
			char name[256] = {};
			Int fx = 500, fy = 500;
			const Int got = sscanf(rest, " %255s %d %d", name, &fx, &fy);
			ok = got >= 1;
			step.text = name;
			a = fx; b = fy; c = 500; d = 200;
		}
		else if (stricmp(word, "key") == 0 || stricmp(word, "text") == 0 || stricmp(word, "shot") == 0)
		{
			step.op = stricmp(word, "key") == 0 ? UiStep::OP_KEY : (stricmp(word, "text") == 0 ? UiStep::OP_TEXT : UiStep::OP_SHOT);
			while (*rest == ' ' || *rest == '\t')
				++rest;
			AsciiString text(rest);
			text.trim();
			step.text = text;
			ok = !text.isEmpty();
		}
		else
		{
			ok = FALSE;
		}
		if (!ok)
		{
			DEBUG_LOG(("-uiScript: line %d is not a step: %s", lineNo, line));
			continue;
		}
		step.a = a; step.b = b; step.c = c; step.d = d;
		s_uiSteps.push_back(step);
	}
	fclose(file);
	s_uiNext = 0;
	updateEnabled();
}

/// Plays the -uiScript steps; called once per client update, after the device messages (which are cut off) were appended.
/// The pointer position is sent on every update, so hovering works as with a real mouse that does not move.
void FrameShots::runUiScript()
{
	if (s_uiSteps.empty() || TheMessageStream == nullptr || TheDisplay == nullptr)
		return;

	const UnsignedInt now = timeGetTime();
	if (s_uiUpdates == 0)
	{
		s_uiStartMs = now;
		if (prepareOutput())
		{
			AsciiString logPath;
			logPath.format("%s\\uiscript.log", s_directory.str());
			s_uiLog = fopen(logPath.str(), "w");
		}
		uiLog("ui script: %u steps", (unsigned)s_uiSteps.size());
	}
	++s_uiUpdates;

	if (s_exitRequested)
		return;

	if (now - s_uiStartMs > s_uiTimeoutMs)
	{
		uiLog("timeout after %u ms at step %u", s_uiTimeoutMs, (unsigned)s_uiNext + 1);
		s_failureExitCode = 5;
		s_exitRequested = TRUE;
		return;
	}

	// the pointer, every update
	uiMouse(GameMessage::MSG_RAW_MOUSE_POSITION);

	while (s_uiNext < s_uiSteps.size())
	{
		UiStep &step = s_uiSteps[s_uiNext];
		if (!s_uiStepStarted)
		{
			s_uiStepStarted = TRUE;
			s_uiStepStartMs = now;
			s_uiStepStartUpdate = s_uiUpdates;
			s_uiPhase = 0;
			uiLog("step %u (line %d)", (unsigned)s_uiNext + 1, step.line);
		}
		const UnsignedInt elapsed = now - s_uiStepStartMs;
		Bool done = TRUE;

		switch (step.op)
		{
		case UiStep::OP_SCALE:
			s_uiScaleW = step.a;
			s_uiScaleH = step.b;
			break;
		case UiStep::OP_TIMEOUT:
			s_uiTimeoutMs = (UnsignedInt)step.a;
			break;
		case UiStep::OP_WAIT:
			done = elapsed >= (UnsignedInt)step.a;
			break;
		case UiStep::OP_WAITFRAMES:
			done = s_uiUpdates - s_uiStepStartUpdate >= (UnsignedInt)step.a;
			break;
		case UiStep::OP_WAITSHELL:
			done = TheShell != nullptr && TheShell->isShellActive() && TheShell->getScreenCount() > 0 && TheShell->top() != nullptr
				&& TheShell->isAnimFinished();
			break;
		case UiStep::OP_WAITGAME:
			done = TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
			break;
		case UiStep::OP_WAITWIN:
		{
			// O29: the window exists and nothing above it is hidden (no reveal: the script waits for the screen)
			GameWindow *win = (TheWindowManager != nullptr && TheNameKeyGenerator != nullptr)
				? TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(step.text)) : nullptr;
			done = win != nullptr && uiShownOnScreen(win);
			if (done) {
				uiLog("waitwin: %s shown after %u ms", step.text.str(), elapsed);
			} else if (elapsed >= (UnsignedInt)step.a) {
				uiLog("waitwin: %s not shown after %u ms, going on", step.text.str(), elapsed);
				done = TRUE;
			}
			break;
		}
		// O29: a click (or two) on a row of a drawn list
		case UiStep::OP_CLICKROW:
		{
			done = FALSE;
			if (s_uiPhase == 0)
			{
				GameWindow *win = uiFindWindow(step.text, "clickrow");
				Int x = 0, y = 0;
				if (win == nullptr || s_uiRow == nullptr || !s_uiRow(win, step.a, &x, &y))
				{
					if (win != nullptr) {
						uiLog("clickrow: no row %d in %s", step.a, step.text.str());
					}
					done = TRUE;
					break;
				}
				s_uiPointer.x = x;
				s_uiPointer.y = y;
				uiMouse(GameMessage::MSG_RAW_MOUSE_POSITION);
				step.b = 0;
				s_uiPhase = 1;
				s_uiStepStartMs = now;
			}
			else if ((s_uiPhase == 1 && elapsed >= 150) || (s_uiPhase == 3 && elapsed >= 80))
			{
				uiMouse(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN);
				s_uiPhase = 2;
				s_uiStepStartMs = now;
			}
			else if (s_uiPhase == 2 && elapsed >= 60)
			{
				uiMouse(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP);
				if (++step.b >= step.d)
				{
					uiLog("%s row %d of %s at %d %d", step.d == 2 ? "dclickrow" : "clickrow", step.a, step.text.str(), s_uiPointer.x, s_uiPointer.y);
					done = TRUE;
				}
				else
				{
					s_uiPhase = 3;
					s_uiStepStartMs = now;
				}
			}
			break;
		}
		case UiStep::OP_MOVE:
			s_uiPointer = uiPoint(step.a, step.b);
			uiMouse(GameMessage::MSG_RAW_MOUSE_POSITION);
			break;
		case UiStep::OP_CLICK:
		case UiStep::OP_RCLICK:
		case UiStep::OP_CLICKWIN:
		{
			const Bool right = step.op == UiStep::OP_RCLICK;
			done = FALSE;
			if (s_uiPhase == 0)
			{
				if (step.op == UiStep::OP_CLICKWIN)
				{
					// the window's own place on the screen, in real client coordinates (no scale); O28: its hidden page is
					// shown first
					GameWindow *win = uiFindWindow(step.text, "clickwin");
					if (win == nullptr)
					{
						done = TRUE;
						break;
					}
					Int x = 0, y = 0, w = 0, h = 0;
					win->winGetScreenPosition(&x, &y);
					win->winGetSize(&w, &h);
					const Int fx = step.a < 0 ? 0 : (step.a > 1000 ? 1000 : step.a);
					const Int fy = step.b < 0 ? 0 : (step.b > 1000 ? 1000 : step.b);
					s_uiPointer.x = x + (w > 1 ? (w - 1) * fx / 1000 : 0);
					s_uiPointer.y = y + (h > 1 ? (h - 1) * fy / 1000 : 0);
				}
				else
				{
					s_uiPointer = uiPoint(step.a, step.b);
				}
				uiMouse(GameMessage::MSG_RAW_MOUSE_POSITION);
				s_uiPhase = 1;
			}
			else if (s_uiPhase == 1 && elapsed >= (UnsignedInt)step.c)
			{
				uiMouse(right ? GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN : GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN);
				s_uiPhase = 2;
				s_uiStepStartMs = now;
			}
			else if (s_uiPhase == 2 && elapsed >= (UnsignedInt)step.d)
			{
				uiMouse(right ? GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP : GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP);
				uiLog("%s at %d %d%s%s", right ? "rclick" : "click", s_uiPointer.x, s_uiPointer.y,
					step.op == UiStep::OP_CLICKWIN ? " on " : "", step.op == UiStep::OP_CLICKWIN ? step.text.str() : "");
				done = TRUE;
			}
			break;
		}
		// O25: a held left button and drags (the ticked slider's held state and drag)
		case UiStep::OP_DOWN:
		case UiStep::OP_DOWNWIN:
		case UiStep::OP_DOWNSLIDER:
		{
			if (step.op == UiStep::OP_DOWNWIN)
			{
				ICoord2D p;
				if (!uiWindowPoint(step.text, step.a, step.b, p, "downwin")) {
					break;
				}
				s_uiPointer = p;
			}
			else if (step.op == UiStep::OP_DOWNSLIDER)
			{
				// O28: on the track at the slider position (the handle's own place when none was given)
				GameWindow *win = uiFindWindow(step.text, "downslider");
				if (win == nullptr) {
					break;
				}
				const Real pos = step.b != 0 ? (Real)step.a / 1000.0f : TickSlider::position(win);
				Int x = 0, y = 0;
				if (!TickSlider::screenPoint(win, pos, &x, &y))
				{
					uiLog("downslider: %s is not a ticked slider", step.text.str());
					break;
				}
				s_uiPointer.x = x;
				s_uiPointer.y = y;
				uiLog("downslider: %s at position %.3f", step.text.str(), pos);
			}
			else
			{
				s_uiPointer = uiPoint(step.a, step.b);
			}
			uiMouse(GameMessage::MSG_RAW_MOUSE_POSITION);
			uiMouse(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN);
			uiLog("down at %d %d", s_uiPointer.x, s_uiPointer.y);
			break;
		}
		case UiStep::OP_DRAG:
		case UiStep::OP_DRAGWIN:
		case UiStep::OP_DRAGSLIDER:
		{
			done = FALSE;
			if (s_uiPhase == 0)
			{
				ICoord2D target;
				if (step.op == UiStep::OP_DRAGWIN)
				{
					if (!uiWindowPoint(step.text, step.a, step.b, target, "dragwin"))
					{
						done = TRUE;
						break;
					}
				}
				else if (step.op == UiStep::OP_DRAGSLIDER)
				{
					// O28: to the slider position on the track (the same pixel the slider maps back to that position)
					GameWindow *win = uiFindWindow(step.text, "dragslider");
					Int x = 0, y = 0;
					if (win == nullptr || !TickSlider::screenPoint(win, (Real)step.a / 1000.0f, &x, &y))
					{
						if (win != nullptr) {
							uiLog("dragslider: %s is not a ticked slider", step.text.str());
						}
						done = TRUE;
						break;
					}
					target.x = x;
					target.y = y;
					uiLog("dragslider: %s to position %.3f", step.text.str(), (Real)step.a / 1000.0f);
				}
				else
				{
					target = uiPoint(step.a, step.b);
				}
				s_uiDragFrom = s_uiPointer;
				s_uiDragTo = target;
				s_uiPhase = 1;
			}
			const UnsignedInt span = step.c > 0 ? (UnsignedInt)step.c : 1;
			const Real f = elapsed >= span ? 1.0f : (Real)elapsed / (Real)span;
			ICoord2D p;
			p.x = s_uiDragFrom.x + (Int)((s_uiDragTo.x - s_uiDragFrom.x) * f);
			p.y = s_uiDragFrom.y + (Int)((s_uiDragTo.y - s_uiDragFrom.y) * f);
			ICoord2D delta;
			delta.x = p.x - s_uiPointer.x;
			delta.y = p.y - s_uiPointer.y;
			s_uiPointer = p;
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_LEFT_DRAG);
			msg->appendPixelArgument(s_uiPointer);
			msg->appendPixelArgument(delta);
			msg->appendIntegerArgument(0);
			if (f >= 1.0f)
			{
				uiLog("drag to %d %d", s_uiPointer.x, s_uiPointer.y);
				done = TRUE;
			}
			break;
		}
		case UiStep::OP_UP:
			uiMouse(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP);
			uiLog("up at %d %d", s_uiPointer.x, s_uiPointer.y);
			break;
		case UiStep::OP_KEY:
		{
			Int key = KEY_NONE;
			for (size_t i = 0; i < ARRAY_SIZE(s_uiKeys); ++i)
			{
				if (step.text.compareNoCase(s_uiKeys[i].name) == 0)
					key = s_uiKeys[i].key;
			}
			if (key == KEY_NONE)
			{
				uiLog("unknown key %s", step.text.str());
				break;
			}
			// down on this update, up on the next one
			done = FALSE;
			if (s_uiPhase == 0)
			{
				// O39: the focus at the moment the key goes down (see the Enter character below)
				s_uiFocusAtKeyDown = TheWindowManager != nullptr ? TheWindowManager->winGetFocus() : nullptr;
				uiKey(key, TRUE);
				s_uiPhase = 1;
			}
			else
			{
				uiKey(key, FALSE);
				// TheSuperHackers @fix (O18, D2) A real Enter also gives Windows a character (WM_CHAR '\r') that the IME
				// manager hands to the focused text entry as GWM_IME_CHAR; that ends an edit (GEM_EDIT_DONE). Do the same.
				// TheSuperHackers @fix O39 Windows hands that character to the window that had the focus when the key went
				// down. A window the key itself opened (the in-game chat: Enter opens it and gives it the focus) never gets
				// it; before, it did and the new chat box closed at once (S28: "text: no window has the keyboard focus").
				if (key == KEY_ENTER && TheWindowManager != nullptr && TheWindowManager->winGetFocus() != nullptr
					&& TheWindowManager->winGetFocus() == s_uiFocusAtKeyDown) {
					TheWindowManager->winSendInputMsg(TheWindowManager->winGetFocus(), GWM_IME_CHAR, (WindowMsgData)VK_RETURN, 0);
				}
				s_uiFocusAtKeyDown = nullptr;
				uiLog("key %s", step.text.str());
				done = TRUE;
			}
			break;
		}
		case UiStep::OP_TEXT:
		{
			GameWindow *focus = TheWindowManager != nullptr ? TheWindowManager->winGetFocus() : nullptr;
			if (focus == nullptr)
			{
				uiLog("text: no window has the keyboard focus");
				break;
			}
			for (Int i = 0; i < step.text.getLength(); ++i)
			{
				TheWindowManager->winSendInputMsg(focus, GWM_IME_CHAR, (WindowMsgData)(UnsignedByte)step.text.getCharAt(i), 0);
			}
			uiLog("text %s", step.text.str());
			break;
		}
		case UiStep::OP_SHOT:
		{
			// two drawn frames after the step starts, so the picture shows the effect of the steps before
			done = FALSE;
			if (s_uiPhase == 0)
			{
				s_uiPhase = 1;
				s_uiStepStartUpdate = TheDisplay->getRenderedFrameCount();
			}
			else if (TheDisplay->getRenderedFrameCount() >= s_uiStepStartUpdate + 2 && prepareOutput())
			{
				AsciiString filePath;
				filePath.format("%s\\%s.png", s_directory.str(), step.text.str());
				TheDisplay->takeScreenShotToFile(SCREENSHOT_PNG, filePath.str());
				uiLog("shot %s", filePath.str());
				done = TRUE;
			}
			break;
		}
		case UiStep::OP_LOADSHOT:
		{
			LoadShot shot;
			shot.name = step.text;
			shot.percent = step.a;
			s_loadShots.push_back(shot);
			uiLog("loadshot %s armed at %d%%", step.text.str(), step.a);
			break;
		}
		case UiStep::OP_EXITMATCH:
			// O43: the quit menu's Exit + Yes (exitQuitMenu): with the menu open, GameLogic::quit surrenders in a network
			// match, stops the recording and leaves to the score screen
			if (TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame())
			{
				if (canOpenQuitMenu()) {
					ToggleQuitMenu();   // as the player's ESC; without it quit() would only open the menu
				}
				uiLog("exitmatch at frame %u (%s)", TheGameLogic->getFrame(), TheGameLogic->isInMultiplayerGame() ? "multiplayer: surrender" : "not multiplayer");
				TheGameLogic->quit(FALSE);
				destroyQuitMenu();
			}
			else
			{
				uiLog("exitmatch: no match running");
			}
			break;
		case UiStep::OP_EXIT:
			uiLog("exit %d", step.a);
			s_failureExitCode = step.a;
			s_exitRequested = TRUE;
			s_uiNext = s_uiSteps.size();
			return;
		}

		if (!done)
			return;
		++s_uiNext;
		s_uiStepStarted = FALSE;
	}
}

void FrameShots::setNoImage(Bool noImage)
{
	s_noImage = noImage;
}

void FrameShots::setTestInput(const char *specs)
{
	s_testInputs.clear();
	const char *p = specs;
	while (p != nullptr && *p != '\0')
	{
		const char *end = strchr(p, ';');
		const size_t len = (end != nullptr) ? (size_t)(end - p) : strlen(p);
		char spec[128];
		const size_t copyLen = std::min(len, sizeof(spec) - 1);
		memcpy(spec, p, copyLen);
		spec[copyLen] = '\0';
		p = (end != nullptr) ? end + 1 : p + len;

		TestInputSpec in = {};
		char kind[16] = {};
		char arg[48] = {};
		unsigned from = 0, to = 0;
		if (sscanf(spec, "%15[^:]:%47[^@]@%u-%u", kind, arg, &from, &to) != 4)
			continue;
		in.from = from;
		in.to = to;
		if (stricmp(kind, "rmb") == 0)
		{
			in.kind = TestInputSpec::KIND_RMB;
			int dx = 0, dy = 0;
			if (sscanf(arg, "%d,%d", &dx, &dy) != 2)
				continue;
			in.dirX = dx;
			in.dirY = dy;
		}
		else
		{
			Int dirX = 0, dirY = 0, key = KEY_NONE;
			if (stricmp(arg, "left") == 0) { dirX = -1; key = KEY_LEFT; }
			else if (stricmp(arg, "right") == 0) { dirX = 1; key = KEY_RIGHT; }
			else if (stricmp(arg, "up") == 0) { dirY = -1; key = KEY_UP; }
			else if (stricmp(arg, "down") == 0) { dirY = 1; key = KEY_DOWN; }
			else continue;
			if (stricmp(kind, "key") == 0)
				in.kind = TestInputSpec::KIND_KEY;
			else if (stricmp(kind, "edge") == 0)
				in.kind = TestInputSpec::KIND_EDGE;
			else
				continue;
			in.key = key;
			in.dirX = dirX;
			in.dirY = dirY;
		}
		s_testInputs.push_back(in);
	}
}

void FrameShots::setTestLogicFps(Int fps)
{
	s_testLogicFps = fps;
}

void FrameShots::injectTestInput()
{
	runUiScript();

	if (s_testLogicFps > 0 && TheGameLogic != nullptr && TheGameLogic->isInGame() && TheFramePacer != nullptr)
		TheFramePacer->setLogicTimeScaleFps(s_testLogicFps);

	if (s_testInputs.empty() || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheMessageStream == nullptr)
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	const Int width = TheDisplay->getWidth();
	const Int height = TheDisplay->getHeight();
	ICoord2D center;
	center.x = width / 2;
	center.y = height / 2;

	for (size_t i = 0; i < s_testInputs.size(); ++i)
	{
		TestInputSpec &in = s_testInputs[i];
		const Bool wantActive = (frame >= in.from && frame < in.to);

		switch (in.kind)
		{
		case TestInputSpec::KIND_KEY:
			if (wantActive != in.active)
			{
				GameMessage *msg = TheMessageStream->appendMessage(wantActive ? GameMessage::MSG_RAW_KEY_DOWN : GameMessage::MSG_RAW_KEY_UP);
				msg->appendIntegerArgument(in.key);
				msg->appendIntegerArgument(wantActive ? KEY_STATE_DOWN : KEY_STATE_UP);
			}
			break;

		case TestInputSpec::KIND_RMB:
			{
				if (wantActive != in.active)
				{
					GameMessage *msg = TheMessageStream->appendMessage(wantActive ? GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN : GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP);
					msg->appendPixelArgument(center);
					msg->appendIntegerArgument(0);
					msg->appendIntegerArgument((Int)timeGetTime());
				}
				ICoord2D pos = center;
				if (wantActive)
				{
					pos.x += in.dirX;
					pos.y += in.dirY;
				}
				if (wantActive || in.active)
				{
					GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_POSITION);
					msg->appendPixelArgument(pos);
					msg->appendIntegerArgument(0);
				}
			}
			break;

		case TestInputSpec::KIND_EDGE:
			{
				// The messages are translated after this call, so the pointer's move away from the edge must still be
				// handled with the mode forced. It is released on the next update.
				if (TheLookAtTranslator != nullptr)
				{
					if (wantActive && !in.active)
						TheLookAtTranslator->setTestForceScreenEdge(true);
					else if (!wantActive && in.active)
						in.pendingRelease = TRUE;
					else if (!wantActive && in.pendingRelease)
					{
						TheLookAtTranslator->setTestForceScreenEdge(false);
						in.pendingRelease = FALSE;
					}
				}
				ICoord2D pos = center;
				if (wantActive)
				{
					pos.x = (in.dirX < 0) ? 0 : (in.dirX > 0) ? width - 1 : center.x;
					pos.y = (in.dirY < 0) ? 0 : (in.dirY > 0) ? height - 1 : center.y;
				}
				if (wantActive || in.active)
				{
					GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_POSITION);
					msg->appendPixelArgument(pos);
					msg->appendIntegerArgument(0);
				}
			}
			break;
		}
		in.active = wantActive;
	}
}

void FrameShots::setDirectory(const char *path)
{
	s_directory = path;
}

void FrameShots::setExitWhenDone(Bool exitWhenDone)
{
	s_exitWhenDone = exitWhenDone;
}

void FrameShots::beginGame()
{
	s_gameStarted = TRUE;
	s_gameStartTicks = queryTicks();
	s_engineUpdatesSinceGameStart = 0;
}

void FrameShots::onLogicUpdated()
{
	if (!s_enabled)
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();

	// The time base is the first logic update of a game. A new game counts its frames from the start again.
	if (!s_gameStarted || frame < s_lastLogicFrame)
	{
		beginGame();
	}
	s_lastLogicFrame = frame;

	// Every listed frame at or below the current frame becomes pending. The logic frame grows by one per
	// update and the comparison is <=, so no listed frame is skipped, even when more than one logic update
	// passes before the next render; those frames then share the first render after them.
	Bool reachedListedFrame = FALSE;
	while (s_nextFrameIndex < s_frames.size() && s_frames[s_nextFrameIndex] <= frame)
	{
		s_pendingFrames.push_back(s_frames[s_nextFrameIndex]);
		++s_nextFrameIndex;
		reachedListedFrame = TRUE;
	}

	if (reachedListedFrame && TheDisplay != nullptr)
	{
		s_renderCountAtLogicUpdate = TheDisplay->getRenderedFrameCount();
	}

	// Test saves run here, between two logic updates, as a save from the menu does. A listed frame below the
	// current one was never reached in this game (it started later, for example from a save) and is skipped.
	while (s_nextSaveIndex < s_saveFrames.size() && s_saveFrames[s_nextSaveIndex] <= frame)
	{
		const UnsignedInt saveFrame = s_saveFrames[s_nextSaveIndex];
		++s_nextSaveIndex;
		if (saveFrame == frame)
		{
			writeSave(frame);
		}
		else
		{
			DEBUG_LOG(("FrameShots: save frame %u skipped, the game is at frame %u", saveFrame, frame));
		}
	}
}

void FrameShots::writeSave(UnsignedInt frame)
{
	AsciiString fileName;
	fileName.format("frame_%08u.sav", frame);
	UnicodeString description;
	description.format(L"Test save at logic frame %u", frame);

	const SaveResult result = TheGameState->saveGame(fileName, description, SAVE_FILE_TYPE_NORMAL);
	DEBUG_LOG(("FrameShots: save at frame %u to '%s', code %d", frame, fileName.str(), (Int)result.saveCode));

	if (!prepareOutput())
		return;
	if (s_savesCsv == nullptr)
	{
		AsciiString csvPath = s_directory;
		csvPath.concat("\\saves.csv");
		s_savesCsv = fopen(csvPath.str(), "w");
		if (s_savesCsv == nullptr)
			return;
		fprintf(s_savesCsv, "logic_frame,save_code,file\n");
	}
	fprintf(s_savesCsv, "%u,%d,%s\n", frame, (Int)result.saveCode, TheGameState->getFilePathInSaveDirectory(fileName).str());
	fflush(s_savesCsv);
}

void FrameShots::onLoadScreenDrawn(Int percent)
{
	if (s_loadShots.empty() || TheDisplay == nullptr || !prepareOutput())
		return;
	// The picture is read back from the frame the loading screen has just presented.
	for (size_t i = 0; i < s_loadShots.size(); )
	{
		if (percent >= s_loadShots[i].percent)
		{
			AsciiString filePath;
			filePath.format("%s\\%s.png", s_directory.str(), s_loadShots[i].name.str());
			TheDisplay->takeScreenShotToFile(SCREENSHOT_PNG, filePath.str());
			uiLog("loadshot %s at %d%%", filePath.str(), percent);
			s_loadShots.erase(s_loadShots.begin() + i);
			break;   // one picture per drawn frame
		}
		++i;
	}
}

void FrameShots::onClientUpdated()
{
	if (!s_enabled)
		return;

	if (s_gameStarted)
	{
		++s_engineUpdatesSinceGameStart;
	}

	if (s_exitRequested)
	{
		// Wait until the screenshot threads have written their files before the process ends.
		if (TheDisplay == nullptr || !TheDisplay->isScreenShotWritePending())
		{
			s_exitRequested = FALSE;
			s_enabled = FALSE;
			exitGame();
		}
		return;
	}

	if (s_pendingFrames.empty() || TheDisplay == nullptr)
		return;

	// Capture after the first render that completed after the logic update which reached the listed frame.
	// The image is read back from the last presented frame, as the screenshot key does after the client
	// update, so it includes the HUD. A render that was skipped (minimized window, time multiplier) does not
	// count. When the render rate is above the logic rate, this render draws the logic frame phase
	// FramePacer::getLogicFramePhase() = min(1, logic fps / update fps), for example 0.5 at 60 render fps and
	// 30 logic fps: the client side interpolation (drawable tilt, particles) is halfway from the previous
	// logic frame to the listed one. At or below the logic rate the phase is 1, the exact logic frame.
	if (TheDisplay->getRenderedFrameCount() == s_renderCountAtLogicUpdate)
		return;

	for (size_t i = 0; i < s_pendingFrames.size(); ++i)
	{
		capture(s_pendingFrames[i]);
	}
	s_pendingFrames.clear();

	if (s_exitWhenDone && s_nextFrameIndex >= s_frames.size() && s_nextSaveIndex >= s_saveFrames.size())
	{
		s_exitRequested = TRUE;
	}
}

Bool FrameShots::prepareOutput()
{
	if (s_outputReady)
		return TRUE;
	if (s_outputFailed)
		return FALSE;

	if (s_directory.isEmpty())
	{
		s_directory = TheGlobalData->getPath_UserData();
		s_directory.concat("FrameShots");
	}
	while (s_directory.endsWith("\\") || s_directory.endsWith("/"))
	{
		s_directory.removeLastChar();
	}
	createDirectories(s_directory);

	AsciiString csvPath = s_directory;
	csvPath.concat("\\frames.csv");
	s_csv = fopen(csvPath.str(), "w");
	if (s_csv == nullptr)
	{
		s_outputFailed = TRUE;
		return FALSE;
	}
	fprintf(s_csv, "logic_frame,ms_since_game_start,engine_updates_since_game_start,render_fps_limit,"
		"actual_update_fps_limit,logic_time_scale_fps,actual_logic_time_scale_fps,current_logic_frame,logic_frame_phase,"
		"camera_x,camera_y,camera_z,camera_zoom,camera_angle,camera_pitch,update_time_ms,base_ratio,render_frames,"
		"engine_updates,eng_ms,logic_ms,logic_updates,client_ms,draw_ms,present_ms,other_ms\n");
	fflush(s_csv);

	s_outputReady = TRUE;
	return TRUE;
}

void FrameShots::capture(UnsignedInt listedFrame)
{
	if (!prepareOutput())
		return;

	if (!s_noImage)
	{
		AsciiString filePath;
		filePath.format("%s\\frame_%08u.png", s_directory.str(), listedFrame);
		TheDisplay->takeScreenShotToFile(SCREENSHOT_PNG, filePath.str());
	}

	const double ms = ticksToMilliseconds(queryTicks() - s_gameStartTicks);
	Coord3D cam = {};
	Real camZoom = 0.0f, camAngle = 0.0f, camPitch = 0.0f;
	if (TheTacticalView != nullptr)
	{
		cam = TheTacticalView->getPosition();
		camZoom = TheTacticalView->getZoom();
		camAngle = TheTacticalView->getAngle();
		camPitch = TheTacticalView->getPitch();
	}
	// TheSuperHackers @feature The means per engine update since the previous row (FrameTiming.h, item 22).
	const FrameTiming::Means timing = FrameTiming::takeMeans();
	fprintf(s_csv, "%u,%.3f,%u,%d,%d,%d,%d,%u,%.3f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.4f,%.5f,%u,%u,%.3f,%.3f,%.2f,%.3f,%.3f,%.3f,%.3f\n",
		listedFrame,
		ms,
		s_engineUpdatesSinceGameStart,
		TheFramePacer->getFramesPerSecondLimit(),
		TheFramePacer->getActualFramesPerSecondLimit(),
		TheFramePacer->getLogicTimeScaleFps(),
		TheFramePacer->getActualLogicTimeScaleFps(),
		TheGameLogic->getFrame(),
		(double)TheFramePacer->getLogicFramePhase(),
		(double)cam.x, (double)cam.y, (double)cam.z, (double)camZoom, (double)camAngle, (double)camPitch,
		(double)TheFramePacer->getUpdateTime() * 1000.0,
		(double)TheFramePacer->getBaseOverUpdateFpsRatio(),
		(unsigned)TheDisplay->getRenderedFrameCount(),
		timing.engineUpdates, timing.engineMs, timing.logicMs, timing.logicUpdates, timing.clientMs, timing.drawMs,
		timing.presentMs, timing.otherMs);
	fflush(s_csv);
}

void FrameShots::failStartup(const char *message, Int exitCode)
{
	DEBUG_LOG(("Startup failure: %s", message));
	s_failureExitCode = (exitCode != 0) ? exitCode : 1;
	if (!s_enabled)
	{
		// A person is probably watching. With -shotFrames nobody may be there, so no dialog then.
		MessageBoxA(nullptr, message, "Command & Conquer", MB_OK | MB_ICONERROR);
	}
	if (TheGameEngine != nullptr)
	{
		TheGameEngine->setQuitting(TRUE);
	}
}

void FrameShots::exitGame()
{
	if (s_csv != nullptr)
	{
		fclose(s_csv);
		s_csv = nullptr;
	}
	if (s_savesCsv != nullptr)
	{
		fclose(s_savesCsv);
		s_savesCsv = nullptr;
	}

	// The normal quit path, as used by the benchmark timer in GameEngine::execute.
	if (TheGameLogic->isInGame())
	{
		if (TheRecorder != nullptr && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
		{
			TheRecorder->stopRecording();
		}
		TheGameLogic->clearGameData();
	}
	TheGameEngine->setQuitting(TRUE);
}
