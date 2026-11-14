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

// MiniaudioAudioDevice.cpp /////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See MiniaudioAudioDevice.h.
//
// Graph: each voice is an ma_sound (spatialization off, volume 1) feeding the voice's own gain node, which applies
// Miles' gains (and, for 3D voices, the occlusion low-pass) and feeds the engine's endpoint. The engine mixes at
// 44.1 kHz stereo, the game's output format; the output device converts when it runs at another rate.
//
// Threads and lock order (the same for every AudioDevice): the manager's lists first, the device lock second, and
// no device lock held while the device calls AudioDeviceListener::onVoiceDone.
// - The audio thread only queues ends (miniaudio forbids starting or freeing a sound inside its end callback, and
//   the manager starts the next loop or portion from onVoiceDone, as it does on Miles' timer thread).
// - The end thread reports them through onVoiceDone, with the delivery lock held and the device lock released. It
//   also reports the ends miniaudio does not call back for: a looping stream reaching its stop time.
// - The main thread deletes released voices in update(), with the delivery lock held, so the end thread never
//   holds a deleted voice.
//
// TheSuperHackers @bugfix The output device. The device opens its own ma_device (the configuration ma_engine_init
// would give it, plus a notification callback) and hands it to the engine, so the engine's mix, period and latency
// are as before. A stop of the output that the device did not ask for (unplugged, invalidated, taken by the system)
// marks it lost. update() on the main thread then waits out miniaudio's own rerouting (it restarts a device on a new
// default device by itself when its notification comes first), and if the output still does not run: reopens it on
// the current default device, or runs the engine on a device of miniaudio's null backend, so the engine clock, ends,
// stream passes and movie sound go on in real time, and tries the real default device again every 2 seconds. The
// listener gets each change (onDeviceEvent). With nullBackend the real output is a null backend device as well: no
// path ever opens another backend.

#include "Lib/BaseType.h"
#include "MiniaudioAudioDevice/MiniaudioAudioDevice.h"

#include "AudioDevice/AudioDeviceCommon.h"
#include "Common/FileSystem.h"
#include "Common/file.h"

#include "miniaudio.h"

