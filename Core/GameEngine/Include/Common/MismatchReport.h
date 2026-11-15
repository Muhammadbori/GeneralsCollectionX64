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

// MismatchReport.h /////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature Automatic mismatch report (item 25, part B).
//
// In network games and in replay playback a small rolling record is kept in memory:
//   - per logic frame (the last 1024): the frame, the logic random seed CRC, and the logic random draws of the frame;
//   - per logic CRC (the last 4, every CRC interval): the CRC after each part of GameLogic::getCRC (objects, seed,
//     partition manager, player list, AI, final) and the CRC after each object with its ID.
// It reads state only and costs a few stores per frame (and per object at a CRC frame). The moment a mismatch is
// detected (Network: GameLogic's CRC compare; replay: RecorderClass::handleCRCMessage) the computer writes a folder
//   <user data>\MismatchReports\<date-time>_f<frame>_p<process id>\
// with report.txt (facts, the rolling record, the CRCs of the players), fingerprint.txt (Fingerprint.h) and replay.rep
// (the replay so far). dx12-tools\mismatch_compare.py compares two such folders: the first frame whose seed or draws
// differ, the first CRC part that differs and the first object whose CRC differs.
//
// -testPerturbFrame <frame> (test only, replay playback only): one extra logic random draw at that frame, so the
// playback CRC check fires and a report is written without a network. -testReportFrame <frame> writes a report at
// that frame of an unperturbed playback, the other side for the compare.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

namespace MismatchReport
{
	void setPerturbFrame(Int frame);
	/// -testReportFrame <frame> (test only, replay playback only): write a report at the end of that frame without a
	/// mismatch, as the other side to compare a perturbed run with.
	void setReportFrame(Int frame);
	/// TRUE once, at the test frame of a replay playback.
	Bool perturbNow(UnsignedInt frame);

	/// TRUE in network games and replay playback: the record is kept.
	Bool recording();

	/// Around GameLogic::update: one rolling line per logic frame.
	struct FrameScope
	{
		FrameScope();
		~FrameScope();
		UnsignedInt m_frame;
		UnsignedInt m_draws;
		Bool m_on;
	};

	// GameLogic::getCRC in a logic update
	void crcBegin(UnsignedInt frame);
	void crcStage(const char *name, UnsignedInt crc);
	void crcObject(UnsignedInt id, UnsignedInt crc);
	void crcEnd(UnsignedInt crc);

	/// A player's CRC at a network compare (before write).
	void notePlayerCRC(Int playerIndex, const UnicodeString &name, UnsignedInt crc);
	/// Writes the report folder (once per game). crcA/crcB: the two CRCs that differ, when known.
	void write(const char *reason, UnsignedInt frame, UnsignedInt crcA, UnsignedInt crcB);

	/// A new game: forget the record (called at a new game's start).
	void reset();
	void shutdown();
}
