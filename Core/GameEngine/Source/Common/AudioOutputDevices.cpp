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

#include "Common/GlobalData.h"

#include <stdio.h>

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
	AsciiString how;
	if (s_lister != nullptr) {
		s_lister(names, how);
	}
	// TheSuperHackers @feature AudioDevices.log: what the options were offered (no device is opened for it).
	if (TheGlobalData != nullptr)
	{
		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat("AudioDevices.log");
		if (FILE *fp = fopen(path.str(), "w"))
		{
			fprintf(fp, "lister: %s\n", s_lister != nullptr ? "yes" : "none (this audio device cannot list its outputs)");
			if (s_lister != nullptr) {
				fprintf(fp, "listed by: %s\n", how.isEmpty() ? "-" : how.str());
			}
			fprintf(fp, "devices: %u\n", (unsigned)names.size());
			for (size_t i = 0; i < names.size(); ++i) {
				fprintf(fp, "device: %s\n", names[i].str());   // UTF-8 as the system gives it
			}
			fclose(fp);
		}
	}
	return s_lister != nullptr;
}
