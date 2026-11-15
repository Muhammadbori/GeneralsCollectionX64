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

// OptionsSections.h ////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The options screen in sections. OptionsMenu.wnd is unchanged: after the menu has set up its
// controls, init() makes a column of section buttons and one page per section in the area of the old groups, moves the
// controls of the old groups (and of the "custom detail" panel) onto the pages, and adds controls for the settings that
// were Options.ini keys only. The section buttons, the pages and the new controls copy the look of the menu's own
// buttons, group frames, sliders, check boxes and labels, so the screen keeps the game's style at any resolution.
// The menu calls: init() at the end of its init, onSystem() first in its system callback, save() first in its save,
// setDefaults() at the end of its defaults, shutdown() when it closes.

#pragma once

#include "Lib/BaseType.h"
#include "GameClient/GameWindow.h"

class OptionPreferences;

namespace OptionsSections
{
	/// 'pref' stays the menu's preferences until shutdown(). 'zeroHour' adds the Zero Hour only check boxes.
	void init(OptionPreferences *pref);
	void shutdown();
	Bool isActive();

	/// A system message of the options window. TRUE when it was one of the new controls or section buttons (handled).
	/// A detail level change returns FALSE: the menu handles it as well.
	Bool onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2);

	/// Writes the new controls into 'pref' and applies the keys the menu's save does not apply (render fps limit, snow,
	/// camera, counter look, screenshot format). The menu's save then applies the rest from 'pref' as before.
	void save(OptionPreferences *pref);

	void setDefaults();
}
