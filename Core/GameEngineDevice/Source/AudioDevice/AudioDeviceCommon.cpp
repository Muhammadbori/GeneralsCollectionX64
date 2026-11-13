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

// AudioDeviceCommon.cpp ////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See AudioDeviceCommon.h.

#include "Lib/BaseType.h"
#include "AudioDevice/AudioDeviceCommon.h"

#include "AudioDevice/AudioFileLength.h"
#include "Common/FileSystem.h"
#include "Common/GameCommon.h"
#include "Common/file.h"

#include <windows.h>

//-------------------------------------------------------------------------------------------------
AudioDeviceClock::AudioDeviceClock()
	: m_ticks(0)
	, m_mode(MODE_NONE)
	, m_lastFrame(0)
	, m_lastRealMS(0)
	, m_realMS(0)
	, m_realTicks(0)
{
}

//-------------------------------------------------------------------------------------------------
void AudioDeviceClock::update(UnsignedInt logicFrame, Bool logicIsRunning)
{
	if (logicIsRunning)
	{
		if (m_mode == MODE_LOGIC && logicFrame > m_lastFrame) {
			m_ticks += logicFrame - m_lastFrame;
		}
		m_lastFrame = logicFrame;
		m_mode = MODE_LOGIC;
		return;
	}

	const UnsignedInt now = (UnsignedInt)::GetTickCount();
	if (m_mode == MODE_REAL)
	{
		m_realMS += (UnsignedInt)(now - m_lastRealMS);
		const UnsignedInt64 due = m_realMS * LOGICFRAMES_PER_SECOND / MSEC_PER_SECOND;
		m_ticks += due - m_realTicks;
		m_realTicks = due;
	}
	else
	{
		m_realMS = 0;
		m_realTicks = 0;
	}
	m_lastRealMS = now;
	m_mode = MODE_REAL;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt64 AudioDeviceClock::msToTicks(Real ms)
{
	if (!(ms > 0.0f)) {
		return 1;
	}
	const UnsignedInt ticks = REAL_TO_UNSIGNEDINT(ms / MSEC_PER_LOGICFRAME_REAL);
	return ticks < 1 ? 1 : ticks;
}

//-------------------------------------------------------------------------------------------------
Bool AudioFileLengths::fromBytes(const AsciiString &fileName, const void *data, UnsignedInt size, Entry *out)
{
	const char *name = fileName.str();
	const size_t len = strlen(name);
	const Bool isWav = len >= 4 && _stricmp(name + len - 4, ".wav") == 0;

	AudioFileLengthInfo info;
	if (isWav) {
		AudioFileLength_ParseWav((const unsigned char *)data, size, &info);
	} else {
		AudioFileLength_ParseMp3((const unsigned char *)data, size, &info);
	}

	const Int64 milesMS = AudioFileLength_MilesMS(info);
	if (milesMS < 0) {
		return FALSE;
	}
	const double playMS = AudioFileLength_MS(info, ALR_BLOCKS);
	out->milesMS = (Int)milesMS;
	out->playMS = playMS > 0.0 ? (Real)playMS : (Real)milesMS;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
const AudioFileLengths::Entry *AudioFileLengths::get(const AsciiString &fileName)
{
	EntryMap::const_iterator it = m_entries.find(fileName);
	if (it != m_entries.end()) {
		return &it->second;
	}
	if (m_missing.find(fileName) != m_missing.end()) {
		return nullptr;
	}

	Entry entry;
	Bool ok = FALSE;
	File *file = TheFileSystem->openFile(fileName.str());
	if (file)
	{
		const UnsignedInt size = (UnsignedInt)file->size();
		char *buffer = file->readEntireAndClose();
		if (buffer) {
			ok = fromBytes(fileName, buffer, size, &entry);
			delete [] buffer;
		}
	}

	if (!ok) {
		m_missing[fileName] = TRUE;
		return nullptr;
	}
	return &(m_entries[fileName] = entry);
}
