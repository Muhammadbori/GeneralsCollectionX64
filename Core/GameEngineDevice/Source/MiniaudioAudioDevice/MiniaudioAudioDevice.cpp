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
// TheSuperHackers @feature See MiniaudioAudioDevice.h. This part opens miniaudio's engine (44.1 kHz stereo, the
// game's output format; the output device converts when it runs at another rate), reads files through
// TheFileSystem, and gives the provider, listener and file lengths. Voices follow.
//
// Threads and lock order (the same for every AudioDevice): the manager's lists first, the device lock second, and
// no device lock held while the device calls AudioDeviceListener::onVoiceDone.

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
	const ma_uint32 kEngineRate = 44100;
	const ma_uint32 kEngineChannels = 2;

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
}

//-------------------------------------------------------------------------------------------------
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

	Coord3D listenerPosition;
	Coord3D listenerLook;
	AudioFileLengths fileLengths;
};

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
	return OPEN_OK;
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::close()
{
	MiniaudioDeviceState *s = m_state;
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
	return 0;
}

void MiniaudioAudioDevice::freeVoice(AudioVoice voice, AudioVoiceKind kind)
{
}

Int MiniaudioAudioDevice::voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const
{
	return 0;
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::prepare2D(AudioVoice voice, Real volume, Real pitchShift, Real delaySeconds)
{
}

Bool MiniaudioAudioDevice::start2D(AudioVoice voice, const AudioSample &sample)
{
	return FALSE;
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
}

void MiniaudioAudioDevice::pause(AudioVoice voice, AudioVoiceKind kind)
{
}

void MiniaudioAudioDevice::resume(AudioVoice voice, AudioVoiceKind kind)
{
}

//-------------------------------------------------------------------------------------------------
void MiniaudioAudioDevice::setVolumePan(AudioVoice voice, AudioVoiceKind kind, Real volume, Real pan)
{
}

void MiniaudioAudioDevice::getPan(AudioVoice voice, AudioVoiceKind kind, Real *pan) const
{
	*pan = 0.5f;
}

void MiniaudioAudioDevice::set3DVolume(AudioVoice voice, Real volume)
{
}

void MiniaudioAudioDevice::set3DPosition(AudioVoice voice, const Coord3D &position)
{
}

void MiniaudioAudioDevice::debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const
{
}

//-------------------------------------------------------------------------------------------------
Int MiniaudioAudioDevice::fileLengthMS(const char *fileName) const
{
	const AudioFileLengths::Entry *entry = m_state->fileLengths.get(AsciiString(fileName));
	return entry != nullptr ? entry->milesMS : -1;
}

void MiniaudioAudioDevice::readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const
{
	memset(info, 0, sizeof(*info));
}

Bool MiniaudioAudioDevice::decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out)
{
	return FALSE;
}

void MiniaudioAudioDevice::freeSample(void *deviceData)
{
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
