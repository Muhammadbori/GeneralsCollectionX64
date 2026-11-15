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

// ReplayControlBar.h ///////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The replay control bar (item 15).
//
// In replay playback (and in a followed replay, -followReplay) a small bar at the top centre of the screen has:
//   - Pause / Play, and Step (one logic frame): they send the same messages as the keys (MSG_META_TOGGLE_PAUSE,
//     MSG_META_STEP_FRAME), so keys and buttons drive one state;
//   - the speeds 1x, 2x, 4x, 8x and Max (as fast as possible). Max is the old fast forward (GlobalData::m_TiVOFastMode),
//     and MSG_META_TOGGLE_FAST_FORWARD_REPLAY toggles between 1x and Max as before (from 2x..8x it goes to 1x);
//     2x..8x multiply the logic steps per second (FramePacer::setReplaySpeedFactor). One engine update runs at most
//     FramePacer::MaxLogicFramesPerRenderUpdate logic updates, so 8x needs a render rate of 60 fps or more;
//   - the time to go to (mm:ss, or h:mm:ss) and "Go to": the replay runs at Max to that time and pauses there. A replay
//     cannot go back: an earlier time only shows a message;
//   - the game time and the replay's length (in a followed replay: the writer's newest frame) and, when following, the
//     delay behind the live game.
// Nothing of it acts outside replays. -noReplayBar hides the bar (the keys still work); the screenshot runs of
// dx12-tools\shotrun.py pass it, so their pictures stay comparable with older builds.
//
// Window names for -uiScript clickwin: ReplayControlBar.wnd:ButtonPause, :ButtonStep, :ButtonSpeed1, :ButtonSpeed2,
// :ButtonSpeed4, :ButtonSpeed8, :ButtonSpeedMax, :EntryTime, :ButtonGoTo, :TextTime.

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

namespace ReplayControl
{
	enum { SPEED_MAX = 0 };   ///< as fast as possible

	void disableBar();   ///< -noReplayBar

	/// 1, 2, 4, 8 or SPEED_MAX.
	Int speed();
	void setSpeed(Int speed);
	/// MSG_META_TOGGLE_FAST_FORWARD_REPLAY: 1x <-> Max (any other speed goes to 1x), with the old message.
	void toggleFastForward();

	/// Run at Max to 'frame' and pause there. FALSE (and a message) when the replay is past it.
	Bool goToFrame(UnsignedInt frame);
	/// "mm:ss", "h:mm:ss" or "m" (minutes) to a logic frame. FALSE when it is not a time.
	Bool parseTime(const UnicodeString &text, UnsignedInt *frame);
	/// "m:ss", or "h:mm:ss" from one hour.
	UnicodeString timeText(UnsignedInt frame);

	/// Every client update (InGameUI::update): makes, refreshes or removes the bar, ends a "go to".
	void update();
}
