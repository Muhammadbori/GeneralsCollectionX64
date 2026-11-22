/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// This file contains thread related functions for compatibility with non-windows platforms.
#pragma once
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>

// TheSuperHackers @build O140 pthread_t is a pointer on macOS: the thread id is a 32-bit number from the system.
#if defined(__APPLE__)
inline unsigned int GetCurrentThreadId()
{
  uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return (unsigned int)id;
}
#else
#include <sys/syscall.h>
inline unsigned int GetCurrentThreadId()
{
  return (unsigned int)syscall(SYS_gettid);
}
#endif

inline void Sleep(unsigned int ms)
{
  usleep(ms * 1000);
}

