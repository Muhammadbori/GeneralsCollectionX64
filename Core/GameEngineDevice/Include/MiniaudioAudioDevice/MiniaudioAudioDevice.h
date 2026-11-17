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

// MiniaudioAudioDevice.h ///////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature An AudioDevice on miniaudio (Dependencies/miniaudio). It reproduces what the game
// hears from Miles with the game's settings (Miles Fast 2D Positional Audio, 2 speakers, 44.1 kHz), as measured
// from the game's mss32.dll with all output captured:
// - volume: gain = 0.9961 x volume^1.661 (-10 dB per halving);
// - 2D pan: L = 0.9961 x (1 - pan)^0.301, R = 0.9961 x pan^0.301;
// - 2D pitch: none, because Miles resets the rate when the manager sets the file after the rate;
// - 3D: full volume inside the minimum distance, min / distance up to the maximum, silence beyond it; linear pan
//   over the front half, rear sources at 0.75; the two distances in either order;
// - occlusion: a low-pass whose cutoff falls with the occlusion value;
// - lengths and stream loop counts: as Miles reports them.
// No miniaudio type appears here; the manager can include this header.

#pragma once

#include "AudioDevice/AudioDevice.h"

struct MiniaudioDeviceState;

class MiniaudioAudioDevice : public AudioDevice
{
public:
	/// For tests. The game uses the defaults (an output device, a decoding thread, an end thread), or nullBackend
	/// for -audio miniaudio-null.
	struct TestOptions
	{
		Bool noOutputDevice;    ///< render with renderForTest() instead of an output device
		Bool endsInUpdate;      ///< report ends from update() on the caller's thread instead of the end thread
		Bool noJobThread;       ///< decode streams in renderForTest() instead of on miniaudio's job thread
		Bool nullBackend;       ///< a device on miniaudio's null backend only: mixed in real time, never sent to any output
		/// -audioTestDeviceLoss A[,B]: A > 0: A seconds after the open the output stops as if by itself (once); the real
		/// device can be opened again B seconds after that (B < 0: never). See testSimulateOutputLoss.
		Real testLossAfterSeconds;
		Real testBackAfterSeconds;
	};

	MiniaudioAudioDevice();
	explicit MiniaudioAudioDevice(const TestOptions &options);
	virtual ~MiniaudioAudioDevice() override;

	virtual OpenResult open(const AudioSettings *settings, AudioDeviceListener *listener) override;
	virtual void close() override;
	virtual Bool isLoaded() const override;
	virtual void *nativeDevice() override;
	virtual void setStreamFileCallbacks() override {}
	virtual void update(UnsignedInt logicFrame, Bool logicIsRunning) override;

	virtual UnsignedInt providerCount() const override { return 1; }
	virtual AsciiString providerName(UnsignedInt index) const override;
	virtual UnsignedInt openProvider(UnsignedInt requested, UnsignedInt failIndex, UnsignedInt *speakerType) override { return 0; }
	virtual Bool createListener(UnsignedInt provider) override { return TRUE; }
	virtual void closeListenerAndProvider(UnsignedInt provider) override {}
	virtual void setSpeakerType(UnsignedInt provider, UnsignedInt speakerType) override {}
	virtual void setListener(const Coord3D &position, const Coord3D &orientation) override;
	virtual void initDelayFilter() override {}

	virtual AudioVoice allocVoice(AudioVoiceKind kind, UnsignedInt provider, Int debugIndex) override;
	virtual void freeVoice(AudioVoice voice, AudioVoiceKind kind) override;
	virtual Int voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const override;
	virtual UnsignedInt voiceGeneration(AudioVoice voice, AudioVoiceKind kind) const override;   ///< O71

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
	virtual void resetVoiceMeter(AudioVoice voice) override;   ///< O55
	virtual Bool readVoiceMeter(AudioVoice voice, Real *peak, Real *rms, UnsignedInt64 *frames) const override;

	virtual Int fileLengthMS(const char *fileName) const override;
	virtual void readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const override;
	virtual Bool decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out) override;
	virtual void freeSample(void *deviceData) override;

	virtual AudioVoice playOneShot(const char *fileName, Real volume, Real pan) override;
	virtual void freeOneShot(AudioVoice voice) override;
	virtual void *movieAudioHandle(AudioVoice reserved2D) override { return nullptr; }
	virtual AudioVoice openMovieSound(UnsignedInt sampleRate, UnsignedInt channels, Real volume) override;
	virtual UnsignedInt pushMovieSound(AudioVoice sound, const Short *frames, UnsignedInt frameCount) override;
	virtual UnsignedInt64 movieSoundPlayedFrames(AudioVoice sound) const override;
	virtual void movieSoundGaps(AudioVoice sound, UnsignedInt *gaps, UnsignedInt64 *gapFrames) const override;
	virtual void closeMovieSound(AudioVoice sound) override;
	virtual const char *name() const override;	///< "miniaudio", and the backend once open: "miniaudio (WASAPI backend)"
	virtual UnsignedInt debugTimerDelay() const override { return 0; }

	/// With TestOptions::noOutputDevice: mixes 'frameCount' stereo float frames at 44.1 kHz into 'out'.
	UnsignedInt renderForTest(float *out, UnsignedInt frameCount);

	/// Tests: the running output device stops as if by itself (miniaudio's audio thread left its loop: no stop of
	/// ours), and the real device cannot be opened again for 'realUnavailableMS' (< 0: for the rest of the run).
	/// The next update() calls handle it as a loss. Nothing happens when no output device runs.
	void testSimulateOutputLoss(Int realUnavailableMS);

private:
	MiniaudioDeviceState *m_state;
};
