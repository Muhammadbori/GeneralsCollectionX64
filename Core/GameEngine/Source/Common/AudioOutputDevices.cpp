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

// TheSuperHackers @feature See AudioOutputDevices.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/AudioOutputDevices.h"

namespace
{
	AudioOutputDevices::Lister s_lister = nullptr;
}

void AudioOutputDevices::setLister(Lister lister)
{
	s_lister = lister;
}

Bool AudioOutputDevices::list(std::vector<AsciiString> &names)
{
	names.clear();
	if (s_lister == nullptr) {
		return FALSE;
	}
	s_lister(names);
	return TRUE;
}
