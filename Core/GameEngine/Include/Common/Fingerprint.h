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

// Fingerprint.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature What decides whether two players can play together (item 25, part A).
//
// The exe build id (GlobalData::m_exeCRC), the INI CRC (m_iniCRC) and a hash per logic-relevant data source in load
// order. A source counts when it holds INI files the game reads into its INI CRC (GameData, Object, Weapon, Armor,
// Locomotor, Upgrade, Science, PlayerTemplate, SpecialPower, FXList, ObjectCreationList, DamageFX, AIData, Crate,
// Rank, Multiplayer, Terrain, Roads, Water, Weather): every archive in mount order (packs first) and the loose
// Data\INI files. Its hash covers those files' names and contents. Archives with only art, sound, text or client INI
// (CommandButton, MappedImages ...) do not count, so art, sound, text and hotkey packs never change the fingerprint.
// Fingerprint.log in the user data folder holds it after the start; mismatch reports carry it.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

class ArchiveFile;

namespace Fingerprint
{
	/// An archive was mounted (in mount order).
	void noteArchive(ArchiveFile *archive);
	/// After the INI CRC is known (GameEngine::init): computes the hashes and writes Fingerprint.log.
	void compute();
	Bool isComputed();
	UnsignedInt overall();
	/// The text of Fingerprint.log (for reports).
	AsciiString text();
	void shutdown();
}
