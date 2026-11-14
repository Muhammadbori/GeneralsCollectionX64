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
#include "Common/ArchiveFileSystem.h"
#include "Common/FileSystem.h"
#include "Common/GameCommon.h"
#include "Common/LocalFileSystem.h"
#include "Common/file.h"

#include <string.h>
#include <windows.h>

//-------------------------------------------------------------------------------------------------
void audioReadWavFormat(const void *fileBytes, AudioSampleInfo *info)
{
	memset(info, 0, sizeof(*info));
	const unsigned char *d = (const unsigned char *)fileBytes;
	if (d == nullptr || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) {
		return;
	}
	const UnsignedInt riffSize = (UnsignedInt)d[4] | ((UnsignedInt)d[5] << 8) | ((UnsignedInt)d[6] << 16) | ((UnsignedInt)d[7] << 24);
	size_t n = (size_t)riffSize + 8;
	if (n > 4096) {
		n = 4096;
	}
	size_t p = 12;
	while (p + 8 <= n)
	{
		const unsigned char *c = d + p;
		const UnsignedInt size = (UnsignedInt)c[4] | ((UnsignedInt)c[5] << 8) | ((UnsignedInt)c[6] << 16) | ((UnsignedInt)c[7] << 24);
		if (memcmp(c, "fmt ", 4) == 0 && size >= 16 && p + 8 + 16 <= n)
		{
			const unsigned char *f = c + 8;
			info->format = (Int)((UnsignedInt)f[0] | ((UnsignedInt)f[1] << 8));
			info->channels = (UnsignedInt)f[2] | ((UnsignedInt)f[3] << 8);
			info->rate = (UnsignedInt)f[4] | ((UnsignedInt)f[5] << 8) | ((UnsignedInt)f[6] << 16) | ((UnsignedInt)f[7] << 24);
			info->bits = (UnsignedInt)f[14] | ((UnsignedInt)f[15] << 8);
			return;
		}
		p += 8 + (size_t)size + (size & 1);
	}
}

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
namespace
{
	Bool isWavName(const AsciiString &fileName)
	{
		const char *name = fileName.str();
		const size_t len = strlen(name);
		return len >= 4 && _stricmp(name + len - 4, ".wav") == 0;
	}

	/// The entry from the parsed headers. The parser's result is not looked at: a WAV of another format tag has no
	/// play length but still a Miles length.
	Bool entryFromInfo(const AudioFileLengthInfo &info, AudioFileLengths::Entry *out)
	{
		const Int64 milesMS = AudioFileLength_MilesMS(info);
		if (milesMS < 0) {
			return FALSE;
		}
		const double playMS = AudioFileLength_MS(info, ALR_BLOCKS);
		out->milesMS = (Int)milesMS;
		out->playMS = playMS > 0.0 ? (Real)playMS : (Real)milesMS;
		return TRUE;
	}

	/// TheSuperHackers @performance TheFileSystem->openFile does not hand the access to the archives (FileSystem.cpp),
	/// so an archived file opened there is copied into memory whole: a music or speech stream of several hundred KB on
	/// the main thread at its first start. Here the archive gets File::STREAMING, so its file reads only what is asked.
	/// The order is TheFileSystem's: a loose file first, then the archives. The streaming file reads through the
	/// archive's shared file, as every archive open does; the devices call get on the main thread only.
	File *openForLength(const char *fileName)
	{
		File *file = nullptr;
		if (TheLocalFileSystem != nullptr) {
			file = TheLocalFileSystem->openFile(fileName, File::READ | File::BINARY);
		}
		if (file == nullptr && TheArchiveFileSystem != nullptr) {
			file = TheArchiveFileSystem->openFile(fileName, File::READ | File::BINARY | File::STREAMING);
		}
		return file;
	}

	/// A file read in blocks for the length parsers, so that the many small reads of a parse (a chunk header, an MP3
	/// frame header) do not each go to the file. The first block is given: 64 KB holds a whole sound effect and the
	/// headers of a long WAV. A read just past the last block (the next frame header of an MP3 walk) doubles the next
	/// block, up to 1 MB or the first block, so that a walk over the whole file takes a few large reads.
	class BlockFileSource : public AudioFileLengthSource
	{
	public:
		enum { SMALL_BLOCK_BYTES = 64 * 1024, LARGE_BLOCK_BYTES = 1024 * 1024 };

