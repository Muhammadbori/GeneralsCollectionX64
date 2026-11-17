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

// TheSuperHackers @feature See VoicePreview.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/VoicePreview.h"

#include "Common/AudioEventInfo.h"
#include "Common/AudioAffect.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "Common/GlobalData.h"
#include "Common/Registry.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameClient/GameText.h"
#include "GameClient/LocalizedFallback.h"

#include <stdio.h>
#include <vector>

namespace
{
	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }

	const Text kTitle = { "GUI:VoicePreviewTitle", L"Listen to the voices:", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C \x0433\x043E\x043B\x043E\x0441\x0430:" };
	const Text kButtons[VoicePreview::UNIT_COUNT] = { { "GUI:VoicePreviewUSADozer", L"USA dozer", L"\x0411\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x0421\x0428\x0410" }, { "GUI:VoicePreviewChinaDozer", L"China dozer", L"\x0411\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x041A\x0438\x0442\x0430\x044F" }, { "GUI:VoicePreviewGLAWorker", L"GLA worker", L"\x0420\x0430\x0431\x043E\x0447\x0438\x0439 GLA" } };
	const Text kTips[VoicePreview::UNIT_COUNT] = { { "GUI:VoicePreviewUSADozerTip", L"Listen: USA dozer (select voice)", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C: \x0431\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x0421\x0428\x0410 (\x0433\x043E\x043B\x043E\x0441 \x043F\x0440\x0438 \x0432\x044B\x0431\x043E\x0440\x0435)" }, { "GUI:VoicePreviewChinaDozerTip", L"Listen: China dozer (select voice)", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C: \x0431\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x041A\x0438\x0442\x0430\x044F (\x0433\x043E\x043B\x043E\x0441 \x043F\x0440\x0438 \x0432\x044B\x0431\x043E\x0440\x0435)" }, { "GUI:VoicePreviewGLAWorkerTip", L"Listen: GLA worker (select voice)", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C: \x0440\x0430\x0431\x043E\x0447\x0438\x0439 GLA (\x0433\x043E\x043B\x043E\x0441 \x043F\x0440\x0438 \x0432\x044B\x0431\x043E\x0440\x0435)" } };
	const Text kPlaying = { "GUI:VoicePreviewPlaying", L"Playing: %hs (%d.%d s)", L"\x0417\x0432\x0443\x0447\x0438\x0442: %hs (%d,%d \x0441)" };
	const Text kMissing = { "GUI:VoicePreviewMissing", L"This voice pack has no such line (%hs).", L"\x0412 \x044D\x0442\x043E\x043C \x043F\x0430\x043A\x0435\x0442\x0435 \x043E\x0437\x0432\x0443\x0447\x043A\x0438 \x043D\x0435\x0442 \x044D\x0442\x043E\x0439 \x0440\x0435\x043F\x043B\x0438\x043A\x0438 (%hs)." };
	const Text kNoSound = { "GUI:VoicePreviewNoSound", L"Found (%hs), but the sound is off or not available.", L"\x041D\x0430\x0439\x0434\x0435\x043D\x043E (%hs), \x043D\x043E \x0437\x0432\x0443\x043A \x0432\x044B\x043A\x043B\x044E\x0447\x0435\x043D \x0438\x043B\x0438 \x043D\x0435\x0434\x043E\x0441\x0442\x0443\x043F\x0435\x043D." };
	const Text kNoUnit = { "GUI:VoicePreviewNoUnit", L"This unit's voice is not in the game data.", L"\x0413\x043E\x043B\x043E\x0441\x0430 \x044D\x0442\x043E\x0433\x043E \x044E\x043D\x0438\x0442\x0430 \x043D\x0435\x0442 \x0432 \x0434\x0430\x043D\x043D\x044B\x0445 \x0438\x0433\x0440\x044B." };

	/// The units by their object names in the game's data (the plain ones, not a general's).
	const char *const kObjects[VoicePreview::UNIT_COUNT] = { "AmericaVehicleDozer", "ChinaVehicleDozer", "GLAInfantryWorker" };
	const char *const kTags[VoicePreview::UNIT_COUNT] = { "usa-dozer", "china-dozer", "gla-worker" };
	const UnsignedInt kMaxFileBytes = 32u * 1024u * 1024u;
	Int s_next[VoicePreview::UNIT_COUNT] = { 0, 0, 0 };

	UnsignedInt be32(const unsigned char *p)
	{
		return ((UnsignedInt)p[0] << 24) | ((UnsignedInt)p[1] << 16) | ((UnsignedInt)p[2] << 8) | (UnsignedInt)p[3];
	}

	UnsignedInt le32(const unsigned char *p)
	{
		return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8) | ((UnsignedInt)p[2] << 16) | ((UnsignedInt)p[3] << 24);
	}

	UnsignedInt le16(const unsigned char *p)
	{
		return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8);
	}

	/// One file of a .big archive, read without mounting the archive (BIGFileSystem's layout: "BIGF", size (LE), count
	/// (BE), the end of the table (BE), then per file its offset (BE), size (BE) and a zero-terminated path).
	Bool readBigEntry(const char *archive, const char *wanted, std::vector<unsigned char> &out)
	{
		FILE *f = fopen(archive, "rb");
		if (f == nullptr) {
			return FALSE;
		}
		Bool found = FALSE;
		unsigned char head[16];
		if (fread(head, 1, 16, f) == 16 && (memcmp(head, "BIGF", 4) == 0 || memcmp(head, "BIG4", 4) == 0))
		{
			const UnsignedInt count = be32(head + 8);
			const UnsignedInt tableEnd = be32(head + 12);
			if (count > 0 && count < 1000000 && tableEnd > 16 && tableEnd < 64u * 1024u * 1024u)
			{
				std::vector<unsigned char> table(tableEnd - 16);
				if (fread(&table[0], 1, table.size(), f) == table.size())
				{
					size_t p = 0;
					for (UnsignedInt i = 0; i < count && !found && p + 8 < table.size(); ++i)
					{
						const UnsignedInt offset = be32(&table[p]);
						const UnsignedInt size = be32(&table[p + 4]);
						p += 8;
						const char *name = (const char *)&table[p];
						size_t len = 0;
						while (p + len < table.size() && table[p + len] != 0) {
							++len;
						}
						if (p + len >= table.size()) {
							break;
						}
						p += len + 1;
						// the same name with either separator, any case
						Bool same = strlen(wanted) == len;
						for (size_t c = 0; same && c < len; ++c)
						{
							char a = name[c], b = wanted[c];
							a = (a == '/') ? '\\' : (char)tolower((unsigned char)a);
							b = (b == '/') ? '\\' : (char)tolower((unsigned char)b);
							same = a == b;
						}
						if (!same) {
							continue;
						}
						found = TRUE;
						if (size == 0 || size > kMaxFileBytes || fseek(f, (long)offset, SEEK_SET) != 0)
						{
							out.clear();
							break;
						}
						out.resize(size);
						if (fread(&out[0], 1, size, f) != size) {
							out.clear();
						}
					}
				}
			}
		}
		fclose(f);
		return found && !out.empty();
	}

	/// The length of a WAV file image in ms (PCM or IMA ADPCM); 0 when it cannot be told.
	Int wavLengthMS(const std::vector<unsigned char> &w)
	{
		if (w.size() < 12 || memcmp(&w[0], "RIFF", 4) != 0 || memcmp(&w[8], "WAVE", 4) != 0) {
			return 0;
		}
		UnsignedInt format = 0, channels = 0, rate = 0, blockAlign = 0, samplesPerBlock = 0, dataBytes = 0;
		size_t p = 12;
		while (p + 8 <= w.size())
		{
			const UnsignedInt size = le32(&w[p + 4]);
			const unsigned char *body = &w[p + 8];
			const size_t room = w.size() - (p + 8);
			if (memcmp(&w[p], "fmt ", 4) == 0 && room >= 16)
			{
				format = le16(body);
				channels = le16(body + 2);
				rate = le32(body + 4);
				blockAlign = le16(body + 12);
				if (size >= 20 && room >= 20) {
					samplesPerBlock = le16(body + 18);
				}
			}
			else if (memcmp(&w[p], "data", 4) == 0)
			{
				dataBytes = (UnsignedInt)((size_t)size < room ? size : room);
			}
			p += 8 + size + (size & 1);
		}
		if (rate == 0 || blockAlign == 0 || channels == 0) {
			return 0;
		}
		UnsignedInt64 frames = 0;
		if (format == 0x11 && samplesPerBlock > 0) {
			frames = (UnsignedInt64)(dataBytes / blockAlign) * samplesPerBlock;   // IMA ADPCM
		} else {
			frames = dataBytes / blockAlign;
		}
		return (Int)(frames * 1000 / rate);
	}

	void logLine(const char *format, ...)
	{
		if (TheGlobalData == nullptr) {
			return;
		}
		char path[_MAX_PATH];
		snprintf(path, sizeof(path), "%sVoicePreview.log", TheGlobalData->getPath_UserData().str());
		FILE *f = fopen(path, "a");
		if (f == nullptr) {
			return;
		}
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, format);
		vfprintf(f, format, args);
		va_end(args);
		fputc('\n', f);
		fclose(f);
	}

	AsciiString packName(const Packs::Pack *pack)
	{
		return pack != nullptr ? pack->name : AsciiString("(base data)");
	}
}

