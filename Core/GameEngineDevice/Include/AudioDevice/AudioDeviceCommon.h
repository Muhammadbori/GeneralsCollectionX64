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

// AudioDeviceCommon.h //////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature Parts shared by the audio devices that do not use Miles (the null device and the
// miniaudio device): the device clock, the file lengths, Miles' stream loop counter and the WAV format reader.
// They reproduce what Miles reports to the manager, so the scripts and the manager see the same numbers.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "AudioDevice/AudioDevice.h"

#include <map>

/// The device's own tick counter, 30 ticks per second.
/// - While game logic runs, it advances by the logic frames since the last call. A frame counter that stands still
///   (paused game) adds nothing; one that starts again at 0 (a new game) adds nothing for that call.
/// - When no game logic runs (shell, map loading), it advances in real time, 30 ticks per second.
class AudioDeviceClock
{
public:
	AudioDeviceClock();
	void update(UnsignedInt logicFrame, Bool logicIsRunning);
	UnsignedInt64 ticks() const { return m_ticks; }

	/// Ticks of a sound of 'ms' milliseconds: the truncation ScriptEngine::isSpeechComplete uses, at least 1.
	static UnsignedInt64 msToTicks(Real ms);

private:
	enum Mode { MODE_NONE, MODE_LOGIC, MODE_REAL };

	UnsignedInt64 m_ticks;
	Mode m_mode;
	UnsignedInt m_lastFrame;
	UnsignedInt m_lastRealMS;
	UnsignedInt64 m_realMS;      ///< real milliseconds since the clock went to real time
	UnsignedInt64 m_realTicks;   ///< ticks already given for them
};

/// The lengths of sound files by name, read once through TheFileSystem.
class AudioFileLengths
{
public:
	struct Entry
	{
		Int milesMS;     ///< what Miles reports (AIL_stream_ms_position): AudioFileLength_MilesMS
		Real playMS;     ///< how long it plays: decoded samples / rate (ADPCM: full blocks)
	};

	/// nullptr when the file cannot be opened or read.
	const Entry *get(const AsciiString &fileName);

	/// The lengths of a whole file in memory. A name ending in ".wav" is read as WAV, any other as MP3
	/// (as dx12-tools/audiolen.py and the -audioLenDump check). Returns FALSE when there is no length.
	static Bool fromBytes(const AsciiString &fileName, const void *data, UnsignedInt size, Entry *out);

	void clear() { m_entries.clear(); }

private:
	typedef std::map<AsciiString, Entry> EntryMap;
	typedef std::map<AsciiString, Bool> MissingMap;
	EntryMap m_entries;
	MissingMap m_missing;
};

/// The 'fmt ' chunk of a WAV file in memory, as Miles' AIL_WAV_info gives it (format, channels, rate, bits). The file
/// size is not passed in, so the walk stays inside the RIFF size and the first 4 KB. Zeros when it is not a WAV file.
void audioReadWavFormat(const void *fileBytes, AudioSampleInfo *info);

/// Miles' stream loop counter (measured with the game's mss32.dll): a stream set to N loops plays exactly N passes, and
/// AIL_stream_loop_count reads N, N-1, ... 1 and stays 1 after the end. 0 means "loop for ever" and reads 0.
inline Int milesStreamLoopCount(Int loopCount, UnsignedInt64 passesDone)
{
	if (loopCount <= 0) {
		return loopCount;
	}
	if (passesDone >= (UnsignedInt64)loopCount) {
		return 1;
	}
	const Int left = loopCount - (Int)passesDone;
	return left < 1 ? 1 : left;
}
