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

// TheSuperHackers @build O152 (headless port) The Microsoft C runtime's low level file functions and their flags, as the
// POSIX ones of the same meaning (macOS, Linux). Text and binary modes are the same there.
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define _open open
#define _close close
#define _read read
#define _write write
#define _lseek lseek
#define _access access
#define _stat stat
#define _fstat fstat
#define _unlink unlink
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _O_EXCL O_EXCL
#define _O_BINARY 0
#define _O_TEXT 0
#define _S_IREAD S_IRUSR
#define _S_IFDIR S_IFDIR
#define _S_IFREG S_IFREG
#define _S_IFMT S_IFMT
#define _S_IWRITE S_IWUSR