		BlockFileSource(File *file, size_t firstBlockBytes)
			: m_file(file)
			, m_size(0)
			, m_blockOffset(0)
			, m_blockBytes(0)
			, m_firstBlockBytes(firstBlockBytes < SMALL_BLOCK_BYTES ? SMALL_BLOCK_BYTES : firstBlockBytes)
			, m_nextBlockBytes(0)
			, m_block(nullptr)
			, m_blockCapacity(0)
		{
			const Int size = file->size();
			m_size = size > 0 ? (uint64_t)size : 0;
			m_nextBlockBytes = m_firstBlockBytes;
		}

		virtual ~BlockFileSource() override
		{
			delete [] m_block;
		}

		BlockFileSource(const BlockFileSource &) = delete;
		BlockFileSource &operator=(const BlockFileSource &) = delete;

		virtual uint64_t size() override { return m_size; }

		virtual bool read(uint64_t offset, void *dst, size_t bytes) override
		{
			if (offset > m_size || bytes > m_size - offset) {
				return false;
			}
			unsigned char *out = (unsigned char *)dst;
			while (bytes > 0)
			{
				if (offset < m_blockOffset || offset >= m_blockOffset + m_blockBytes)
				{
					// Onward: at most one first block past the end of the last block (the next MP3 frame header). The new
					// block starts where the last one ended, so the file is read without gaps or overlaps.
					const uint64_t blockEnd = m_blockOffset + m_blockBytes;
					const Bool onward = m_blockBytes > 0 && offset >= blockEnd && offset - blockEnd < SMALL_BLOCK_BYTES;
					const size_t largest = m_firstBlockBytes > LARGE_BLOCK_BYTES ? m_firstBlockBytes : LARGE_BLOCK_BYTES;
					m_nextBlockBytes = onward ? (m_nextBlockBytes < largest / 2 ? m_nextBlockBytes * 2 : largest) : m_firstBlockBytes;
					const uint64_t start = onward ? blockEnd : offset - offset % SMALL_BLOCK_BYTES;
					const uint64_t left = m_size - start;
					const size_t want = (size_t)(left < m_nextBlockBytes ? left : m_nextBlockBytes);
					if (m_blockCapacity < want) {
						delete [] m_block;
						m_block = new unsigned char [want];
						m_blockCapacity = want;
					}
					m_blockBytes = 0;
					if (m_file->seek((Int)start, File::START) != (Int)start || m_file->read(m_block, (Int)want) != (Int)want) {
						return false;
					}
					m_blockOffset = start;
					m_blockBytes = want;
				}
				const size_t at = (size_t)(offset - m_blockOffset);
				const size_t take = bytes < m_blockBytes - at ? bytes : m_blockBytes - at;
				memcpy(out, m_block + at, take);
				out += take;
				offset += take;
				bytes -= take;
			}
			return true;
		}

	private:
		File *m_file;
		uint64_t m_size;
		uint64_t m_blockOffset;
		size_t m_blockBytes;
		size_t m_firstBlockBytes;
		size_t m_nextBlockBytes;
		unsigned char *m_block;
		size_t m_blockCapacity;
	};
}

//-------------------------------------------------------------------------------------------------
Bool AudioFileLengths::fromBytes(const AsciiString &fileName, const void *data, UnsignedInt size, Entry *out)
{
	AudioFileLengthInfo info;
	if (isWavName(fileName)) {
		AudioFileLength_ParseWav((const unsigned char *)data, size, &info);
	} else {
		AudioFileLength_ParseMp3((const unsigned char *)data, size, &info);
	}
	return entryFromInfo(info, out);
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

	// The same lengths as fromBytes over the whole file (the parsers are the same; an MP3 with a frame count header
	// is not walked frame by frame, which the lengths do not use).
	Entry entry = { 0, 0.0f };
	Bool ok = FALSE;
	File *file = openForLength(fileName.str());
	if (file)
	{
		// A WAV needs its chunk headers only. The play length of an MP3 without a frame count header (none of the 105
		// MP3s of the two games has one) needs every frame header, which is every page of the file: one read of the
		// whole file costs the least then (the parse itself reads no more than the headers either way).
		const Bool wav = isWavName(fileName);
		const Int fileBytes = file->size();
		const size_t wholeFile = fileBytes > 0 ? (size_t)fileBytes : 0;
		BlockFileSource source(file, wav ? (size_t)BlockFileSource::SMALL_BLOCK_BYTES : wholeFile);
		AudioFileLengthInfo info;
		if (wav) {
			AudioFileLength_ParseWavFrom(source, &info);
		} else {
			AudioFileLength_ParseMp3From(source, &info, false);
		}
		ok = entryFromInfo(info, &entry);
		file->close();
	}

	if (!ok) {
		m_missing[fileName] = TRUE;
		return nullptr;
	}
	return &(m_entries[fileName] = entry);
}