UnicodeString VoicePreview::title()
{
	return textOf(kTitle);
}

UnicodeString VoicePreview::buttonText(Int unit)
{
	return (unit >= 0 && unit < UNIT_COUNT) ? textOf(kButtons[unit]) : UnicodeString::TheEmptyString;
}

UnicodeString VoicePreview::tooltip(Int unit)
{
	return (unit >= 0 && unit < UNIT_COUNT) ? textOf(kTips[unit]) : UnicodeString::TheEmptyString;
}

Bool VoicePreview::play(Int unit, const Packs::Pack *pack, UnicodeString *status)
{
	stop();
	if (status != nullptr) {
		status->clear();
	}
	if (unit < 0 || unit >= UNIT_COUNT || TheAudio == nullptr) {
		return FALSE;
	}
	// the unit's select voice and its lines, from the game's data
	const ThingTemplate *thing = TheThingFactory != nullptr ? TheThingFactory->findTemplate(AsciiString(kObjects[unit]), FALSE) : nullptr;
	const AudioEventRTS *voice = thing != nullptr ? thing->getVoiceSelect() : nullptr;
	const AudioEventInfo *info = (voice != nullptr && voice->getEventName().isNotEmpty()) ? TheAudio->findAudioEventInfo(voice->getEventName()) : nullptr;
	if (info == nullptr || info->m_sounds.empty())
	{
		logLine("%s: pack %s: no select voice in the game data (object %s)", kTags[unit], packName(pack).str(), kObjects[unit]);
		if (status != nullptr) {
			*status = textOf(kNoUnit);
		}
		return FALSE;
	}
	const Int count = (Int)info->m_sounds.size();
	const Int index = s_next[unit] % count;
	s_next[unit] = (index + 1) % count;
	const AsciiString &sound = info->m_sounds[index];

	// the file, as AudioEventRTS::generateFilename and adjustForLocalization name it
	const AudioSettings *settings = TheAudio->getAudioSettings();
	AsciiString folder = settings->m_audioRoot;
	folder.concat("\\");
	folder.concat(info->m_soundType == AT_Streaming ? settings->m_streamingFolder : settings->m_soundsFolder);
	folder.concat("\\");
	AsciiString leaf = sound;
	leaf.concat(".");
	leaf.concat(settings->m_soundsExtension);
	AsciiString localized = folder;
	localized.concat(GetRegistryLanguage());
	localized.concat("\\");
	localized.concat(leaf);
	AsciiString plain = folder;
	plain.concat(leaf);

	// the archives the chosen pack would give the game, first one wins
	std::vector<AsciiString> archives;
	Packs::previewArchives(Packs::KIND_VOICE, pack, archives);
	std::vector<unsigned char> bytes;
	AsciiString foundIn, foundAs;
	const AsciiString names[2] = { localized, plain };
	for (Int n = 0; n < 2 && foundIn.isEmpty(); ++n)
	{
		for (size_t a = 0; a < archives.size() && foundIn.isEmpty(); ++a)
		{
			if (readBigEntry(archives[a].str(), names[n].str(), bytes))
			{
				foundIn = archives[a];
				foundAs = names[n];
			}
		}
	}
	if (foundIn.isEmpty())
	{
		logLine("%s: pack %s: line %d/%d %s: NOT FOUND in %d archive(s) (%s)", kTags[unit], packName(pack).str(), index + 1, count,
			sound.str(), (Int)archives.size(), localized.str());
		if (status != nullptr) {
			status->format(textOf(kMissing).str(), sound.str());
		}
		return FALSE;
	}

	// the unit's voice in the game: a 2D sound at the line's volume times the sound effects volume
	const Int lengthMS = wavLengthMS(bytes);
	const Real volume = info->m_volume * TheAudio->getVolume(AudioAffect_Sound);
	const Bool played = TheAudio->playPreviewSound(&bytes[0], (UnsignedInt)bytes.size(), volume);
	logLine("%s: pack %s: line %d/%d %s: archive %s, file %s, %u bytes, %d ms, volume %.2f: %s", kTags[unit], packName(pack).str(),
		index + 1, count, sound.str(), foundIn.str(), foundAs.str(), (unsigned)bytes.size(), lengthMS, volume,
		played ? "PLAYED" : "NOT PLAYED (no audio device or an unknown format)");
	if (status != nullptr)
	{
		if (played) {
			status->format(textOf(kPlaying).str(), sound.str(), lengthMS / 1000, (lengthMS % 1000) / 100);
		} else {
			status->format(textOf(kNoSound).str(), sound.str());
		}
	}
	return played;
}

void VoicePreview::stop()
{
	if (TheAudio != nullptr) {
		TheAudio->stopPreviewSound();
	}
}
