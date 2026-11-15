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

// GameSpeedControl.h ///////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The game speed as a multiplier of the normal LOGICFRAMES_PER_SECOND logic steps per second
// (0.5x to 3x in steps of 0.5x), and the slider of a menu that sets it together with a "Normal" check box: checked, the
// speed is 1x and the slider cannot be moved; unchecked, the slider sets it. The speed is the number of logic steps per
// second only; it never changes the render frame limit. Used by the skirmish menus; meant for any menu with a speed
// slider (the generals challenge screen).

#pragma once

#include "Common/GameCommon.h"
#include "Common/UnicodeString.h"

class GameWindow;

namespace GameSpeed
{
	enum
	{
		StepLogicFps = LOGICFRAMES_PER_SECOND / 2,   ///< one step is 0.5x
		MinStep = 1,                                  ///< 0.5x
		NormalStep = 2,                               ///< 1x
		MaxStep = 6,                                  ///< 3x
	};

	Int stepToLogicFps(Int step);
	/// The nearest step of a logic time scale fps. 1000 and above, and 61 (the old top slider position "UNCAP" as
	/// Skirmish.ini stored it, which meant the normal speed) are the normal step.
	Int logicFpsToStep(Int logicFps);
	/// "0.5x", "1x", "1.5x" ... "3x".
	UnicodeString multiplierText(Int logicFps);
	/// The fallback text of a label that is not in the game's string file: English, or Russian for a Russian game.
	const WideChar *localized(const WideChar *english, const WideChar *russian);
}

class GameSpeedControl
{
public:
	GameSpeedControl();

	/// Takes over a menu's speed slider and its text: the slider's range becomes the speed steps, and a "Normal" check box
	/// is made right of the text, looking like 'checkBoxTemplate' (a check box of the same menu; the default look without
	/// one). 'checkBoxName' is its window name for its id, e.g. "SkirmishGameOptionsMenu.wnd:CheckBoxGameSpeedNormal".
	void init(GameWindow *parent, GameWindow *slider, GameWindow *text, GameWindow *checkBoxTemplate, const char *checkBoxName,
		Int logicFps, Bool normal, Int checkBoxWidth = 160);   ///< checkBoxWidth: the room right of the text for the box
	void reset();   ///< forget the windows (the menu's layout is going away)

	Bool onSliderTrack(GameWindow *control, Int position);   ///< GSM_SLIDER_TRACK; TRUE when it was this slider
	Bool onSelected(GameWindow *control);                    ///< GBM_SELECTED; TRUE when it was the check box

	Int getLogicFps() const;        ///< the speed to play at: 1x while Normal is checked
	Int getSliderLogicFps() const;  ///< the slider's own speed, kept while Normal is checked
	Bool isNormal() const { return m_normal; }

private:
	void apply();

	GameWindow *m_slider;
	GameWindow *m_text;
	GameWindow *m_checkBox;
	Int m_sliderStep;
	Bool m_normal;
};
