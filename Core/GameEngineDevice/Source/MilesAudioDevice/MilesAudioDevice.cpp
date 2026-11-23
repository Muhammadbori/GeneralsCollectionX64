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

// MilesAudioDevice.cpp /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @refactor The AIL code of MilesAudioManager, moved one to one.
// The comment above each method names the manager code it replaces (MilesAudioManager.cpp line at 7b73865f8;
// the -audioLog and -audioLenDump code of d350063a9 and 566fb41bc is named by function).
//
// Threads and lock order (the same for every AudioDevice): the manager's lists first, a device lock second, and
// no device lock held while the device calls AudioDeviceListener::onVoiceDone. This device has no lock of its
// own: Miles calls the end callbacks on its timer thread and they call onVoiceDone directly.

#include "Lib/BaseType.h"
#include "MilesAudioDevice/MilesAudioDevice.h"

#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"     // PROVIDER_ERROR
#include "Common/FileSystem.h"
#include "Common/file.h"

#include "MilesLoader.h"
#include "mss/mss.h"

#ifdef _WIN32
#include <dsound.h>
#endif

namespace
{
	AudioDeviceListener *s_listener = nullptr;

	// MilesAudioManager.cpp:3052-3066 (setSampleCompleted, set3DSampleCompleted, setStreamCompleted)
	void AILCALLBACK sampleCompleted(HSAMPLE sampleCompleted)
	{
		if (s_listener)
			s_listener->onVoiceDone((uintptr_t)sampleCompleted, AVK_2D, 0);
	}

	void AILCALLBACK sample3DCompleted(H3DSAMPLE sample3DCompleted)
	{
		if (s_listener)
			s_listener->onVoiceDone((uintptr_t)sample3DCompleted, AVK_3D, 0);
	}

	void AILCALLBACK streamCompleted(HSTREAM streamCompleted)
	{
		if (s_listener)
			s_listener->onVoiceDone((uintptr_t)streamCompleted, AVK_Stream, 0);
	}

	// MilesAudioManager.cpp:3070-3100 (streaming file callbacks into TheFileSystem; 32-bit handles)
	U32 AILCALLBACK streamingFileOpen(char const *fileName, void **file_handle)
	{
#if defined(RTS_DEBUG)
		if (sizeof(U32) != sizeof(File*)) {
			RELEASE_CRASH(("streamingFileOpen - This function requires work in order to compile on non 32-bit platforms."));
		}
#endif
		(*file_handle) = (void *) TheFileSystem->openFile(fileName, File::READ | File::STREAMING);
		return ((*file_handle) != nullptr);
	}

	void AILCALLBACK streamingFileClose(void *fileHandle)
	{
		((File*) fileHandle)->close();
	}

	S32 AILCALLBACK streamingFileSeek(void *fileHandle, S32 offset, U32 type)
	{
		return ((File*) fileHandle)->seek(offset, (File::seekMode) type);
	}

	U32 AILCALLBACK streamingFileRead(void *file_handle, void *buffer, U32 bytes)
	{
		return ((File*) file_handle)->read(buffer, bytes);
	}

	static_assert(sizeof(AILSOUNDINFO) <= sizeof(((AudioSampleInfo *)nullptr)->deviceData), "AILSOUNDINFO must fit into AudioSampleInfo::deviceData");

	// The game's speaker numbers and the Miles 3D speaker constants have always had the same values: the manager
	// used to store the Miles constant and the game's number in the same variable.
	static_assert(AST_2Speakers == AIL_3D_2_SPEAKER && AST_Headphones == AIL_3D_HEADPHONE && AST_Surround == AIL_3D_SURROUND
		&& AST_4Speakers == AIL_3D_4_SPEAKER && AST_5_1Surround == AIL_3D_51_SPEAKER && AST_7_1Surround == AIL_3D_71_SPEAKER,
		"the game's speaker numbers must equal the Miles 3D speaker constants");

	/// The Miles constant for the game's speaker number. Other numbers pass unchanged, as before.
	S32 milesSpeakerType(UnsignedInt speakerType)
	{
		switch (speakerType)
		{
			case AST_2Speakers: return AIL_3D_2_SPEAKER;
			case AST_Headphones: return AIL_3D_HEADPHONE;
			case AST_Surround: return AIL_3D_SURROUND;
			case AST_4Speakers: return AIL_3D_4_SPEAKER;
			case AST_5_1Surround: return AIL_3D_51_SPEAKER;
			case AST_7_1Surround: return AIL_3D_71_SPEAKER;
		}
		return (S32)speakerType;
	}
}

