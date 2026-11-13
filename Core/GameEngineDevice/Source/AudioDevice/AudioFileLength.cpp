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

// AudioFileLength.cpp //////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See AudioFileLength.h. Mirrors dx12-tools/audiolen.py (wav_info, mp3_header,
// mp3_info) step by step, so the two can be compared on every audio file of the game archives.

#include "AudioDevice/AudioFileLength.h"

#include <string.h>

namespace
{
	uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
	uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
	uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }

	bool parseWav(const unsigned char *d, size_t n, AudioFileLengthInfo *r)
	{
		if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
			return false;

		// Walk the chunks; a later chunk of the same id replaces an earlier one (as audiolen.py does).
		size_t p = 12;
		const unsigned char *fmt = nullptr;
		size_t fmtLen = 0;
		bool haveData = false;
		size_t dataOff = 0;
		uint64_t dataSize = 0;
		bool haveFact = false;
		uint32_t fact = 0;
		while (p + 8 <= n)
		{
			const unsigned char *id = d + p;
			const uint32_t size = le32(d + p + 4);
			const size_t bodyOff = p + 8;
			const size_t bodyLen = bodyOff >= n ? 0 : ((uint64_t)size < (uint64_t)(n - bodyOff) ? (size_t)size : n - bodyOff);
			if (memcmp(id, "fmt ", 4) == 0) {
				fmt = d + bodyOff;
				fmtLen = bodyLen;
			} else if (memcmp(id, "fact", 4) == 0 && size >= 4) {
				if (bodyLen >= 4) {
					fact = le32(d + bodyOff);
					haveFact = true;
				}
			} else if (memcmp(id, "data", 4) == 0) {
				haveData = true;
				dataOff = bodyOff;
				dataSize = size;
			}
			p += 8 + (size_t)size + (size & 1);
		}
		if (fmt == nullptr || !haveData || fmtLen < 16)
			return false;

		const uint32_t tag = le16(fmt);
		const uint32_t ch = le16(fmt + 2);
		const uint32_t rate = le32(fmt + 4);
		const uint32_t byteRate = le32(fmt + 8);
		const uint32_t align = le16(fmt + 12);
		const uint32_t bits = le16(fmt + 14);
		const uint64_t inFile = dataOff <= n ? (uint64_t)(n - dataOff) : 0;
		const uint64_t dataBytes = dataSize < inFile ? dataSize : inFile;

		r->hasHeader = true;
		r->formatTag = tag;
		r->channels = ch;
		r->rate = rate;
		r->bits = bits;
		r->blockAlign = align;
		r->byteRate = byteRate;
		r->dataBytes = dataBytes;

		if (tag == 1)
		{
			r->kind = AFK_WAV_PCM;
			if (align == 0 || rate == 0)
				return false;
			r->samples = dataBytes / align;
			r->hasSamples = true;
			return true;
		}
		if (tag == 0x11)
		{
			r->kind = AFK_WAV_IMA_ADPCM;
			if (align == 0 || rate == 0 || bits == 0 || ch == 0)
				return false;
			const uint64_t spb = fmtLen >= 20 ? le16(fmt + 18) : ((uint64_t)(align - 4 * ch) * 8) / (bits * ch) + 1;
			const uint64_t full = dataBytes / align;
			const uint64_t rem = dataBytes % align;
			uint64_t fromBlocks = full * spb;
			if (rem && rem > 4 * (uint64_t)ch)
				fromBlocks += ((rem - 4 * (uint64_t)ch) * 8) / ((uint64_t)bits * ch) + 1;
			r->samplesBlocks = fromBlocks;
			r->hasFact = haveFact;
			r->samples = haveFact ? fact : fromBlocks;
			r->hasSamples = true;
			return true;
		}
		r->kind = AFK_WAV_OTHER;
		return false;
	}

	struct Mp3Header
	{
		uint32_t ver, kbps, rate, spf, length, channels;
	};

	const uint32_t kBitratesV1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
	const uint32_t kBitratesV2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
	const uint32_t kRates[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };

	// audiolen.py mp3_header: MPEG-1/2/2.5 Layer III only.
	bool mp3Header(const unsigned char *d, size_t n, size_t p, Mp3Header *h)
	{
		if (p + 4 > n || d[p] != 0xFF || (d[p + 1] & 0xE0) != 0xE0)
			return false;
		const uint32_t ver = (d[p + 1] >> 3) & 3;
		const uint32_t layer = (d[p + 1] >> 1) & 3;
		const uint32_t bri = d[p + 2] >> 4;
		const uint32_t sri = (d[p + 2] >> 2) & 3;
		const uint32_t pad = (d[p + 2] >> 1) & 1;
		const uint32_t mode = d[p + 3] >> 6;
		if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3)
			return false;
		h->ver = ver;
		h->kbps = ver == 3 ? kBitratesV1[bri] : kBitratesV2[bri];
		h->rate = kRates[ver][sri];
		h->spf = ver == 3 ? 1152 : 576;
		h->length = (ver == 3 ? 144u : 72u) * h->kbps * 1000u / h->rate + pad;
		h->channels = mode == 3 ? 1 : 2;
		return true;
	}

	bool parseMp3(const unsigned char *d, size_t n, AudioFileLengthInfo *r)
	{
		size_t p = 0;
		if (n >= 10 && memcmp(d, "ID3", 3) == 0) {
			const size_t sz = ((size_t)d[6] << 21) | ((size_t)d[7] << 14) | ((size_t)d[8] << 7) | d[9];
			p = 10 + sz + ((d[5] & 0x10) ? 10 : 0);
		}
		const size_t end = (n >= 128 && memcmp(d + n - 128, "TAG", 3) == 0) ? n - 128 : n;
		Mp3Header h;
		while (p < end && !mp3Header(d, n, p, &h))
			++p;
		if (!mp3Header(d, n, p, &h)) {
			r->kind = AFK_UNKNOWN;
			return false;
		}
		r->kind = AFK_MP3;
		r->hasHeader = true;
		r->channels = h.channels;
		r->rate = h.rate;
		r->bits = 0;
		r->blockAlign = 0;
		r->mp3Kbps = h.kbps;

		const size_t side = h.ver == 3 ? (h.channels == 2 ? 32 : 17) : (h.channels == 2 ? 17 : 9);
		bool haveTag = false;
		uint32_t tagFrames = 0;
		if (p + 8 + side <= n && (memcmp(d + p + 4 + side, "Xing", 4) == 0 || memcmp(d + p + 4 + side, "Info", 4) == 0)) {
			const uint32_t flags = be32(d + p + 8 + side);
			if (flags & 1) {
				tagFrames = be32(d + p + 12 + side);
				haveTag = true;
			}
		} else if (p + 40 <= n && memcmp(d + p + 36, "VBRI", 4) == 0) {
			tagFrames = be32(d + p + 50);
			haveTag = true;
		}

		size_t q = p;
		uint64_t frames = 0;
		Mp3Header hh;
		while (q < end) {
			if (!mp3Header(d, n, q, &hh) || hh.length == 0)
				break;
			++frames;
			q += hh.length;
		}
		r->dataBytes = end - p;
		r->mp3Frames = frames;
		r->mp3HasFrameTag = haveTag;
		r->samples = (haveTag ? tagFrames : frames) * (uint64_t)h.spf;
		r->hasSamples = true;
		return true;
	}
}

