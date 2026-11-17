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
// TheSuperHackers @feature The replay control bar (item 15; O61, row 48: the watch panel).
//
// In replay playback (and in a followed replay: a LAN room observer, an outside viewer, -followReplay) a small panel is
// docked at the right edge of the screen, under the top counters (it follows OwnUiScale and stays clear of the counters
// and of the money list on the left):
//   - the time "now / total" (in a followed replay the total is the live point). A click opens the seek line under the
//     panel: the track from 0 to the total, the handle at the time shown, the time above the handle while it is dragged;
//     on release the replay goes there. Forward it runs at Max to that frame; backward it restarts the replay from its
//     first frame (the map loads again) and runs at Max to it, with "Seeking..." (Russian "Peremotka...") on screen. While a seek runs, the bar
//     and the pause, step and fast forward keys do nothing. No key seeks;
//   - in a followed replay the delay behind the live point, "-0:10", or the live mark "LIVE" at the live point. A click
//     on the delay runs at Max back to the live point (the newest frame the viewer may have: an outside viewer is always
//     the host's delay behind the match, and its own play-out reserve);
//   - for an outside viewer, the host's delay in words ("Host delay: 1 min"), apart from its own minus value;
//   - Pause / Play: the same message as the key (MSG_META_TOGGLE_PAUSE), so key and button drive one state;
//   - one speed button: 1x -> 2x -> 4x -> 8x -> 1x, showing the speed. Max (as fast as possible, GlobalData::
//     m_TiVOFastMode) is not on the button; the fast forward key still toggles 1x <-> Max (the button then shows "Max",
//     and a click goes to 1x). 2x..8x multiply the logic steps per second (FramePacer::setReplaySpeedFactor). The step
//     key (MSG_META_STEP_FRAME) works as before.
// O61 (row 49): a followed replay that starts behind its live point (an outside viewer coming into a running match)
// runs at Max to the live point first ("Catching up with live..."), once; after that the viewer's own pauses and seeks stand.
// Nothing of it acts outside replays. -noReplayBar hides the panel (the keys still work); the screenshot runs of
// dx12-tools\shotrun.py pass it, so their pictures stay comparable with older builds.
//
// Window names for -uiScript clickwin: ReplayControlBar.wnd:ButtonPause, :ButtonSpeed, :ButtonTime, :ButtonDelay,
// :SeekLine, :TextHostDelay, :TextSeek.

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

namespace ReplayControl
{
	enum { SPEED_MAX = 0 };   ///< as fast as possible

	void disableBar();   ///< -noReplayBar
	/// -replaySpeed <1|2|4|8|max> (test): the speed a replay plays at from its first frame (O18 proof runs).
	void configureStartSpeed(Int speed);

	/// 1, 2, 4, 8 or SPEED_MAX.
	Int speed();
	void setSpeed(Int speed);
	/// MSG_META_TOGGLE_FAST_FORWARD_REPLAY: 1x <-> Max (any other speed goes to 1x), with the old message.
	void toggleFastForward();

	/// Run at Max to 'frame' and pause there (backward: the replay restarts from its first frame).
	Bool goToFrame(UnsignedInt frame);
	/// O61: go to 'frame' and play on as before the seek (paused stays paused). Forward at Max; backward the replay
	/// restarts from its first frame. Clamped to the total (followed: the live point).
	Bool seekTo(UnsignedInt frame);
	/// O61: TRUE while a seek runs (forward, backward, back to live): the keys and the panel wait.
	Bool isSeeking();
	/// O61: from the engine's update (Win32GameEngine::update), outside the client's update: a requested backward seek
	/// ends the playback and starts the replay again.
	void pollRewind();

	/// "mm:ss", "h:mm:ss" or "m" (minutes) to a logic frame. FALSE when it is not a time.
	Bool parseTime(const UnicodeString &text, UnsignedInt *frame);
	/// "m:ss", or "h:mm:ss" from one hour.
	UnicodeString timeText(UnsignedInt frame);

	/// Every client update (InGameUI::update): makes, refreshes or removes the panel, ends a seek.
	void update();
}