//-------------------------------------------------------------------------------------------------
MilesAudioDevice::MilesAudioDevice()
	: m_providerCount(0)
	, m_digitalHandle(nullptr)
	, m_listener(nullptr)
	, m_delayFilter(nullptr)
	, m_loaded(FALSE)
	, m_loadAttempted(FALSE)
{
}

//-------------------------------------------------------------------------------------------------
MilesAudioDevice::~MilesAudioDevice()
{
	if (s_listener)
		s_listener = nullptr;
}

//-------------------------------------------------------------------------------------------------
// openDevice :1436-1468 (the AIL part; buildProviderList :2856-2865)
AudioDevice::OpenResult MilesAudioDevice::open(const AudioSettings *settings, AudioDeviceListener *listener)
{
	s_listener = listener;
	m_loadAttempted = TRUE;

	// Load the Miles Sound System on runtime here instead of importing it into the executable.
	if (!MilesLoader::load())
	{
		DEBUG_LOG(("Failed to load mss32.dll (error %d). Audio will be turned off.", MilesLoader::getLastError()));
		return OPEN_NO_LIBRARY;
	}

	m_loaded = TRUE;

	AIL_set_redist_directory("MSS\\");
	AIL_startup();

	// AIL_quick_startup should be replaced later with a call to actually pick which device to use, etc
	Int retval = AIL_quick_startup(settings->m_useDigital, settings->m_useMidi, settings->m_outputRate, settings->m_outputBits, settings->m_outputChannels);

	// Quick handles tells us where to store the various devices. For now, we're only interested in the digital handle.
	HDIGDRIVER digitalHandle = nullptr;
	AIL_quick_handles(&digitalHandle, nullptr, nullptr);
	m_digitalHandle = digitalHandle;

	if (!retval) {
		return OPEN_NO_OUTPUT;
	}

	HPROENUM next = HPROENUM_FIRST;
	char *name;
	UnsignedInt index = 0;
	HPROVIDER id;
	while (index < MAX_PROVIDERS && AIL_enumerate_3D_providers(&next, &id, &name)) {
		m_providers[index].id = id;
		m_providers[index].name.set(name);	// set it to the AsciiString
		++index;
	}
	m_providerCount = index;
	return OPEN_OK;
}

//-------------------------------------------------------------------------------------------------
// closeDevice :1487-1501 (the AIL part; the manager releases voices and the provider first)
void MilesAudioDevice::close()
{
	if (m_loadAttempted)
	{
		if (m_loaded)
		{
			AIL_shutdown();
			m_loaded = FALSE;
		}
		MilesLoader::unload();
		m_loadAttempted = FALSE;
	}
	m_digitalHandle = nullptr;
}

//-------------------------------------------------------------------------------------------------
// init :438
void MilesAudioDevice::setStreamFileCallbacks()
{
	AIL_set_file_callbacks(streamingFileOpen, streamingFileClose, streamingFileSeek, streamingFileRead);
}