bool AudioFileLength_ParseWav(const unsigned char *data, size_t size, AudioFileLengthInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->kind = AFK_UNKNOWN;
	info->fileBytes = size;
	return parseWav(data, size, info);
}

bool AudioFileLength_ParseMp3(const unsigned char *data, size_t size, AudioFileLengthInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->kind = AFK_UNKNOWN;
	info->fileBytes = size;
	return parseMp3(data, size, info);
}

bool AudioFileLength_Parse(const unsigned char *data, size_t size, AudioFileLengthInfo *info)
{
	if (size >= 12 && memcmp(data, "RIFF", 4) == 0)
		return AudioFileLength_ParseWav(data, size, info);
	return AudioFileLength_ParseMp3(data, size, info);
}

double AudioFileLength_MS(const AudioFileLengthInfo &info, AudioLengthRule rule)
{
	if (!info.hasHeader || info.rate == 0)
		return -1.0;
	if (rule == ALR_MILES)
	{
		switch (info.kind)
		{
		case AFK_WAV_PCM:
		case AFK_WAV_IMA_ADPCM:
		case AFK_WAV_OTHER:
			return info.byteRate ? (double)info.dataBytes * 1000.0 / (double)info.byteRate : -1.0;
		case AFK_MP3:
			return info.mp3Kbps ? (double)info.fileBytes * 8.0 / (double)info.mp3Kbps : -1.0;
		default:
			return -1.0;
		}
	}
	switch (info.kind)
	{
	case AFK_WAV_PCM:
	case AFK_WAV_IMA_ADPCM:
		if (rule == ALR_BYTE_RATE)
			return info.byteRate ? (double)info.dataBytes * 1000.0 / (double)info.byteRate : -1.0;
		if (info.kind == AFK_WAV_IMA_ADPCM && rule == ALR_BLOCKS)
			return (double)info.samplesBlocks * 1000.0 / (double)info.rate;
		return info.hasSamples ? (double)info.samples * 1000.0 / (double)info.rate : -1.0;
	case AFK_WAV_OTHER:
		if (rule == ALR_BYTE_RATE && info.byteRate)
			return (double)info.dataBytes * 1000.0 / (double)info.byteRate;
		return -1.0;
	case AFK_MP3:
		if (rule == ALR_BYTE_RATE)
			return info.mp3Kbps ? (double)info.dataBytes * 8.0 / (double)info.mp3Kbps : -1.0;
		return (double)info.samples * 1000.0 / (double)info.rate;
	default:
		return -1.0;
	}
}

int64_t AudioFileLength_MilesMS(const AudioFileLengthInfo &info)
{
	const double ms = AudioFileLength_MS(info, ALR_MILES);
	if (ms < 0.0)
		return -1;
	// round to nearest, ties to even, without depending on the FPU rounding mode
	const double whole = (double)(int64_t)ms;    // ms >= 0, so this is floor
	const double frac = ms - whole;
	int64_t r = (int64_t)whole;
	if (frac > 0.5 || (frac == 0.5 && (r & 1)))
		++r;
	return r;
}
