/*
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

// M2 (plan10): a unicode string in a save file, an Xfer CRC or a replay is a run of 2-byte UTF-16 units on every platform.
// Where WideChar is 2 bytes wide nothing is converted (RTS_WIRE_CONVERT == 0) and these helpers are not used.

#pragma once

#include "Common/UnicodeString.h"

#if RTS_WIRE_CONVERT

// The units of a string, for writing. 'count' is at most 255 (the length header is one byte); more is reported through
// 'tooLong' and the units are cut.
struct XferWireText
{
	WireChar units[255 + 1];
	Int count;
	Bool tooLong;

	explicit XferWireText(const UnicodeString &s)
	{
		const size_t need = WireText::encodedLength(s.str(), static_cast<size_t>(s.getLength()));
		tooLong = need > 255;
		count = static_cast<Int>(WireText::encode(units, 255, s.str(), static_cast<size_t>(s.getLength())));
		units[count] = 0;
	}
};

#endif
