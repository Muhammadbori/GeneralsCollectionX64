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

// TheSuperHackers @build O165 (headless port) The last path separator of a file system path: '\' or '/'. On Windows the
// paths from GetModuleFileName have only '\'; on macOS and Linux they have '/'. Paths inside the game data (map names,
// archive entries) keep their own '\' and do not need this.
#pragma once

#include <string.h>

inline const char *lastPathSeparator(const char *path)
{
	const char *back = strrchr(path, '\\');
	const char *slash = strrchr(path, '/');
	if (back == nullptr)
		return slash;
	if (slash == nullptr)
		return back;
	return back > slash ? back : slash;
}

inline char *lastPathSeparator(char *path)
{
	return const_cast<char *>(lastPathSeparator(static_cast<const char *>(path)));
}
