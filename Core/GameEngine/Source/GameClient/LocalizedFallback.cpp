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

// LocalizedFallback.cpp ////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See LocalizedFallback.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/LocalizedFallback.h"

#include "Common/Registry.h"
#include "GameClient/GameText.h"

namespace
{
	Bool hasCyrillic(const UnicodeString &text)
	{
		for (Int i = 0; i < text.getLength(); ++i)
		{
			const WideChar c = text.getCharAt(i);
			if (c >= 0x0400 && c <= 0x04FF) {
				return TRUE;
			}
		}
		return FALSE;
	}
}

Bool isRussianText()
{
	static Int s_russian = -1;
	if (s_russian < 0)
	{
		if (stricmp(GetRegistryLanguage().str(), "russian") == 0) {
			s_russian = 1;
		} else if (TheGameText == nullptr) {
			return FALSE;   // not decided yet: the string file is not loaded
		} else {
			// Labels every Generals string file has.
			s_russian = (hasCyrillic(TheGameText->fetch("GUI:Options")) || hasCyrillic(TheGameText->fetch("GUI:Accept"))) ? 1 : 0;
		}
	}
	return s_russian == 1;
}

const WideChar *localizedFallback(const WideChar *english, const WideChar *russian)
{
	return isRussianText() ? russian : english;
}

namespace
{
	UnicodeString fetchedOr(const char *label, const WideChar *english, const WideChar *russian)
	{
		if (isRussianText())
			return UnicodeString(russian);
		return TheGameText != nullptr ? TheGameText->FETCH_OR_SUBSTITUTE(label, english) : UnicodeString(english);
	}
}

UnicodeString FastForwardText::instructions()
{
	return fetchedOr("GUI:FastForwardInstructions", L"Press F to toggle Fast Forward", L"\x041A\x043B\x0430\x0432\x0438\x0448\x0430 F: \x0443\x0441\x043A\x043E\x0440\x0435\x043D\x043D\x0430\x044F \x043F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0430");
}

UnicodeString FastForwardText::on()
{
	return fetchedOr("GUI:FF_ON", L"Fast Forward is on", L"\x0423\x0441\x043A\x043E\x0440\x0435\x043D\x043D\x0430\x044F \x043F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0430 \x0432\x043A\x043B\x044E\x0447\x0435\x043D\x0430");
}

UnicodeString FastForwardText::off()
{
	return fetchedOr("GUI:FF_OFF", L"Fast Forward is off", L"\x0423\x0441\x043A\x043E\x0440\x0435\x043D\x043D\x0430\x044F \x043F\x0435\x0440\x0435\x043C\x043E\x0442\x043A\x0430 \x0432\x044B\x043A\x043B\x044E\x0447\x0435\x043D\x0430");
}
