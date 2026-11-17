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

// NullAudioDevice.cpp //////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See NullAudioDevice.h.
//
// Threads and lock order (the same for every AudioDevice): the manager's lists first, a device lock second, and
// no device lock held while the device calls AudioDeviceListener::onVoiceDone. This device has no thread and no
// lock: the manager calls it on the main thread, and it reports ends from update(), on the main thread too.

#include "Lib/BaseType.h"
#include "AudioDevice/NullAudioDevice.h"

#include "AudioDevice/AudioFileLength.h"

#include <string.h>
#include <windows.h>

namespace
{
	enum { WAV_HEADER_BYTES = 44, WAV_FORMAT_PCM = 1, WAV_FORMAT_IMA_ADPCM = 0x11 };
}

//-------------------------------------------------------------------------------------------------
NullAudioDevice::NullAudioDevice()
	: m_listener(nullptr)
	, m_open(FALSE)
	, m_generationCounter(0)
{
}

//-------------------------------------------------------------------------------------------------
NullAudioDevice::~NullAudioDevice()
{
	close();
}

//-------------------------------------------------------------------------------------------------
AudioDevice::OpenResult NullAudioDevice::open(const AudioSettings *settings, AudioDeviceListener *listener)
{
	m_listener = listener;
	m_open = TRUE;
	return OPEN_OK;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::close()
{
	for (size_t i = 0; i < m_voices.size(); ++i) {
		delete m_voices[i];
	}
	m_voices.clear();
	for (std::list<Voice *>::iterator it = m_oneShots.begin(); it != m_oneShots.end(); ++it) {
		delete *it;
	}
	m_oneShots.clear();
	for (std::list<MovieSound *>::iterator it = m_movieSounds.begin(); it != m_movieSounds.end(); ++it) {
		delete *it;
	}
	m_movieSounds.clear();
	m_sampleLengths.clear();
	m_fileLengths.clear();
	m_listener = nullptr;
	m_open = FALSE;
}

//-------------------------------------------------------------------------------------------------
// The clock, and the ends of the voices whose time is up, in the order the voices were created.
void NullAudioDevice::update(UnsignedInt logicFrame, Bool logicIsRunning)
{
	deleteDeadVoices();
	m_clock.update(logicFrame, logicIsRunning);
	if (!m_open || m_listener == nullptr) {
		return;
	}

	struct Ended
	{
		Voice *voice;
		UnsignedInt generation;
	};
	std::vector<Ended> ended;

	const UnsignedInt64 now = m_clock.ticks();
	for (size_t i = 0; i < m_voices.size(); ++i)
	{
		Voice *v = m_voices[i];
		if (!v->playing || v->paused || v->dead) {
			continue;
		}
		if ((v->kind == AVK_Stream && v->loopCount <= 0) || v->loopsNatively) {
			continue;	// loops for ever (O72: or a sample looping in the device)
		}
		const UnsignedInt64 passes = v->kind == AVK_Stream ? (UnsignedInt64)v->loopCount : 1;
		if (now >= v->startTick + v->passTicks * passes)
		{
			v->playing = FALSE;
			v->ended = TRUE;
			Ended e = { v, v->generation };
			ended.push_back(e);
		}
	}

	// The manager may start, stop or release voices from onVoiceDone; a voice released or started again since
	// it was collected has a new generation and its end is dropped, as Miles drops it.
	for (size_t i = 0; i < ended.size(); ++i)
	{
		Voice *v = ended[i].voice;
		if (!v->dead && v->generation == ended[i].generation) {
			m_listener->onVoiceDone((uintptr_t)v, v->kind, v->generation);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::deleteDeadVoices()
{
	size_t kept = 0;
	for (size_t i = 0; i < m_voices.size(); ++i)
	{
		if (m_voices[i]->dead) {
			delete m_voices[i];
		} else {
			m_voices[kept++] = m_voices[i];
		}
	}
	m_voices.resize(kept);
}

//-------------------------------------------------------------------------------------------------
AsciiString NullAudioDevice::providerName(UnsignedInt index) const
{
	if (index == 0) {
		return AsciiString(name());
	}
	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
NullAudioDevice::Voice *NullAudioDevice::newVoice(AudioVoiceKind kind)
{
	Voice *v = NEW Voice;
	v->kind = kind;
	v->debugIndex = 0;
	v->generation = ++m_generationCounter;   // O71
	v->loopsNatively = FALSE;
	v->playing = FALSE;
	v->ended = FALSE;
	v->paused = FALSE;
	v->dead = FALSE;
	v->startTick = 0;
	v->pausedTick = 0;
	v->passTicks = 1;
	v->loopCount = 1;	// Miles' default stream loop count
	v->volume = 1.0f;
	v->pan = 0.5f;
	v->playMS = 0.0f;
	return v;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::startVoice(Voice *v, Real ms)
{
	v->generation = ++m_generationCounter;
	v->loopsNatively = FALSE;   // O72
	v->playing = TRUE;
	v->ended = FALSE;
	v->paused = FALSE;
	v->startTick = m_clock.ticks();
	v->passTicks = AudioDeviceClock::msToTicks(ms);
}

//-------------------------------------------------------------------------------------------------
UnsignedInt64 NullAudioDevice::passesDone(const Voice *v) const
{
	if (v->ended) {
		return v->loopCount > 0 ? (UnsignedInt64)v->loopCount : 0;
	}
	if (!v->playing) {
		return 0;
	}
	const UnsignedInt64 now = v->paused ? v->pausedTick : m_clock.ticks();
	return now > v->startTick ? (now - v->startTick) / v->passTicks : 0;
}

//-------------------------------------------------------------------------------------------------
AudioVoice NullAudioDevice::allocVoice(AudioVoiceKind kind, UnsignedInt provider, Int debugIndex)
{
	Voice *v = newVoice(kind);
	v->debugIndex = debugIndex;
	m_voices.push_back(v);
	return (AudioVoice)v;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::freeVoice(AudioVoice voice, AudioVoiceKind kind)
{
	Voice *v = toVoice(voice);
	v->generation = ++m_generationCounter;
	v->loopsNatively = FALSE;   // O72
	v->playing = FALSE;
	v->dead = TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool NullAudioDevice::setVoiceLooping(AudioVoice voice, AudioVoiceKind kind, Bool loop)
{
	// O72: as miniaudio: looping, no end; set FALSE, the end comes at the end of the pass now playing
	Voice *v = toVoice(voice);
	if (kind == AVK_Stream || !v->playing || v->dead) {
		return FALSE;
	}
	if (loop) {
		v->loopsNatively = TRUE;
	} else if (v->loopsNatively) {
		const UnsignedInt64 now = v->paused ? v->pausedTick : m_clock.ticks();
		if (now > v->startTick && v->passTicks > 0) {
			v->startTick += ((now - v->startTick) / v->passTicks) * v->passTicks;
		}
		v->loopsNatively = FALSE;
	}
	return TRUE;
}

Int NullAudioDevice::playingVoiceCount() const
{
	Int n = 0;
	for (size_t i = 0; i < m_voices.size(); ++i) {
		if (m_voices[i]->playing && !m_voices[i]->dead) {
			++n;
		}
	}
	return n;
}

UnsignedInt NullAudioDevice::voiceGeneration(AudioVoice voice, AudioVoiceKind kind) const
{
	return ((const Voice *)voice)->generation;
}

Int NullAudioDevice::voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const
{
	return toVoice(voice)->debugIndex;
}

//-------------------------------------------------------------------------------------------------
// Miles' init: the voice stops, a queued end is dropped. 2D voices play at pitch 1, as with Miles, which resets the
// rate set here when the file is set (measured with the game's mss32.dll); so neither pitch nor delay changes the length.
void NullAudioDevice::prepare2D(AudioVoice voice, Real volume, Real pitchShift, Real delaySeconds)
{
	Voice *v = toVoice(voice);
	v->generation = ++m_generationCounter;
	v->loopsNatively = FALSE;   // O72
	v->playing = FALSE;
	v->ended = FALSE;
	v->paused = FALSE;
	v->volume = volume;
	v->pan = 0.5f;
}

//-------------------------------------------------------------------------------------------------
Bool NullAudioDevice::start2D(AudioVoice voice, const AudioSample &sample)
{
	if (!sample.data) {
		return FALSE;
	}
	SampleLengthMap::const_iterator it = m_sampleLengths.find(sample.data);
	startVoice(toVoice(voice), it != m_sampleLengths.end() ? it->second : 0.0f);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// 3D voices honour the pitch shift with Miles (the rate is set after the file), so the sound is shorter or longer.
void NullAudioDevice::play3D(AudioVoice voice, const AudioSample &sample, Real distanceArg1, Real distanceArg2,
	const Coord3D &position, Real volume, Real pitchShift, Bool useOcclusion, Real occlusion)
{
	Voice *v = toVoice(voice);
	v->volume = volume;
	SampleLengthMap::const_iterator it = m_sampleLengths.find(sample.data);
	Real ms = it != m_sampleLengths.end() ? it->second : 0.0f;
	if (pitchShift > 0.0f) {
		ms /= pitchShift;
	}
	startVoice(v, ms);
}

//-------------------------------------------------------------------------------------------------
// Like AIL_open_stream: null when the file cannot be opened; the stream does not play before startStream.
AudioVoice NullAudioDevice::openStream(const char *fileName)
{
	const AudioFileLengths::Entry *entry = m_fileLengths.get(AsciiString(fileName));
	if (entry == nullptr) {
		return 0;
	}
	Voice *v = newVoice(AVK_Stream);
	v->playMS = entry->playMS;
	m_voices.push_back(v);
	return (AudioVoice)v;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::startStream(AudioVoice stream, Int loopCount)
{
	Voice *v = toVoice(stream);
	if (loopCount >= 0) {
		v->loopCount = loopCount;
	}
	startVoice(v, v->playMS);
}

//-------------------------------------------------------------------------------------------------
Int NullAudioDevice::streamLoopCount(AudioVoice stream) const
{
	const Voice *v = toVoice(stream);
	return milesStreamLoopCount(v->loopCount, passesDone(v));
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::release(AudioVoice voice, AudioVoiceKind kind)
{
	Voice *v = toVoice(voice);
	v->generation = ++m_generationCounter;
	v->loopsNatively = FALSE;   // O72
	v->playing = FALSE;
	v->paused = FALSE;
	if (kind == AVK_Stream) {
		v->dead = TRUE;	// Miles closes the stream
	}
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::pause(AudioVoice voice, AudioVoiceKind kind)
{
	Voice *v = toVoice(voice);
	if (v->playing && !v->paused) {
		v->paused = TRUE;
		v->pausedTick = m_clock.ticks();
	}
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::resume(AudioVoice voice, AudioVoiceKind kind)
{
	Voice *v = toVoice(voice);
	if (v->paused) {
		v->startTick += m_clock.ticks() - v->pausedTick;
		v->paused = FALSE;
	}
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::setVolumePan(AudioVoice voice, AudioVoiceKind kind, Real volume, Real pan)
{
	Voice *v = toVoice(voice);
	v->volume = volume;
	v->pan = pan;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::getPan(AudioVoice voice, AudioVoiceKind kind, Real *pan) const
{
	*pan = toVoice(voice)->pan;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::set3DVolume(AudioVoice voice, Real volume)
{
	toVoice(voice)->volume = volume;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const
{
	const Voice *v = toVoice(voice);
	*volume = v->volume;
	if (kind != AVK_3D) {
		*pan = v->pan;
	}
	if (kind != AVK_Stream) {
		*rate = 0;	// no playback rate without a library
	}
}

//-------------------------------------------------------------------------------------------------
Int NullAudioDevice::fileLengthMS(const char *fileName) const
{
	const AudioFileLengths::Entry *entry = m_fileLengths.get(AsciiString(fileName));
	return entry != nullptr ? entry->milesMS : -1;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const
{
	audioReadWavFormat(fileBytes, info);
}

//-------------------------------------------------------------------------------------------------
// The cache keeps the file bytes (ownedByDevice FALSE, so the cache frees them as it frees a PCM file). The size it
// counts is the size of Miles' decompressed image for IMA ADPCM (a 44-byte header and 16-bit samples of full
// blocks; not measured against Miles) and the file size for PCM, as with Miles.
Bool NullAudioDevice::decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out)
{
	if (info->format != WAV_FORMAT_PCM && info->format != WAV_FORMAT_IMA_ADPCM) {
		return FALSE;
	}
	AudioFileLengthInfo lengthInfo;
	if (!AudioFileLength_ParseWav((const unsigned char *)fileBytes, fileSize, &lengthInfo)) {
		return FALSE;
	}

	out->data = fileBytes;
	out->ownedByDevice = FALSE;
	if (lengthInfo.kind == AFK_WAV_IMA_ADPCM) {
		out->bytes = (UnsignedInt)(WAV_HEADER_BYTES + lengthInfo.samplesBlocks * lengthInfo.channels * 2);
	} else {
		out->bytes = fileSize;
	}

	const double ms = AudioFileLength_MS(lengthInfo, ALR_BLOCKS);
	m_sampleLengths[fileBytes] = ms > 0.0 ? (Real)ms : 0.0f;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// Mission briefings (Miles: AIL_quick_load_and_play). Nothing reads their end.
AudioVoice NullAudioDevice::playOneShot(const char *fileName, Real volume, Real pan)
{
	Voice *v = newVoice(AVK_Stream);
	v->volume = volume;
	v->pan = pan;
	m_oneShots.push_back(v);
	return (AudioVoice)v;
}

//-------------------------------------------------------------------------------------------------
void NullAudioDevice::freeOneShot(AudioVoice voice)
{
	Voice *v = toVoice(voice);
	m_oneShots.remove(v);
	delete v;
}

//-------------------------------------------------------------------------------------------------
// Movie sound: nothing is played, but the frames are taken at the pace a device with output takes them (real time,
// from the first frame pushed), with about two seconds of queue, so a player waiting on the queue keeps its timing.
AudioVoice NullAudioDevice::openMovieSound(UnsignedInt sampleRate, UnsignedInt channels, Real volume)
{
	if (!m_open || sampleRate == 0 || channels == 0 || channels > 2) {
		return 0;
	}
	MovieSound *m = NEW MovieSound;
	m->sampleRate = sampleRate;
	m->pushed = 0;
	m->started = FALSE;
	m->startMS = 0;
	m->gaps = 0;
	m->gapFrames = 0;
	m_movieSounds.push_back(m);
	return (AudioVoice)m;
}

UnsignedInt64 NullAudioDevice::movieSoundPlayedFrames(AudioVoice sound) const
{
	const MovieSound *m = (const MovieSound *)sound;
	if (!m->started) {
		return 0;
	}
	const UnsignedInt64 elapsed = (UnsignedInt)(::GetTickCount() - m->startMS);
	const UnsignedInt64 played = elapsed * m->sampleRate / 1000;
	return played < m->pushed ? played : m->pushed;
}

UnsignedInt NullAudioDevice::pushMovieSound(AudioVoice sound, const Short *frames, UnsignedInt frameCount)
{
	MovieSound *m = (MovieSound *)sound;
	if (m->started && frameCount > 0)
	{
		// A device with output would have played silence since the queue ran dry: count that as a gap.
		const UnsignedInt64 due = (UnsignedInt64)(UnsignedInt)(::GetTickCount() - m->startMS) * m->sampleRate / 1000;
		if (due > m->pushed) {
			++m->gaps;
			m->gapFrames += due - m->pushed;
		}
	}
	const UnsignedInt64 queued = m->pushed - movieSoundPlayedFrames(sound);
	const UnsignedInt64 capacity = (UnsignedInt64)m->sampleRate * 2;
	const UnsignedInt64 room = queued < capacity ? capacity - queued : 0;
	const UnsignedInt taken = frameCount < room ? frameCount : (UnsignedInt)room;
	if (taken > 0 && !m->started) {
		m->started = TRUE;
		m->startMS = (UnsignedInt)::GetTickCount();
	}
	m->pushed += taken;
	return taken;
}

void NullAudioDevice::movieSoundGaps(AudioVoice sound, UnsignedInt *gaps, UnsignedInt64 *gapFrames) const
{
	const MovieSound *m = (const MovieSound *)sound;
	*gaps = m->gaps;
	*gapFrames = m->gapFrames;
}

void NullAudioDevice::closeMovieSound(AudioVoice sound)
{
	MovieSound *m = (MovieSound *)sound;
	m_movieSounds.remove(m);
	delete m;
}