#include <windows.h>
#include <atomic>
#include <list>
#include <math.h>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
	// Miles' laws, measured with the game's mss32.dll (Fast 2D provider, 2 speakers, 44.1 kHz 16-bit output).
	const double kHeadroom = 0.9961;             ///< full scale minus the 16-bit mix headroom
	const double kVolumeExponent = 1.660964;     ///< log2(sqrt(10)): -10 dB per halving of the volume
	const double kPanExponent = 0.301;           ///< 2D pan: L = (1 - pan)^0.301, R = pan^0.301
	const double kRearFactor = 0.75;             ///< 3D sources behind the listener
	/// Miles applies no pitch to 2D samples in the manager's call order (the rate is set before the file, and
	/// setting the file resets it). Set to false to play 2D samples at the pitch the INI asks for.
	const bool kMilesResets2DPitch = true;

	const ma_uint32 kEngineRate = 44100;
	const ma_uint32 kEngineChannels = 2;
	const ma_uint32 kRampFrames = 64;            ///< gain changes glide over 64 frames, so they do not click
	const DWORD kEndThreadPollMS = 5;            ///< the end thread also wakes this often to check stop times

	enum { WAV_FORMAT_PCM = 1, WAV_FORMAT_IMA_ADPCM = 0x11, WAV_HEADER_BYTES = 44 };

	UnsignedInt rd16(const unsigned char *p) { return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8); }
	UnsignedInt rd32(const unsigned char *p) { return (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8) | ((UnsignedInt)p[2] << 16) | ((UnsignedInt)p[3] << 24); }
	void wr16(unsigned char *p, UnsignedInt v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
	void wr32(unsigned char *p, UnsignedInt v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

	double volumeGain(double volume)
	{
		return volume > 0.0 ? kHeadroom * pow(volume, kVolumeExponent) : 0.0;
	}

	void gains2D(double volume, double pan, float *left, float *right)
	{
		const double g = volumeGain(volume);
		const double p = pan < 0.0 ? 0.0 : (pan > 1.0 ? 1.0 : pan);
		*left = (float)(g * pow(1.0 - p, kPanExponent));
		*right = (float)(g * pow(p, kPanExponent));
	}

	/// The occlusion low-pass. Miles' response at occlusion 0.25, 0.5 and 0.75 is one 2-pole low-pass (Q 0.7071)
	/// at 16552, 11139 and 5795 Hz (fits within 0.15 dB from 250 Hz to 12 kHz); in between the cutoff is
	/// interpolated. At full occlusion it is a steep low-pass at about 540 Hz over a floor 27 dB down.
	struct OcclusionSetting
	{
		int biquads;     ///< 0 = no filter
		double cutoff;
		double q;
		double dry;      ///< part of the unfiltered sound mixed back (full occlusion only)
	};

	OcclusionSetting occlusionFor(double occlusion)
	{
		OcclusionSetting s = { 0, 0.0, 0.707107, 0.0 };
		if (!(occlusion > 0.02)) {
			return s;
		}
		if (occlusion >= 0.999) {
			s.biquads = 2;
			s.cutoff = 540.0;
			s.q = 0.45;
			s.dry = -0.046;
			return s;
		}
		const double fc = occlusion <= 0.5
			? 16552.0 + (occlusion - 0.25) * (11139.0 - 16552.0) / 0.25
			: 11139.0 + (occlusion - 0.5) * (5795.0 - 11139.0) / 0.25;
		if (fc >= 20000.0) {
			return s;
		}
		s.biquads = 1;
		s.cutoff = fc < 100.0 ? 100.0 : fc;
		return s;
	}

	//---------------------------------------------------------------------------------------------
	// The voice's gain node: stereo in, stereo out.
	struct GainNode
	{
		ma_node_base base;
		std::atomic<float> targetLeft;
		std::atomic<float> targetRight;
		std::atomic<int> snap;                 ///< jump to the targets (a new sound starts)
		float left;
		float right;

		OcclusionSetting requested;            ///< written by the device before 'requestedVersion' changes
		std::atomic<int> requestedVersion;
		int appliedVersion;
		OcclusionSetting active;
		ma_lpf2 lpf[2];
		bool lpfReady;
	};

	ma_lpf2_config lpfConfig(double cutoff, double q)
	{
		return ma_lpf2_config_init(ma_format_f32, kEngineChannels, kEngineRate, cutoff, q);
	}

	void gainNodeProcess(ma_node *node, const float **framesIn, ma_uint32 *frameCountIn, float **framesOut, ma_uint32 *frameCountOut)
	{
		GainNode *n = (GainNode *)node;
		const ma_uint32 frames = *frameCountOut;
		const float *in = framesIn[0];
		float *out = framesOut[0];

		const int version = n->requestedVersion.load(std::memory_order_acquire);
		if (version != n->appliedVersion)
		{
			n->active = n->requested;
			n->appliedVersion = version;
			if (n->lpfReady && n->active.biquads > 0) {
				ma_lpf2_config c = lpfConfig(n->active.cutoff, n->active.q);
				ma_lpf2_reinit(&c, &n->lpf[0]);
				ma_lpf2_reinit(&c, &n->lpf[1]);
			}
		}
		if (n->lpfReady && n->active.biquads > 0)
		{
			ma_lpf2_process_pcm_frames(&n->lpf[0], out, in, frames);
			if (n->active.biquads > 1) {
				ma_lpf2_process_pcm_frames(&n->lpf[1], out, out, frames);
			}
			if (n->active.dry != 0.0) {
				const float dry = (float)n->active.dry;
				for (ma_uint32 i = 0; i < frames * kEngineChannels; ++i) {
					out[i] = dry * in[i] + (1.0f - dry) * out[i];
				}
			}
			in = out;
		}

		const float targetLeft = n->targetLeft.load(std::memory_order_relaxed);
		const float targetRight = n->targetRight.load(std::memory_order_relaxed);
		if (n->snap.exchange(0, std::memory_order_acq_rel)) {
			n->left = targetLeft;
			n->right = targetRight;
		}
		const ma_uint32 ramp = frames < kRampFrames ? frames : kRampFrames;
		const float stepLeft = ramp ? (targetLeft - n->left) / ramp : 0.0f;
		const float stepRight = ramp ? (targetRight - n->right) / ramp : 0.0f;
		for (ma_uint32 i = 0; i < frames; ++i)
		{
			if (i < ramp) {
				n->left = (i + 1 == ramp) ? targetLeft : n->left + stepLeft;
				n->right = (i + 1 == ramp) ? targetRight : n->right + stepRight;
			}
			out[2 * i] = in[2 * i] * n->left;
			out[2 * i + 1] = in[2 * i + 1] * n->right;
		}
		*frameCountIn = frames;
	}

	ma_node_vtable g_gainNodeVtable = { gainNodeProcess, nullptr, 1, 1, 0 };

	//---------------------------------------------------------------------------------------------
	// Files through TheFileSystem, so streams come out of the game's archives.
	ma_result vfsOpen(ma_vfs *, const char *path, ma_uint32 openMode, ma_vfs_file *file)
	{
		if (openMode & MA_OPEN_MODE_WRITE) {
			return MA_INVALID_OPERATION;
		}
		File *f = TheFileSystem->openFile(path, File::READ | File::STREAMING);
		if (f == nullptr) {
			return MA_DOES_NOT_EXIST;
		}
		*file = (ma_vfs_file)f;
		return MA_SUCCESS;
	}

	ma_result vfsOpenW(ma_vfs *, const wchar_t *, ma_uint32, ma_vfs_file *) { return MA_NOT_IMPLEMENTED; }

	ma_result vfsClose(ma_vfs *, ma_vfs_file file)
	{
		((File *)file)->close();
		return MA_SUCCESS;
	}

	ma_result vfsRead(ma_vfs *, ma_vfs_file file, void *dst, size_t sizeInBytes, size_t *bytesRead)
	{
		Int n = ((File *)file)->read(dst, (Int)sizeInBytes);
		if (n < 0) {
			n = 0;
		}
		if (bytesRead) {
			*bytesRead = (size_t)n;
		}
		return (n == 0 && sizeInBytes > 0) ? MA_AT_END : MA_SUCCESS;
	}

	ma_result vfsWrite(ma_vfs *, ma_vfs_file, const void *, size_t, size_t *) { return MA_NOT_IMPLEMENTED; }

	ma_result vfsSeek(ma_vfs *, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
	{
		const File::seekMode mode = origin == ma_seek_origin_start ? File::START : (origin == ma_seek_origin_current ? File::CURRENT : File::END);
		return ((File *)file)->seek((Int)offset, mode) < 0 ? MA_ERROR : MA_SUCCESS;
	}

	ma_result vfsTell(ma_vfs *, ma_vfs_file file, ma_int64 *cursor)
	{
		*cursor = ((File *)file)->seek(0, File::CURRENT);
		return MA_SUCCESS;
	}

	ma_result vfsInfo(ma_vfs *, ma_vfs_file file, ma_file_info *info)
	{
		info->sizeInBytes = (ma_uint64)((File *)file)->size();
		return MA_SUCCESS;
	}

	/// A decoded sample: a 44-byte PCM WAV header (the size the manager's cache counts, as Miles' decompressed
	/// image) followed by 16-bit samples.
	struct DecodedSample
	{
		UnsignedInt channels;
		UnsignedInt rate;
		ma_uint64 frames;
		const void *pcm;
	};

	bool readDecodedSample(const void *data, DecodedSample *out)
	{
		const unsigned char *d = (const unsigned char *)data;
		if (d == nullptr || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0 || memcmp(d + 36, "data", 4) != 0) {
			return false;
		}
		out->channels = rd16(d + 22);
		out->rate = rd32(d + 24);
		if (out->channels == 0 || out->rate == 0 || rd16(d + 34) != 16) {
			return false;
		}
		out->frames = rd32(d + 40) / (out->channels * 2);
		out->pcm = d + WAV_HEADER_BYTES;
		return true;
	}
}

//-------------------------------------------------------------------------------------------------
/// Movie sound: a ring buffer of 16-bit frames the video player fills; the audio thread plays silence when it runs
/// dry (the decoder is behind) and counts only the frames it really played.
struct MiniaudioMovieSource
{
	ma_data_source_base base;        ///< first: miniaudio sees this struct as the data source
	ma_pcm_rb buffer;
	ma_uint32 channels;
	ma_uint32 sampleRate;
	std::atomic<ma_uint64> played;
	// -audioLog, written by the audio thread: a gap is a run of silence after the first frame played that more frames
	// follow (the silence after the last frame, until the close, is not a gap).
	ma_uint64 dryFrames;                 ///< the silence since the last frame played (audio thread only)
	std::atomic<UnsignedInt> gaps;
	std::atomic<ma_uint64> gapFrames;
};

namespace
{
	ma_result movieRead(ma_data_source *source, void *framesOut, ma_uint64 frameCount, ma_uint64 *framesRead)
	{
		MiniaudioMovieSource *m = (MiniaudioMovieSource *)source;
		short *out = (short *)framesOut;
		ma_uint64 done = 0;
		while (done < frameCount)
		{
			ma_uint32 n = (ma_uint32)((frameCount - done) > 0x7FFFFFFF ? 0x7FFFFFFF : (frameCount - done));
			void *p = nullptr;
			if (ma_pcm_rb_acquire_read(&m->buffer, &n, &p) != MA_SUCCESS || n == 0) {
				break;
			}
			memcpy(out + done * m->channels, p, (size_t)n * m->channels * sizeof(short));
			ma_pcm_rb_commit_read(&m->buffer, n);
			done += n;
		}
		const ma_uint64 playedBefore = m->played.fetch_add(done, std::memory_order_relaxed);
		if (done > 0 && m->dryFrames > 0) {
			m->gaps.fetch_add(1, std::memory_order_relaxed);
			m->gapFrames.fetch_add(m->dryFrames, std::memory_order_relaxed);
			m->dryFrames = 0;
		}
		if (done < frameCount && playedBefore + done > 0) {
			m->dryFrames += frameCount - done;
		}
		if (done < frameCount) {
			memset(out + done * m->channels, 0, (size_t)(frameCount - done) * m->channels * sizeof(short));
		}
		if (framesRead) {
			*framesRead = frameCount;
		}
		return MA_SUCCESS;
	}

	ma_result movieSeek(ma_data_source *, ma_uint64) { return MA_NOT_IMPLEMENTED; }

	ma_result movieFormat(ma_data_source *source, ma_format *format, ma_uint32 *channels, ma_uint32 *sampleRate, ma_channel *channelMap, size_t channelMapCap)
	{
		MiniaudioMovieSource *m = (MiniaudioMovieSource *)source;
		if (format) *format = ma_format_s16;
		if (channels) *channels = m->channels;
		if (sampleRate) *sampleRate = m->sampleRate;
		if (channelMap) ma_channel_map_init_standard(ma_standard_channel_map_default, channelMap, channelMapCap, m->channels);
		return MA_SUCCESS;
	}

	ma_data_source_vtable g_movieVtable = { movieRead, movieSeek, movieFormat, nullptr, nullptr, nullptr, 0 };
}

//-------------------------------------------------------------------------------------------------
struct MiniaudioVoice
{
	MiniaudioDeviceState *state;
	AudioVoiceKind kind;
	Int debugIndex;

	GainNode node;
	bool nodeReady;
	ma_sound sound;
	bool soundReady;
	ma_audio_buffer_ref buffer;
	MiniaudioMovieSource *movie;     ///< movie sound only

	UnsignedInt generation;          ///< changes when the voice starts again or is released (under the device lock)
	std::atomic<UnsignedInt> endGeneration;  ///< the generation of the sound now playing, read by the audio thread
	bool playing;
	bool paused;
	bool ended;
	bool dead;                       ///< released stream or freed voice: deleted by update()

	Real volume;
	Real pan;
	Real pitch;
	Int rate;                        ///< the file's sample rate (for the -audioLog values)

	Real volume3D;
	Coord3D position;
	Real distanceArg1;
	Real distanceArg2;

	Int loopCount;                   ///< streams: as set (Miles' default 1; 0 = for ever)
	ma_uint64 passFrames;            ///< streams: one pass in engine frames
	ma_uint64 startTime;
	ma_uint64 pauseTime;
	bool hasStopTime;
	ma_uint64 stopTime;
	bool startedOnce;
};

struct MiniaudioEnd
{
	MiniaudioVoice *voice;
	UnsignedInt generation;
};

struct MiniaudioDeviceState
{
	MiniaudioAudioDevice::TestOptions options;
	ma_context context;              ///< the output's context: the default backends, or nullBackend: the null backend only
	bool contextReady;
	ma_device device;                ///< the output device, given to the engine (ma_engine_config::pDevice)
	bool deviceReady;
	ma_context fallbackContext;      ///< the null backend for the fallback output (without nullBackend)
	bool fallbackContextReady;
	ma_device fallbackDevice;        ///< runs the engine on the null backend while the output is lost
	bool fallbackReady;
	int output;                      ///< OUTPUT_REAL, OUTPUT_FALLBACK or OUTPUT_NONE (main thread)
	std::atomic<int> ourStop;        ///< set while the device stops or closes an output itself: no loss
	std::atomic<int> lost;           ///< a stop of an output that the device did not ask for
	std::atomic<UnsignedInt> lostAtMS;
	std::atomic<UnsignedInt> lastNotifyMS;   ///< the last notification of an output (miniaudio may be rerouting)
	std::atomic<UnsignedInt> reroutes;       ///< miniaudio's own reroutes
	UnsignedInt reroutesSeen;
	UnsignedInt lostSinceMS;         ///< when the real output was lost (main thread)
	UnsignedInt nextRetryMS;
	bool retryFailedLogged;
	UnsignedInt openMS;
	bool testLossDone;               ///< TestOptions::testLossAfterSeconds has fired
	bool testRealBlocked;            ///< testSimulateOutputLoss: the real device cannot be opened
	bool testRealBlockedForever;
	UnsignedInt testRealBackMS;
	char name[64];                   ///< name(): the backend once open
	ma_vfs_callbacks vfs;            ///< miniaudio is given its address as the ma_vfs
	ma_resource_manager resourceManager;
	bool resourceManagerReady;
	ma_engine engine;
	bool engineReady;
	AudioDeviceListener *listener;

	std::mutex lock;                 ///< the device lock
	std::mutex deliveryLock;         ///< held while ends are reported and while dead voices are deleted
	std::mutex endQueueLock;         ///< the audio thread's queue
	std::vector<MiniaudioEnd> endQueue;
	HANDLE endEvent;
	HANDLE endThread;
	std::atomic<int> quit;

	std::vector<MiniaudioVoice *> voices;
	std::list<MiniaudioVoice *> oneShots;
	std::list<MiniaudioVoice *> movieSounds;
	Coord3D listenerPosition;
	Coord3D listenerLook;
	AudioFileLengths fileLengths;
};

namespace
{
	MiniaudioVoice *toVoice(AudioVoice voice) { return (MiniaudioVoice *)voice; }

	ma_uint64 engineTime(MiniaudioDeviceState *s) { return ma_engine_get_time_in_pcm_frames(&s->engine); }

	//---------------------------------------------------------------------------------------------
	// The output device.
	enum { OUTPUT_NONE, OUTPUT_REAL, OUTPUT_FALLBACK };
	const UnsignedInt kLossGraceMS = 300;        ///< after the last notification: miniaudio's own reroute is done by then
	const UnsignedInt kRetryMS = 2000;           ///< the real default device is tried this often while it is lost
	const UnsignedInt kMaxCatchUpMS = 10000;     ///< the most engine time the fallback makes up for at its start

	/// What ma_engine_init's own device callback does.
	void outputData(ma_device *device, void *out, const void *, ma_uint32 frameCount)
	{
		MiniaudioDeviceState *s = (MiniaudioDeviceState *)device->pUserData;
		ma_engine_read_pcm_frames(&s->engine, out, frameCount, nullptr);
	}

	/// On miniaudio's threads (the audio thread for started and stopped, its notification thread for rerouted).
	void outputNotification(const ma_device_notification *n)
	{
		MiniaudioDeviceState *s = (MiniaudioDeviceState *)n->pDevice->pUserData;
		const UnsignedInt now = (UnsignedInt)::GetTickCount();
		s->lastNotifyMS.store(now);
		if (n->type == ma_device_notification_type_stopped && !s->ourStop.load()) {
			if (!s->lost.exchange(1)) {
				s->lostAtMS.store(now);
			}
		} else if (n->type == ma_device_notification_type_rerouted) {
			s->reroutes.fetch_add(1);
		}
	}

	/// The device configuration ma_engine_init gives the device it makes itself (miniaudio 0.11.26), with our
	/// callbacks: the same format, rate, channels and period, so the engine mixes and latency are as before.
	ma_device_config outputConfig(MiniaudioDeviceState *s)
	{
		ma_device_config dc = ma_device_config_init(ma_device_type_playback);
		dc.playback.pDeviceID = nullptr;
		dc.playback.format = ma_format_f32;
		dc.playback.channels = kEngineChannels;
		dc.sampleRate = kEngineRate;
		dc.dataCallback = outputData;
		dc.pUserData = s;
		dc.notificationCallback = outputNotification;
		dc.periodSizeInFrames = 0;
		dc.periodSizeInMilliseconds = 0;
		dc.noPreSilencedOutputBuffer = MA_TRUE;
		dc.noClip = MA_TRUE;
		return dc;
	}

	void setOutputName(MiniaudioDeviceState *s, ma_device *d, const char *suffix)
	{
		snprintf(s->name, sizeof(s->name), "miniaudio (%s backend%s)", ma_get_backend_name(d->pContext->backend), suffix);
	}

	void outputEvent(MiniaudioDeviceState *s, const char *event, const char *fmt, ...)
	{
		if (s->listener == nullptr) {
			return;
		}
		char detail[384];
		va_list args;
		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
		s->listener->onDeviceEvent(event, detail);
	}

	/// Stops and closes an output of ours: its stop is no loss.
	void closeOutput(MiniaudioDeviceState *s, ma_device *d, bool *ready)
	{
		if (!*ready) {
			return;
		}
		s->ourStop.store(1);
		ma_device_uninit(d);
		s->ourStop.store(0);
		*ready = false;
	}

	bool realAvailable(MiniaudioDeviceState *s, UnsignedInt now)
	{
		if (s->testRealBlocked && !s->testRealBlockedForever && (Int)(now - s->testRealBackMS) >= 0) {
			s->testRealBlocked = false;
		}
		return !s->testRealBlocked;
	}

	/// Opens the real output into s->device, not started.
	bool initReal(MiniaudioDeviceState *s, UnsignedInt now)
	{
		if (!s->contextReady || !realAvailable(s, now)) {
			return false;
		}
		ma_device_config dc = outputConfig(s);
		if (ma_device_init(&s->context, &dc, &s->device) != MA_SUCCESS) {
			return false;
		}
		s->deviceReady = true;
		return true;
	}

	bool startReal(MiniaudioDeviceState *s)
	{
		if (ma_device_start(&s->device) != MA_SUCCESS) {
			closeOutput(s, &s->device, &s->deviceReady);
			return false;
		}
		s->output = OUTPUT_REAL;
		setOutputName(s, &s->device, "");
		return true;
	}

	/// Brings the engine time up to the wall clock since the loss (at most kMaxCatchUpMS), while no output reads it, so
	/// ends and stream passes come when they would have come on a running output. Returns the milliseconds made up.
	UnsignedInt catchUp(MiniaudioDeviceState *s, UnsignedInt now, UnsignedInt lostAt)
	{
		const Int sinceLoss = (Int)(now - lostAt);    // signed: a notification may carry a later tick than 'now'
		UnsignedInt missedMS = sinceLoss > 0 ? (UnsignedInt)sinceLoss : 0;
		if (missedMS > kMaxCatchUpMS) {
			missedMS = kMaxCatchUpMS;
		}
		ma_uint64 missed = (ma_uint64)missedMS * kEngineRate / 1000;
		float scratch[1024 * kEngineChannels];
		while (missed > 0)
		{
			const ma_uint64 n = missed < 1024 ? missed : 1024;
			ma_engine_read_pcm_frames(&s->engine, scratch, n, nullptr);
			missed -= n;
		}
		return missedMS;
	}

	/// Runs the engine on the null backend, after making up the engine time lost since 'lostAt'.
	void openFallback(MiniaudioDeviceState *s, UnsignedInt now, UnsignedInt lostAt)
	{
		ma_context *context = &s->context;
		if (!s->options.nullBackend)
		{
			if (!s->fallbackContextReady)
			{
				const ma_backend backends[1] = { ma_backend_null };
				ma_context_config cc = ma_context_config_init();
				s->fallbackContextReady = ma_context_init(backends, 1, &cc, &s->fallbackContext) == MA_SUCCESS;
			}
			context = s->fallbackContextReady ? &s->fallbackContext : nullptr;
		}
		ma_device_config dc = outputConfig(s);
		if (context == nullptr || ma_device_init(context, &dc, &s->fallbackDevice) != MA_SUCCESS)
		{
			s->output = OUTPUT_NONE;
			outputEvent(s, "no_output", "fallback=failed engine_frames=%llu", (unsigned long long)engineTime(s));
			return;
		}
		s->fallbackReady = true;
		const UnsignedInt missedMS = catchUp(s, now, lostAt);
		if (ma_device_start(&s->fallbackDevice) != MA_SUCCESS)
		{
			closeOutput(s, &s->fallbackDevice, &s->fallbackReady);
			s->output = OUTPUT_NONE;
			outputEvent(s, "no_output", "fallback=not_started engine_frames=%llu", (unsigned long long)engineTime(s));
			return;
		}
		s->output = OUTPUT_FALLBACK;
		setOutputName(s, &s->fallbackDevice, ", fallback");
		outputEvent(s, "null_fallback", "device=%s endpoint=%s caught_up_ms=%u engine_frames=%llu", s->name, s->fallbackDevice.playback.name,
			missedMS, (unsigned long long)engineTime(s));
	}

	/// Back to the real default device from the fallback (or from no output).
	void restoreReal(MiniaudioDeviceState *s, UnsignedInt now)
	{
		if (!initReal(s, now))
		{
			if (!s->retryFailedLogged) {
				s->retryFailedLogged = true;
				outputEvent(s, "retry_failed", "next_try_ms=%u", kRetryMS);
			}
			if (s->output == OUTPUT_NONE) {
				openFallback(s, now, now);
			}
			return;
		}
		closeOutput(s, &s->fallbackDevice, &s->fallbackReady);    // never two outputs reading the engine at once
		if (!startReal(s)) {
			openFallback(s, now, now);
			return;
		}
		outputEvent(s, "restored", "device=%s endpoint=%s after_ms=%u engine_frames=%llu", s->name, s->device.playback.name,
			now - s->lostSinceMS, (unsigned long long)engineTime(s));
	}

	void simulateOutputLoss(MiniaudioDeviceState *s, Int realUnavailableMS)
	{
		ma_device *active = s->output == OUTPUT_REAL ? &s->device : (s->output == OUTPUT_FALLBACK ? &s->fallbackDevice : nullptr);
		if (active == nullptr) {
			return;
		}
		s->testRealBlocked = true;
		s->testRealBlockedForever = realUnavailableMS < 0;
		s->testRealBackMS = (UnsignedInt)::GetTickCount() + (UnsignedInt)(realUnavailableMS < 0 ? 0 : realUnavailableMS);
		ma_device_stop(active);    // not ourStop: the notification sees a stop nobody asked for, as after an unplug
	}

	/// update(): the test switch, miniaudio's own reroutes, a loss, and the retries of the real device.
	void watchOutput(MiniaudioDeviceState *s)
	{
		const UnsignedInt now = (UnsignedInt)::GetTickCount();
		if (s->options.testLossAfterSeconds > 0.0f && !s->testLossDone && now - s->openMS >= (UnsignedInt)(s->options.testLossAfterSeconds * 1000.0f))
		{
			s->testLossDone = true;
			simulateOutputLoss(s, s->options.testBackAfterSeconds < 0.0f ? -1 : (Int)(s->options.testBackAfterSeconds * 1000.0f));
		}

		const UnsignedInt reroutes = s->reroutes.load();
		if (reroutes != s->reroutesSeen && s->output == OUTPUT_REAL)
		{
			s->reroutesSeen = reroutes;
			outputEvent(s, "rerouted", "device=%s endpoint=%s engine_frames=%llu", s->name, s->device.playback.name, (unsigned long long)engineTime(s));
		}

		if (s->lost.load())
		{
			ma_device *active = s->output == OUTPUT_REAL ? &s->device : &s->fallbackDevice;
			const bool ready = s->output == OUTPUT_REAL ? s->deviceReady : (s->output == OUTPUT_FALLBACK ? s->fallbackReady : false);
			if (ready && ma_device_get_state(active) == ma_device_state_started) {
				s->lost.store(0);    // it runs again: miniaudio restarted it after its own reroute
				return;
			}
			if ((Int)(now - s->lastNotifyMS.load()) < (Int)kLossGraceMS) {
				return;              // miniaudio may still be rerouting it (signed: the notification may be newer than 'now')
			}
			s->lost.store(0);
			const UnsignedInt lostAt = s->lostAtMS.load();
			outputEvent(s, "lost", "device=%s endpoint=%s engine_frames=%llu", s->name, ready ? active->playback.name : "-", (unsigned long long)engineTime(s));
			if (s->output == OUTPUT_REAL)
			{
				closeOutput(s, &s->device, &s->deviceReady);
				s->output = OUTPUT_NONE;
				s->lostSinceMS = lostAt;
				s->retryFailedLogged = false;
				if (initReal(s, now))
				{
					const UnsignedInt missedMS = catchUp(s, now, lostAt);
					if (startReal(s)) {
						outputEvent(s, "restored", "device=%s endpoint=%s after_ms=%u caught_up_ms=%u engine_frames=%llu", s->name, s->device.playback.name,
							now - s->lostSinceMS, missedMS, (unsigned long long)engineTime(s));
						return;
					}
					openFallback(s, now, now);    // the engine time is made up already
					s->nextRetryMS = now + kRetryMS;
					return;
				}
			}
			else
			{
				closeOutput(s, &s->fallbackDevice, &s->fallbackReady);
				s->output = OUTPUT_NONE;
			}
			openFallback(s, now, lostAt);
			s->nextRetryMS = now + kRetryMS;
			return;
		}

		if (s->output != OUTPUT_REAL && (Int)(now - s->nextRetryMS) >= 0)
		{
			s->nextRetryMS = now + kRetryMS;
			restoreReal(s, now);
		}
	}

	void onSoundEnd(void *userData, ma_sound *)
	{
		MiniaudioVoice *v = (MiniaudioVoice *)userData;
		MiniaudioDeviceState *s = v->state;
		{
			std::lock_guard<std::mutex> guard(s->endQueueLock);
			MiniaudioEnd e = { v, v->endGeneration.load(std::memory_order_acquire) };
			s->endQueue.push_back(e);
		}
		if (s->endEvent) {
			SetEvent(s->endEvent);
		}
	}

	bool initNode(MiniaudioDeviceState *s, MiniaudioVoice *v)
	{
		GainNode &n = v->node;
		n.targetLeft.store(0.0f);
		n.targetRight.store(0.0f);
		n.snap.store(1);
		n.left = 0.0f;
		n.right = 0.0f;
		n.requested = occlusionFor(0.0);
		n.active = n.requested;
		n.requestedVersion.store(0);
		n.appliedVersion = 0;
		n.lpfReady = false;

		const ma_lpf2_config c = lpfConfig(1000.0, 0.707107);
		if (ma_lpf2_init(&c, nullptr, &n.lpf[0]) == MA_SUCCESS) {
			if (ma_lpf2_init(&c, nullptr, &n.lpf[1]) == MA_SUCCESS) {
				n.lpfReady = true;
			} else {
				ma_lpf2_uninit(&n.lpf[0], nullptr);
			}
		}

		const ma_uint32 channels[1] = { kEngineChannels };
		ma_node_config nc = ma_node_config_init();
		nc.vtable = &g_gainNodeVtable;
		nc.pInputChannels = channels;
		nc.pOutputChannels = channels;
		if (ma_node_init(ma_engine_get_node_graph(&s->engine), &nc, nullptr, &n.base) != MA_SUCCESS) {
			if (n.lpfReady) {
				ma_lpf2_uninit(&n.lpf[0], nullptr);
				ma_lpf2_uninit(&n.lpf[1], nullptr);
				n.lpfReady = false;
			}
			return false;
		}
		ma_node_attach_output_bus(&n.base, 0, ma_engine_get_endpoint(&s->engine), 0);
		v->nodeReady = true;
		return true;
	}

	MiniaudioVoice *newVoice(MiniaudioDeviceState *s, AudioVoiceKind kind)
	{
		MiniaudioVoice *v = new MiniaudioVoice;
		v->state = s;
		v->kind = kind;
		v->debugIndex = 0;
		v->nodeReady = false;
		v->soundReady = false;
		v->movie = nullptr;
		v->generation = 0;
		v->endGeneration.store(0);
		v->playing = false;
		v->paused = false;
		v->ended = false;
		v->dead = false;
		v->volume = 1.0f;
		v->pan = 0.5f;
		v->pitch = 1.0f;
		v->rate = 0;
		v->volume3D = 1.0f;
		v->position.x = v->position.y = v->position.z = 0.0f;
		v->distanceArg1 = 0.0f;
		v->distanceArg2 = 0.0f;
		v->loopCount = 1;
		v->passFrames = 1;
		v->startTime = 0;
		v->pauseTime = 0;
		v->hasStopTime = false;
		v->stopTime = 0;
		v->startedOnce = false;
		if (!initNode(s, v)) {
			delete v;
			return nullptr;
		}
		return v;
	}

	void stopSound(MiniaudioVoice *v)
	{
		if (v->soundReady) {
			ma_sound_uninit(&v->sound);
			v->soundReady = false;
		}
	}

	void deleteVoice(MiniaudioVoice *v)
	{
		stopSound(v);
		if (v->movie) {
			ma_pcm_rb_uninit(&v->movie->buffer);
			ma_data_source_uninit(&v->movie->base);
			delete v->movie;
			v->movie = nullptr;
		}
		if (v->nodeReady) {
			ma_node_uninit(&v->node.base, nullptr);
			if (v->node.lpfReady) {
				ma_lpf2_uninit(&v->node.lpf[0], nullptr);
				ma_lpf2_uninit(&v->node.lpf[1], nullptr);
			}
		}
		delete v;
	}

	void setTargets(MiniaudioVoice *v, float left, float right)
	{
		v->node.targetLeft.store(left, std::memory_order_relaxed);
		v->node.targetRight.store(right, std::memory_order_relaxed);
	}

	void applyGains2D(MiniaudioVoice *v)
	{
		float left, right;
		gains2D(v->volume, v->pan, &left, &right);
		setTargets(v, left, right);
	}

	/// Miles' 3D gains: full inside the minimum distance, min/distance up to the maximum, silence beyond it (the
	/// two distances in either order); pan linear in the angle over the front half, sources behind at 0.75.
	void applyGains3D(MiniaudioDeviceState *s, MiniaudioVoice *v)
	{
		const double g = volumeGain(v->volume3D);
		const double rx = (double)v->position.x - s->listenerPosition.x;
		const double ry = (double)v->position.y - s->listenerPosition.y;
		const double rz = (double)v->position.z - s->listenerPosition.z;
		const double distance = sqrt(rx * rx + ry * ry + rz * rz);
		const double lo = v->distanceArg1 < v->distanceArg2 ? v->distanceArg1 : v->distanceArg2;
		const double hi = v->distanceArg1 < v->distanceArg2 ? v->distanceArg2 : v->distanceArg1;
		const double falloff = distance <= lo ? 1.0 : (distance <= hi ? lo / distance : 0.0);

		// look; up is (0, 0, -1) as the game sets it; right = up x look
		double lx = s->listenerLook.x, ly = s->listenerLook.y, lz = s->listenerLook.z;
		const double ll = sqrt(lx * lx + ly * ly + lz * lz);
		if (ll > 0.0) {
			lx /= ll; ly /= ll; lz /= ll;
		} else {
			lx = 0.0; ly = 1.0; lz = 0.0;
		}
		const double ux = 0.0, uy = 0.0, uz = -1.0;
		const double gx = uy * lz - uz * ly, gy = uz * lx - ux * lz, gz = ux * ly - uy * lx;
		const double lateral = rx * gx + ry * gy + rz * gz;
		const double ahead = rx * lx + ry * ly + rz * lz;

		double angle = (lateral == 0.0 && ahead == 0.0) ? 0.0 : atan2(lateral, ahead) * 180.0 / 3.14159265358979323846;
		double rear = 1.0;
		if (ahead < 0.0) {
			rear = kRearFactor;
			angle = angle > 0.0 ? 180.0 - angle : -180.0 - angle;
		}
		const double p = 0.5 + angle / 180.0;
		const double gain = g * falloff * rear;
		setTargets(v, (float)(gain * (1.0 - p)), (float)(gain * p));
	}

	bool startFromBuffer(MiniaudioDeviceState *s, MiniaudioVoice *v, const AudioSample &sample)
	{
		DecodedSample d;
		if (!readDecodedSample(sample.data, &d)) {
			return false;
		}
		if (ma_audio_buffer_ref_init(ma_format_s16, d.channels, d.pcm, d.frames, &v->buffer) != MA_SUCCESS) {
			return false;
		}
		v->buffer.sampleRate = d.rate;
		v->rate = (Int)d.rate;

		ma_sound_config sc = ma_sound_config_init_2(&s->engine);
		sc.pDataSource = &v->buffer;
		sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
		if (ma_sound_init_ex(&s->engine, &sc, &v->sound) != MA_SUCCESS) {
			return false;
		}
		v->soundReady = true;
		ma_node_attach_output_bus(&v->sound, 0, &v->node.base, 0);
		v->endGeneration.store(v->generation, std::memory_order_release);
		ma_sound_set_end_callback(&v->sound, onSoundEnd, v);
		return true;
	}

	void writeWavHeader(unsigned char *w, UnsignedInt channels, UnsignedInt rate, UnsignedInt dataBytes)
	{
		memcpy(w, "RIFF", 4); wr32(w + 4, 36 + dataBytes); memcpy(w + 8, "WAVE", 4);
		memcpy(w + 12, "fmt ", 4); wr32(w + 16, 16); wr16(w + 20, WAV_FORMAT_PCM); wr16(w + 22, channels);
		wr32(w + 24, rate); wr32(w + 28, rate * channels * 2); wr16(w + 32, channels * 2); wr16(w + 34, 16);
		memcpy(w + 36, "data", 4); wr32(w + 40, dataBytes);
	}

	/// Reports queued ends and stop-time ends through the listener; the device lock is released while it calls.
	void deliverEnds(MiniaudioDeviceState *s)
	{
		std::lock_guard<std::mutex> delivery(s->deliveryLock);
		std::vector<MiniaudioVoice *> ended;
		{
			std::lock_guard<std::mutex> guard(s->lock);
			std::vector<MiniaudioEnd> queued;
			{
				std::lock_guard<std::mutex> q(s->endQueueLock);
				queued.swap(s->endQueue);
			}
			for (size_t i = 0; i < queued.size(); ++i)
			{
				MiniaudioVoice *v = queued[i].voice;
				if (v->dead || !v->playing || v->paused || v->generation != queued[i].generation) {
					continue;	// released or started again since: dropped, as Miles drops it
				}
				v->playing = false;
				v->ended = true;
				ended.push_back(v);
			}
			if (s->engineReady)
			{
				const ma_uint64 now = engineTime(s);
				for (size_t i = 0; i < s->voices.size(); ++i)
				{
					MiniaudioVoice *v = s->voices[i];
					if (v->playing && !v->paused && !v->dead && v->hasStopTime && now >= v->stopTime) {
						v->playing = false;
						v->ended = true;
						ended.push_back(v);
					}
				}
			}
		}
		if (s->listener) {
			for (size_t i = 0; i < ended.size(); ++i) {
				s->listener->onVoiceDone((uintptr_t)ended[i], ended[i]->kind);
			}
		}
	}

	DWORD WINAPI endThreadMain(LPVOID param)
	{
		MiniaudioDeviceState *s = (MiniaudioDeviceState *)param;
		while (!s->quit.load())
		{
			WaitForSingleObject(s->endEvent, kEndThreadPollMS);
			if (s->quit.load()) {
				break;
			}
			deliverEnds(s);
		}
		return 0;
	}

	void deleteDeadVoices(MiniaudioDeviceState *s)
	{
		std::lock_guard<std::mutex> delivery(s->deliveryLock);
		std::lock_guard<std::mutex> guard(s->lock);
		size_t kept = 0;
		std::vector<MiniaudioVoice *> dead;
		for (size_t i = 0; i < s->voices.size(); ++i)
		{
			if (s->voices[i]->dead) {
				dead.push_back(s->voices[i]);
			} else {
				s->voices[kept++] = s->voices[i];
			}
		}
		s->voices.resize(kept);
		if (dead.empty()) {
			return;
		}
		{
			std::lock_guard<std::mutex> q(s->endQueueLock);
			size_t k = 0;
			for (size_t i = 0; i < s->endQueue.size(); ++i)
			{
				bool gone = false;
				for (size_t j = 0; j < dead.size(); ++j) {
					gone = gone || s->endQueue[i].voice == dead[j];
				}
				if (!gone) {
					s->endQueue[k++] = s->endQueue[i];
				}
			}
			s->endQueue.resize(k);
		}
		for (size_t j = 0; j < dead.size(); ++j) {
			deleteVoice(dead[j]);
		}
	}
}

//-------------------------------------------------------------------------------------------------
MiniaudioAudioDevice::MiniaudioAudioDevice()
	: m_state(new MiniaudioDeviceState)
{
	m_state->options.noOutputDevice = FALSE;
	m_state->options.endsInUpdate = FALSE;
	m_state->options.noJobThread = FALSE;
	m_state->options.nullBackend = FALSE;
	m_state->contextReady = false;
	m_state->options.testLossAfterSeconds = 0.0f;
	m_state->options.testBackAfterSeconds = -1.0f;
	m_state->deviceReady = false;
	m_state->fallbackContextReady = false;
	m_state->fallbackReady = false;
	m_state->output = OUTPUT_NONE;
	m_state->ourStop.store(0);
	m_state->lost.store(0);
	m_state->lostAtMS.store(0);
	m_state->lastNotifyMS.store(0);
	m_state->reroutes.store(0);
	m_state->reroutesSeen = 0;
	m_state->lostSinceMS = 0;
	m_state->nextRetryMS = 0;
	m_state->retryFailedLogged = false;
	m_state->openMS = 0;
	m_state->testLossDone = false;
	m_state->testRealBlocked = false;
	m_state->testRealBlockedForever = false;
	m_state->testRealBackMS = 0;
	snprintf(m_state->name, sizeof(m_state->name), "miniaudio");
	m_state->resourceManagerReady = false;
	m_state->engineReady = false;
	m_state->listener = nullptr;
	m_state->endEvent = nullptr;
	m_state->endThread = nullptr;
	m_state->quit.store(0);
	m_state->listenerPosition.x = m_state->listenerPosition.y = m_state->listenerPosition.z = 0.0f;
	m_state->listenerLook.x = 0.0f; m_state->listenerLook.y = 1.0f; m_state->listenerLook.z = 0.0f;
}

MiniaudioAudioDevice::MiniaudioAudioDevice(const TestOptions &options)
	: MiniaudioAudioDevice()
{
	m_state->options = options;
}

MiniaudioAudioDevice::~MiniaudioAudioDevice()
{
	close();
	delete m_state;
}

//-------------------------------------------------------------------------------------------------
AudioDevice::OpenResult MiniaudioAudioDevice::open(const AudioSettings *settings, AudioDeviceListener *listener)
{
	MiniaudioDeviceState *s = m_state;
	s->listener = listener;

	memset(&s->vfs, 0, sizeof(s->vfs));
	s->vfs.onOpen = vfsOpen;
	s->vfs.onOpenW = vfsOpenW;
	s->vfs.onClose = vfsClose;
	s->vfs.onRead = vfsRead;
	s->vfs.onWrite = vfsWrite;
	s->vfs.onSeek = vfsSeek;
	s->vfs.onTell = vfsTell;
	s->vfs.onInfo = vfsInfo;

	ma_engine_config ec = ma_engine_config_init();
	ec.channels = kEngineChannels;
	ec.sampleRate = kEngineRate;
	if (s->options.noOutputDevice) {
		ec.noDevice = MA_TRUE;
	}
	else
	{
		// The context ma_engine_init would make (the default backends in miniaudio's order), or for -audio miniaudio-null
		// a context that knows the null backend only, so no device of it can be anything else: it pulls the mix in real
		// time and discards it. The device is ours and given to the engine, which takes its rate, channels and period.
		ma_context_config cc = ma_context_config_init();
		ma_result made;
		if (s->options.nullBackend) {
			const ma_backend backends[1] = { ma_backend_null };
			made = ma_context_init(backends, 1, &cc, &s->context);
		} else {
			made = ma_context_init(nullptr, 0, &cc, &s->context);
		}
		if (made != MA_SUCCESS) {
			return OPEN_NO_OUTPUT;
		}
		s->contextReady = true;
		ma_device_config dc = outputConfig(s);
		if (ma_device_init(&s->context, &dc, &s->device) != MA_SUCCESS) {
			ma_context_uninit(&s->context);
			s->contextReady = false;
			return OPEN_NO_OUTPUT;
		}
		s->deviceReady = true;
		ec.pDevice = &s->device;
	}
	if (s->options.noJobThread)
	{
		ma_resource_manager_config rc = ma_resource_manager_config_init();
		rc.pVFS = (ma_vfs *)&s->vfs;
		rc.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
		rc.jobThreadCount = 0;
		if (ma_resource_manager_init(&rc, &s->resourceManager) != MA_SUCCESS) {
			return OPEN_NO_OUTPUT;
		}
		s->resourceManagerReady = true;
		ec.pResourceManager = &s->resourceManager;
	}
	else
	{
		ec.pResourceManagerVFS = (ma_vfs *)&s->vfs;
	}

	if (ma_engine_init(&ec, &s->engine) != MA_SUCCESS)
	{
		closeOutput(s, &s->device, &s->deviceReady);
		if (s->resourceManagerReady) {
			ma_resource_manager_uninit(&s->resourceManager);
			s->resourceManagerReady = false;
		}
		if (s->contextReady) {
			ma_context_uninit(&s->context);
			s->contextReady = false;
		}
		return OPEN_NO_OUTPUT;
	}
	s->engineReady = true;

	// The backend in use, for the manager's DEVICE and DEVICECHOICE log lines. ma_engine_init has started the device.
	if (s->deviceReady) {
		s->output = OUTPUT_REAL;
		setOutputName(s, &s->device, "");
	} else {
		s->output = OUTPUT_NONE;
		snprintf(s->name, sizeof(s->name), "miniaudio (no device)");
	}
	s->openMS = (UnsignedInt)::GetTickCount();
	s->lost.store(0);
	s->reroutesSeen = s->reroutes.load();
	s->testLossDone = false;
	s->testRealBlocked = false;

	if (!s->options.endsInUpdate)
	{
		s->quit.store(0);
		s->endEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		s->endThread = CreateThread(nullptr, 0, endThreadMain, s, 0, nullptr);
	}
	return OPEN_OK;
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::close()
{
	MiniaudioDeviceState *s = m_state;
	if (s->endThread)
	{
		s->quit.store(1);
		SetEvent(s->endEvent);
		WaitForSingleObject(s->endThread, INFINITE);
		CloseHandle(s->endThread);
		s->endThread = nullptr;
	}
	if (s->endEvent) {
		CloseHandle(s->endEvent);
		s->endEvent = nullptr;
	}
	for (size_t i = 0; i < s->voices.size(); ++i) {
		deleteVoice(s->voices[i]);
	}
	s->voices.clear();
	for (std::list<MiniaudioVoice *>::iterator it = s->oneShots.begin(); it != s->oneShots.end(); ++it) {
		deleteVoice(*it);
	}
	s->oneShots.clear();
	for (std::list<MiniaudioVoice *>::iterator it = s->movieSounds.begin(); it != s->movieSounds.end(); ++it) {
		deleteVoice(*it);
	}
	s->movieSounds.clear();
	s->endQueue.clear();
	// The outputs before the engine: no audio thread may read the engine while it goes.
	closeOutput(s, &s->device, &s->deviceReady);
	closeOutput(s, &s->fallbackDevice, &s->fallbackReady);
	s->output = OUTPUT_NONE;
	s->lost.store(0);
	if (s->engineReady) {
		ma_engine_uninit(&s->engine);
		s->engineReady = false;
	}
	if (s->resourceManagerReady) {
		ma_resource_manager_uninit(&s->resourceManager);
		s->resourceManagerReady = false;
	}
	if (s->contextReady) {
		ma_context_uninit(&s->context);
		s->contextReady = false;
	}
	if (s->fallbackContextReady) {
		ma_context_uninit(&s->fallbackContext);
		s->fallbackContextReady = false;
	}
	snprintf(s->name, sizeof(s->name), "miniaudio");
	s->fileLengths.clear();
	s->listener = nullptr;
}

const char *MiniaudioAudioDevice::name() const { return m_state->name; }
Bool MiniaudioAudioDevice::isLoaded() const { return m_state->engineReady; }
void *MiniaudioAudioDevice::nativeDevice() { return m_state->engineReady ? &m_state->engine : nullptr; }

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::update(UnsignedInt logicFrame, Bool logicIsRunning)
{
	if (m_state->options.endsInUpdate && m_state->engineReady) {
		deliverEnds(m_state);
	}
	deleteDeadVoices(m_state);
	if (m_state->engineReady && !m_state->options.noOutputDevice) {
		watchOutput(m_state);
	}
}

void MiniaudioAudioDevice::testSimulateOutputLoss(Int realUnavailableMS)
{
	if (m_state->engineReady && !m_state->options.noOutputDevice) {
		simulateOutputLoss(m_state, realUnavailableMS);
	}
}

//-------------------------------------------------------------------------------------------------
AsciiString MiniaudioAudioDevice::providerName(UnsignedInt index) const
{
	return index == 0 ? AsciiString(name()) : AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::setListener(const Coord3D &position, const Coord3D &orientation)
{
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	s->listenerPosition = position;
	s->listenerLook = orientation;
	for (size_t i = 0; i < s->voices.size(); ++i) {
		if (s->voices[i]->kind == AVK_3D && s->voices[i]->playing) {
			applyGains3D(s, s->voices[i]);
		}
	}
}

//-------------------------------------------------------------------------------------------------
AudioVoice MiniaudioAudioDevice::allocVoice(AudioVoiceKind kind, UnsignedInt provider, Int debugIndex)
{
	MiniaudioDeviceState *s = m_state;
	if (!s->engineReady) {
		return 0;
	}
	MiniaudioVoice *v = newVoice(s, kind);
	if (v == nullptr) {
		return 0;
	}
	v->debugIndex = debugIndex;
	std::lock_guard<std::mutex> guard(s->lock);
	s->voices.push_back(v);
	return (AudioVoice)v;
}

void MiniaudioAudioDevice::freeVoice(AudioVoice voice, AudioVoiceKind kind)
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	MiniaudioVoice *v = toVoice(voice);
	++v->generation;
	stopSound(v);
	v->playing = false;
	v->dead = true;
}

Int MiniaudioAudioDevice::voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const
{
	return toVoice(voice)->debugIndex;
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::prepare2D(AudioVoice voice, Real volume, Real pitchShift, Real delaySeconds)
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	MiniaudioVoice *v = toVoice(voice);
	++v->generation;
	stopSound(v);
	v->playing = false;
	v->paused = false;
	v->ended = false;
	v->volume = volume;
	v->pan = 0.5f;
	v->pitch = pitchShift;
	applyGains2D(v);
	// delaySeconds: Miles' Mono Delay Filter with the manager's settings (Mix 0) changes nothing; left out.
}

Bool MiniaudioAudioDevice::start2D(AudioVoice voice, const AudioSample &sample)
{
	if (sample.data == nullptr) {
		return FALSE;
	}
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	MiniaudioVoice *v = toVoice(voice);
	if (!startFromBuffer(s, v, sample)) {
		return FALSE;
	}
	if (!kMilesResets2DPitch && v->pitch > 0.0f) {
		ma_sound_set_pitch(&v->sound, v->pitch);
	}
	v->node.snap.store(1);
	ma_sound_start(&v->sound);
	v->playing = true;
	return TRUE;
}

void MiniaudioAudioDevice::play3D(AudioVoice voice, const AudioSample &sample, Real distanceArg1, Real distanceArg2,
	const Coord3D &position, Real volume, Real pitchShift, Bool useOcclusion, Real occlusion)
{
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	MiniaudioVoice *v = toVoice(voice);
	++v->generation;
	stopSound(v);
	v->playing = false;
	v->paused = false;
	v->ended = false;
	v->distanceArg1 = distanceArg1;
	v->distanceArg2 = distanceArg2;
	v->position = position;
	v->volume3D = volume;
	v->pitch = pitchShift;
	applyGains3D(s, v);

	v->node.requested = occlusionFor(useOcclusion ? occlusion : 0.0);
	v->node.requestedVersion.fetch_add(1, std::memory_order_release);

	if (!startFromBuffer(s, v, sample)) {
		return;
	}
	if (pitchShift > 0.0f) {
		// The pitch shift as given. (The manager gives Miles the whole rate REAL_TO_INT(rate x pitch shift); miniaudio's
		// resampler steps its ratio in thousandths, so the whole rate would play further from Miles than this does.)
		ma_sound_set_pitch(&v->sound, pitchShift);
	}
	v->node.snap.store(1);
	ma_sound_start(&v->sound);
	v->playing = true;
}

//-------------------------------------------------------------------------------------------------
AudioVoice MiniaudioAudioDevice::openStream(const char *fileName)
{
	MiniaudioDeviceState *s = m_state;
	if (!s->engineReady) {
		return 0;
	}
	const AudioFileLengths::Entry *entry = s->fileLengths.get(AsciiString(fileName));
	if (entry == nullptr) {
		return 0;	// like AIL_open_stream: the file cannot be opened
	}
	MiniaudioVoice *v = newVoice(s, AVK_Stream);
	if (v == nullptr) {
		return 0;
	}
	if (ma_sound_init_from_file(&s->engine, fileName, MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT,
		nullptr, nullptr, &v->sound) != MA_SUCCESS)
	{
		deleteVoice(v);
		return 0;
	}
	v->soundReady = true;
	ma_node_attach_output_bus(&v->sound, 0, &v->node.base, 0);
	ma_sound_set_end_callback(&v->sound, onSoundEnd, v);
	ma_uint32 rate = 0;
	ma_sound_get_data_format(&v->sound, nullptr, nullptr, &rate, nullptr, 0);
	v->rate = (Int)rate;
	const ma_uint64 frames = (ma_uint64)(entry->playMS * (kEngineRate / 1000.0) + 0.5);
	v->passFrames = frames > 0 ? frames : 1;
	applyGains2D(v);

	std::lock_guard<std::mutex> guard(s->lock);
	s->voices.push_back(v);
	return (AudioVoice)v;
}

/// Miles: AIL_set_stream_loop_count (when loopCount >= 0) and AIL_start_stream, which plays from the start.
/// N passes: one pass plays to its end callback; more loop and stop at N x the pass length (end found by polling).
void MiniaudioAudioDevice::startStream(AudioVoice stream, Int loopCount)
{
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	MiniaudioVoice *v = toVoice(stream);
	if (!v->soundReady) {
		return;
	}
	if (loopCount >= 0) {
		v->loopCount = loopCount;
	}
	++v->generation;
	v->endGeneration.store(v->generation, std::memory_order_release);
	if (v->startedOnce) {
		ma_sound_stop(&v->sound);
		ma_sound_seek_to_pcm_frame(&v->sound, 0);
	}
	const ma_uint64 now = engineTime(s);
	ma_sound_set_looping(&v->sound, v->loopCount != 1 ? MA_TRUE : MA_FALSE);
	if (v->loopCount > 1) {
		v->hasStopTime = true;
		v->stopTime = now + v->passFrames * (ma_uint64)v->loopCount;
		ma_sound_set_stop_time_in_pcm_frames(&v->sound, v->stopTime);
	} else {
		v->hasStopTime = false;
		ma_sound_set_stop_time_in_pcm_frames(&v->sound, ~(ma_uint64)0);
	}
	v->node.snap.store(1);
	ma_sound_start(&v->sound);
	v->startTime = now;
	v->startedOnce = true;
	v->playing = true;
	v->paused = false;
	v->ended = false;
}

Int MiniaudioAudioDevice::streamLoopCount(AudioVoice stream) const
{
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	const MiniaudioVoice *v = toVoice(stream);
	if (v->loopCount <= 0) {
		return v->loopCount;
	}
	if (v->ended) {
		return 1;
	}
	if (!v->playing) {
		return v->loopCount;
	}
	const ma_uint64 now = v->paused ? v->pauseTime : engineTime(s);
	const ma_uint64 passes = now > v->startTime ? (now - v->startTime) / v->passFrames : 0;
	return milesStreamLoopCount(v->loopCount, passes);
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::release(AudioVoice voice, AudioVoiceKind kind)
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	MiniaudioVoice *v = toVoice(voice);
	++v->generation;
	stopSound(v);
	v->playing = false;
	v->paused = false;
	if (kind == AVK_Stream) {
		v->dead = true;	// Miles closes the stream; the voice goes at the next update
	}
}

void MiniaudioAudioDevice::pause(AudioVoice voice, AudioVoiceKind kind)
{
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	MiniaudioVoice *v = toVoice(voice);
	if (!v->playing || v->paused || !v->soundReady) {
		return;
	}
	ma_sound_stop(&v->sound);
	v->paused = true;
	v->pauseTime = engineTime(s);
}

void MiniaudioAudioDevice::resume(AudioVoice voice, AudioVoiceKind kind)
{
	MiniaudioDeviceState *s = m_state;
	std::lock_guard<std::mutex> guard(s->lock);
	MiniaudioVoice *v = toVoice(voice);
	if (!v->paused || !v->soundReady) {
		return;
	}
	const ma_uint64 now = engineTime(s);
	const ma_uint64 held = now - v->pauseTime;
	v->startTime += held;
	if (v->hasStopTime) {
		v->stopTime += held;
		ma_sound_set_stop_time_in_pcm_frames(&v->sound, v->stopTime);
	}
	ma_sound_start(&v->sound);
	v->paused = false;
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::setVolumePan(AudioVoice voice, AudioVoiceKind kind, Real volume, Real pan)
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	MiniaudioVoice *v = toVoice(voice);
	v->volume = volume;
	v->pan = pan;
	applyGains2D(v);
}

void MiniaudioAudioDevice::getPan(AudioVoice voice, AudioVoiceKind kind, Real *pan) const
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	*pan = toVoice(voice)->pan;
}

void MiniaudioAudioDevice::set3DVolume(AudioVoice voice, Real volume)
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	MiniaudioVoice *v = toVoice(voice);
	v->volume3D = volume;
	applyGains3D(m_state, v);
}

void MiniaudioAudioDevice::set3DPosition(AudioVoice voice, const Coord3D &position)
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	MiniaudioVoice *v = toVoice(voice);
	v->position = position;
	applyGains3D(m_state, v);
}

