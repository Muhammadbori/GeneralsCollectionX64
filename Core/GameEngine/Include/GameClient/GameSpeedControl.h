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
		Int logicFps, Bool normal, Int checkBoxWidth = 160, GameWindow *rightNeighbour = nullptr);   ///< checkBoxWidth: the room right of the text for the box; rightNeighbour: the next control of the row (its label's text must not be touched)
	/// The menu's shutdown: destroys the Normal box (made here, not by the layout) and forgets the windows. The layout is
	/// not always destroyed after a shutdown (the shell only hides it when a game starts and runs its init again after
	/// the game), so a box only forgotten stayed behind, outside the menu's transition, and the next init added another.
	void reset();
	/// O24: the Normal box is made hidden; the menu shows it once its own enter transition is over (it takes no part in
	/// the transition) and it goes with reset() before the leave transition starts.
	void setShown(Bool shown);

	Bool onSliderTrack(GameWindow *control, Int position);   ///< GSM_SLIDER_TRACK; TRUE when it was this slider
	Bool onSelected(GameWindow *control);                    ///< GBM_SELECTED; TRUE when it was the check box

	Int getLogicFps() const;        ///< the speed to play at: 1x while Normal is checked
	Int getSliderLogicFps() const;  ///< the slider's own speed, kept while Normal is checked
	Bool isNormal() const { return m_normal; }
	/// O25: the slider draws the value above its handle, so the separate speed text is hidden (keep it hidden).
	Bool isTextHidden() const { return m_textHidden; }

private:
	void apply();

	GameWindow *m_slider;
	GameWindow *m_text;
	GameWindow *m_checkBox;
	Int m_sliderStep;
	Bool m_normal;
	Bool m_shown;
	Bool m_textHidden;
};
