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

// AudioDevice.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @refactor The sound device under the audio manager (dx12-notes/av-2-design.md).
// The manager keeps every game rule (requests, limits, priorities, fades, volumes, the file cache
// policy); a device only does what it is told. No sound library type appears in this header.
// Each method is one step of the manager's current Miles code, so that the calls reach the library
// in the same order and with the same arguments as before.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

struct AudioSettings;

/// Opaque handle of a 2D sample, a 3D sample, a stream or a one-shot sound; 0 means none.
/// It is the value the manager already passes to notifyOfAudioCompletion().
typedef uintptr_t AudioVoice;

/// The kind of a voice. The values equal the manager's PlayingAudioType (PAT_Sample, PAT_3DSample,
/// PAT_Stream), so the completion flags do not change.
enum AudioVoiceKind CPP_11(: Int)
{
	AVK_2D,
	AVK_3D,
	AVK_Stream,
};

/// What the header of a sample file says (Miles: AIL_WAV_info). The device keeps its own copy of the
/// library's description in 'deviceData' between readSampleInfo and decodeSample.
struct AudioSampleInfo
{
	Int format;           ///< WAVE_FORMAT_PCM (1), WAVE_FORMAT_IMA_ADPCM (0x11), ...
	UnsignedInt channels;
	UnsignedInt rate;
	UnsignedInt bits;
	UnsignedByte deviceData[64];
};

/// The speaker setup, as the game numbers it (AudioManager::translateSpeakerTypeToUnsignedInt, the order of
/// TheSpeakerTypes in GameAudio.cpp). openProvider and setSpeakerType use these values.
enum AudioSpeakerType CPP_11(: UnsignedInt)
{
	AST_2Speakers,
	AST_Headphones,
	AST_Surround,
	AST_4Speakers,
	AST_5_1Surround,
	AST_7_1Surround,
};

/// A sample ready for start2D/play3D. 'data' is what the device plays (Miles: the complete in-memory WAV
/// image) and 'bytes' is what the manager's file cache counts against its size limit.
struct AudioSample
{
	void *data;
	UnsignedInt bytes;
	Bool ownedByDevice;   ///< produced by the device (Miles: ADPCM decompression) and freed by freeSample
};

/// Receives the end of a voice. Called on the device's own thread (Miles: its timer thread).
class AudioDeviceListener
{
public:
	virtual ~AudioDeviceListener() {}
	virtual void onVoiceDone(uintptr_t voice, UnsignedInt kind) = 0;
};

class AudioDevice
{
public:
	enum OpenResult
	{
		OPEN_OK,
		OPEN_NO_LIBRARY,  ///< the library could not be loaded: the manager turns audio off and stops opening
		OPEN_NO_OUTPUT,   ///< loaded, but no output: the manager turns audio off and carries on as before
	};

	virtual ~AudioDevice() {}

	// --- device -------------------------------------------------------------------------------
	virtual OpenResult open(const AudioSettings *settings, AudioDeviceListener *listener) = 0;
	virtual void close() = 0;               ///< also unloads the library when open() loaded it
	virtual Bool isLoaded() const = 0;
	virtual void *nativeDevice() = 0;       ///< AudioManager::getDevice()
	virtual void setStreamFileCallbacks() = 0; ///< stream files through TheFileSystem
	/// Called by the manager every update on the main thread, before it processes its requests and playing lists.
	/// 'logicFrame' is the game logic frame; 'logicIsRunning' is FALSE outside a game and while a map loads.
	/// A device with its own output (Miles) ignores it; the null device advances its clock here.
	virtual void update(UnsignedInt logicFrame, Bool logicIsRunning) = 0;

	// --- providers, speakers, listener ---------------------------------------------------------
	virtual UnsignedInt providerCount() const = 0;
	virtual AsciiString providerName(UnsignedInt index) const = 0;
	/// The device's provider policy. Miles: probes the speaker configuration, picks "Dolby Surround"
	/// or "Miles Fast 2D Positional Audio" (ignoring 'requested', as before) and falls back to the latter.
	/// 'speakerType' is updated only when the probe gives a value. Returns the index or 'failIndex'.
	virtual UnsignedInt openProvider(UnsignedInt requested, UnsignedInt failIndex, UnsignedInt *speakerType) = 0;
	virtual Bool createListener(UnsignedInt provider) = 0; ///< TRUE when the listener exists (the manager's log reads it)
	virtual void closeListenerAndProvider(UnsignedInt provider) = 0;
	virtual void setSpeakerType(UnsignedInt provider, UnsignedInt speakerType) = 0;
	virtual void setListener(const Coord3D &position, const Coord3D &orientation) = 0; ///< only when a listener exists
	virtual void initDelayFilter() = 0;

