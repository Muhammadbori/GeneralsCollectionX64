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

// NullAudioDevice.h ////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature An audio device without output, used when the Miles library cannot be loaded
// (the 64-bit build). It plays nothing, but its voices last as long as the sound would, on the device clock
// (AudioDeviceClock: logic frames in a game, real time outside one), its file lengths are the ones Miles
// reports, and its stream loop counter counts like Miles'. So the manager, the scripts and the limits see
// what they see with Miles, and a 64-bit game takes the same script branches as a 32-bit game with Miles.

#pragma once

#include "AudioDevice/AudioDevice.h"
#include "AudioDevice/AudioDeviceCommon.h"

#include <list>
#include <vector>

class NullAudioDevice : public AudioDevice
{
public:
	NullAudioDevice();
	virtual ~NullAudioDevice() override;

	virtual OpenResult open(const AudioSettings *settings, AudioDeviceListener *listener) override;
	virtual void close() override;
	virtual Bool isLoaded() const override { return m_open; }
	virtual void *nativeDevice() override { return nullptr; }
	virtual void setStreamFileCallbacks() override {}
	virtual void update(UnsignedInt logicFrame, Bool logicIsRunning) override;

	virtual UnsignedInt providerCount() const override { return 1; }
	virtual AsciiString providerName(UnsignedInt index) const override;
	virtual UnsignedInt openProvider(UnsignedInt requested, UnsignedInt failIndex, UnsignedInt *speakerType) override { return 0; }
	virtual Bool createListener(UnsignedInt provider) override { return TRUE; }
	virtual void closeListenerAndProvider(UnsignedInt provider) override {}
	virtual void setSpeakerType(UnsignedInt provider, UnsignedInt speakerType) override {}
	virtual void setListener(const Coord3D &position, const Coord3D &orientation) override {}
	virtual void initDelayFilter() override {}

	virtual AudioVoice allocVoice(AudioVoiceKind kind, UnsignedInt provider, Int debugIndex) override;
	virtual void freeVoice(AudioVoice voice, AudioVoiceKind kind) override;
	virtual Int voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const override;

	virtual void prepare2D(AudioVoice voice, Real volume, Real pitchShift, Real delaySeconds) override;
	virtual Bool start2D(AudioVoice voice, const AudioSample &sample) override;
	virtual void play3D(AudioVoice voice, const AudioSample &sample, Real distanceArg1, Real distanceArg2,
		const Coord3D &position, Real volume, Real pitchShift, Bool useOcclusion, Real occlusion) override;

	virtual AudioVoice openStream(const char *fileName) override;
	virtual void startStream(AudioVoice stream, Int loopCount) override;
	virtual Int streamLoopCount(AudioVoice stream) const override;

	virtual void release(AudioVoice voice, AudioVoiceKind kind) override;
	virtual void pause(AudioVoice voice, AudioVoiceKind kind) override;
	virtual void resume(AudioVoice voice, AudioVoiceKind kind) override;

	virtual void setVolumePan(AudioVoice voice, AudioVoiceKind kind, Real volume, Real pan) override;
	virtual void getPan(AudioVoice voice, AudioVoiceKind kind, Real *pan) const override;
	virtual void set3DVolume(AudioVoice voice, Real volume) override;
	virtual void set3DPosition(AudioVoice voice, const Coord3D &position) override {}
	virtual void debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const override;

	virtual Int fileLengthMS(const char *fileName) const override;
	virtual void readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const override;
	virtual Bool decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out) override;
	virtual void freeSample(void *deviceData) override {}  ///< never called: decodeSample leaves the bytes to the cache

	virtual AudioVoice playOneShot(const char *fileName, Real volume, Real pan) override;
	virtual void freeOneShot(AudioVoice voice) override;
	virtual void *movieAudioHandle(AudioVoice reserved2D) override { return nullptr; }
	virtual AudioVoice openMovieSound(UnsignedInt sampleRate, UnsignedInt channels, Real volume) override;
	virtual UnsignedInt pushMovieSound(AudioVoice sound, const Short *frames, UnsignedInt frameCount) override;
	virtual UnsignedInt64 movieSoundPlayedFrames(AudioVoice sound) const override;
	virtual void closeMovieSound(AudioVoice sound) override;
	virtual const char *name() const override { return "No audio device"; }
	virtual UnsignedInt debugTimerDelay() const override { return 0; }

private:
	struct Voice
	{
		AudioVoiceKind kind;
		Int debugIndex;
		UnsignedInt generation;   ///< changes when the voice is started again or released: a queued end of the old sound is dropped
		Bool playing;             ///< started, not ended, not released
		Bool ended;               ///< played to its end (a stream's loop counter then reads 1, as Miles')
		Bool paused;
		Bool dead;                ///< released stream or freed voice, deleted at the next update
		UnsignedInt64 startTick;
		UnsignedInt64 pausedTick;
		UnsignedInt64 passTicks;  ///< one pass
		Int loopCount;            ///< streams: as set (Miles' default 1, 0 = for ever); samples: 1
		Real volume;
		Real pan;
		Real playMS;              ///< streams: one pass in milliseconds
	};

	static Voice *toVoice(AudioVoice voice) { return (Voice *)voice; }
	Voice *newVoice(AudioVoiceKind kind);
	void startVoice(Voice *v, Real ms);
	UnsignedInt64 passesDone(const Voice *v) const;
	void deleteDeadVoices();

	AudioDeviceListener *m_listener;
	Bool m_open;
	AudioDeviceClock m_clock;
	mutable AudioFileLengths m_fileLengths;

	std::vector<Voice *> m_voices;          ///< 2D and 3D voices and streams, in creation order
	std::list<Voice *> m_oneShots;

	/// Movie sound: taken in real time, about two seconds ahead at most, as a device with output would.
	struct MovieSound
	{
		UnsignedInt sampleRate;
		UnsignedInt64 pushed;
		Bool started;
		UnsignedInt startMS;
	};
	std::list<MovieSound *> m_movieSounds;

	typedef std::map<const void *, Real> SampleLengthMap;
	SampleLengthMap m_sampleLengths;        ///< play length in ms of each sample decodeSample has seen, by its data
};