//-------------------------------------------------------------------------------------------------
AsciiString MilesAudioDevice::providerName(UnsignedInt index) const
{
	if (index < m_providerCount) {
		return m_providers[index].name;
	}
	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
// getProviderIndex :1697-1706
UnsignedInt MilesAudioDevice::providerIndex(const char *providerName) const
{
	for (UnsignedInt i = 0; i < m_providerCount; ++i) {
		if (m_providers[i].name == providerName) {
			return i;
		}
	}
	return PROVIDER_ERROR;
}

//-------------------------------------------------------------------------------------------------
// selectProvider :1727-1792 (the speaker probe, the provider choice and AIL_open_3D_provider)
UnsignedInt MilesAudioDevice::openProvider(UnsignedInt requested, UnsignedInt failIndex, UnsignedInt *speakerType)
{
	UnsignedInt providerNdx = requested;

#ifdef _WIN32 // O159: DirectSound's speaker configuration (the default speakers elsewhere)
	Bool useDolby = FALSE;
	LPDIRECTSOUND lpDirectSoundInfo;
	AIL_get_DirectSound_info( nullptr, (void**)&lpDirectSoundInfo, nullptr );
	if( lpDirectSoundInfo )
	{
		DWORD speakerConfig;
		lpDirectSoundInfo->GetSpeakerConfig( &speakerConfig );
		switch( DSSPEAKER_CONFIG( speakerConfig ) )
		{
			case DSSPEAKER_DIRECTOUT:
				*speakerType = AST_2Speakers;
				break;
			case DSSPEAKER_MONO:
				*speakerType = AST_2Speakers;
				break;
			case DSSPEAKER_STEREO:
				*speakerType = AST_2Speakers;
				break;
			case DSSPEAKER_HEADPHONE:
				*speakerType = AST_Headphones;
				useDolby = TRUE;
				break;
			case DSSPEAKER_QUAD:
				*speakerType = AST_4Speakers;
				useDolby = TRUE;
				break;
			case DSSPEAKER_SURROUND:
				*speakerType = AST_Surround;
				useDolby = TRUE;
				break;
			case DSSPEAKER_5POINT1:
				*speakerType = AST_5_1Surround;
				useDolby = TRUE;
				break;
			case DSSPEAKER_7POINT1:
				*speakerType = AST_7_1Surround;
				useDolby = TRUE;
				break;
		}
	}
#else
	Bool useDolby = FALSE;
#endif

	Bool success = FALSE;
	if( useDolby )
	{
		providerNdx = providerIndex( "Dolby Surround" );
	}
	else
	{
		providerNdx = providerIndex( "Miles Fast 2D Positional Audio" );
	}
	success = providerNdx < m_providerCount && AIL_open_3D_provider( m_providers[providerNdx].id ) == 0;

	if( !success )
	{
		// try to select a failsafe
		providerNdx = providerIndex( "Miles Fast 2D Positional Audio" );
		success = providerNdx < m_providerCount && AIL_open_3D_provider( m_providers[providerNdx].id ) == 0;
	}

	return success ? providerNdx : failIndex;
}

//-------------------------------------------------------------------------------------------------
// createListener :2877
Bool MilesAudioDevice::createListener(UnsignedInt provider)
{
	m_listener = AIL_open_3D_listener(m_providers[provider].id);
	// initial listener position will be (0, 0, 0)
	return m_listener != nullptr;
}

//-------------------------------------------------------------------------------------------------
// unselectProvider :1819-1822
void MilesAudioDevice::closeListenerAndProvider(UnsignedInt provider)
{
	AIL_close_3D_listener((H3DPOBJECT)m_listener);
	m_listener = nullptr;

	AIL_close_3D_provider(m_providers[provider].id);
}

//-------------------------------------------------------------------------------------------------
// setSpeakerType :1841
void MilesAudioDevice::setSpeakerType(UnsignedInt provider, UnsignedInt speakerType)
{
	AIL_set_3D_speaker_type( m_providers[provider].id, milesSpeakerType(speakerType) );
}

//-------------------------------------------------------------------------------------------------
// setDeviceListenerPosition :2649-2659
void MilesAudioDevice::setListener(const Coord3D &position, const Coord3D &orientation)
{
	if (m_listener) {
		AIL_set_3D_orientation((H3DPOBJECT)m_listener, orientation.x, orientation.y, orientation.z, 0, 0, -1);

		Real x = position.x;
		Real y = position.y;
		Real z = position.z;
		AIL_set_3D_position( (H3DPOBJECT)m_listener, x, y, z );
	}
}

//-------------------------------------------------------------------------------------------------
// initDelayFilter :2882-2899
void MilesAudioDevice::initDelayFilter()
{
	if (m_delayFilter != nullptr) {
		return;
	}

	char* filterName;
	HPROENUM enumFLTs = HPROENUM_FIRST;
	HPROVIDER currentProvider;
	while (AIL_enumerate_filters(&enumFLTs, &currentProvider, &filterName )) {
		if (strcmp(filterName,"Mono Delay Filter") == 0) {
			m_delayFilter = currentProvider;
			break;
		}
	}
}

//-------------------------------------------------------------------------------------------------
// initSamplePools :2917-2934 (one voice per call, in the manager's loop order)
AudioVoice MilesAudioDevice::allocVoice(AudioVoiceKind kind, UnsignedInt provider, Int debugIndex)
{
	if (kind == AVK_2D) {
		HSAMPLE sample = AIL_allocate_sample_handle((HDIGDRIVER)m_digitalHandle);
		if (sample) {
			AIL_init_sample(sample);
			AIL_set_sample_user_data(sample, 0, debugIndex);
		}
		return (AudioVoice)sample;
	}
	if (kind == AVK_3D) {
		H3DSAMPLE sample = AIL_allocate_3D_sample_handle(m_providers[provider].id);
		if (sample) {
			AIL_set_3D_user_data(sample, 0, debugIndex);
		}
		return (AudioVoice)sample;
	}
	return 0;
}

//-------------------------------------------------------------------------------------------------
// freeAllMilesHandles :1254, :1262
void MilesAudioDevice::freeVoice(AudioVoice voice, AudioVoiceKind kind)
{
	if (kind == AVK_2D) {
		AIL_release_sample_handle((HSAMPLE)voice);
	} else if (kind == AVK_3D) {
		AIL_release_3D_sample_handle((H3DSAMPLE)voice);
	}
}

//-------------------------------------------------------------------------------------------------
// audioDebugDisplay :236, :291
Int MilesAudioDevice::voiceDebugIndex(AudioVoice voice, AudioVoiceKind kind) const
{
	if (kind == AVK_2D) {
		return AIL_sample_user_data((HSAMPLE)voice, 0);
	}
	if (kind == AVK_3D) {
		return AIL_3D_user_data((H3DSAMPLE)voice, 0);
	}
	return 0;
}

//-------------------------------------------------------------------------------------------------
// playSample :2798-2804 with initFilters :1334-1357
void MilesAudioDevice::prepare2D(AudioVoice voice, Real volume, Real pitchShift, Real delaySeconds)
{
	HSAMPLE milesSample = (HSAMPLE)voice;

	AIL_init_sample(milesSample);

	// Prep any sort of filtering, etc, here
	AIL_register_EOS_callback(milesSample, sampleCompleted);

	// set the sample volume
	AIL_set_sample_volume_pan(milesSample, volume, 0.5f);

	// pitch shift (the manager reports a zero pitch shift itself)
	if (pitchShift != 0.0f) {
		AIL_set_sample_playback_rate(milesSample, REAL_TO_INT(AIL_sample_playback_rate(milesSample) * pitchShift));
	}

	// set up delay filter, if applicable
	if (delaySeconds > 0.0f) {
		Real value;
		value = delaySeconds;
		AIL_set_sample_processor(milesSample, DP_FILTER, (HPROVIDER)m_delayFilter);
		AIL_set_filter_sample_preference(milesSample, "Mono Delay Time", &value);

		value = 0.0;
		AIL_set_filter_sample_preference(milesSample, "Mono Delay", &value);
		AIL_set_filter_sample_preference(milesSample, "Mono Delay Mix", &value);
	}
}

//-------------------------------------------------------------------------------------------------
// playSample :2806-2814
Bool MilesAudioDevice::start2D(AudioVoice voice, const AudioSample &sample)
{
	HSAMPLE milesSample = (HSAMPLE)voice;

	if (sample.data) {
		AIL_set_sample_file(milesSample, sample.data, 0);

		// Start playback
		AIL_start_sample(milesSample);
		return TRUE;
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
// playSample3D :2820-2848 with initFilters3D :1361-1378
void MilesAudioDevice::play3D(AudioVoice voice, const AudioSample &sample, Real distanceArg1, Real distanceArg2,
	const Coord3D &position, Real volume, Real pitchShift, Bool useOcclusion, Real occlusion)
{
	H3DSAMPLE sample3D = (H3DSAMPLE)voice;

	AIL_set_3D_sample_file(sample3D, sample.data);

	// Prep any sort of filtering, etc, here
	AIL_register_3D_EOS_callback(sample3D, sample3DCompleted);

	// Set the position values of the sample here. The arguments arrive as the manager passes them
	// today (min range, max range), although mss.h declares (max_dist, min_dist).
	AIL_set_3D_sample_distances(sample3D, distanceArg1, distanceArg2);

	// Set the position of the sample here
	Real x = position.x;
	Real y = position.y;
	Real z = position.z;
	AIL_set_3D_position( sample3D, x, y, z );

	// set the sample volume
	AIL_set_3D_sample_volume(sample3D, volume);

	// pitch shift (the manager reports a zero pitch shift itself)
	if (pitchShift != 0.0f) {
		AIL_set_3D_sample_playback_rate(sample3D, REAL_TO_INT(AIL_3D_sample_playback_rate(sample3D) * pitchShift));
	}

	// Low pass filter
	if (useOcclusion) {
		AIL_set_3D_sample_occlusion(sample3D, occlusion);
	}

	// Start playback
	AIL_start_3D_sample(sample3D);
}

//-------------------------------------------------------------------------------------------------
// playAudioEvent :694
AudioVoice MilesAudioDevice::openStream(const char *fileName)
{
	return (AudioVoice)AIL_open_stream((HDIGDRIVER)m_digitalHandle, fileName, 0);
}

//-------------------------------------------------------------------------------------------------
// playStream :2783-2792
void MilesAudioDevice::startStream(AudioVoice stream, Int loopCount)
{
	HSTREAM milesStream = (HSTREAM)stream;
	if (loopCount >= 0) {
		AIL_set_stream_loop_count(milesStream, loopCount);
	}
	AIL_register_stream_callback(milesStream, streamCompleted);
	AIL_start_stream(milesStream);
}

//-------------------------------------------------------------------------------------------------
// hasMusicTrackCompleted :1428
Int MilesAudioDevice::streamLoopCount(AudioVoice stream) const
{
	return AIL_stream_loop_count((HSTREAM)stream);
}

//-------------------------------------------------------------------------------------------------
// releaseMilesHandles :1043-1070
void MilesAudioDevice::release(AudioVoice voice, AudioVoiceKind kind)
{
	switch (kind)
	{
		case AVK_2D:
			AIL_register_EOS_callback((HSAMPLE)voice, nullptr);
			AIL_stop_sample((HSAMPLE)voice);
			break;
		case AVK_3D:
			AIL_register_3D_EOS_callback((H3DSAMPLE)voice, nullptr);
			AIL_stop_3D_sample((H3DSAMPLE)voice);
			break;
		case AVK_Stream:
			AIL_register_stream_callback((HSTREAM)voice, nullptr);
			AIL_close_stream((HSTREAM)voice);
			break;
	}
}

//-------------------------------------------------------------------------------------------------
// pauseAudio :531, :542, :562
void MilesAudioDevice::pause(AudioVoice voice, AudioVoiceKind kind)
{
	switch (kind)
	{
		case AVK_2D: AIL_stop_sample((HSAMPLE)voice); break;
		case AVK_3D: AIL_stop_3D_sample((H3DSAMPLE)voice); break;
		case AVK_Stream: AIL_pause_stream((HSTREAM)voice, 1); break;
	}
}

//-------------------------------------------------------------------------------------------------
// resumeAudio :598, :609, :629
void MilesAudioDevice::resume(AudioVoice voice, AudioVoiceKind kind)
{
	switch (kind)
	{
		case AVK_2D: AIL_resume_sample((HSAMPLE)voice); break;
		case AVK_3D: AIL_resume_3D_sample((H3DSAMPLE)voice); break;
		case AVK_Stream: AIL_pause_stream((HSTREAM)voice, 0); break;
	}
}

//-------------------------------------------------------------------------------------------------
// playAudioEvent :711, adjustPlayingVolume :1302/:1310/:1313, adjustVolumeOfPlayingAudio :2196/:2225,
// processFadingList :2486/:2498
void MilesAudioDevice::setVolumePan(AudioVoice voice, AudioVoiceKind kind, Real volume, Real pan)
{
	if (kind == AVK_2D) {
		AIL_set_sample_volume_pan((HSAMPLE)voice, volume, pan);
	} else if (kind == AVK_Stream) {
		AIL_set_stream_volume_pan((HSTREAM)voice, volume, pan);
	}
}

//-------------------------------------------------------------------------------------------------
// adjustPlayingVolume :1301/:1308, adjustVolumeOfPlayingAudio :2195/:2224
void MilesAudioDevice::getPan(AudioVoice voice, AudioVoiceKind kind, Real *pan) const
{
	if (kind == AVK_2D) {
		AIL_sample_volume_pan((HSAMPLE)voice, nullptr, pan);
	} else if (kind == AVK_Stream) {
		AIL_stream_volume_pan((HSTREAM)voice, nullptr, pan);
	}
}

//-------------------------------------------------------------------------------------------------
// adjustPlayingVolume :1305, adjustVolumeOfPlayingAudio :2210, processFadingList :2492
void MilesAudioDevice::set3DVolume(AudioVoice voice, Real volume)
{
	AIL_set_3D_sample_volume((H3DSAMPLE)voice, volume);
}

//-------------------------------------------------------------------------------------------------
// processPlayingList :2385-2389
void MilesAudioDevice::set3DPosition(AudioVoice voice, const Coord3D &position)
{
	Real x = position.x;
	Real y = position.y;
	Real z = position.z;
	AIL_set_3D_position( (H3DSAMPLE)voice, x, y, z );
}

//-------------------------------------------------------------------------------------------------
// getFileLengthMS :2605-2622 (the empty-name check and the conversion to Real stay in the manager),
// dumpAudioLengths (566fb41bc)
Int MilesAudioDevice::fileLengthMS(const char *fileName) const
{
	// Load it as a stream to get the file info without actually opening the file.
	HSTREAM stream = AIL_open_stream((HDIGDRIVER)m_digitalHandle, fileName, 0);
	if (!stream) {
		return -1;
	}

	long retVal;
	AIL_stream_ms_position(stream, &retVal, nullptr);
	// Now close the stream
	AIL_close_stream(stream);

	return (Int)retVal;
}

//-------------------------------------------------------------------------------------------------
// playAudioEvent, the -audioLog START lines of d350063a9 (stream, 3D, 2D)
void MilesAudioDevice::debugVoiceValues(AudioVoice voice, AudioVoiceKind kind, Real *volume, Real *pan, Int *rate) const
{
	switch (kind)
	{
		case AVK_2D:
			AIL_sample_volume_pan((HSAMPLE)voice, volume, pan);
			*rate = (Int)AIL_sample_playback_rate((HSAMPLE)voice);
			break;
		case AVK_3D:
			*volume = (Real)AIL_3D_sample_volume((H3DSAMPLE)voice);
			*rate = (Int)AIL_3D_sample_playback_rate((H3DSAMPLE)voice);
			break;
		case AVK_Stream:
			AIL_stream_volume_pan((HSTREAM)voice, volume, pan);
			break;
	}
}

//-------------------------------------------------------------------------------------------------
// AudioFileCache::openFile :3173-3174
void MilesAudioDevice::readSampleInfo(const void *fileBytes, AudioSampleInfo *info) const
{
	AILSOUNDINFO soundInfo;
	AIL_WAV_info(fileBytes, &soundInfo);
	memcpy(info->deviceData, &soundInfo, sizeof(soundInfo));
	info->format = soundInfo.format;
	info->channels = soundInfo.channels;
	info->rate = soundInfo.rate;
	info->bits = soundInfo.bits;
}

//-------------------------------------------------------------------------------------------------
// AudioFileCache::openFile :3184-3207 (the AIL part; the cache keeps its own bookkeeping)
Bool MilesAudioDevice::decodeSample(AudioSampleInfo *info, void *fileBytes, UnsignedInt fileSize, AudioSample *out)
{
	AILSOUNDINFO soundInfo;
	memcpy(&soundInfo, info->deviceData, sizeof(soundInfo));

	if (soundInfo.format == WAVE_FORMAT_IMA_ADPCM) {
		void *decompressFileBuffer;
		U32 newFileSize;
		AIL_decompress_ADPCM(&soundInfo, &decompressFileBuffer, &newFileSize);
		out->data = decompressFileBuffer;
		out->bytes = newFileSize;
		out->ownedByDevice = TRUE;
		return TRUE;
	}
	if (soundInfo.format == WAVE_FORMAT_PCM) {
		out->data = fileBytes;
		out->bytes = fileSize;
		out->ownedByDevice = FALSE;
		return TRUE;
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
// AudioFileCache::releaseOpenAudioFile :3259-3260
void MilesAudioDevice::freeSample(void *deviceData)
{
	// Files read in via AIL_decompress_ADPCM must be freed with AIL_mem_free_lock.
	AIL_mem_free_lock(deviceData);
}

//-------------------------------------------------------------------------------------------------
// friend_forcePlayAudioEventRTS :3040-3045
AudioVoice MilesAudioDevice::playOneShot(const char *fileName, Real volume, Real pan)
{
	HAUDIO haud = AIL_quick_load_and_play(fileName, 1, 0);
	AIL_quick_set_volume(haud, volume, pan);
	return (AudioVoice)haud;
}

//-------------------------------------------------------------------------------------------------
// stopAllAudioImmediately :1237
void MilesAudioDevice::freeOneShot(AudioVoice voice)
{
	AIL_quick_unload((HAUDIO)voice);
}

//-------------------------------------------------------------------------------------------------
// getHandleForBink :2984-2986
void *MilesAudioDevice::movieAudioHandle(AudioVoice reserved2D)
{
	AILLPDIRECTSOUND lpDS;
	AIL_get_DirectSound_info((HSAMPLE)reserved2D, &lpDS, nullptr);
	return lpDS;
}

//-------------------------------------------------------------------------------------------------
// audioDebugDisplay :154
UnsignedInt MilesAudioDevice::debugTimerDelay() const
{
	return AIL_get_timer_highest_delay();
}
