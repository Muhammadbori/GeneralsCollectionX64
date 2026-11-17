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

#include <math.h>
#include <stdio.h>
#include <vector>

namespace
{
	struct Text { const char *label; const WideChar *english; const WideChar *russian; };
	UnicodeString textOf(const Text &t) { return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian)); }

	const Text kTitle = { "GUI:VoicePreviewTitle", L"Listen to the voices:", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C \x0433\x043E\x043B\x043E\x0441\x0430:" };
	const Text kButtons[VoicePreview::UNIT_COUNT] = { { "GUI:VoicePreviewUSADozer", L"USA dozer", L"\x0411\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x0421\x0428\x0410" }, { "GUI:VoicePreviewChinaDozer", L"China dozer", L"\x0411\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x041A\x0438\x0442\x0430\x044F" }, { "GUI:VoicePreviewGLAWorker", L"GLA worker", L"\x0420\x0430\x0431\x043E\x0447\x0438\x0439 GLA" } };
	const Text kTips[VoicePreview::UNIT_COUNT] = { { "GUI:VoicePreviewUSADozerTip", L"Listen: USA dozer (select voice)", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C: \x0431\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x0421\x0428\x0410 (\x0433\x043E\x043B\x043E\x0441 \x043F\x0440\x0438 \x0432\x044B\x0431\x043E\x0440\x0435)" }, { "GUI:VoicePreviewChinaDozerTip", L"Listen: China dozer (select voice)", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C: \x0431\x0443\x043B\x044C\x0434\x043E\x0437\x0435\x0440 \x041A\x0438\x0442\x0430\x044F (\x0433\x043E\x043B\x043E\x0441 \x043F\x0440\x0438 \x0432\x044B\x0431\x043E\x0440\x0435)" }, { "GUI:VoicePreviewGLAWorkerTip", L"Listen: GLA worker (select voice)", L"\x041F\x0440\x043E\x0441\x043B\x0443\x0448\x0430\x0442\x044C: \x0440\x0430\x0431\x043E\x0447\x0438\x0439 GLA (\x0433\x043E\x043B\x043E\x0441 \x043F\x0440\x0438 \x0432\x044B\x0431\x043E\x0440\x0435)" } };
	// O51 (row 32): the player never sees a file name: the unit and "line n of m", or the phrase from the pack's phrases.txt
	const Text kPlaying = { "GUI:VoicePreviewPlayingLine", L"%ls: line %d of %d (%d.%d s)", L"%ls: \x0440\x0435\x043F\x043B\x0438\x043A\x0430 %d \x0438\x0437 %d (%d,%d \x0441)" };
	const Text kPlayingPhrase = { "GUI:VoicePreviewPlayingPhrase", L"%ls: \x201C%ls\x201D (%d.%d s)", L"%ls: \x00AB%ls\x00BB (%d,%d \x0441)" };
	const Text kPhraseNoUnit = { "GUI:VoicePreviewPhraseNoUnit", L"\x201C%ls\x201D (%d.%d s)", L"\x00AB%ls\x00BB (%d,%d \x0441)" };   // O69
	const Text kPhraseOnly = { "GUI:VoicePreviewPhraseOnly", L"\x201C%ls\x201D", L"\x00AB%ls\x00BB" };   // O69
	UnicodeString s_statusNoUnit;    ///< O69: the shorter forms of the last status
	UnicodeString s_statusPhrase;
	const Text kMissing = { "GUI:VoicePreviewMissingLine", L"This voice pack has no such line (%ls, line %d of %d).", L"\x0412 \x044D\x0442\x043E\x043C \x043F\x0430\x043A\x0435\x0442\x0435 \x043E\x0437\x0432\x0443\x0447\x043A\x0438 \x043D\x0435\x0442 \x044D\x0442\x043E\x0439 \x0440\x0435\x043F\x043B\x0438\x043A\x0438 (%ls, \x0440\x0435\x043F\x043B\x0438\x043A\x0430 %d \x0438\x0437 %d)." };
	const Text kNoSound = { "GUI:VoicePreviewNoSoundLine", L"Found (%ls, line %d of %d), but the sound is off or not available.", L"\x041D\x0430\x0439\x0434\x0435\x043D\x043E (%ls, \x0440\x0435\x043F\x043B\x0438\x043A\x0430 %d \x0438\x0437 %d), \x043D\x043E \x0437\x0432\x0443\x043A \x0432\x044B\x043A\x043B\x044E\x0447\x0435\x043D \x0438\x043B\x0438 \x043D\x0435\x0434\x043E\x0441\x0442\x0443\x043F\x0435\x043D." };
	const Text kNoUnit = { "GUI:VoicePreviewNoUnit", L"This unit's voice is not in the game data.", L"\x0413\x043E\x043B\x043E\x0441\x0430 \x044D\x0442\x043E\x0433\x043E \x044E\x043D\x0438\x0442\x0430 \x043D\x0435\x0442 \x0432 \x0434\x0430\x043D\x043D\x044B\x0445 \x0438\x0433\x0440\x044B." };

	/// The units by their object names in the game's data (the plain ones, not a general's).
	const char *const kObjects[VoicePreview::UNIT_COUNT] = { "AmericaVehicleDozer", "ChinaVehicleDozer", "GLAInfantryWorker" };
	const char *const kTags[VoicePreview::UNIT_COUNT] = { "usa-dozer", "china-dozer", "gla-worker" };
	const UnsignedInt kMaxFileBytes = 32u * 1024u * 1024u;
	Int s_next[VoicePreview::UNIT_COUNT] = { 0, 0, 0 };
	// O55: the preview playing now, for its meter line when it is replaced or the screen goes
	Bool s_playing = FALSE;
	Bool s_playingMuted = FALSE;   ///< O57: pressed while the window had lost the focus
	AsciiString s_playingWhat;
	Int s_playingMS = 0;

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

	/// TheSuperHackers @feature O51 (row 32) The phrase a voice line says, from the voice pack's phrases.txt
	/// (Packs\Voice\<Pack>\phrases.txt; the base data's: Packs\Voice\phrases.txt): UTF-8 lines "<sound name> = <phrase>",
	/// ';' or '#' starts a comment line, an empty phrase or a missing file or name: none (the status says "line n of m").
	UnicodeString phraseOf(const Packs::Pack *pack, const AsciiString &sound)
	{
		AsciiString path;
		if (pack != nullptr) {
			path = pack->folder;
		} else {
			char exe[_MAX_PATH];
			const DWORD n = GetModuleFileNameA(nullptr, exe, ARRAY_SIZE(exe));
			if (n == 0 || n >= ARRAY_SIZE(exe)) {
				return UnicodeString::TheEmptyString;
			}
			char *slash = strrchr(exe, '\\');
			if (slash != nullptr) {
				slash[1] = '\0';
			}
			path = exe;
			path.concat("Packs\\");
			path.concat(Packs::kindFolder(Packs::KIND_VOICE));
			path.concat("\\");
		}
		path.concat("phrases.txt");
		FILE *f = fopen(path.str(), "rb");
		if (f == nullptr) {
			return UnicodeString::TheEmptyString;
		}
		UnicodeString found;
		char line[1024];
		Bool first = TRUE;
		while (found.isEmpty() && fgets(line, sizeof(line), f) != nullptr)
		{
			char *text = line;
			if (first && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
				text += 3;
			}
			first = FALSE;
			AsciiString all(text);
			all.trim();
			if (all.isEmpty() || all.getCharAt(0) == ';' || all.getCharAt(0) == '#') {
				continue;
			}
			const char *eq = strchr(all.str(), '=');
			if (eq == nullptr) {
				continue;
			}
			AsciiString key;
			key.set(all.str(), (Int)(eq - all.str()));
			key.trim();
			if (stricmp(key.str(), sound.str()) != 0) {
				continue;
			}
			AsciiString value(eq + 1);
			value.trim();
			WideChar wide[512];
			const int w = MultiByteToWideChar(CP_UTF8, 0, value.str(), -1, wide, ARRAY_SIZE(wide));
			if (w > 1) {
				found = wide;
			}
			break;
		}
		fclose(f);
		return found;
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
			status->format(textOf(kMissing).str(), textOf(kButtons[unit]).str(), index + 1, count);
		}
		return FALSE;
	}

	// TheSuperHackers @bugfix O55 The unit's voice as in a match: a 2D sound at the line's volume times the user's sound
	// effects volume (the Options slider). Not the sound volume of the moment: while the Options screen is open the shell
	// map's script (ShellOptionsOpened) turns it to 5 % (0.72 x 0.05 = 0.036), so every preview played at about 0.03,
	// about -50 dB after the device's volume curve: not heard.
	const Int lengthMS = wavLengthMS(bytes);
	const Real lineVolume = info->m_volume;
	const Real soundVolume = TheAudio->getSystemSoundVolume();
	const Real volume = lineVolume * soundVolume;
	const Bool played = TheAudio->playPreviewSound(&bytes[0], (UnsignedInt)bytes.size(), volume);
	logLine("%s: pack %s: line %d/%d %s: archive %s, file %s, %u bytes, %d ms, volume %.3f = line %.2f x sound %.2f (now %.3f, script %.2f): %s",
		kTags[unit], packName(pack).str(), index + 1, count, sound.str(), foundIn.str(), foundAs.str(), (unsigned)bytes.size(), lengthMS,
		volume, lineVolume, soundVolume, TheAudio->getVolume(AudioAffect_Sound), TheAudio->getScriptSoundVolume(),
		played ? (TheAudio->isAudioMuted() ? "PLAYED, muted (window not focused)" : "PLAYED") : "NOT PLAYED (no audio device or an unknown format)");
	if (played)
	{
		s_playing = TRUE;
		s_playingWhat.format("%s: pack %s: line %d/%d %s: volume %.3f", kTags[unit], packName(pack).str(), index + 1, count, sound.str(), volume);
		s_playingMS = lengthMS;
		s_playingMuted = TheAudio->isAudioMuted();
	}
	if (status != nullptr)
	{
		s_statusNoUnit.clear();
		s_statusPhrase.clear();
		const UnicodeString unitName = textOf(kButtons[unit]);
		const UnicodeString phrase = played ? phraseOf(pack, sound) : UnicodeString::TheEmptyString;
		if (played && !phrase.isEmpty()) {
			status->format(textOf(kPlayingPhrase).str(), unitName.str(), phrase.str(), lengthMS / 1000, (lengthMS % 1000) / 100);
			s_statusNoUnit.format(textOf(kPhraseNoUnit).str(), phrase.str(), lengthMS / 1000, (lengthMS % 1000) / 100);
			s_statusPhrase.format(textOf(kPhraseOnly).str(), phrase.str());
		} else if (played) {
			status->format(textOf(kPlaying).str(), unitName.str(), index + 1, count, lengthMS / 1000, (lengthMS % 1000) / 100);
		} else {
			status->format(textOf(kNoSound).str(), unitName.str(), index + 1, count);
		}
	}
	return played;
}

