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

// AudioOutputDevices.h /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The sound output devices for the options (AudioOutputDevice of Options.ini). The audio device
// in use registers how to list them (the miniaudio device does); without one there is no list and the options show
// only the system default. The names are UTF-8 as the system gives them; the device opens the one of that name, else
// the system default.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

#include <vector>

// Each listing writes AudioDevices.log in the user data folder: whether a lister is there, how it listed (for miniaudio:
// the backend of the context, or why the context failed) and the names. Listing only enumerates: no device is opened, so
// the real list can be checked in a run with -audio miniaudio-null (the options' Sound page lists them).

namespace AudioOutputDevices
{
	/// Fills 'names'; 'how' says how they were listed (a backend name, or what failed).
	typedef void (*Lister)(std::vector<AsciiString> &names, AsciiString &how);

	void setLister(Lister lister);
	/// FALSE when the audio device in use cannot list its outputs.
	Bool list(std::vector<AsciiString> &names);
}
