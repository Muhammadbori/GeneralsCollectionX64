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

// AudioFileLength.h ////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The length of a sound file from its headers only, for a device without the Miles
// library (the null audio device's fileLengthMS). No game dependency: it reads a byte buffer. The same
// algorithm as dx12-tools/audiolen.py, checked against its table row by row; AudioFileLength_MilesMS equals
// the lengths Miles reports for all 13,639 audio files of Generals and Zero Hour (dx12-notes/av-3-design.md).

#pragma once

#include <stddef.h>
#include <stdint.h>

enum AudioFileKind
{
	AFK_UNKNOWN,       ///< neither RIFF/WAVE nor an MP3 frame
	AFK_WAV_PCM,
	AFK_WAV_IMA_ADPCM,
	AFK_WAV_OTHER,     ///< a WAV of another format tag: header values set, no length
	AFK_MP3,
};

/// Which sample count an IMA ADPCM WAV length uses (the headers disagree in most game files).
enum AudioLengthRule
{
	ALR_FACT,          ///< the 'fact' chunk sample count (the data size when there is none)
	ALR_BLOCKS,        ///< full blocks x samples per block, plus a partial last block (what decoders produce)
	ALR_BYTE_RATE,     ///< data bytes / the fmt average byte rate
	ALR_MILES,         ///< what Miles' AIL_stream_ms_position reports (AV-3 MEASURE): WAV data bytes x 1000 / byte rate,
	                   ///< MP3 whole file bytes x 8 / kbps of the first frame; see AudioFileLength_MilesMS for the rounding
};

struct AudioFileLengthInfo
{
	AudioFileKind kind;
	bool hasHeader;         ///< channels, rate, bits, block align and data bytes are set
	uint32_t formatTag;     ///< WAV format tag
	uint32_t channels;
	uint32_t rate;
	uint32_t bits;          ///< 0 for MP3
	uint32_t blockAlign;    ///< 0 for MP3
	uint32_t byteRate;      ///< WAV fmt average bytes per second (0 = not given)
	uint64_t dataBytes;     ///< WAV: data chunk bytes inside the file; MP3: bytes from the first frame to the end (no ID3v1)
	bool hasSamples;
	uint64_t samples;       ///< PCM: data / block align; ADPCM: 'fact' if present else blocks; MP3: frames x samples per frame
	bool hasFact;
	uint64_t samplesBlocks; ///< ADPCM only
	uint64_t mp3Frames;     ///< MP3: frames walked
	bool mp3HasFrameTag;    ///< MP3: Xing/Info/VBRI frame count used
	uint32_t mp3Kbps;       ///< MP3: bit rate of the first frame
	uint64_t fileBytes;     ///< the whole file (MP3: ID3 tags included, as Miles counts them)
};

/// Fills 'info' from the file bytes. Returns false when no length can be read (info.kind still tells why).
/// AudioFileLength_Parse picks the reader by the content (RIFF = WAV, else MP3); the other two take the
/// reader the file name asks for, as audiolen.py does (.wav / .mp3).
bool AudioFileLength_Parse(const unsigned char *data, size_t size, AudioFileLengthInfo *info);
bool AudioFileLength_ParseWav(const unsigned char *data, size_t size, AudioFileLengthInfo *info);
bool AudioFileLength_ParseMp3(const unsigned char *data, size_t size, AudioFileLengthInfo *info);

/// Length in milliseconds (not rounded). PCM and MP3 ignore the rule except ALR_BYTE_RATE, which gives the
/// byte-rate reading for a WAV and the constant-bit-rate reading (bytes x 8 / kbps) for an MP3.
/// Returns a negative value when that reading does not exist for the file.
double AudioFileLength_MS(const AudioFileLengthInfo &info, AudioLengthRule rule);

/// The whole milliseconds Miles reports: AudioFileLength_MS(info, ALR_MILES) rounded to nearest, ties to even
/// (the x87 default rounding; Music.big chi_05.mp3 is exactly 195992.5 and Miles gives 195992).
/// Returns -1 when there is no Miles reading.
int64_t AudioFileLength_MilesMS(const AudioFileLengthInfo &info);