void VoicePreview::lastStatusShorter(UnicodeString *withoutUnit, UnicodeString *phraseOnly)
{
	*withoutUnit = s_statusNoUnit;
	*phraseOnly = s_statusPhrase;
}

void VoicePreview::logMeter(const char *what, Real peak, Real rms, UnsignedInt64 frames, UnsignedInt64 expectedFrames, Real systemSoundVolume,
	Bool muted)
{
	if (muted)
	{
		char expectedText[32];
		if (expectedFrames > 0) snprintf(expectedText, sizeof(expectedText), "%llu", (unsigned long long)expectedFrames); else strlcpy(expectedText, "?", sizeof(expectedText));
		logLine("%s: out muted (window not focused), frames %llu of %s", what, (unsigned long long)frames, expectedText);
		return;
	}
	char peakText[32], rmsText[32], expected[32];
	if (peak > 0.0f) snprintf(peakText, sizeof(peakText), "%.1f", 20.0 * log10((double)peak)); else strlcpy(peakText, "-inf", sizeof(peakText));
	if (rms > 0.0f) snprintf(rmsText, sizeof(rmsText), "%.1f", 20.0 * log10((double)rms)); else strlcpy(rmsText, "-inf", sizeof(rmsText));
	if (expectedFrames > 0) snprintf(expected, sizeof(expected), "%llu", (unsigned long long)expectedFrames); else strlcpy(expected, "?", sizeof(expected));
	const Bool tooQuiet = systemSoundVolume >= 0.2f && peak < 0.01f;   // -40 dBFS
	logLine("%s: out peak %s dBFS, rms %s dBFS, frames %llu of %s%s", what, peakText, rmsText, (unsigned long long)frames, expected,
		tooQuiet ? ": TOO QUIET" : "");
}

void VoicePreview::stop()
{
	// O55: the meter of the preview that is replaced or ends with the screen
	if (s_playing && TheAudio != nullptr)
	{
		Real peak = 0.0f, rms = 0.0f;
		UnsignedInt64 frames = 0;
		if (TheAudio->previewMeter(&peak, &rms, &frames)) {
			logMeter(s_playingWhat.str(), peak, rms, frames, (UnsignedInt64)s_playingMS * 44100 / 1000, TheAudio->getSystemSoundVolume(),
				s_playingMuted || TheAudio->isAudioMuted());
		} else {
			logLine("%s: no output meter on this audio device", s_playingWhat.str());
		}
	}
	s_playing = FALSE;
	if (TheAudio != nullptr) {
		TheAudio->stopPreviewSound();
	}
}
