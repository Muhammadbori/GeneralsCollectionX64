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

// TickSlider.h /////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O25 (items 28, 29) The slider look the user chose (variant 2 of the mock sheet): one thin track,
// the part left of the handle filled in blue, a large round handle (lit while held), the value written above the handle
// and a tick under the track at every step (or at a few marks of a continuous range) with its label.
//
// It is the game's own horizontal slider gadget with another draw and input function: SliderData, GSM_SET_SLIDER,
// GSM_SET_MIN_MAX, GadgetSliderGetPosition and the GSM_SLIDER_TRACK message to the owner stay as they were, so the code
// that reads a slider does not change. The thumb button of the gadget is hidden; the whole window takes the clicks.
// Behaviour: the handle follows the mouse smoothly; a stepped slider shows (and reports) the step it would take while
// dragged and snaps to it on release; a click on the track or on a tick label jumps there (a click on the handle keeps
// it where it is); a continuous slider stays where it is dropped; left / right keys move one step.
// Everything is drawn (no art files). The window's height decides what is shown: the value line above, the handle and
// track, the label line below; a low window drops the value line first (the menu shows the value elsewhere), then the
// labels.

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

class GameWindow;
class GameFont;

namespace TickSlider
{
	typedef UnicodeString (*ValueTextFunc)(Int position, void *context);

	/// Turns a horizontal slider gadget into the ticked slider. 'continuous': a range of many values (a volume) that stays
	/// where it is dropped, with tick marks at 0, 25, 50, 75 and 100 % of the range; else a mark at every step.
	void attach(GameWindow *slider, GameFont *font, Bool continuous);
	Bool isAttached(GameWindow *slider);

	/// The labels of the marks: one per step from the minimum (stepped), or one per mark (continuous, five marks). Labels
	/// that would touch their neighbours are left out (the first and the last are kept); no labels: numbers.
	void setLabels(GameWindow *slider, const UnicodeString *labels, Int count);
	/// The text above the handle; nullptr: the label of the step, or the number (with 'suffix', e.g. " %").
	void setValueText(GameWindow *slider, ValueTextFunc func, void *context);
	void setSuffix(GameWindow *slider, const UnicodeString &suffix);

	/// The window height for the full look (value line, handle, label line) in 'font'.
	Int fullHeight(GameFont *font);
	/// The height of the handle row alone (no value, no labels).
	Int handleHeight(GameFont *font);
	/// The track's centre line, in pixels below the slider window's top (to line up a title or a check box with it).
	Int trackCenter(GameWindow *slider);
}
