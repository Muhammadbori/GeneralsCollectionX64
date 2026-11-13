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
// Miles' gains and feeds the engine's endpoint. The engine mixes at
// 44.1 kHz stereo, the game's output format; the output device converts when it runs at another rate.
//
// Threads and lock order (the same for every AudioDevice): the manager's lists first, the device lock second, and
// no device lock held while the device calls AudioDeviceListener::onVoiceDone.
// - The audio thread only queues ends (miniaudio forbids starting or freeing a sound inside its end callback, and
//   the manager starts the next loop or portion from onVoiceDone, as it does on Miles' timer thread).
// - The end thread reports them through onVoiceDone, with the delivery lock held and the device lock released.
// - The main thread deletes released voices in update(), with the delivery lock held, so the end thread never
//   holds a deleted voice.

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
#include <string.h>
#include <vector>

namespace
{
	// Miles' laws, measured with the game's mss32.dll (Fast 2D provider, 2 speakers, 44.1 kHz 16-bit output).
	const double kHeadroom = 0.9961;             ///< full scale minus the 16-bit mix headroom
	const double kVolumeExponent = 1.660964;     ///< log2(sqrt(10)): -10 dB per halving of the volume
	const double kPanExponent = 0.301;           ///< 2D pan: L = (1 - pan)^0.301, R = pan^0.301
	/// Miles applies no pitch to 2D samples in the manager's call order (the rate is set before the file, and
	/// setting the file resets it). Set to false to play 2D samples at the pitch the INI asks for.
	const bool kMilesResets2DPitch = true;

	const ma_uint32 kEngineRate = 44100;
	const ma_uint32 kEngineChannels = 2;
	const ma_uint32 kRampFrames = 64;            ///< gain changes glide over 64 frames, so they do not click
	const DWORD kEndThreadPollMS = 5;            ///< the end thread also wakes this often

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
	};

	void gainNodeProcess(ma_node *node, const float **framesIn, ma_uint32 *frameCountIn, float **framesOut, ma_uint32 *frameCountOut)
	{
		GainNode *n = (GainNode *)node;
		const ma_uint32 frames = *frameCountOut;
		const float *in = framesIn[0];
		float *out = framesOut[0];

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


	ma_uint64 startTime;
	ma_uint64 pauseTime;
};

struct MiniaudioEnd
{
	MiniaudioVoice *voice;
	UnsignedInt generation;
};

struct MiniaudioDeviceState
{
	MiniaudioAudioDevice::TestOptions options;
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
	Coord3D listenerPosition;
	Coord3D listenerLook;
	AudioFileLengths fileLengths;
};

namespace
{
	MiniaudioVoice *toVoice(AudioVoice voice) { return (MiniaudioVoice *)voice; }

	ma_uint64 engineTime(MiniaudioDeviceState *s) { return ma_engine_get_time_in_pcm_frames(&s->engine); }

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

		const ma_uint32 channels[1] = { kEngineChannels };
		ma_node_config nc = ma_node_config_init();
		nc.vtable = &g_gainNodeVtable;
		nc.pInputChannels = channels;
		nc.pOutputChannels = channels;
		if (ma_node_init(ma_engine_get_node_graph(&s->engine), &nc, nullptr, &n.base) != MA_SUCCESS) {
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
		v->startTime = 0;
		v->pauseTime = 0;
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
		if (v->nodeReady) {
			ma_node_uninit(&v->node.base, nullptr);
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

	/// Reports queued ends through the listener; the device lock is released while it calls.
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
		if (s->resourceManagerReady) {
			ma_resource_manager_uninit(&s->resourceManager);
			s->resourceManagerReady = false;
		}
		return OPEN_NO_OUTPUT;
	}
	s->engineReady = true;

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
	s->endQueue.clear();
	if (s->engineReady) {
		ma_engine_uninit(&s->engine);
		s->engineReady = false;
	}
	if (s->resourceManagerReady) {
		ma_resource_manager_uninit(&s->resourceManager);
		s->resourceManagerReady = false;
	}
	s->fileLengths.clear();
	s->listener = nullptr;
}

Bool MiniaudioAudioDevice::isLoaded() const { return m_state->engineReady; }
void *MiniaudioAudioDevice::nativeDevice() { return m_state->engineReady ? &m_state->engine : nullptr; }

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::update(UnsignedInt logicFrame, Bool logicIsRunning)
{
	if (m_state->options.endsInUpdate && m_state->engineReady) {
		deliverEnds(m_state);
	}
	deleteDeadVoices(m_state);
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
}

//-------------------------------------------------------------------------------------------------
AudioVoice MiniaudioAudioDevice::openStream(const char *fileName)
{
	return 0;
}

void MiniaudioAudioDevice::startStream(AudioVoice stream, Int loopCount)
{
}

Int MiniaudioAudioDevice::streamLoopCount(AudioVoice stream) const
{
	return 1;
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
	v->startTime += engineTime(s) - v->pauseTime;
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
}

void MiniaudioAudioDevice::set3DPosition(AudioVoice voice, const Coord3D &position)
{
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
AudioVoice MiniaudioAudioDevice::playOneShot(const char *fileName, Real volume, Real pan)
{
	return 0;
}

void MiniaudioAudioDevice::freeOneShot(AudioVoice voice)
{
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
