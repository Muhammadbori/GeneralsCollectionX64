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

#pragma once

#include <stddef.h>
#include <wchar.h>
#include <type_traits>

// Shared project wide-character type, currently using the native representation.
// Changing it also requires adapting wide literals, character traits, and CRT/platform APIs.
typedef wchar_t unichar;

// M2 (plan10): the in-memory text stays `unichar` (= wchar_t: 2 bytes on Windows, 4 bytes on macOS), but every FORMAT that
// leaves the process (LAN packets, game packets, Xfer saves and CRC, replays, GameMessage wide-char arguments) is made of
// 2-byte UTF-16 units: `WireChar`. On Windows wchar_t already is that, so WireChar == wchar_t and nothing is converted.
// Where wchar_t is wider (macOS) WireChar is char16_t and the code goes through WireText below. RTS_FORCE_WIRE_CONVERT
// takes the conversion paths on Windows too (a test build: the bytes must stay the same).
#if defined(RTS_FORCE_WIRE_CONVERT) || (WCHAR_MAX > 0xFFFF)
	#define RTS_WIRE_CONVERT 1
	typedef char16_t WireChar;
#else
	#define RTS_WIRE_CONVERT 0
	typedef wchar_t WireChar;
#endif
static_assert(sizeof(WireChar) == 2, "WireChar is one UTF-16 unit: 2 bytes on every platform");

namespace WireText
{
	// Encodes n characters of src (UTF-32 code points when the source char is 4 bytes wide, UTF-16 units otherwise) as
	// UTF-16 units. At most cap units are written (a surrogate pair is never cut in half); returns the units written.
	// Does not write a terminator.
	template<typename SrcChar>
	inline size_t encode(WireChar *dst, size_t cap, const SrcChar *src, size_t n)
	{
		size_t out = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const unsigned long c = static_cast<unsigned long>(static_cast<typename std::make_unsigned<SrcChar>::type>(src[i]));
			if (c < 0x10000UL || sizeof(SrcChar) < 4)
			{
				if (out + 1 > cap) break;
				dst[out++] = static_cast<WireChar>(c);
			}
			else
			{
				if (out + 2 > cap) break;
				const unsigned long v = c - 0x10000UL;
				dst[out++] = static_cast<WireChar>(0xD800UL + (v >> 10));
				dst[out++] = static_cast<WireChar>(0xDC00UL + (v & 0x3FFUL));
			}
		}
		return out;
	}

	// Number of UTF-16 units encode() needs for n characters of src.
	template<typename SrcChar>
	inline size_t encodedLength(const SrcChar *src, size_t n)
	{
		if (sizeof(SrcChar) < 4) return n;
		size_t units = 0;
		for (size_t i = 0; i < n; ++i)
			units += (static_cast<unsigned long>(static_cast<typename std::make_unsigned<SrcChar>::type>(src[i])) >= 0x10000UL) ? 2 : 1;
		return units;
	}

	// Number of units of the first n characters of src that fit into cap units without cutting a surrogate pair.
	template<typename SrcChar>
	inline size_t fitUnits(const SrcChar *src, size_t n, size_t cap)
	{
		size_t units = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const size_t need = (sizeof(SrcChar) >= 4 && static_cast<unsigned long>(static_cast<typename std::make_unsigned<SrcChar>::type>(src[i])) >= 0x10000UL) ? 2 : 1;
			if (units + need > cap) break;
			units += need;
		}
		return units;
	}

	// Decodes n UTF-16 units to characters of DstChar (surrogate pairs become one code point when DstChar is 4 bytes wide,
	// else the units are copied as they are). At most cap characters are written; returns the characters written.
	// A lone surrogate is kept as it is. Does not write a terminator.
	template<typename DstChar>
	inline size_t decode(DstChar *dst, size_t cap, const WireChar *src, size_t n)
	{
		size_t out = 0;
		for (size_t i = 0; i < n && out < cap; ++i)
		{
			unsigned long c = static_cast<unsigned long>(static_cast<unsigned short>(src[i]));
			if (sizeof(DstChar) >= 4 && c >= 0xD800UL && c < 0xDC00UL && i + 1 < n)
			{
				const unsigned long d = static_cast<unsigned long>(static_cast<unsigned short>(src[i + 1]));
				if (d >= 0xDC00UL && d < 0xE000UL)
				{
					c = 0x10000UL + ((c - 0xD800UL) << 10) + (d - 0xDC00UL);
					++i;
				}
			}
			dst[out++] = static_cast<DstChar>(c);
		}
		return out;
	}

	// Length of a zero terminated wire string.
	inline size_t length(const WireChar *s)
	{
		size_t n = 0;
		while (s[n] != 0) ++n;
		return n;
	}
}
