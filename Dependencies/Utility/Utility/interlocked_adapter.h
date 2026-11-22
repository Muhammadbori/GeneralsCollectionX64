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

// This file provides the Interlocked functions of the Windows SDK. The VC6 headers have them neither for volatile
// pointers nor for 8, 16 and 64 bit values, so VC6 gets its own implementations.
#pragma once

#if defined(_MSC_VER) && _MSC_VER < 1300

	#include "interlocked_x86asm.inl"

#elif defined(_WIN32)

	#include <windows.h>

#else

	// TheSuperHackers @build O143 (headless port, part 2) macOS and Linux: the Interlocked functions the game uses, with
	// the GCC/Clang atomic builtins (sequentially consistent, as the Windows functions are full barriers).
	inline long InterlockedIncrement(volatile long *value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
	inline long InterlockedDecrement(volatile long *value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }
	inline long InterlockedExchange(volatile long *target, long value) { return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST); }
	inline long InterlockedExchangeAdd(volatile long *target, long value) { return __atomic_fetch_add(target, value, __ATOMIC_SEQ_CST); }
	inline long InterlockedCompareExchange(volatile long *target, long exchange, long comparand)
	{
		__atomic_compare_exchange_n(target, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		return comparand;
	}
	inline void *InterlockedExchangePointer(void *volatile *target, void *value) { return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST); }
	inline void *InterlockedCompareExchangePointer(void *volatile *target, void *exchange, void *comparand)
	{
		__atomic_compare_exchange_n(target, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		return comparand;
	}

#endif
