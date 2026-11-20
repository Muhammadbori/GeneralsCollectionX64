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

// CollectionUserData.cpp ///////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O118 Our own user data folders (CollectionUserData.h).

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CollectionUserData.h"

AsciiString CollectionUserData::makeOurs(const AsciiString &originalDir, const char *gameName)
{
	if (originalDir.isEmpty() || gameName == nullptr) {
		return AsciiString::TheEmptyString;
	}

	// Documents\ = the original folder's parent
	AsciiString documents = originalDir;
	documents.trimEnd('\\');
	const char *slash = strrchr(documents.str(), '\\');
	if (slash == nullptr) {
		return AsciiString::TheEmptyString;
	}
	AsciiString base;
	for (const char *c = documents.str(); c <= slash; ++c) {
		base.concat(*c);
	}

	AsciiString collection = base;
	collection.concat("Generals Collection x64");
	CreateDirectoryA(collection.str(), nullptr);

	AsciiString ours = collection;
	ours.concat('\\');
	ours.concat(gameName);
	ours.concat('\\');
	CreateDirectoryA(ours.str(), nullptr);

	// the player's settings carry over once: read the original Options.ini, never change it
	AsciiString ourOptions = ours;
	ourOptions.concat("Options.ini");
	if (GetFileAttributesA(ourOptions.str()) == INVALID_FILE_ATTRIBUTES)
	{
		AsciiString originalOptions = originalDir;
		if (!originalOptions.endsWith("\\")) {
			originalOptions.concat('\\');
		}
		originalOptions.concat("Options.ini");
		if (GetFileAttributesA(originalOptions.str()) != INVALID_FILE_ATTRIBUTES) {
			CopyFileA(originalOptions.str(), ourOptions.str(), TRUE);
		}
	}
	return ours;
}
