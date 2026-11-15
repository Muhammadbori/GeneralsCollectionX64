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

// LocalizedFallback.h //////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The fallback text of a label that is not in the game's string file (CSF): English, or
// Russian when the game shows Russian text. Use it as the substitute of TheGameText->FETCH_OR_SUBSTITUTE, so a string
// file that has the label still wins. The game is Russian when its language is "russian", or when its string file is
// Russian (the one-folder Russian collection keeps the Russian text under Data\English, so the language reads english).

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

const WideChar *localizedFallback(const WideChar *english, const WideChar *russian);
Bool isRussianText();   ///< TRUE when the game shows Russian text (decided once)

namespace FastForwardText
{
	// TheSuperHackers @fix (O18, S18) The replay fast forward texts. The Russian string file has these labels with the
	// English text (GUI:FastForwardInstructions, GUI:FF_ON, GUI:FF_OFF), so a Russian game gets its Russian text here
	// and any other game the string file's text.
	UnicodeString instructions();
	UnicodeString on();
	UnicodeString off();
}
