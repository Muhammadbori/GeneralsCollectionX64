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

// MilesAudioDevice.h ///////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @refactor The Miles Sound System behind AudioDevice. Every method is the AIL code of
// MilesAudioManager it replaces, in the same order and with the same arguments.

#pragma once

#include "AudioDevice/AudioDevice.h"

class MilesAudioDevice : public AudioDevice
{
public:
	MilesAudioDevice();
	virtual ~MilesAudioDevice() override;

	virtual OpenResult open(const AudioSettings *settings, AudioDeviceListener *listener) override;
	virtual void close() override;
	virtual Bool isLoaded() const override { return m_loaded; }
	virtual void *nativeDevice() override { return m_digitalHandle; }
	virtual void setStreamFileCallbacks() override;

	virtual UnsignedInt providerCount() const override { return m_providerCount; }
	virtual AsciiString providerName(UnsignedInt index) const override;
	virtual UnsignedInt openProvider(UnsignedInt requested, UnsignedInt failIndex, UnsignedInt *speakerType) override;
	virtual Bool createListener(UnsignedInt provider) override;
	virtual void closeListenerAndProvider(UnsignedInt provider) override;
	virtual void setSpeakerType(UnsignedInt provider, UnsignedInt speakerType) override;
	virtual void setListener(const Coord3D &position, const Coord3D &orientation) override;
	virtual void initDelayFilter() override;

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
	virtual void set3DPosition(AudioVoice voice, const Coord3D &position) override;
	virtual void debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const override;

	virtual Int fileLengthMS(const char *fileName) const override;
	virtual void readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const override;
	virtual Bool decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out) override;
	virtual void freeSample(void *deviceData) override;

	virtual AudioVoice playOneShot(const char *fileName, Real volume, Real pan) override;
	virtual void freeOneShot(AudioVoice voice) override;
	virtual void *movieAudioHandle(AudioVoice reserved2D) override;
	virtual const char *name() const override { return "Miles Sound System"; }
	virtual UnsignedInt debugTimerDelay() const override;

private:
	enum { MAX_PROVIDERS = 64 };

	UnsignedInt providerIndex(const char *providerName) const;

	struct Provider
	{
		AsciiString name;
		void *id;              ///< HPROVIDER
	};

	Provider m_providers[MAX_PROVIDERS];
	UnsignedInt m_providerCount;
	void *m_digitalHandle;     ///< HDIGDRIVER
	void *m_listener;          ///< H3DPOBJECT
	void *m_delayFilter;       ///< HPROVIDER
	Bool m_loaded;
	Bool m_loadAttempted;
};