	// --- voices --------------------------------------------------------------------------------
	/// 2D: allocate, init, debug index; 3D: allocate from the provider, debug index. Returns 0 on failure.
	virtual AudioVoice allocVoice(AudioVoiceKind kind, UnsignedInt provider, Int debugIndex) = 0;
	virtual void freeVoice(AudioVoice voice, AudioVoiceKind kind) = 0;
	virtual Int voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const = 0;

	/// The first steps of playSample + initFilters: init, end callback, volume with centre pan, pitch (when
	/// pitchShift != 0: rate after init x pitchShift), delay filter (when delaySeconds > 0). The manager then
	/// loads the file (which can stop other voices when the file cache is full) and calls start2D, as before.
	virtual void prepare2D(AudioVoice voice, Real volume, Real pitchShift, Real delaySeconds) = 0;
	/// The file and start, when 'sample.data' is not null. Returns TRUE when it started.
	virtual Bool start2D(AudioVoice voice, const AudioSample &sample) = 0;
	/// The steps of playSample3D + initFilters3D: file, end callback, distances (passed through in the
	/// manager's order), position, volume, pitch (when pitchShift != 0), occlusion (when useOcclusion), start.
	virtual void play3D(AudioVoice voice, const AudioSample &sample, Real distanceArg1, Real distanceArg2,
		const Coord3D &position, Real volume, Real pitchShift, Bool useOcclusion, Real occlusion) = 0;

	virtual AudioVoice openStream(const char *fileName) = 0;
	/// Loop count (when loopCount >= 0), end callback, start.
	virtual void startStream(AudioVoice stream, Int loopCount) = 0;
	virtual Int streamLoopCount(AudioVoice stream) const = 0;

	/// Unregisters the end callback first, so no end arrives for a released voice; then stops (2D, 3D)
	/// or closes (stream).
	virtual void release(AudioVoice voice, AudioVoiceKind kind) = 0;
	virtual void pause(AudioVoice voice, AudioVoiceKind kind) = 0;
	virtual void resume(AudioVoice voice, AudioVoiceKind kind) = 0;

	virtual void setVolumePan(AudioVoice voice, AudioVoiceKind kind, Real volume, Real pan) = 0; ///< 2D and streams
	virtual void getPan(AudioVoice voice, AudioVoiceKind kind, Real *pan) const = 0;             ///< 2D and streams
	virtual void set3DVolume(AudioVoice voice, Real volume) = 0;
	virtual void set3DPosition(AudioVoice voice, const Coord3D &position) = 0;
	/// For -audioLog only: what the library reports for a started voice. 2D: volume, pan, rate; 3D: volume, rate;
	/// streams: volume, pan. A value the kind does not have is left as it is.
	virtual void debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const = 0;

	// --- files ---------------------------------------------------------------------------------
	/// The length the library reports in whole milliseconds, or -1 when the file cannot be opened.
	virtual Int fileLengthMS(const char *fileName) const = 0;
	virtual void readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const = 0;
	/// IMA ADPCM: decompresses into device memory (ownedByDevice); PCM: plays the file bytes as they are.
	/// Returns FALSE for other formats. 'fileBytes' stays owned by the caller.
	virtual Bool decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out) = 0;
	virtual void freeSample(void *deviceData) = 0;  ///< only for ownedByDevice samples

	// --- one-shots (mission briefings), movies, debug -----------------------------------------
	virtual AudioVoice playOneShot(const char *fileName, Real volume, Real pan) = 0;
	virtual void freeOneShot(AudioVoice voice) = 0;
	virtual void *movieAudioHandle(AudioVoice reserved2D) = 0;  ///< stage 2 only (Bink's DirectSound)
	virtual const char *name() const = 0;
	virtual UnsignedInt debugTimerDelay() const = 0;
};
