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

// TheSuperHackers @feature See HotkeyLetters.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/HotkeyLetters.h"

#include "Common/OptionPreferences.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"

#include <map>
#include <wctype.h>

namespace
{
	struct Entry
	{
		const void *command;
		WideChar letter;
		DisplayString *text;
		Int fontSize;
	};
	std::map<const GameWindow *, Entry> s_entries;
	Int s_enabled = -1;   ///< -1: not read yet

	void freeText(Entry &e)
	{
		if (e.text != nullptr && TheDisplayStringManager != nullptr) {
			TheDisplayStringManager->freeDisplayString(e.text);
		}
		e.text = nullptr;
	}

	/// The character after the first single '&' ("&&" is a plain '&'), upper case; 0: none.
	WideChar letterOf(const UnicodeString &label)
	{
		for (const WideChar *p = label.str(); p != nullptr && *p != 0; ++p)
		{
			if (*p != L'&') {
				continue;
			}
			if (p[1] == L'&') {
				++p;
				continue;
			}
			const WideChar c = p[1];
			return (c > L' ') ? (WideChar)towupper(c) : 0;
		}
		return 0;
	}
}

void HotkeyLetters::setButton(GameWindow *button, const void *command, const UnicodeString &label)
{
	if (button == nullptr) {
		return;
	}
	Entry &e = s_entries[button];
	const WideChar letter = letterOf(label);
	if (e.command != command || e.letter != letter) {
		freeText(e);
	}
	e.command = command;
	e.letter = letter;
}

void HotkeyLetters::clear()
{
	for (std::map<const GameWindow *, Entry>::iterator it = s_entries.begin(); it != s_entries.end(); ++it) {
		freeText(it->second);
	}
	s_entries.clear();
}

void HotkeyLetters::setEnabled(Bool on)
{
	s_enabled = on ? 1 : 0;
}

Bool HotkeyLetters::enabled()
{
	if (s_enabled < 0)
	{
		OptionPreferences pref;
		OptionPreferences::const_iterator it = pref.find("ShowHotkeyLetters");
		s_enabled = (it != pref.end() && stricmp(it->second.str(), "no") == 0) ? 0 : 1;
	}
	return s_enabled != 0;
}

void HotkeyLetters::draw(GameWindow *button, Int x, Int y, Int w, Int h)
{
	if (button == nullptr || s_entries.empty() || !enabled() || TheDisplay == nullptr || TheDisplayStringManager == nullptr
		|| TheWindowManager == nullptr) {
		return;
	}
	std::map<const GameWindow *, Entry>::iterator it = s_entries.find(button);
	if (it == s_entries.end()) {
		return;
	}
	Entry &e = it->second;
	// the window still carries that command (a window of another screen at the same address never gets a letter)
	if (e.letter == 0 || e.command == nullptr || GadgetButtonGetData(button) != e.command) {
		return;
	}
	// a quarter of the button's height, at least 8: readable at 800x600, small at 4K
	const Int size = max(8, h / 4);
	if (e.text == nullptr || e.fontSize != size)
	{
		freeText(e);
		e.text = TheDisplayStringManager->newDisplayString();
		if (e.text == nullptr) {
			return;
		}
		e.text->setFont(TheWindowManager->winFindFont(AsciiString("Arial"), size, TRUE));
		UnicodeString t;
		t.concat(e.letter);
		e.text->setText(t);
		e.fontSize = size;
	}
	Int tw = 0, th = 0;
	e.text->getSize(&tw, &th);
	if (tw <= 0 || th <= 0 || tw + 6 > w || th + 2 > h) {
		return;
	}
	// a dark plate in the top right corner, the letter white with a black edge
	const Int px = x + w - tw - 4;
	const Int py = y + 1;
	TheDisplay->drawFillRect(px - 2, py, tw + 4, th, GameMakeColor(0, 0, 0, 170));
	e.text->draw(px, py, GameMakeColor(255, 255, 255, 255), GameMakeColor(0, 0, 0, 255));
}