void MiniaudioAudioDevice::debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const
{
	std::lock_guard<std::mutex> guard(m_state->lock);
	const MiniaudioVoice *v = toVoice(voice);
	switch (kind)
	{
		case AVK_2D:
			*volume = v->volume;
			*pan = v->pan;
			*rate = v->rate;
			break;
		case AVK_3D:
			*volume = v->volume3D;
			*rate = v->pitch > 0.0f ? REAL_TO_INT(v->rate * v->pitch) : v->rate;
			break;
		case AVK_Stream:
			*volume = v->volume;
			*pan = v->pan;
			break;
	}
}

//-------------------------------------------------------------------------------------------------
Int MiniaudioAudioDevice::fileLengthMS(const char *fileName) const
{
	const AudioFileLengths::Entry *entry = m_state->fileLengths.get(AsciiString(fileName));
	return entry != nullptr ? entry->milesMS : -1;
}

void MiniaudioAudioDevice::readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const
{
	audioReadWavFormat(fileBytes, info);
}

/// Decodes PCM and IMA ADPCM WAV files to 16-bit samples at the file's own rate and channels, behind a 44-byte
/// WAV header: the same bytes Miles' AIL_decompress_ADPCM image holds, so the manager's cache counts the same.
Bool MiniaudioAudioDevice::decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out)
{
	if (info->format != WAV_FORMAT_PCM && info->format != WAV_FORMAT_IMA_ADPCM) {
		return FALSE;
	}
	ma_decoder_config dc = ma_decoder_config_init(ma_format_s16, 0, 0);
	dc.encodingFormat = ma_encoding_format_wav;
	ma_decoder decoder;
	if (ma_decoder_init_memory(fileBytes, fileSize, &dc, &decoder) != MA_SUCCESS) {
		return FALSE;
	}
	const UnsignedInt channels = decoder.outputChannels;
	const UnsignedInt rate = decoder.outputSampleRate;
	std::vector<short> pcm;
	ma_uint64 total = 0;
	ma_uint64 known = 0;
	if (ma_decoder_get_length_in_pcm_frames(&decoder, &known) == MA_SUCCESS && known > 0) {
		pcm.resize((size_t)(known * channels));
		ma_decoder_read_pcm_frames(&decoder, pcm.data(), known, &total);
	} else {
		short chunk[4096];
		const ma_uint64 chunkFrames = sizeof(chunk) / sizeof(chunk[0]) / (channels ? channels : 1);
		for (;;)
		{
			ma_uint64 got = 0;
			ma_decoder_read_pcm_frames(&decoder, chunk, chunkFrames, &got);
			if (got == 0) {
				break;
			}
			pcm.insert(pcm.end(), chunk, chunk + got * channels);
			total += got;
		}
	}
	ma_decoder_uninit(&decoder);
	if (channels == 0 || rate == 0) {
		return FALSE;
	}

	const UnsignedInt dataBytes = (UnsignedInt)(total * channels * 2);
	unsigned char *image = new unsigned char[WAV_HEADER_BYTES + dataBytes];
	writeWavHeader(image, channels, rate, dataBytes);
	memcpy(image + WAV_HEADER_BYTES, pcm.data(), dataBytes);

	out->data = image;
	out->bytes = WAV_HEADER_BYTES + dataBytes;
	out->ownedByDevice = TRUE;
	return TRUE;
}

