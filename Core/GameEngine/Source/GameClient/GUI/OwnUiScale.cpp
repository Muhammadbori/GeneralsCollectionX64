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

// TheSuperHackers @feature See OwnUiScale.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/OwnUiScale.h"

#include "Common/OptionPreferences.h"
#include "GameClient/GameFont.h"

namespace
{
	const Real kDefaultScale = 0.82f;
	const Real kLowest = 0.6f;
	const Real kHighest = 1.2f;
	Real s_scale = -1.0f;   ///< < 0: not read yet
}

void OwnUi::reload()
{
	s_scale = kDefaultScale;
	OptionPreferences pref;
	OptionPreferences::const_iterator it = pref.find("OwnUiScale");
	if (it != pref.end())
	{
		const Real value = (Real)atof(it->second.str());
		if (value > 0.0f) {
			s_scale = value < kLowest ? kLowest : (value > kHighest ? kHighest : value);
		}
	}
}

Real OwnUi::scale()
{
	if (s_scale < 0.0f) {
		reload();
	}
	return s_scale;
}

Int OwnUi::size(Int pixels)
{
	const Int scaled = REAL_TO_INT_FLOOR(pixels * scale() + 0.5f);
	return scaled > 0 ? scaled : (pixels > 0 ? 1 : pixels);
}

GameFont *OwnUi::font(GameFont *font)
{
	if (font == nullptr || TheFontLibrary == nullptr) {
		return font;
	}
	enum { MIN_POINTS = 8 };
	Int points = REAL_TO_INT_FLOOR(font->pointSize * scale() + 0.5f);
	if (points < MIN_POINTS) {
		points = font->pointSize < MIN_POINTS ? font->pointSize : (Int)MIN_POINTS;
	}
	if (points == font->pointSize) {
		return font;
	}
	GameFont *scaled = TheFontLibrary->getFont(font->nameString, points, font->bold);
	return scaled != nullptr ? scaled : font;
}
