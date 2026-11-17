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

// VoicePreview.h ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O47 (row 32) The options' "listen" buttons under the voice pack list: the select line of the USA
// dozer, the China dozer and the GLA worker, from the voice pack chosen in the list at that moment, before it is applied.
//   - The lines come from the game's own data: the unit's VoiceSelect event (AmericaVehicleDozer, ChinaVehicleDozer,
//     GLAInfantryWorker) and that event's Sounds list; each press takes the next line of the list.
//   - The file (<AudioRoot>\<SoundsFolder>\<language>\<name>.wav, else without the language folder, as
//     AudioEventRTS::adjustForLocalization looks) is read straight from the archives the chosen pack would mount, in the
//     game's order (Packs::previewArchives: the pack's archives, its fallback's, then the archive list without what the
//     pack leaves out). Nothing is mounted; the pack itself still takes effect after the restart.
//   - It plays once, 2D, at the line's volume times the sound effects volume (as the unit's voice in the game), through
//     AudioManager::playPreviewSound; a new press stops the one before; leaving the section or the screen stops it.
//   - Each press writes a line to VoicePreview.log in the user data folder (the pack, the archive, the file, its length in
//     ms, played / not played / not found), for runs without sound.

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"
#include "Common/Packs.h"

namespace VoicePreview
{
	enum Unit { UNIT_USA_DOZER, UNIT_CHINA_DOZER, UNIT_GLA_WORKER, UNIT_COUNT };

	UnicodeString title();                ///< "Listen:" over the buttons
	UnicodeString buttonText(Int unit);   ///< the short button text
	UnicodeString tooltip(Int unit);      ///< the full text

	/// Plays the next select line of 'unit' from 'pack' (nullptr: the base data). 'status': one line for the screen.
	/// TRUE when it plays.
	Bool play(Int unit, const Packs::Pack *pack, UnicodeString *status);
	/// Stops the line (no-op when nothing plays) and frees what it held.
	void stop();
	/// O55: a meter line in VoicePreview.log: "<what>: out peak -8.2 dBFS, rms -21.0 dBFS, frames n of m", and
	/// "TOO QUIET" when the peak is under -40 dBFS while the sound effects volume is at least 20 %. 'expectedFrames' 0:
	/// not known.
	void logMeter(const char *what, Real peak, Real rms, UnsignedInt64 frames, UnsignedInt64 expectedFrames, Real systemSoundVolume);
}