void MiniaudioAudioDevice::freeSample(void *deviceData)
{
	delete [] (unsigned char *)deviceData;
}

//-------------------------------------------------------------------------------------------------
/// Mission briefings (Miles: AIL_quick_load_and_play + AIL_quick_set_volume(volume, 0.5)): a 2D stream with
/// the 2D gains of the volume at the centre. Nothing reads their end.
AudioVoice MiniaudioAudioDevice::playOneShot(const char *fileName, Real volume, Real pan)
{
	MiniaudioDeviceState *s = m_state;
	if (!s->engineReady) {
		return 0;
	}
	MiniaudioVoice *v = newVoice(s, AVK_Stream);
	if (v == nullptr) {
		return 0;
	}
	if (ma_sound_init_from_file(&s->engine, fileName, MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT,
		nullptr, nullptr, &v->sound) != MA_SUCCESS)
	{
		deleteVoice(v);
		return 0;
	}
	v->soundReady = true;
	ma_node_attach_output_bus(&v->sound, 0, &v->node.base, 0);
	v->volume = volume;
	v->pan = pan;
	applyGains2D(v);
	v->node.snap.store(1);
	ma_sound_start(&v->sound);
	v->playing = true;

	std::lock_guard<std::mutex> guard(s->lock);
	s->oneShots.push_back(v);
	return (AudioVoice)v;
}

