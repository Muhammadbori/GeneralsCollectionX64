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

// HotkeyLetters.h //////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O49 (row 36) The hotkey letter of each command button, drawn in its top right corner at run
// time: the same letter the game binds (HotKeyManager::searchHotKey: the character after '&' in the button's text
// label of the string file in use), so it is right for every hotkey set and both languages; nothing where a button has
// no hotkey. Settings -> Controls: "Show hotkey letters on buttons" (Options.ini ShowHotkeyLetters, default yes);
// screen side only, so it can change in a match. It replaces the "Original + letters on icons" pack, whose letters were
// drawn into the icons for another layout.

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

class GameWindow;

namespace HotkeyLetters
{
	/// ControlBar::setControlCommand: 'button' now carries 'command' with the text label 'label' (as fetched).
	void setButton(GameWindow *button, const void *command, const UnicodeString &label);
	/// The push button draw: the letter over the button at x, y (w x h), when it still carries the same command.
	void draw(GameWindow *button, Int x, Int y, Int w, Int h);
	/// ControlBar::reset and its destructor: forgets every button.
	void clear();
	void setEnabled(Bool on);
	Bool enabled();   ///< read from Options.ini the first time
}