void MiniaudioAudioDevice::freeOneShot(AudioVoice voice)
{
	MiniaudioDeviceState *s = m_state;
	MiniaudioVoice *v = toVoice(voice);
	{
		std::lock_guard<std::mutex> guard(s->lock);
		s->oneShots.remove(v);
	}
	deleteVoice(v);
}

//-------------------------------------------------------------------------------------------------
/// Movie sound: a voice whose data source is a ring buffer of about two seconds; the gains are the volume itself on
/// both channels (linear, as Bink's volume).
AudioVoice MiniaudioAudioDevice::openMovieSound(UnsignedInt sampleRate, UnsignedInt channels, Real volume)
{
	MiniaudioDeviceState *s = m_state;
	if (!s->engineReady || sampleRate == 0 || channels == 0 || channels > 2) {
		return 0;
	}
	MiniaudioVoice *v = newVoice(s, AVK_Stream);
	if (v == nullptr) {
		return 0;
	}
	MiniaudioMovieSource *m = new MiniaudioMovieSource;
	m->channels = channels;
	m->sampleRate = sampleRate;
	m->played.store(0);
	m->dryFrames = 0;
	m->gaps.store(0);
	m->gapFrames.store(0);
	ma_data_source_config dc = ma_data_source_config_init();
	dc.vtable = &g_movieVtable;
	if (ma_data_source_init(&dc, &m->base) != MA_SUCCESS) {
		delete m;
		deleteVoice(v);
		return 0;
	}
	if (ma_pcm_rb_init(ma_format_s16, channels, sampleRate * 2, nullptr, nullptr, &m->buffer) != MA_SUCCESS) {
		ma_data_source_uninit(&m->base);
		delete m;
		deleteVoice(v);
		return 0;
	}
	v->movie = m;

	ma_sound_config sc = ma_sound_config_init_2(&s->engine);
	sc.pDataSource = &m->base;
	sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
	if (ma_sound_init_ex(&s->engine, &sc, &v->sound) != MA_SUCCESS) {
		deleteVoice(v);
		return 0;
	}
	v->soundReady = true;
	ma_node_attach_output_bus(&v->sound, 0, &v->node.base, 0);
	v->volume = volume;
	setTargets(v, volume, volume);
	v->node.snap.store(1);
	ma_sound_start(&v->sound);
	v->playing = true;

	std::lock_guard<std::mutex> guard(s->lock);
	s->movieSounds.push_back(v);
	return (AudioVoice)v;
}

UnsignedInt MiniaudioAudioDevice::pushMovieSound(AudioVoice sound, const Short *frames, UnsignedInt frameCount)
{
	MiniaudioMovieSource *m = toVoice(sound)->movie;
	if (m == nullptr) {
		return 0;
	}
	UnsignedInt done = 0;
	while (done < frameCount)
	{
		ma_uint32 n = frameCount - done;
		void *p = nullptr;
		if (ma_pcm_rb_acquire_write(&m->buffer, &n, &p) != MA_SUCCESS || n == 0) {
			break;
		}
		memcpy(p, frames + (size_t)done * m->channels, (size_t)n * m->channels * sizeof(short));
		ma_pcm_rb_commit_write(&m->buffer, n);
		done += n;
	}
	return done;
}

UnsignedInt64 MiniaudioAudioDevice::movieSoundPlayedFrames(AudioVoice sound) const
{
	const MiniaudioMovieSource *m = toVoice(sound)->movie;
	return m ? m->played.load(std::memory_order_relaxed) : 0;
}

void MiniaudioAudioDevice::movieSoundGaps(AudioVoice sound, UnsignedInt *gaps, UnsignedInt64 *gapFrames) const
{
	const MiniaudioMovieSource *m = toVoice(sound)->movie;
	*gaps = m ? m->gaps.load(std::memory_order_relaxed) : 0;
	*gapFrames = m ? m->gapFrames.load(std::memory_order_relaxed) : 0;
}

void MiniaudioAudioDevice::closeMovieSound(AudioVoice sound)
{
	MiniaudioDeviceState *s = m_state;
	MiniaudioVoice *v = toVoice(sound);
	{
		std::lock_guard<std::mutex> guard(s->lock);
		s->movieSounds.remove(v);
	}
	deleteVoice(v);
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniaudioAudioDevice::renderForTest(float *out, UnsignedInt frameCount)
{
	MiniaudioDeviceState *s = m_state;
	if (!s->engineReady) {
		return 0;
	}
	ma_uint64 read = 0;
	ma_engine_read_pcm_frames(&s->engine, out, frameCount, &read);
	if (s->resourceManagerReady) {
		while (ma_resource_manager_process_next_job(&s->resourceManager) == MA_SUCCESS) {
		}
	}
	return (UnsignedInt)read;
}
