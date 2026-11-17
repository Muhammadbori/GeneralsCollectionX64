/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: MilesAudioManager.cpp
/*---------------------------------------------------------------------------*/
/* EA Pacific                                                                */
/* Confidential Information	                                                 */
/* Copyright (C) 2001 - All Rights Reserved                                  */
/* DO NOT DISTRIBUTE                                                         */
/*---------------------------------------------------------------------------*/
/* Project:    RTS3                                                          */
/* File name:  MilesAudioManager.cpp                                         */
/* Created:    John K. McDonald, Jr., 3/21/2002                              */
/* Desc:       This is the implementation for the MilesAudioManager, which   */
/*						 interfaces with the Miles Sound System.                       */
/* Revision History:                                                         */
/*		7/18/2002 : Initial creation                                           */
/*---------------------------------------------------------------------------*/

#include "Lib/BaseType.h"
#include "GameClient/VoicePreview.h"
#include "MilesAudioDevice/MilesAudioManager.h"

#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/CommandLine.h"
#include "Common/GameEngine.h"
#include "Common/AsciiString.h"
#include "Common/AudioEventInfo.h"
#include "Common/FileSystem.h"
#include "Common/GameCommon.h"
#include "Common/GameSounds.h"
#include "Common/CRCDebug.h"
#include "Common/FrameRateLimit.h"
#include "Common/GlobalData.h"
#include "Common/ScopedMutex.h"

#include "GameClient/DebugDisplay.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"

#include "Common/file.h"

#include <Utility/interlocked_adapter.h>
#ifdef RTS_HAS_MINIAUDIO
#include "MiniaudioAudioDevice/MiniaudioAudioDevice.h"
#endif
#include <map>
#include <algorithm>   // O72

enum { INFINITE_LOOP_COUNT = 1000000 };

namespace
{
	// O72 (-audioLog): the gap at each restarted loop pass or portion, and the balance of the sounds
	enum { GAP_2D_LOOP, GAP_2D_PORTION, GAP_3D_LOOP, GAP_3D_PORTION, GAP_KINDS };
	const char *const kGapKindName[GAP_KINDS] = { "2D_loop", "2D_portion", "3D_loop", "3D_portion" };
	std::vector<Real> s_gaps[GAP_KINDS];
	UnsignedInt s_soundsStarted = 0;
	UnsignedInt s_soundsEnded = 0;     ///< ended by themselves
	UnsignedInt s_soundsStopped = 0;   ///< stopped by the game (culled, killed, level end ...)
	UnsignedInt s_nativeLoops = 0;
	MilesAudioManager *s_idleManager = nullptr;
}

//-------------------------------------------------------------------------------------------------
MilesAudioManager::MilesAudioManager() :
	m_providerCount(0),
	m_selectedProvider(PROVIDER_ERROR),
	m_selectedSpeakerType(0),
	m_lastProvider(PROVIDER_ERROR),
	m_digitalHandle(nullptr),
	m_num2DSamples(0),
	m_num3DSamples(0),
	m_numStreams(0),
	m_deviceOpened(false),
	m_milesLoaded(false),
	m_hasListener(false),
	m_device(&m_milesDevice),
	m_miniaudioDevice(nullptr),
	m_binkHandle(nullptr),
	m_pref3DProvider(AsciiString::TheEmptyString),
	m_prefSpeaker(AsciiString::TheEmptyString),
	m_previewVoice(0),
	m_previewBytes(nullptr)
{
	m_previewSample.data = nullptr;
	m_previewSample.bytes = 0;
	m_previewSample.ownedByDevice = FALSE;
	m_audioCache = NEW AudioFileCache(m_device);
	// O72: the main thread's frame wait wakes for the ends (FrameRateLimit::wait)
	m_voiceEndsEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	s_idleManager = this;
	FrameRateLimit::setIdleWork(&MilesAudioManager::idleVoiceEnds, m_voiceEndsEvent);
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature O47 The options' voice preview: a 2D voice of its own (not one of the pools, so nothing the
// game plays is cut), the sample decoded by the device as for any sample, released by stopPreviewSound.
Bool MilesAudioManager::playPreviewSound( const void *wavBytes, UnsignedInt size, Real volume )
{
	stopPreviewSound();
	if (!m_deviceOpened || !m_milesLoaded || wavBytes == nullptr || size < 44) {
		return FALSE;
	}
	unsigned char *copy = new unsigned char[size];
	memcpy(copy, wavBytes, size);
	AudioSampleInfo info;
	m_device->readSampleInfo(copy, &info);
	AudioSample decoded;
	if (!m_device->decodeSample(&info, copy, size, &decoded))
	{
		delete [] copy;
		return FALSE;
	}
	if (decoded.ownedByDevice)
	{
		delete [] copy;   // the device made its own image
		copy = nullptr;
	}
	const AudioVoice voice = m_device->allocVoice(AVK_2D, m_selectedProvider, 0);
	if (voice == 0)
	{
		if (decoded.ownedByDevice) {
			m_device->freeSample(decoded.data);
		}
		delete [] copy;
		return FALSE;
	}
	m_previewVoice = voice;
	m_previewSample = decoded;
	m_previewBytes = copy;
	m_device->prepare2D(voice, volume, 1.0f, 0.0f);
	m_device->resetVoiceMeter(voice);   // O55
	if (!m_device->start2D(voice, decoded))
	{
		stopPreviewSound();
		return FALSE;
	}
	return TRUE;
}

Bool MilesAudioManager::previewMeter( Real *peak, Real *rms, UnsignedInt64 *frames )
{
	return m_previewVoice != 0 && m_device->readVoiceMeter(m_previewVoice, peak, rms, frames);
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature O55 -voiceMeter (tests): the output meter of the USA dozer's select voice in a match, written to
// VoicePreview.log when the sound is released, to set beside the options' preview of the same line.
namespace
{
	AudioVoice s_meteredVoice = 0;
	AsciiString s_meteredInfo;
	Int s_meteredLengthMS = 0;

	Bool voiceMeterWanted()
	{
		static int s_state = -1;
		if (s_state < 0)
		{
			s_state = 0;
			for (int arg = 1; arg < __argc; ++arg) {
				if (stricmp(__argv[arg], "-voiceMeter") == 0) {
					s_state = 1;
				}
			}
		}
		return s_state == 1;
	}
}

void MilesAudioManager::stopPreviewSound()
{
	if (m_previewVoice != 0)
	{
		m_device->release(m_previewVoice, AVK_2D);
		m_device->freeVoice(m_previewVoice, AVK_2D);
		m_previewVoice = 0;
	}
	if (m_previewSample.data != nullptr && m_previewSample.ownedByDevice) {
		m_device->freeSample(m_previewSample.data);
	}
	m_previewSample.data = nullptr;
	m_previewSample.bytes = 0;
	m_previewSample.ownedByDevice = FALSE;
	delete [] m_previewBytes;
	m_previewBytes = nullptr;
}

//-------------------------------------------------------------------------------------------------
// -audioLog: a small number for a 2D or 3D sample handle, in the order the handles are first seen. The raw handle is a
// pointer and differs from run to run; the order of use of the pool does not.
static int audioLogVoiceId(AudioVoice voice)
{
	static std::map<AudioVoice, int> ids;
	std::map<AudioVoice, int>::iterator it = ids.find(voice);
	if (it == ids.end())
	{
		it = ids.insert(std::make_pair(voice, (int)ids.size())).first;
	}
	return it->second;
}

//-------------------------------------------------------------------------------------------------
MilesAudioManager::~MilesAudioManager()
{
	DEBUG_ASSERTCRASH(m_binkHandle == nullptr, ("Leaked a Bink handle. Chuybregts"));
	releaseHandleForBink();
	FrameRateLimit::setIdleWork(nullptr, nullptr);   // O72
	s_idleManager = nullptr;
	closeDevice();
	if (m_voiceEndsEvent != nullptr) {
		CloseHandle((HANDLE)m_voiceEndsEvent);
		m_voiceEndsEvent = nullptr;
	}
	delete m_audioCache;
	delete m_miniaudioDevice;
	m_miniaudioDevice = nullptr;

	DEBUG_ASSERTCRASH(this == TheAudio, ("Umm..."));
	TheAudio = nullptr;
}

//-------------------------------------------------------------------------------------------------
#if defined(RTS_DEBUG)
AudioHandle MilesAudioManager::addAudioEvent( const AudioEventRTS *eventToAdd )
{
	if (TheGlobalData->m_preloadReport) {
		if (!eventToAdd->getEventName().isEmpty()) {
			m_allEventsLoaded.insert(eventToAdd->getEventName());
		}
	}

	return AudioManager::addAudioEvent(eventToAdd);
}
#endif

#if defined(RTS_DEBUG)
//-------------------------------------------------------------------------------------------------
void MilesAudioManager::audioDebugDisplay(DebugDisplayInterface *dd, void *, FILE *fp )
{
	std::list<PlayingAudio *>::iterator it;

	static char buffer[128] = { 0 };
	if (buffer[0] == 0) {
		// AIL_MSS_version was an empty macro in the Miles header of this code: the version text stays empty.
	}

	Coord3D lookPos = TheTacticalView->getPosition();
	const Coord3D *mikePos = TheAudio->getListenerPosition();
	Coord3D distanceVector = TheTacticalView->get3DCameraPosition();
	distanceVector.sub( *mikePos );

	Int now = TheGameLogic->getFrame();
	static Int lastCheck = now;
	const Int frames = 60;
	static Int latency = 0;
	static Int worstLatency = 0;
	if( lastCheck + frames <= now )
	{
		latency = m_device->debugTimerDelay();
		if( latency > worstLatency )
		{
			worstLatency = latency;
		}
		lastCheck = now;
	}

	if( dd )
	{
		dd->printf("Miles Sound System version: %s    ", buffer);
		dd->printf("Memory Usage : %d/%d\n", m_audioCache->getCurrentlyUsedSize(), m_audioCache->getMaxSize());
		dd->printf("Sound: %s    ", (isOn(AudioAffect_Sound) ? "Yes" : "No"));
		dd->printf("3DSound: %s    ", (isOn(AudioAffect_Sound3D) ? "Yes" : "No"));
		dd->printf("Speech: %s    ", (isOn(AudioAffect_Speech) ? "Yes" : "No"));
		dd->printf("Music: %s\n", (isOn(AudioAffect_Music) ? "Yes" : "No"));
		dd->printf("Channels Available: ");
		dd->printf("%u Sounds    ", getNumAvailable2DSamples());
		dd->printf("%u 3D Sounds\n", getNumAvailable3DSamples());
		dd->printf("Volume: ");
		dd->printf("Sound: %d    ", REAL_TO_INT(m_soundVolume * 100.0f));
		dd->printf("3DSound: %d    ", REAL_TO_INT(m_sound3DVolume * 100.0f));
		dd->printf("Speech: %d    ", REAL_TO_INT(m_speechVolume * 100.0f));
		dd->printf("Music: %d\n", REAL_TO_INT(m_musicVolume * 100.0f));
		dd->printf("Current 3D Provider: %s    ",

		TheAudio->getProviderName(m_selectedProvider).str());
		dd->printf("Current Speaker Type: %s\n", TheAudio->translateUnsignedIntToSpeakerType(TheAudio->getSpeakerType()).str());

		dd->printf( "Looking at: (%d,%d,%d) -- Microphone at: (%d,%d,%d)\n",
				(Int)lookPos.x, (Int)lookPos.y, (Int)lookPos.z, (Int)mikePos->x, (Int)mikePos->y, (Int)mikePos->z );
		dd->printf( "Camera distance from microphone: %d -- Zoom Volume: %d%%\n",
				(Int)distanceVector.length(), (Int)(TheAudio->getZoomVolume()*100.0f) );
		dd->printf( "Worst latency: %d -- Current latency: %d\n", worstLatency, latency );

		dd->printf("-----------------------------------------------------------\n");
		dd->printf("Playing Audio\n");
	}
	if( fp )
	{
		fprintf( fp, "Miles Sound System version: %s    ", buffer );
		fprintf( fp, "Memory Usage : %d/%d\n", m_audioCache->getCurrentlyUsedSize(), m_audioCache->getMaxSize() );
		fprintf( fp, "Sound: %s    ", (isOn(AudioAffect_Sound) ? "Yes" : "No") );
		fprintf( fp, "3DSound: %s    ", (isOn(AudioAffect_Sound3D) ? "Yes" : "No") );
		fprintf( fp, "Speech: %s    ", (isOn(AudioAffect_Speech) ? "Yes" : "No") );
		fprintf( fp, "Music: %s\n", (isOn(AudioAffect_Music) ? "Yes" : "No") );
		fprintf( fp, "Channels Available: " );
		fprintf( fp, "%u Sounds    ", getNumAvailable2DSamples() );
		fprintf( fp, "%u 3D Sounds\n", getNumAvailable3DSamples() );
		fprintf( fp, "Volume: ");
		fprintf( fp, "Sound: %d    ", REAL_TO_INT(m_soundVolume * 100.0f) );
		fprintf( fp, "3DSound: %d    ", REAL_TO_INT(m_sound3DVolume * 100.0f) );
		fprintf( fp, "Speech: %d    ", REAL_TO_INT(m_speechVolume * 100.0f) );
		fprintf( fp, "Music: %d\n", REAL_TO_INT(m_musicVolume * 100.0f) );
		fprintf( fp, "Current 3D Provider: %s    ", TheAudio->getProviderName(m_selectedProvider).str());
		fprintf( fp, "Current Speaker Type: %s\n", TheAudio->translateUnsignedIntToSpeakerType(TheAudio->getSpeakerType()).str());

		fprintf( fp, "Looking at: (%d,%d,%d) -- Microphone at: (%d,%d,%d)\n",
				(Int)lookPos.x, (Int)lookPos.y, (Int)lookPos.z, (Int)mikePos->x, (Int)mikePos->y, (Int)mikePos->z );
		fprintf( fp, "Camera distance from microphone: %d -- Zoom Volume: %d%%\n",
				(Int)distanceVector.length(), (Int)(TheAudio->getZoomVolume()*100.0f) );

		fprintf( fp, "-----------------------------------------------------------\n" );
		fprintf( fp, "Playing Audio\n" );
	}

	PlayingAudio *playing = nullptr;
	Int channel;
	Int channelCount;
	Real volume = 0.0f;
	AsciiString filenameNoSlashes;

	const Int maxChannels = 64;
	PlayingAudio *playingArray[maxChannels] = { nullptr };

	// 2-D Sounds
	if( dd )
	{
		dd->printf("-----------------------------------------------------Sounds\n");
		channelCount = TheAudio->getNum2DSamples();
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			playing = *it;
			playingArray[(int)m_device->voiceDebugIndex(playing->m_voice, AVK_2D)] = playing;
		}

		for (Int i = 1; i <= maxChannels && i <= channelCount; ++i) {
			playing = playingArray[i];
			if (!playing) {
				dd->printf("%2d: Silence\n", i);
				continue;
			}

			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			filenameNoSlashes = event->getFilename();
			filenameNoSlashes = getFileName(filenameNoSlashes.str());

			// Calculate Sample volume
			volume = 100.0f;
			volume *= getEffectiveVolume(event);

			dd->printf("%2d: %-20s - (%s) Volume: %d (2D)\n", i, event->getEventName().str(), filenameNoSlashes.str(), REAL_TO_INT(volume));
			playingArray[i] = nullptr;
		}
	}
	if( fp )
	{
		fprintf( fp, "-----------------------------------------------------Sounds\n" );
		channelCount = TheAudio->getNum2DSamples();
		channel = 1;
		for( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it )
		{
			playing = *it;
			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			filenameNoSlashes = event->getFilename();
			filenameNoSlashes = getFileName(filenameNoSlashes.str());

			// Calculate Sample volume
			volume = 100.0f;
			volume *= getEffectiveVolume( event );

			fprintf( fp, "%2d: %-20s - (%s) Volume: %d (2D)\n", channel++, event->getEventName().str(), filenameNoSlashes.str(), REAL_TO_INT( volume ) );
		}
		for( int i = channel; i <= channelCount; ++i )
		{
			fprintf( fp, "%2d: Silence\n", i );
		}
	}

	const Coord3D *microphonePos = TheAudio->getListenerPosition();

	// Now 3D Sounds
	if( dd )
	{
		dd->printf("--------------------------------------------------3D Sounds\n");
		channelCount = TheAudio->getNum3DSamples();
		for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
			playing = *it;
			playingArray[(int)m_device->voiceDebugIndex(playing->m_voice, AVK_3D)] = playing;
		}

		for (Int i = 1; i <= maxChannels && i <= channelCount; ++i)
		{
			playing = playingArray[i];
			if (!playing)
			{
				dd->printf("%2d: Silence\n", i);
				continue;
			}

			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			filenameNoSlashes = event->getFilename();
			filenameNoSlashes = getFileName(filenameNoSlashes.str());

			// Calculate Sample volume
			volume = 100.0f;
			volume *= getEffectiveVolume(event);
			Real dist = -1.0f;
			const Coord3D *pos = event->getPosition();
			char distStr[32];
			if( pos )
			{
				Coord3D vector = *microphonePos;
				vector.sub( *pos );
				dist = vector.length();
				sprintf( distStr, "%d", REAL_TO_INT( dist ) );
			}
			else
			{
				sprintf( distStr, "???" );
			}
			char str[32];
			switch( event->getOwnerType() )
			{
				case OT_Positional:
					sprintf( str, "(3D)" );
					break;
				case OT_Object:
					sprintf( str, "(3DObj)" );
					break;
				case OT_Drawable:
					sprintf( str, "(3DDraw)" );
					break;
				case OT_Dead:
					sprintf( str, "(3DDead)" );
					break;

			}

			dd->printf("%2d: %-20s - (%s) Volume: %d, Dist: %s, %s\n",
				i, event->getEventName().str(), filenameNoSlashes.str(), REAL_TO_INT(volume), distStr, str );
			playingArray[i] = nullptr;
		}
	}
	if( fp )
	{
		fprintf( fp, "--------------------------------------------------3D Sounds\n" );
		channelCount = TheAudio->getNum3DSamples();
		channel = 1;
		for( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it )
		{
			playing = *it;
			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			filenameNoSlashes = event->getFilename();
			filenameNoSlashes = getFileName(filenameNoSlashes.str());

			// Calculate Sample volume
			volume = 100.0f;
			volume *= getEffectiveVolume( event );
			fprintf( fp, "%2d: %-24s - (%s) Volume: %d \n", channel++, event->getEventName().str(), filenameNoSlashes.str(), REAL_TO_INT( volume ) );
		}

		for( int i = channel; i <= channelCount; ++i )
		{
			fprintf( fp, "%2d: Silence\n", i );
		}
	}

	// Now Streams
	if( dd )
	{
		dd->printf("----------------------------------------------------Streams\n");
		channelCount = TheAudio->getNumStreams();
		channel = 1;
		for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
			playing = *it;
			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			filenameNoSlashes = event->getFilename();
			filenameNoSlashes = getFileName(filenameNoSlashes.str());

			// Calculate Sample volume
			volume = 100.0f;
			volume *= getEffectiveVolume(event);

			dd->printf("%2d: %-24s - (%s)  Volume: %d (Stream)\n", channel++, event->getEventName().str(), filenameNoSlashes.str(), REAL_TO_INT(volume));
		}

		for ( int i = channel; i <= channelCount; ++i) {
			dd->printf("%2d: Silence\n", i);
		}
		dd->printf("===========================================================\n");
	}
	if( fp )
	{
		fprintf( fp, "----------------------------------------------------Streams\n" );
		channelCount = TheAudio->getNumStreams();
		channel = 1;
		for( it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it )
		{
			playing = *it;
			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			filenameNoSlashes = event->getFilename();
			filenameNoSlashes = getFileName(filenameNoSlashes.str());

			// Calculate Sample volume
			volume = 100.0f;
			volume *= getEffectiveVolume(event);

			fprintf( fp, "%2d: %-24s - (%s)  Volume: %d (Stream)\n", channel++, event->getEventName().str(), filenameNoSlashes.str(), REAL_TO_INT( volume ) );
		}

		for( int i = channel; i <= channelCount; ++i )
		{
			fprintf( fp, "%2d: Silence\n", i );
		}
		fprintf( fp, "===========================================================\n" );
	}
}
#endif

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::init()
{
	AudioManager::init();
#ifdef INTENSE_DEBUG
	DEBUG_LOG(("Sound has temporarily been disabled in debug builds only. jkmcd"));
	// for now, RTS_DEBUG builds only should have no sound. ask jkmcd or srj about this.
	return;
#endif

	// We should now know how many samples we want to load
	openDevice();
	m_audioCache->setMaxSize(getAudioSettings()->m_maxCacheSize);

	// Now, set the file callbacks to load the streams from Biggie files
	m_device->setStreamFileCallbacks();
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::postProcessLoad()
{
	AudioManager::postProcessLoad();

	if (CommandLine::isAudioLenDumpEnabled())
	{
		dumpAudioLengths();
		TheGameEngine->setQuitting(TRUE);
	}
}

//-------------------------------------------------------------------------------------------------
// -audioLenDump: the length Miles reports (AIL_stream_ms_position, whole milliseconds, before the conversion to Real that
// getFileLengthMS does) for every audio file the file system sees. The path is written as the file system lists it.
void MilesAudioManager::dumpAudioLengths()
{
	AsciiString outPath = TheGlobalData->getPath_UserData();
	outPath.concat("AudioLengths.tsv");
	FILE *out = fopen(outPath.str(), "w");
	if (!out)
	{
		return;
	}
	fprintf(out, "path\tms\n");

	if (!m_milesLoaded || (m_device == &m_milesDevice && !m_digitalHandle))
	{
		fprintf(out, "# no audio device: Miles is not loaded (64-bit build, audio off, or mss32.dll missing)\n");
		fclose(out);
		return;
	}

	FilenameList files;
	TheFileSystem->getFileListInDirectory("Data\\Audio\\", "*.wav", files, TRUE);
	TheFileSystem->getFileListInDirectory("Data\\Audio\\", "*.mp3", files, TRUE);
	for (FilenameList::const_iterator it = files.begin(); it != files.end(); ++it)
	{
		const Int ms = m_device->fileLengthMS(it->str());
		fprintf(out, "%s\t%d\n", it->str(), ms);
	}
	fclose(out);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::reset()
{
#if defined(RTS_DEBUG)
	dumpAllAssetsUsed();
	m_allEventsLoaded.clear();
#endif

	AudioManager::reset();
	stopAllAudioImmediately();
	logAudioBalance("reset");   // O72
  removeAllAudioRequests();
  // This must come after stopAllAudioImmediately() and removeAllAudioRequests(), to ensure that
  // sounds pointing to the temporary AudioEventInfo handles are deleted before their info is deleted
  removeLevelSpecificAudioEventInfos();
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::update()
{
	AudioManager::update();
	setDeviceListenerPosition();

	// The device's clock: the null device reports the ends of its voices here, Miles does nothing. Before the
	// requests, so that a sound started in this update counts its length from this frame.
	const Bool logicIsRunning = TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isLoadingMap();
	m_device->update(TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0, logicIsRunning);
	processVoiceEnds();   // O71: the ends reported since the last update (and the null device's of this one)

	processRequestList();
	processPlayingList();
	processFadingList();
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::stopAudio( AudioAffect which )
{
	// All we really need to do is:
	// 1) Remove the EOS callback.
	// 2) Stop the sample, (so that when we later unload it, bad stuff doesn't happen)
	// 3) Set the status to stopped, so that when we next process the playing list, we will
	//		correctly clean up the sample.

	AUDIO_LOG("STOPCAT", "affect=0x%x", (unsigned)which);
	std::list<PlayingAudio *>::iterator it;

	PlayingAudio *playing = nullptr;
	if (BitIsSet(which, AudioAffect_Sound)) {
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			playing = *it;
			stopPlayingAudio(playing, "stopcat");
		}
	}

	if (BitIsSet(which, AudioAffect_Sound3D)) {
		for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
			playing = *it;
			stopPlayingAudio(playing, "stopcat");
		}
	}

	if (BitIsSet(which, AudioAffect_Speech | AudioAffect_Music)) {
		for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
			playing = *it;
			if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
				if (!BitIsSet(which, AudioAffect_Music)) {
					continue;
				}
			} else {
				if (!BitIsSet(which, AudioAffect_Speech)) {
					continue;
				}
			}
			stopPlayingAudio(playing, "stopcat");
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::pauseAudio( AudioAffect which )
{
	AUDIO_LOG("PAUSE", "affect=0x%x", (unsigned)which);
	// O71: an end reported before the pause starts its next portion or loop now, as it did at once on the device's
	// thread, so that the pause below pauses it instead of it starting later unpaused
	processVoiceEnds();
	std::list<PlayingAudio *>::iterator it;

	PlayingAudio *playing = nullptr;
	if (BitIsSet(which, AudioAffect_Sound)) {
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			playing = *it;
			if (!playing->isPlaying()) {
				continue;
			}
			DEBUG_ASSERTCRASH(playing->m_voice, ("Sample is not expected to be null"));
			m_device->pause(playing->m_voice, AVK_2D);
		}
	}

	if (BitIsSet(which, AudioAffect_Sound3D)) {
		for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
			playing = *it;
			if (!playing->isPlaying()) {
				continue;
			}
			DEBUG_ASSERTCRASH(playing->m_voice, ("3D Sample is not expected to be null"));
			m_device->pause(playing->m_voice, AVK_3D);
		}
	}

	if (BitIsSet(which, AudioAffect_Speech | AudioAffect_Music)) {
		for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
			playing = *it;
			if (!playing->isPlaying()) {
				continue;
			}
			if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
				if (!BitIsSet(which, AudioAffect_Music)) {
					continue;
				}
			} else {
				if (!BitIsSet(which, AudioAffect_Speech)) {
					continue;
				}
			}
			DEBUG_ASSERTCRASH(playing->m_voice, ("Stream is not expected to be null"));
			m_device->pause(playing->m_voice, AVK_Stream);
		}
	}

	//Get rid of PLAY audio requests when pausing audio.
	std::list<AudioRequest*>::iterator ait;
	for (ait = m_audioRequests.begin(); ait != m_audioRequests.end(); )
	{
		AudioRequest *req = (*ait);
		if( req->m_request == AR_Play )
		{
			deleteInstance(req);
			ait = m_audioRequests.erase(ait);
		}
		else
		{
			ait++;
		}
	}
}


//-------------------------------------------------------------------------------------------------
void MilesAudioManager::resumeAudio( AudioAffect which )
{
	AUDIO_LOG("RESUME", "affect=0x%x", (unsigned)which);
	std::list<PlayingAudio *>::iterator it;

	PlayingAudio *playing = nullptr;
	if (BitIsSet(which, AudioAffect_Sound)) {
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			playing = *it;
			if (!playing->isPlaying()) {
				continue;
			}
			DEBUG_ASSERTCRASH(playing->m_voice, ("Sample is not expected to be null"));
			m_device->resume(playing->m_voice, AVK_2D);
		}
	}

	if (BitIsSet(which, AudioAffect_Sound3D)) {
		for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
			playing = *it;
			if (!playing->isPlaying()) {
				continue;
			}
			DEBUG_ASSERTCRASH(playing->m_voice, ("3D Sample is not expected to be null"));
			m_device->resume(playing->m_voice, AVK_3D);
		}
	}

	if (BitIsSet(which, AudioAffect_Speech | AudioAffect_Music)) {
		for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
			playing = *it;
			if (!playing->isPlaying()) {
				continue;
			}
			if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
				if (!BitIsSet(which, AudioAffect_Music)) {
					continue;
				}
			} else {
				if (!BitIsSet(which, AudioAffect_Speech)) {
					continue;
				}
			}
			DEBUG_ASSERTCRASH(playing->m_voice, ("Stream is not expected to be null"));
			m_device->resume(playing->m_voice, AVK_Stream);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::pauseAmbient( Bool shouldPause )
{

}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::playAudioEvent( AudioRequest* req )
{
	DEBUG_ASSERTCRASH(req->m_pendingEvent != nullptr, ("audio request was expected to contain a valid audio event"));

	AudioEventRTS* event = req->m_pendingEvent.Peek();

#ifdef INTENSIVE_AUDIO_DEBUG
	DEBUG_LOG(("MILES (%d) - Processing play request: %d (%s)", TheGameLogic->getFrame(), event->getPlayingHandle(), event->getEventName().str()));
#endif
	const AudioEventInfo *info = event->getAudioEventInfo();
	if (!info) {
		return;
	}

	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing = nullptr;

	AudioHandle handleToKill = event->getHandleToKill();

	AsciiString fileToPlay = event->getFilename();
	PlayingAudio *newPlaying = allocatePlayingAudio();
	newPlaying->m_requestStop = req->m_requestStop;

	switch(info->m_soundType)
	{
		case AT_Music:
		case AT_Streaming:
		{
		#ifdef INTENSIVE_AUDIO_DEBUG
			DEBUG_LOG(("- Stream"));
		#endif

			if ((info->m_soundType == AT_Streaming) && event->getUninterruptible()) {
				stopAllSpeech();
			}

			Bool foundSoundToReplace = false;
			if (handleToKill) {
				for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
					playing = (*it);

					if (playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handleToKill)
					{
						//Release this streaming channel immediately because we are going to play another sound in it's place.
						stopPlayingAudio(playing, "replace");
						foundSoundToReplace = true;
						break;
					}
				}
			}

			AudioVoice stream;
			if (!handleToKill || foundSoundToReplace) {
				stream = m_device->openStream(fileToPlay.str());
			} else {
				stream = 0;
			}

			// Put this on here, so that the audio event RTS will be cleaned up regardless.
			newPlaying->m_audioEventRTS = req->m_pendingEvent;
			newPlaying->m_voice = stream;
			newPlaying->m_type = PAT_Stream;

			if (!stream) {
				AUDIO_LOG_EVENT("NOSTART", event, "pool=stream why=%s", (handleToKill && !foundSoundToReplace) ? "replace_target_gone" : "open_failed");
			}
			if (stream) {
				if ((info->m_soundType == AT_Streaming) && event->getUninterruptible()) {
					setDisallowSpeech(TRUE);
	 			}
				m_device->setVolumePan(stream, AVK_Stream, getEffectiveVolume(event), 0.5f);
				playStream(event, stream);
				if (AudioLog::g_on) {
				Real logVol = 0.0f, logPan = 0.0f;
				Int logRate = 0;
				m_device->debugVoiceValues(stream, AVK_Stream, &logVol, &logPan, &logRate);
				AUDIO_LOG_EVENT("START", event, "pool=stream slot=%d file=%s eff=%.3f dvol=%.3f pan=%.3f loops=%d",
					(int)m_playingStreams.size(), fileToPlay.str(), getEffectiveVolume(event), logVol, logPan, (int)m_device->streamLoopCount(stream));
			}

				++s_soundsStarted;   // O72
				CriticalSectionClass::LockClass lock(m_playingStreamsCS);
				m_playingStreams.push_back(newPlaying);
				newPlaying = nullptr;
			}
			break;
		}

		case AT_SoundEffect:
		{
		#ifdef INTENSIVE_AUDIO_DEBUG
			DEBUG_LOG(("- Sound"));
		#endif


			if (event->isPositionalAudio()) {
				// Sounds that are non-global are positional 3-D sounds. Deal with them accordingly
			#ifdef INTENSIVE_AUDIO_DEBUG
				DEBUG_LOG((" Positional"));
			#endif
				Bool foundSoundToReplace = false;
				if (handleToKill)
				{
					for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
						playing = (*it);

						if( playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handleToKill )
						{
							//Release this 3D sound channel immediately because we are going to play another sound in it's place.
							stopPlayingAudio(playing, "replace");
							foundSoundToReplace = true;
							break;
						}
					}
				}

				AudioVoice sample3D;
				if( !handleToKill || foundSoundToReplace )
				{
					sample3D = getAvailable3DSample( event );
					if( !sample3D )
					{
						//If we don't have an available sample, kill the lowest priority assuming we have one that is lower
						//than the sound we are trying to add. One possibility for strangeness is when an interrupt sound
						//that wants to kill a handle to replace it, it's possible that another request already killed it,
						//in which case we need to attempt to find another sound to kill.
						if( killLowestPrioritySoundImmediately( event ) )
						{
							sample3D = getAvailable3DSample( event );
						}
					}
				}
				else
				{
					sample3D = 0;
				}
				// Push it onto the list of playing things
				newPlaying->m_audioEventRTS = req->m_pendingEvent;
				newPlaying->m_voice = sample3D;
				newPlaying->m_file = nullptr;
				newPlaying->m_type = PAT_3DSample;

				if (sample3D) {
					newPlaying->m_file = playSample3D(event, sample3D);
				}

				if( !newPlaying->m_file )
				{
					AUDIO_LOG_EVENT("NOSTART", event, "pool=3D why=%s", !sample3D ? ((handleToKill && !foundSoundToReplace) ? "replace_target_gone" : "no_sample") : "no_file");
					#ifdef INTENSIVE_AUDIO_DEBUG
						DEBUG_LOG((" Killed (no handles available)"));
					#endif
				}
				else
				{
					if (AudioLog::g_on) {
						const Coord3D *logPos = getCurrentPositionFromEvent(event);
						const Bool logGlobal = (event->getAudioEventInfo()->m_type & ST_GLOBAL) != 0;
						const Real logMin = logGlobal ? TheAudio->getAudioSettings()->m_globalMinRange : event->getAudioEventInfo()->m_minDistance;
						const Real logMax = logGlobal ? TheAudio->getAudioSettings()->m_globalMaxRange : event->getAudioEventInfo()->m_maxDistance;
						const Real logOcclusion = (logPos && event->getAudioEventInfo()->m_lowPassFreq > 0 && !isOnScreen(logPos)) ? 1.0f - event->getAudioEventInfo()->m_lowPassFreq : 0.0f;
						Real logVol = 0.0f, logPan = 0.0f;
						Int logRate = 0;
						m_device->debugVoiceValues(sample3D, AVK_3D, &logVol, &logPan, &logRate);
						AUDIO_LOG_EVENT("START", event, "pool=3D voice=%d file=%s eff=%.3f dvol=%.3f rate=%d min=%.1f max=%.1f occl=%.3f delay=%.1f",
							audioLogVoiceId(sample3D), event->getFilename().str(), getEffectiveVolume(event), (float)logVol,
							(int)logRate, logMin, logMax, logOcclusion, event->getDelay());
					}
					armNativeLoop(newPlaying);   // O72
					++s_soundsStarted;
					CriticalSectionClass::LockClass lock(m_playing3DSoundsCS);
					m_playing3DSounds.push_back(newPlaying);
					newPlaying = nullptr;
					#ifdef INTENSIVE_AUDIO_DEBUG
						DEBUG_LOG((" Playing."));
					#endif
				}
			}
			else
			{
				// UI sounds are always 2-D. All other sounds should be Positional
				// Unit acknowledgment, etc, falls into the UI category of sound.
				Bool foundSoundToReplace = false;
				if (handleToKill) {
					for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
						playing = (*it);

						if (playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handleToKill)
						{
							//Release this 2D sound channel immediately because we are going to play another sound in it's place.
							stopPlayingAudio(playing, "replace");
							foundSoundToReplace = true;
							break;
						}
					}
				}

				AudioVoice sample;
				if( !handleToKill || foundSoundToReplace )
				{
					sample = getAvailable2DSample(event);
					if( !sample )
					{
						//If we don't have an available sample, kill the lowest priority assuming we have one that is lower
						//than the sound we are trying to add. One possibility for strangeness is when an interrupt sound
						//that wants to kill a handle to replace it, it's possible that another request already killed it,
						//in which case we need to attempt to find another sound to kill.
						if( killLowestPrioritySoundImmediately( event ) )
						{
							sample = getAvailable2DSample( event );
						}
					}
					}
				else
				{
					sample = 0;
				}

				// Push it onto the list of playing things
				newPlaying->m_audioEventRTS = req->m_pendingEvent;
				newPlaying->m_voice = sample;
				newPlaying->m_file = nullptr;
				newPlaying->m_type = PAT_Sample;

				if (sample) {
					newPlaying->m_file = playSample(event, sample);
				}

				if (!newPlaying->m_file) {
					AUDIO_LOG_EVENT("NOSTART", event, "pool=2D why=%s", !sample ? ((handleToKill && !foundSoundToReplace) ? "replace_target_gone" : "no_sample") : "no_file");
					#ifdef INTENSIVE_AUDIO_DEBUG
						DEBUG_LOG((" Killed (no handles available)"));
					#endif
				} else {
					if (AudioLog::g_on) {
						Real logVol = 0.0f, logPan = 0.0f;
						Int logRate = 0;
						m_device->debugVoiceValues(sample, AVK_2D, &logVol, &logPan, &logRate);
						AUDIO_LOG_EVENT("START", event, "pool=2D voice=%d file=%s eff=%.3f dvol=%.3f pan=%.3f rate=%d delay=%.1f",
							audioLogVoiceId(sample), event->getFilename().str(), getEffectiveVolume(event), logVol, logPan,
							(int)logRate, event->getDelay());
					}
					armNativeLoop(newPlaying);   // O72
					++s_soundsStarted;
					CriticalSectionClass::LockClass lock(m_playingSoundsCS);
					m_playingSounds.push_back(newPlaying);
					newPlaying = nullptr;
					#ifdef INTENSIVE_AUDIO_DEBUG
						DEBUG_LOG((" Playing."));
					#endif
				}
			}
			break;
		}
	}

	// If we were able to successfully play audio, then we set it to null above. (And it will be freed
	// later. However, if audio is non-null at this point, then it must be freed.
	if (newPlaying) {
		stopPlayingAudio(newPlaying, nullptr);
		releasePlayingAudio(newPlaying);
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::stopAudioEvent( AudioHandle handle )
{
	AUDIO_LOG("STOPREQ", "h=%u", (UnsignedInt)handle);
#ifdef INTENSIVE_AUDIO_DEBUG
	DEBUG_LOG(("MILES (%d) - Processing stop request: %d", TheGameLogic->getFrame(), handle));
#endif

	std::list<PlayingAudio *>::iterator it;
	if ( handle == AHSV_StopTheMusic || handle == AHSV_StopTheMusicFade ) {
		// for music, find and stop the currently playing music stream(s).
		for ( it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it ) {
			PlayingAudio *playing = (*it);

			if( playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music )
			{
				if( handle == AHSV_StopTheMusicFade )
				{
					fadePlayingAudio(playing);
				}
				else
				{
					stopPlayingAudio(playing, "stopmusic");
				}
			}
		}
		return;
	}

	// Look for it in the request list.
	{
		std::list<AudioRequest*>::iterator it;
		for( it = m_audioRequests.begin(); it != m_audioRequests.end(); it++ )
		{
			AudioRequest *req = (*it);
			if( req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == handle )
			{
				if (req->m_pendingEvent->getAudioEventInfo()->m_soundType == AT_SoundEffect)
				{
					req->m_requestStop = true;
				}
				else
				{
					deleteInstance(req);
					m_audioRequests.erase(it);
				}
				return;
			}
		}
	}

	for ( it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it ) {
		PlayingAudio *playing = (*it);

		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			stopPlayingAudio(playing, "stopreq");
			return;
		}
	}

	for ( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it ) {
		PlayingAudio *playing = (*it);

		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			playing->m_requestStop = true;
			endNativeLoop(playing);   // O72
			return;
		}
	}

	for ( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it ) {
		PlayingAudio *playing = (*it);

		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
		#ifdef INTENSIVE_AUDIO_DEBUG
			DEBUG_LOG((" (%s)", playing->m_audioEventRTS->getEventName()));
		#endif
			playing->m_requestStop = true;
			endNativeLoop(playing);   // O72
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::killAudioEventImmediately( AudioHandle audioEvent )
{
	AUDIO_LOG("KILLCALL", "h=%u", (UnsignedInt)audioEvent);
	//First look for it in the request list.
	std::list<AudioRequest*>::iterator ait;
	for( ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ait++ )
	{
		AudioRequest *req = (*ait);
		if( req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == audioEvent )
		{
			deleteInstance(req);
			m_audioRequests.erase(ait);
			return;
		}
	}

	//Look for matching 3D sound to kill
	std::list<PlayingAudio *>::iterator it;
	for( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); it++ )
	{
		PlayingAudio *playing = (*it);

		if( playing->m_audioEventRTS->getPlayingHandle() == audioEvent )
		{
			stopPlayingAudio(playing, "kill");
			return;
		}
	}

	//Look for matching 2D sound to kill
	for( it = m_playingSounds.begin(); it != m_playingSounds.end(); it++ )
	{
		PlayingAudio *playing = (*it);

		if( playing->m_audioEventRTS->getPlayingHandle() == audioEvent )
		{
			stopPlayingAudio(playing, "kill");
			return;
		}
	}

	//Look for matching steaming sound to kill
	for( it = m_playingStreams.begin(); it != m_playingStreams.end(); it++ )
	{
		PlayingAudio *playing = (*it);

		if( playing->m_audioEventRTS->getPlayingHandle() == audioEvent )
		{
			stopPlayingAudio(playing, "kill");
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::pauseAudioEvent( AudioHandle handle )
{
	// pause audio
}

//-------------------------------------------------------------------------------------------------
void *MilesAudioManager::loadFileForRead( AudioEventRTS *eventToLoadFrom )
{
	return m_audioCache->openFile(eventToLoadFrom);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::closeFile( void *fileRead )
{
	m_audioCache->closeFile(fileRead);
}


//-------------------------------------------------------------------------------------------------
PlayingAudio *MilesAudioManager::allocatePlayingAudio()
{
	return NEW PlayingAudio; // poolify
}


//-------------------------------------------------------------------------------------------------
void MilesAudioManager::releaseMilesHandles( PlayingAudio *playing )
{
	playing->m_nativeLoop = FALSE;   // O72: the device's release stops the sound, looping or not
	playing->m_occluded = -1;
	switch (playing->m_type)
	{
		case PAT_Sample:
		{
			// O70: under the list's lock, as the end thread handles a sample's end (notifyOfAudioCompletion)
			CriticalSectionClass::LockClass lock(m_playingSoundsCS);
			if (playing->m_voice && playing->m_voice == s_meteredVoice)
			{
				// O55 (-voiceMeter)
				Real peak = 0.0f, rms = 0.0f;
				UnsignedInt64 frames = 0;
				if (m_device->readVoiceMeter(playing->m_voice, &peak, &rms, &frames)) {
					VoicePreview::logMeter(s_meteredInfo.str(), peak, rms, frames, 0, getSystemSoundVolume(), isAudioMuted());
				}
				s_meteredVoice = 0;
			}
			if (playing->m_voice) {
				m_device->release(playing->m_voice, AVK_2D);
				m_availableSamples.push_back(playing->m_voice);
				playing->m_voice = 0;
			}
			break;
		}
		case PAT_3DSample:
		{
			CriticalSectionClass::LockClass lock(m_playing3DSoundsCS);   // O70
			if (playing->m_voice) {
				m_device->release(playing->m_voice, AVK_3D);
				m_available3DSamples.push_back(playing->m_voice);
				playing->m_voice = 0;
			}
			break;
		}
		case PAT_Stream:
		{
			if (playing->m_voice) {
				m_device->release(playing->m_voice, AVK_Stream);
				playing->m_voice = 0;
			}
			break;
		}
	}
	playing->m_type = PAT_INVALID;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::releasePlayingAudio( PlayingAudio *playing )
{
	DEBUG_ASSERTCRASH(playing->m_status == PS_Stopped, ("PlayingAudio must be stopped by now"));
	DEBUG_ASSERTCRASH(playing->m_type == PAT_INVALID, ("PlayingAudio must be invalidated by now"));

	if (playing->m_file) {
		closeFile(playing->m_file);
		playing->m_file = nullptr;
	}

	delete playing;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::stopPlayingAudio( PlayingAudio *playing, const char *why )
{
	// The sample or stream reported its end (or a loop gap wait) through the Miles callback: eos, not a decision of the manager.
	const Bool endedByItself = (playing->m_status == PS_Stopping);

	// Advance from Playing to Stopping
	InterlockedCompareExchange(reinterpret_cast<volatile long*>(&playing->m_status), PS_Stopping, PS_Playing);
	// Advance from Stopping to Stopped
	const long prevStatus = InterlockedCompareExchange(reinterpret_cast<volatile long*>(&playing->m_status), PS_Stopped, PS_Stopping);
	if (prevStatus != PS_Stopping) {
		return;
	}
	if (endedByItself) {   // O72
		++s_soundsEnded;
	} else {
		++s_soundsStopped;
	}
	if (why && playing->m_audioEventRTS != nullptr) {
		AUDIO_LOG_EVENT("END", playing->m_audioEventRTS.Peek(), "pool=%s why=%s",
			playing->m_type == PAT_3DSample ? "3D" : (playing->m_type == PAT_Sample ? "2D" : "stream"), endedByItself ? "eos" : why);
	}
	releaseMilesHandles(playing);
	playing->m_rerequestOnNextUpdate = false;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::rerequestPlayingAudio( PlayingAudio *playing )
{
	AudioRequest *req = allocateAudioRequest();
	req->m_pendingEvent = playing->m_audioEventRTS;
	req->m_requiresCheckForSample = true;
	req->m_requestStop = playing->m_requestStop;
	appendAudioRequest(req);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::rerequestPlayingAudioWhenSignalled( PlayingAudio *playing )
{
	if (playing->m_rerequestOnNextUpdate) {
		rerequestPlayingAudio(playing);
		playing->m_rerequestOnNextUpdate = false;
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::fadePlayingAudio( PlayingAudio *playing )
{
	if (playing->m_audioEventRTS != nullptr) {
		AUDIO_LOG_EVENT("FADE", playing->m_audioEventRTS.Peek(), "");
	}
	playing->m_fade = true;
}

//-------------------------------------------------------------------------------------------------
PlayingAudio *MilesAudioManager::findActiveMusic( const AsciiString* trackName )
{
	std::list<PlayingAudio *>::const_iterator it;
	PlayingAudio *playing;
	for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
		playing = *it;
		if (!playing->isPlaying()) {
			continue;
		}
		if (playing->m_fade) {
			continue;
		}
		if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType != AT_Music) {
			continue;
		}
		if (trackName && *trackName != playing->m_audioEventRTS->getEventName()) {
			continue;
		}
		return playing;
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
const PlayingAudio *MilesAudioManager::findActiveMusic( const AsciiString* trackName ) const
{
	return const_cast<MilesAudioManager*>(this)->findActiveMusic(trackName);
}


//-------------------------------------------------------------------------------------------------
void MilesAudioManager::releasePlayingAudioInListIfStopped(std::list<PlayingAudio *> &list, CriticalSectionClass &cs)
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;
	CriticalSectionClass::LockClass lock(cs);

	for (it = list.begin(); it != list.end(); )
	{
		playing = (*it);

		if (playing->m_status == PS_Stopped)
		{
			releasePlayingAudio(playing);
			it = list.erase(it);
			continue;
		}

		++it;
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::stopAllAudioImmediately()
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;

	//
	// Stop audio without locking. Calls into Miles.
	//
	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		playing = (*it);
		stopPlayingAudio(playing, "stopall");
	}

	for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
		playing = (*it);
		stopPlayingAudio(playing, "stopall");
	}

	for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
		playing = (*it);
		stopPlayingAudio(playing, "stopall");
	}

	for (it = m_fadingAudio.begin(); it != m_fadingAudio.end(); ++it) {
		playing = (*it);
		stopPlayingAudio(playing, "stopall");
	}

	//
	// Release audio with locking. Must not call into Miles!
	//
	releasePlayingAudioInListIfStopped(m_playingSounds, m_playingSoundsCS);
	releasePlayingAudioInListIfStopped(m_playing3DSounds, m_playing3DSoundsCS);
	releasePlayingAudioInListIfStopped(m_playingStreams, m_playingStreamsCS);
	releasePlayingAudioInListIfStopped(m_fadingAudio, m_fadingAudioCS);

	DEBUG_ASSERTCRASH(m_playingSounds.empty(), ("List is expected to be empty now"));
	DEBUG_ASSERTCRASH(m_playing3DSounds.empty(), ("List is expected to be empty now"));
	DEBUG_ASSERTCRASH(m_playingStreams.empty(), ("List is expected to be empty now"));
	DEBUG_ASSERTCRASH(m_fadingAudio.empty(), ("List is expected to be empty now"));

	std::list<AudioVoice>::iterator hit;
	for (hit = m_audioForcePlayed.begin(); hit != m_audioForcePlayed.end(); ++hit) {
		if (*hit) {
			m_device->freeOneShot(*hit);
		}
	}

	m_audioForcePlayed.clear();
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::freeAllMilesHandles()
{
	stopPreviewSound();   // O47: the options' voice preview has a voice of its own

	// First, we need to ensure that we don't have any sample handles open. To that end, we must stop
	// all of our currently playing audio.
	stopAllAudioImmediately();

	// Walks through the available 2-D and 3-D handles and releases them
	std::vector<AudioVoice>::iterator it;
	for ( it = m_availableSamples.begin(); it != m_availableSamples.end(); ++it ) {
		m_device->freeVoice(*it, AVK_2D);
	}

	m_availableSamples.clear();
	m_num2DSamples = 0;

	std::vector<AudioVoice>::iterator it3D;
	for ( it3D = m_available3DSamples.begin(); it3D != m_available3DSamples.end(); ++it3D ) {
		m_device->freeVoice(*it3D, AVK_3D);
	}

	m_available3DSamples.clear();
	m_num3DSamples = 0;
	m_numStreams = 0;
}

//-------------------------------------------------------------------------------------------------
AudioVoice MilesAudioManager::getAvailable2DSample( AudioEventRTS *event )
{
	if (!m_availableSamples.empty()) {
		AudioVoice retSample = m_availableSamples.back();
		m_availableSamples.pop_back();
		return retSample;
	}

	// Find the first sample of lower priority than my augmented priority that is interruptible and take its handle
	return 0;
}

//-------------------------------------------------------------------------------------------------
AudioVoice MilesAudioManager::getAvailable3DSample( AudioEventRTS *event )
{
	if (!m_available3DSamples.empty()) {
		AudioVoice retSample = m_available3DSamples.back();
		m_available3DSamples.pop_back();
		return retSample;
	}

	// Find the first sample of lower priority than my augmented priority that is interruptible and take its handle
	return 0;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::adjustPlayingVolume( PlayingAudio *playing )
{
	Real pan;
	if (playing->m_type == PAT_Sample) {
		m_device->getPan(playing->m_voice, AVK_2D, &pan);
		m_device->setVolumePan(playing->m_voice, AVK_2D, getEffectiveVolume(playing->m_audioEventRTS.Peek()), pan);

	} else if (playing->m_type == PAT_3DSample) {
		m_device->set3DVolume(playing->m_voice, getEffectiveVolume(playing->m_audioEventRTS.Peek()));

	} else if (playing->m_type == PAT_Stream) {
		m_device->getPan(playing->m_voice, AVK_Stream, &pan);
		if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music ) {
			m_device->setVolumePan(playing->m_voice, AVK_Stream, getEffectiveVolume(playing->m_audioEventRTS.Peek()), pan);

		} else {
			m_device->setVolumePan(playing->m_voice, AVK_Stream, getEffectiveVolume(playing->m_audioEventRTS.Peek()), pan);

		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::stopAllSpeech()
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;
	for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
		playing = (*it);
		if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Streaming) {
			stopPlayingAudio(playing, "stopspeech");
		}
	}

}

//-------------------------------------------------------------------------------------------------
AsciiString MilesAudioManager::nextMusicTrack()
{
	AsciiString trackName;

	if (PlayingAudio *playing = findActiveMusic()) {
		trackName = playing->m_audioEventRTS->getEventName();
	}

	// Stop currently playing music
	TheAudio->removeAudioEvent(AHSV_StopTheMusic);

	trackName = nextTrackName(trackName);
	AudioEventRTS newTrack(trackName);
	TheAudio->addAudioEvent(&newTrack);

	return trackName;
}

//-------------------------------------------------------------------------------------------------
AsciiString MilesAudioManager::prevMusicTrack()
{
	AsciiString trackName;

	if (PlayingAudio *playing = findActiveMusic()) {
		trackName = playing->m_audioEventRTS->getEventName();
	}

	// Stop currently playing music
	TheAudio->removeAudioEvent(AHSV_StopTheMusic);

	trackName = prevTrackName(trackName);
	AudioEventRTS newTrack(trackName);
	TheAudio->addAudioEvent(&newTrack);

	return trackName;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isMusicPlaying() const
{
	return findActiveMusic() != nullptr;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::hasMusicTrackCompleted( const AsciiString& trackName, Int numberOfTimes ) const
{
	if (const PlayingAudio *playing = findActiveMusic(&trackName)) {
		if (INFINITE_LOOP_COUNT - m_device->streamLoopCount(playing->m_voice) >= numberOfTimes) {
			AUDIO_LOG("MUSICDONE", "track=%s n=%d result=1 completed=%d", trackName.str(), numberOfTimes, (int)(INFINITE_LOOP_COUNT - m_device->streamLoopCount(playing->m_voice)));
			return TRUE;
		}
		AUDIO_LOG("MUSICDONE", "track=%s n=%d result=0 completed=%d", trackName.str(), numberOfTimes, (int)(INFINITE_LOOP_COUNT - m_device->streamLoopCount(playing->m_voice)));
		return FALSE;
	}
	AUDIO_LOG("MUSICDONE", "track=%s n=%d result=0 not_playing", trackName.str(), numberOfTimes);
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::openDevice()
{
	if (!TheGlobalData->m_audioOn) {
		return;
	}

	m_deviceOpened = true;

	// The device loads its library, starts it and opens the output.
	const AudioSettings *audioSettings = getAudioSettings();
	m_device = chooseAudioDevice();
	m_audioCache->setDevice(m_device);
	AudioDevice::OpenResult openResult = m_device->open(audioSettings, this);
	if (openResult == AudioDevice::OPEN_NO_OUTPUT && m_device != &m_milesDevice && m_device != &m_nullDevice)
	{
		// TheSuperHackers @feature No output device (none present, or it cannot be opened): the null device, so the game
		// keeps the lengths, ends and loop counts it reads, as with no Miles library below. Miles keeps its old
		// behaviour (audio off) for the 32-bit game.
		AUDIO_LOG("DEVICECHOICE", "device=%s result=no_output fallback=%s", m_device->name(), m_nullDevice.name());
		m_device->close();
		m_device = &m_nullDevice;
		m_audioCache->setDevice(m_device);
		openResult = m_device->open(audioSettings, this);
	}
	else if (openResult == AudioDevice::OPEN_OK && m_device != &m_milesDevice && m_device != &m_nullDevice)
	{
		// The name now says which backend is in use (the DEVICE line's provider says it too).
		AUDIO_LOG("DEVICECHOICE", "device=%s result=ok", m_device->name());
	}
	if (openResult == AudioDevice::OPEN_NO_LIBRARY)
	{
		// TheSuperHackers @feature No Miles library (the 64-bit build, or mss32.dll missing): the null device plays
		// nothing, but gives the file lengths, the ends of the sounds and the music loop counts that the scripts and
		// this manager read with Miles, so the game takes the same script branches as with Miles.
		m_device->close();
		m_device = &m_nullDevice;
		m_audioCache->setDevice(m_device);
		openResult = m_device->open(audioSettings, this);
	}
	if (openResult == AudioDevice::OPEN_NO_LIBRARY)
	{
		setOn(false, AudioAffect_All);
		return;
	}

	m_milesLoaded = true;

	m_selectedSpeakerType = TheAudio->translateSpeakerTypeToUnsignedInt(m_prefSpeaker);

	// For now, we're only interested in the digital handle.
	m_digitalHandle = m_device->nativeDevice();

	if (openResult == AudioDevice::OPEN_OK) {
		buildProviderList();
	} else {
		// if we couldn't initialize any devices, turn sound off (fail silently)
		setOn( false, AudioAffect_All );
	}

	UnsignedInt preferredProvider = TheAudio->getProviderIndex(m_pref3DProvider);
	if (preferredProvider == PROVIDER_ERROR && m_device != &m_milesDevice) {
		preferredProvider = 0;	// the null and miniaudio devices have one provider, which no preference names
	}
	selectProvider(preferredProvider);

	// Now that we're all done, update the cached variables so that everything is in sync.
	TheAudio->refreshCachedVariables();

	AUDIO_LOG("DEVICE", "provider=%s speaker=%u(%s) digital_on=%d outputrate=%d bits=%d channels=%d",
		isValidProvider() ? m_provider3D[m_selectedProvider].name.str() : "none", (unsigned)m_selectedSpeakerType,
		TheAudio->translateUnsignedIntToSpeakerType(m_selectedSpeakerType).str(), (int)isOn(AudioAffect_Sound3D),
		(int)audioSettings->m_outputRate, (int)audioSettings->m_outputBits, (int)audioSettings->m_outputChannels);

	if (!isValidProvider()) {
		return;
	}

	initDelayFilter();
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature The sound library. By default Miles on the 32-bit game (unchanged), and miniaudio on the
// 64-bit game when the build has it (RTS_AUDIO_BACKEND=miniaudio); headless runs never open an output there and take
// the null device. -audio miles|miniaudio|miniaudio-null|null overrides this for a test run; a library the build does
// not have is reported (debug log and -audioLog) and the default is kept. Miles that cannot be loaded falls back to the
// null device in openDevice, as before. miniaudio-null is the miniaudio device on miniaudio's null backend: decoding,
// mixing, its threads and all manager paths run in real time, and nothing reaches an output device, so unattended runs
// stay silent whatever the volumes; a build without miniaudio takes the null device for it, not its default.
AudioDevice *MilesAudioManager::chooseAudioDevice()
{
#if defined(RTS_HAS_MINIAUDIO) && defined(_WIN64)
	const char *defaultName = TheGlobalData->m_headless ? "null" : "miniaudio";
#else
	const char *defaultName = "miles";
#endif
	const char *requested = CommandLine::getAudioDevice();
	const char *chosen = defaultName;
	const char *why = "default";
	if (requested != nullptr)
	{
		if (stricmp(requested, "miles") == 0 || stricmp(requested, "null") == 0) {
			chosen = stricmp(requested, "miles") == 0 ? "miles" : "null";
			why = "requested";
		} else if (stricmp(requested, "miniaudio") == 0 || stricmp(requested, "miniaudio-null") == 0) {
#ifdef RTS_HAS_MINIAUDIO
			chosen = stricmp(requested, "miniaudio") == 0 ? "miniaudio" : "miniaudio-null";
			why = "requested";
#else
			why = "requested_not_in_build";
			if (stricmp(requested, "miniaudio-null") == 0) {
				chosen = "null";	// asked for silence: never fall back to a device with output
			}
			DEBUG_LOG(("-audio %s: this build has no miniaudio device (RTS_AUDIO_BACKEND=Miles); using %s", requested, chosen));
#endif
		} else {
			why = "requested_unknown";
			DEBUG_LOG(("-audio %s: unknown (miles, miniaudio, miniaudio-null or null); using %s", requested, defaultName));
		}
	}

	AudioDevice *device = &m_milesDevice;
	if (strcmp(chosen, "null") == 0) {
		device = &m_nullDevice;
	}
#ifdef RTS_HAS_MINIAUDIO
	if (strcmp(chosen, "miniaudio") == 0 || strcmp(chosen, "miniaudio-null") == 0) {
		if (m_miniaudioDevice == nullptr) {
			MiniaudioAudioDevice::TestOptions options = { FALSE, FALSE, FALSE, FALSE, 0.0f, -1.0f };
			options.nullBackend = strcmp(chosen, "miniaudio-null") == 0;
			// TheSuperHackers @feature -audioTestDeviceLoss A[,B]: the output stops as if by itself A seconds after the
			// open, and the real device comes back B seconds later (never without B). Works on the null backend too.
			CommandLine::getAudioTestDeviceLoss(&options.testLossAfterSeconds, &options.testBackAfterSeconds);
			m_miniaudioDevice = NEW MiniaudioAudioDevice(options);
		}
		device = m_miniaudioDevice;
	}
#endif
	// Only when something other than the 32-bit default happens, so the 32-bit -audioLog stays as it was. The device is
	// not open yet; for miniaudio, openDevice logs the backend after the open.
	if (requested != nullptr || device != &m_milesDevice) {
		AUDIO_LOG("DEVICECHOICE", "requested=%s chosen=%s why=%s", requested ? requested : "-",
			strcmp(chosen, "miniaudio-null") == 0 ? "miniaudio (null backend requested)" : device->name(), why);
	}
	return device;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::closeDevice()
{
	if (m_deviceOpened)
	{
		if (m_milesLoaded)
		{
			freeAllMilesHandles();
			unselectProvider();
			m_milesLoaded = false;
		}
		logAudioBalance("exit");   // O72
		m_device->close();
		m_deviceOpened = false;
		CriticalSectionClass::LockClass lock(m_voiceEndsCS);   // O71: the voices are gone; the end thread too
		m_voiceEnds.clear();
	}
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isCurrentlyPlaying( AudioHandle handle )
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;

	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		playing = *it;
		if (!playing->isPlayingOrRequested()) {
			continue;
		}
		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			return true;
		}
	}

	for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
		playing = *it;
		if (!playing->isPlayingOrRequested()) {
			continue;
		}
		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			return true;
		}
	}

	for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
		playing = *it;
		if (!playing->isPlayingOrRequested()) {
			continue;
		}
		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			return true;
		}
	}

	// if something is requested, it is also considered playing
	std::list<AudioRequest *>::iterator ait;
	AudioRequest *req = nullptr;
	for (ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ++ait) {
		req = *ait;
		if (req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == handle) {
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::notifyOfAudioCompletion( uintptr_t handle, UnsignedInt flags )
{
	// O71: now called on the main thread only (processVoiceEnds). The O70 list locks stay: they cost nothing there.
	// TheSuperHackers @bugfix O70 The end of a sample comes on the device's end thread while the main thread may stop
	// the same sound: stopPlayingAudio -> releaseMilesHandles cleared playing->m_voice (and gave the voice back to the
	// pool) between this thread's find and its restart of the next portion or loop, which then prepared voice 0
	// (crash in prepare2D, S41 hs07). The whole end is handled with the list's lock held, and the main thread releases a
	// sample's voice under the same lock (releaseMilesHandles), so the two never interleave; a sound that is no longer
	// playing (stopping or stopped by the main thread) is not restarted. Lock order as everywhere: the list first, the
	// device second; the device calls this without its own lock. Streams keep their path (their completion only
	// restarts music, and their lists are also locked in another order by the fading code).
	if (flags == PAT_Sample)
	{
		CriticalSectionClass::LockClass lock(m_playingSoundsCS);
		notifyOfAudioCompletionLocked(handle, flags, 0);
	}
	else if (flags == PAT_3DSample)
	{
		CriticalSectionClass::LockClass lock(m_playing3DSoundsCS);
		notifyOfAudioCompletionLocked(handle, flags, 0);
	}
	else
	{
		notifyOfAudioCompletionLocked(handle, flags, 0);
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::onVoiceDone( uintptr_t voice, UnsignedInt kind, UnsignedInt generation )
{
	// TheSuperHackers @bugfix O71 Only queued here (the device's end thread, or the main thread for the null device).
	// Handling the end on the device's thread raced with the main thread on everything the end touches: the
	// PlayingAudio and its voice (O70 closed that for 2D and 3D samples, not for streams and the fading list), the
	// event's portion and loop state, the file cache, and the object or drawable whose position a 3D sound follows
	// (getCurrentPositionFromEvent). processVoiceEnds handles the ends on the main thread at the next update, so none
	// of it is shared any more, and no list lock is held across a file read on another thread.
	{
		CriticalSectionClass::LockClass lock(m_voiceEndsCS);
		VoiceEnd e = { voice, kind, generation };
		m_voiceEnds.push_back(e);
	}
	// TheSuperHackers @bugfix O72 Not only at the next update: the main thread handles the end while it sleeps out its
	// frame (FrameRateLimit::wait, woken by this event), so a restart waits for the rest of the frame's work at most
	if (m_voiceEndsEvent != nullptr) {
		SetEvent((HANDLE)m_voiceEndsEvent);
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::idleVoiceEnds()
{
	if (s_idleManager != nullptr && s_idleManager->m_deviceOpened) {
		s_idleManager->processVoiceEnds();
	}
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::loopsWithoutDecision(AudioEventRTS *event) const
{
	// O72: a pass of this loop is like the one before it: endless, one sound (no pick among several, no "all"), no delay
	// between passes, and the sound portion now (an attack has played; the decay comes only after a stop, and the native
	// loop is ended then: endNativeLoop)
	const AudioEventInfo *info = event != nullptr ? event->getAudioEventInfo() : nullptr;
	return info != nullptr && info->m_soundType == AT_SoundEffect && BitIsSet(info->m_control, AC_LOOP)
		&& !BitIsSet(info->m_control, AC_ALL) && info->m_loopCount == 0 && info->m_sounds.size() == 1
		&& info->m_delayMax <= 0 && event->getNextPlayPortion() == PP_Sound;
}

void MilesAudioManager::armNativeLoop(PlayingAudio *playing)
{
	playing->m_nativeLoop = FALSE;
	playing->m_occluded = -1;
	if (playing->m_voice == 0 || playing->m_requestStop || !loopsWithoutDecision(playing->m_audioEventRTS.Peek())) {
		return;
	}
	if (playing->m_type != PAT_Sample && playing->m_type != PAT_3DSample) {
		return;
	}
	if (m_device->setVoiceLooping(playing->m_voice, (AudioVoiceKind)playing->m_type, TRUE)) {
		playing->m_nativeLoop = TRUE;
		++s_nativeLoops;
		AUDIO_LOG_EVENT("NATIVELOOP", playing->m_audioEventRTS.Peek(), "voice=%d", audioLogVoiceId(playing->m_voice));
	}
}

void MilesAudioManager::endNativeLoop(PlayingAudio *playing)
{
	if (playing->m_nativeLoop && playing->m_voice != 0) {
		m_device->setVoiceLooping(playing->m_voice, (AudioVoiceKind)playing->m_type, FALSE);
		AUDIO_LOG_EVENT("NATIVELOOPEND", playing->m_audioEventRTS.Peek(), "voice=%d", audioLogVoiceId(playing->m_voice));
	}
	playing->m_nativeLoop = FALSE;
}

void MilesAudioManager::noteRestartGap(PlayingAudio *playing, Bool loop)
{
	if (!AudioLog::g_on || playing->m_voice == 0 || playing->m_file == nullptr) {
		return;
	}
	const Bool is3D = playing->m_type == PAT_3DSample;
	const Real gap = m_device->restartGapMs(playing->m_voice, (AudioVoiceKind)playing->m_type);
	if (gap < 0.0f) {
		return;
	}
	const Int kind = is3D ? (loop ? GAP_3D_LOOP : GAP_3D_PORTION) : (loop ? GAP_2D_LOOP : GAP_2D_PORTION);
	s_gaps[kind].push_back(gap);
	AUDIO_LOG_EVENT("GAP", playing->m_audioEventRTS.Peek(), "kind=%s gap_ms=%.2f voice=%d", kGapKindName[kind], gap,
		audioLogVoiceId(playing->m_voice));
}

void MilesAudioManager::logAudioBalance(const char *when)
{
	if (!AudioLog::g_on) {
		return;
	}
	Int listed = 0;
	const std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds, &m_playingStreams, &m_fadingAudio };
	for (Int l = 0; l < 4; ++l) {
		for (std::list<PlayingAudio *>::const_iterator it = lists[l]->begin(); it != lists[l]->end(); ++it) {
			if ((*it)->m_status == PS_Playing) {
				++listed;
			}
		}
	}
	AUDIO_LOG("BALANCE", "when=%s started=%u ended=%u stopped=%u still_playing=%d device_voices_playing=%d native_loops=%u",
		when, s_soundsStarted, s_soundsEnded, s_soundsStopped, listed, (int)m_device->playingVoiceCount(), s_nativeLoops);
	for (Int k = 0; k < GAP_KINDS; ++k)
	{
		std::vector<Real> v = s_gaps[k];
		if (v.empty()) {
			AUDIO_LOG("GAPSUM", "when=%s kind=%s count=0", when, kGapKindName[k]);
			continue;
		}
		std::sort(v.begin(), v.end());
		AUDIO_LOG("GAPSUM", "when=%s kind=%s count=%u median_ms=%.2f p90_ms=%.2f max_ms=%.2f", when, kGapKindName[k],
			(unsigned)v.size(), v[v.size() / 2], v[(v.size() * 9) / 10], v.back());
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::processVoiceEnds()
{
	std::vector<VoiceEnd> ends;
	{
		CriticalSectionClass::LockClass lock(m_voiceEndsCS);
		if (m_voiceEnds.empty()) {
			return;
		}
		ends.swap(m_voiceEnds);
	}
	for (size_t i = 0; i < ends.size(); ++i)
	{
		const VoiceEnd &e = ends[i];
		if (e.voice != 0 && e.voice == m_previewVoice) {
			continue;   // O47: the voice preview ended; stopPreviewSound frees it
		}
		if (e.kind == PAT_Sample) {
			CriticalSectionClass::LockClass lock(m_playingSoundsCS);
			notifyOfAudioCompletionLocked(e.voice, e.kind, e.generation);
		} else if (e.kind == PAT_3DSample) {
			CriticalSectionClass::LockClass lock(m_playing3DSoundsCS);
			notifyOfAudioCompletionLocked(e.voice, e.kind, e.generation);
		} else {
			notifyOfAudioCompletionLocked(e.voice, e.kind, e.generation);
		}
	}
}

void MilesAudioManager::notifyOfAudioCompletionLocked( uintptr_t handle, UnsignedInt flags, UnsignedInt generation )
{
	PlayingAudio *playing = findPlayingAudioFrom(handle, flags);
	if (!playing) {
		// O71: stopped and released by the main thread between the end and this update: nothing to do
		AUDIO_LOG("ENDGONE", "voice=%p kind=%u", (void *)handle, (unsigned)flags);
		return;
	}
	// O71: the voice was prepared or started again since this end (the ABA case: the old sound was stopped, its
	// voice released and taken by a new sound before this update): the end is the old sound's, the new one plays on.
	// The voice is live here: a PlayingAudio holds it.
	if (generation != 0 && m_device->voiceGeneration(handle, (AudioVoiceKind)flags) != generation) {
		AUDIO_LOG("ENDSTALE", "voice=%p kind=%u", (void *)handle, (unsigned)flags);
		return;
	}
	// O70: stopped or being stopped by the main thread meanwhile (its voice may already be gone): nothing to restart
	if ((flags == PAT_Sample || flags == PAT_3DSample) && (playing->m_status != PS_Playing || playing->m_voice != handle)) {
		return;
	}

	if (getDisallowSpeech() && playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Streaming) {
		setDisallowSpeech(FALSE);
	}

	if (playing->m_audioEventRTS->getAudioEventInfo()->m_control & AC_LOOP) {
		if (playing->m_audioEventRTS->getNextPlayPortion() == PP_Attack) {
			playing->m_audioEventRTS->setNextPlayPortion(PP_Sound);
		}
		if (playing->m_audioEventRTS->getNextPlayPortion() == PP_Sound) {
			// First, decrease the loop count.
			playing->m_audioEventRTS->decreaseLoopCount();

			// Now, try to start the next loop
			if (startNextLoop(playing)) {
				return;
			}
		}
	}

	playing->m_audioEventRTS->advanceNextPlayPortion();

	switch (playing->m_type)
	{
	case PAT_Sample:
	{
		if (playing->m_audioEventRTS->getNextPlayPortion() != PP_Done) {
			closeFile(playing->m_file);	// close it so as not to leak it.
			playing->m_file = playSample(playing->m_audioEventRTS.Peek(), playing->m_voice);

			// If we don't have a file now, then we should drop to the stopped status so that
			// We correctly close this handle.
			if (playing->m_file) {
				noteRestartGap(playing, FALSE);   // O72
				armNativeLoop(playing);
				return;
			}
		}
		break;
	}
	case PAT_3DSample:
	{
		if (playing->m_audioEventRTS->getNextPlayPortion() != PP_Done) {
			closeFile(playing->m_file);	// close it so as not to leak it.
			playing->m_file = playSample3D(playing->m_audioEventRTS.Peek(), playing->m_voice);

			// If we don't have a file now, then we should drop to the stopped status so that
			// We correctly close this handle.
			if (playing->m_file) {
				noteRestartGap(playing, FALSE);   // O72
				armNativeLoop(playing);
				return;
			}
		}
		break;
	}
	case PAT_Stream:
	{
		if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
			playStream(playing->m_audioEventRTS.Peek(), playing->m_voice);
			return;
		}
		break;
	}
	}

	// it will be cleaned up on the next frame update
	InterlockedCompareExchange(reinterpret_cast<volatile long*>(&playing->m_status), PS_Stopping, PS_Playing);
}

//-------------------------------------------------------------------------------------------------
PlayingAudio *MilesAudioManager::findPlayingAudioFrom( uintptr_t handle, UnsignedInt flags )
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;

	if (flags == PAT_Sample) {
		AudioVoice sample = handle;
		CriticalSectionClass::LockClass lock(m_playingSoundsCS);
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			playing = *it;
			if (playing->m_voice == sample) {
				return playing;
			}
		}
	}

	if (flags == PAT_3DSample) {
		AudioVoice sample3D = handle;
		CriticalSectionClass::LockClass lock(m_playing3DSoundsCS);
		for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
			playing = *it;
			if (playing->m_voice == sample3D) {
				return playing;
			}
		}
	}

	if (flags == PAT_Stream) {
		AudioVoice stream = handle;
		{
			CriticalSectionClass::LockClass lock(m_playingStreamsCS);
			for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
				playing = *it;
				if (playing->m_voice == stream) {
					return playing;
				}
			}
		}
		{
			CriticalSectionClass::LockClass lock(m_fadingAudioCS);
			for (it = m_fadingAudio.begin(); it != m_fadingAudio.end(); ++it) {
				playing = *it;
				if (playing->m_voice == stream) {
					return playing;
				}
			}
		}
	}

	return nullptr;
}


//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getProviderCount() const
{
	return m_providerCount;
}

//-------------------------------------------------------------------------------------------------
AsciiString MilesAudioManager::getProviderName( UnsignedInt providerNum ) const
{
	if (isOn(AudioAffect_Sound3D) && providerNum < m_providerCount) {
		return m_provider3D[providerNum].name;
	}

	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getProviderIndex( AsciiString providerName ) const
{
	for (UnsignedInt i = 0; i < m_providerCount; ++i) {
		if (providerName == m_provider3D[i].name) {
			return i;
		}
	}

	return PROVIDER_ERROR;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::selectProvider( UnsignedInt providerNdx )
{
	if (!isOn(AudioAffect_Sound3D))
	{
		return;
	}

	if (providerNdx == m_selectedProvider)
	{
		return;
	}

	if (isValidProvider())
	{
		freeAllMilesHandles();
		unselectProvider();
	}

	// The device probes the speaker configuration (updating m_selectedSpeakerType), chooses its provider and
	// falls back to its failsafe one, as this function did.
	providerNdx = m_device->openProvider(providerNdx, PROVIDER_ERROR, &m_selectedSpeakerType);
	Bool success = providerNdx != PROVIDER_ERROR;

	if( !success )
	{
		m_selectedProvider = PROVIDER_ERROR;
	}

	if ( success )
	{
		m_selectedProvider = providerNdx;

		initSamplePools();

		createListener();
		setSpeakerType(m_selectedSpeakerType);
		if (TheVideoPlayer)
		{
			TheVideoPlayer->notifyVideoPlayerOfNewProvider(TRUE);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::unselectProvider()
{
	if (!(isOn(AudioAffect_Sound3D) && isValidProvider())) {
		return;
	}

	if (TheVideoPlayer) {
		TheVideoPlayer->notifyVideoPlayerOfNewProvider(FALSE);
	}
	m_device->closeListenerAndProvider(m_selectedProvider);
	m_hasListener = false;
	m_lastProvider = m_selectedProvider;

	m_selectedProvider = PROVIDER_ERROR;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getSelectedProvider() const
{
	return m_selectedProvider;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::setSpeakerType( UnsignedInt speakerType )
{
	if (!isValidProvider()) {
		return;
	}

	m_device->setSpeakerType( m_selectedProvider, speakerType );
	m_selectedSpeakerType = speakerType;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getSpeakerType()
{
	if (!isValidProvider()) {
		return 0;
	}

	return m_selectedSpeakerType;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getNum2DSamples() const
{
	return m_num2DSamples;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getNum3DSamples() const
{
	return m_num3DSamples;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getNumStreams() const
{
	return m_numStreams;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getNumAvailable2DSamples() const
{
	return (UnsignedInt)m_availableSamples.size();
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::getNumAvailable3DSamples() const
{
	return (UnsignedInt)m_available3DSamples.size();
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::doesViolateLimit( AudioEventRTS *event ) const
{
	Int limit = event->getAudioEventInfo()->m_limit;
	if (limit == 0) {
		return false;
	}

	Int totalCount = 0;
	Int totalRequestCount = 0;

	std::list<PlayingAudio *>::const_iterator it;
	if (!event->isPositionalAudio()) {
		// 2-D
		for ( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it ) {
			if (!(*it)->isPlayingOrRequested()) {
				continue;
			}
			if ((*it)->m_audioEventRTS->getEventName() == event->getEventName()) {
				if (totalCount == 0) {
					// This is the oldest audio of this type playing.
					event->setHandleToKill((*it)->m_audioEventRTS->getPlayingHandle());
				}
				++totalCount;
			}
		}
	} else {
		// 3-D
		for ( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it ) {
			if (!(*it)->isPlayingOrRequested()) {
				continue;
			}
			if ((*it)->m_audioEventRTS->getEventName() == event->getEventName()) {
				if (totalCount == 0) {
					// This is the oldest audio of this type playing.
					event->setHandleToKill((*it)->m_audioEventRTS->getPlayingHandle());
				}
				++totalCount;
			}
		}
	}

	// Also check the request list in case we've requested to play this sound.
	std::list<AudioRequest*>::const_iterator arIt;
	for (arIt = m_audioRequests.begin(); arIt != m_audioRequests.end(); ++arIt) {
		AudioRequest *req = (*arIt);
		if( req->m_pendingEvent && req->m_pendingEvent->getEventName() == event->getEventName() )
		{
			totalRequestCount++;
			totalCount++;
		}
	}

	//If our event is an interrupting type, then normally we would always add it. The exception is when we have requested
	//multiple sounds in the same frame and those requests violate the limit. Because we don't have any "old" sounds to
	//remove in the case of an interrupt, we need to catch it early and prevent the sound from being added if we already
	//reached the limit
	if( event->getAudioEventInfo()->m_control & AC_INTERRUPT )
	{
		if( totalRequestCount < limit )
		{
			Int totalPlayingCount = totalCount - totalRequestCount;
			if( totalRequestCount + totalPlayingCount < limit )
			{
				//We aren't exceeding the actual limit, then clear the kill handle.
				event->setHandleToKill(0);
				return false;
			}

			//We are exceeding the limit - the kill handle will kill the
			//oldest playing sound to enforce the actual limit.
			return false;
		}
	}

	if( totalCount < limit )
	{
		event->setHandleToKill(0);
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isPlayingAlready( AudioEventRTS *event ) const
{
	std::list<PlayingAudio *>::const_iterator it;
	if (!event->isPositionalAudio()) {
		// 2-D
		for ( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it ) {
			if (!(*it)->isPlaying()) {
				continue;
			}
			if ((*it)->m_audioEventRTS->getEventName() == event->getEventName()) {
				return true;
			}
		}
	} else {
		// 3-D
		for ( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it ) {
			if (!(*it)->isPlaying()) {
				continue;
			}
			if ((*it)->m_audioEventRTS->getEventName() == event->getEventName()) {
				return true;
			}
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isObjectPlayingVoice( UnsignedInt objID ) const
{
	if (objID == 0) {
		return false;
	}

	std::list<PlayingAudio *>::const_iterator it;
		// 2-D
	for ( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it ) {
		if (!(*it)->isPlaying()) {
			continue;
		}
		if ((*it)->m_audioEventRTS->getObjectID() == objID && (*it)->m_audioEventRTS->getAudioEventInfo()->m_type & ST_VOICE) {
			return true;
		}
	}

	// 3-D
	for ( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it ) {
		if (!(*it)->isPlaying()) {
			continue;
		}
		if ((*it)->m_audioEventRTS->getObjectID() == objID && (*it)->m_audioEventRTS->getAudioEventInfo()->m_type & ST_VOICE) {
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
AudioEventRTS* MilesAudioManager::findLowestPrioritySound( AudioEventRTS *event )
{
	const AudioPriority priority = event->getAudioEventInfo()->m_priority;
	if( priority == AP_LOWEST )
	{
		//If the event we pass in is the lowest priority, don't bother checking because
		//there is nothing lower priority than lowest.
		return nullptr;
	}

	AudioEventRTS *lowestPriorityEvent = nullptr;
	AudioPriority lowestPriority = priority;

	std::list<PlayingAudio *>::const_iterator it;
	if( event->isPositionalAudio() )
	{
		//3D
		for( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it )
		{
			if (!(*it)->isPlaying()) {
				continue;
			}
			AudioEventRTS *itEvent = (*it)->m_audioEventRTS.Peek();
			const AudioPriority itPriority = itEvent->getAudioEventInfo()->m_priority;
			if( itPriority < lowestPriority )
			{
				lowestPriorityEvent = itEvent;
				lowestPriority = itPriority;
				if( lowestPriority == AP_LOWEST )
				{
					return lowestPriorityEvent;
				}
			}
		}
	}
	else
	{
		//2D
		for( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it )
		{
			if (!(*it)->isPlaying()) {
				continue;
			}
			AudioEventRTS *itEvent = (*it)->m_audioEventRTS.Peek();
			const AudioPriority itPriority = itEvent->getAudioEventInfo()->m_priority;
			if( itPriority < lowestPriority )
			{
				lowestPriorityEvent = itEvent;
				lowestPriority = itPriority;
				if( lowestPriority == AP_LOWEST )
				{
					return lowestPriorityEvent;
				}
			}
		}
	}
	return lowestPriorityEvent;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isPlayingLowerPriority( AudioEventRTS *event ) const
{
	//We don't actually want to do anything to this CONST function. Remember, we're
	//just checking to see if there is a lower priority sound.
	AudioPriority priority = event->getAudioEventInfo()->m_priority;
	if( priority == AP_LOWEST )
	{
		//If the event we pass in is the lowest priority, don't bother checking because
		//there is nothing lower priority than lowest.
		return false;
	}
	std::list<PlayingAudio *>::const_iterator it;
	if (!event->isPositionalAudio()) {
		// 2-D
		for ( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it ) {
			if (!(*it)->isPlaying()) {
				continue;
			}
			if ((*it)->m_audioEventRTS->getAudioEventInfo()->m_priority < priority) {
				//event->setHandleToKill((*it)->m_audioEventRTS->getPlayingHandle());
				return true;
			}
		}
	} else {
		// 3-D
		for ( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it ) {
			if (!(*it)->isPlaying()) {
				continue;
			}
			if ((*it)->m_audioEventRTS->getAudioEventInfo()->m_priority < priority) {
				//event->setHandleToKill((*it)->m_audioEventRTS->getPlayingHandle());
				return true;
			}
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::killLowestPrioritySoundImmediately( AudioEventRTS *event )
{
	//Actually, we want to kill the LOWEST PRIORITY SOUND, not the first "lower" priority
	//sound we find, because it could easily be
	AudioEventRTS *lowestPriorityEvent = findLowestPrioritySound( event );
	if( lowestPriorityEvent )
	{
		AUDIO_LOG_EVENT("KILLLOW", event, "victim=%u", (UnsignedInt)lowestPriorityEvent->getPlayingHandle());
		std::list<PlayingAudio *>::iterator it;
		if( event->isPositionalAudio() )
		{
			for( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it )
			{
				PlayingAudio *playing = (*it);
				if (!playing->isPlaying())
				{
					continue;
				}
				if( playing->m_audioEventRTS.Peek() == lowestPriorityEvent )
				{
					// Stop this 3D sound channel immediately because we are going to play another sound in it's place.
					stopPlayingAudio(playing, "killlow");
					return TRUE;
				}
			}
		}
		else
		{
			for( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it )
			{
				PlayingAudio *playing = (*it);
				if (!playing->isPlaying())
				{
					continue;
				}
				if( playing->m_audioEventRTS.Peek() == lowestPriorityEvent )
				{
					// Stop this sound channel immediately because we are going to play another sound in it's place.
					stopPlayingAudio(playing, "killlow");
					return TRUE;
				}
			}
		}
	}
	return FALSE;
}


//-------------------------------------------------------------------------------------------------
void MilesAudioManager::adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume)
{
	Real pan;
	std::list<PlayingAudio *>::iterator it;

	PlayingAudio *playing = nullptr;
	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		playing = *it;
		if (!playing->isPlaying()) {
			continue;
		}
		AudioEventRTS *itEvent = playing->m_audioEventRTS.Peek();

		if (itEvent->getEventName() == eventName) {
			DEBUG_ASSERTCRASH(playing->m_voice, ("Sample is not expected to be null"));
			itEvent->setVolume(newVolume);
			m_device->getPan(playing->m_voice, AVK_2D, &pan);
			m_device->setVolumePan(playing->m_voice, AVK_2D, getEffectiveVolume(itEvent), pan);
		}
	}

	for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
		playing = *it;
		if (!playing->isPlaying()) {
			continue;
		}
		AudioEventRTS *itEvent = playing->m_audioEventRTS.Peek();

		if (itEvent->getEventName() == eventName) {
			DEBUG_ASSERTCRASH(playing->m_voice, ("3D Sample is not expected to be null"));
			itEvent->setVolume(newVolume);
			m_device->set3DVolume(playing->m_voice, getEffectiveVolume(itEvent));
		}
	}

	for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
		playing = *it;
		if (!playing->isPlaying()) {
			continue;
		}
		AudioEventRTS *itEvent = playing->m_audioEventRTS.Peek();

		if (itEvent->getEventName() == eventName) {
			DEBUG_ASSERTCRASH(playing->m_voice, ("Stream is not expected to be null"));
			itEvent->setVolume(newVolume);
			m_device->getPan(playing->m_voice, AVK_Stream, &pan);
			m_device->setVolumePan(playing->m_voice, AVK_Stream, getEffectiveVolume(itEvent), pan);
		}
	}
}


//-------------------------------------------------------------------------------------------------
void MilesAudioManager::removePlayingAudio( AsciiString eventName )
{
	std::list<PlayingAudio *>::iterator it;

	PlayingAudio *playing = nullptr;
	for( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it )
	{
		playing = *it;
		if( playing->m_audioEventRTS->getEventName() == eventName )
		{
			stopPlayingAudio(playing, "removeevent");
		}
	}

	for( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it )
	{
		playing = *it;
		if( playing->m_audioEventRTS->getEventName() == eventName )
		{
			stopPlayingAudio(playing, "removeevent");
		}
	}

	for( it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it )
	{
		playing = *it;
		if( playing->m_audioEventRTS->getEventName() == eventName )
		{
			stopPlayingAudio(playing, "removeevent");
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::removeAllDisabledAudio()
{
	std::list<PlayingAudio *>::iterator it;

	PlayingAudio *playing = nullptr;
	for( it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it )
	{
		playing = *it;
		if( playing->m_audioEventRTS->getVolume() == 0.0f )
		{
			stopPlayingAudio(playing, "disabled");
		}
	}

	for( it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it )
	{
		playing = *it;
		if( playing->m_audioEventRTS->getVolume() == 0.0f )
		{
			stopPlayingAudio(playing, "disabled");
		}
	}

	for( it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it )
	{
		playing = *it;
		if( playing->m_audioEventRTS->getVolume() == 0.0f )
		{
			stopPlayingAudio(playing, "disabled");
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::processRequestList()
{
	std::list<AudioRequest*>::iterator it;
	for (it = m_audioRequests.begin(); it != m_audioRequests.end(); ) {
		AudioRequest *req = (*it);
		if (!shouldProcessRequestThisFrame(req)) {
			adjustRequest(req);
			++it;
			continue;
		}

		if (!req->m_requiresCheckForSample || checkForSample(req)) {
			processRequest(req);
		}
		deleteInstance(req);
		it = m_audioRequests.erase(it);
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::processPlayingList()
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;

	//
	// Stop audio without locking. Calls into Miles.
	// Update the position of the audio if it is positional.
	//
	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		playing = (*it);

		if (playing->m_status == PS_Stopping) {
			rerequestPlayingAudioWhenSignalled(playing);
			stopPlayingAudio(playing);
			continue;
		}

		if (playing->m_status == PS_Stopped) {
			continue;
		}

		if (m_volumeHasChanged)
		{
			adjustPlayingVolume(playing);
		}
	}

	for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it) {
		playing = (*it);

		if (playing->m_status == PS_Stopping) {
			rerequestPlayingAudioWhenSignalled(playing);
			stopPlayingAudio(playing);
			continue;
		}

		if (playing->m_status == PS_Stopped) {
			continue;
		}

		if (m_volumeHasChanged)
		{
			adjustPlayingVolume(playing);
		}

		const Coord3D *pos = getCurrentPositionFromEvent(playing->m_audioEventRTS.Peek());
		if (pos)
		{
			if( playing->m_audioEventRTS->isDead() )
			{
				stopAudioEvent( playing->m_audioEventRTS->getPlayingHandle() );
			}
			else
			{
				Real volForConsideration = getEffectiveVolume(playing->m_audioEventRTS.Peek());
				volForConsideration /= (m_sound3DVolume > 0.0f ? m_soundVolume : 1.0f);
				Bool playAnyways = BitIsSet( playing->m_audioEventRTS->getAudioEventInfo()->m_type, ST_GLOBAL) ||
					playing->m_audioEventRTS->getAudioEventInfo()->m_priority == AP_CRITICAL;
				if( volForConsideration < m_audioSettings->m_minVolume && !playAnyways )
				{
					stopPlayingAudio(playing, "range");
				}
				else
				{
					DEBUG_ASSERTCRASH(playing->m_voice, ("3D Sample is not expected to be null"));
					m_device->set3DPosition( playing->m_voice, *pos );
					const Real lowPass = playing->m_audioEventRTS->getAudioEventInfo()->m_lowPassFreq;
					if (playing->m_nativeLoop && lowPass > 0.0f)
					{
						// O72: a native loop gets no new play3D per pass, which set the occlusion from the screen
						const Int occluded = isOnScreen(pos) ? 0 : 1;
						if (occluded != playing->m_occluded) {
							playing->m_occluded = occluded;
							m_device->set3DOcclusion(playing->m_voice, occluded != 0, 1.0f - lowPass);
						}
					}
				}
			}
		}
		else
		{
			stopPlayingAudio(playing, "nopos");
		}
	}

	for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ++it) {
		playing = (*it);

		if (playing->m_status == PS_Stopping) {
			rerequestPlayingAudioWhenSignalled(playing);
			stopPlayingAudio(playing);
			continue;
		}

		if (playing->m_status == PS_Stopped) {
			continue;
		}

		if (m_volumeHasChanged)
		{
			adjustPlayingVolume(playing);
		}
	}

	//
	// Release audio with locking. Must not call into Miles!
	//
	releasePlayingAudioInListIfStopped(m_playingSounds, m_playingSoundsCS);
	releasePlayingAudioInListIfStopped(m_playing3DSounds, m_playing3DSoundsCS);
	releasePlayingAudioInListIfStopped(m_playingStreams, m_playingStreamsCS);

	//
	// Transfer streams to fade list when signaled.
	//
	{
		CriticalSectionClass::LockClass lock(m_playingStreamsCS);
		for (it = m_playingStreams.begin(); it != m_playingStreams.end(); ) {
			playing = (*it);

			if (playing->m_fade)
			{
				{
					CriticalSectionClass::LockClass lock(m_fadingAudioCS);
					m_fadingAudio.push_back(playing);
				}
				it = m_playingStreams.erase(it);
				continue;
			}

			++it;
		}
	}

	if (m_volumeHasChanged) {
		m_volumeHasChanged = false;

		// TheSuperHackers @bugfix Push speech volume changes because Bink movie audio bypasses the Miles mixer.
		if (TheVideoPlayer) {
			TheVideoPlayer->setVolume(getVolume(AudioAffect_Speech));
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::processFadingList()
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;

	//
	// Stop audio without locking. Calls into Miles.
	//
	for (it = m_fadingAudio.begin(); it != m_fadingAudio.end(); ++it) {
		playing = (*it);

		if (playing->m_framesFaded >= getAudioSettings()->m_fadeAudioFrames) {
			stopPlayingAudio(playing, "fadeend");
			continue;
		}

		if (playing->m_status == PS_Stopped) {
			continue;
		}

		++playing->m_framesFaded;
		Real volume = getEffectiveVolume(playing->m_audioEventRTS.Peek());
		volume *= (1.0f - 1.0f * playing->m_framesFaded / getAudioSettings()->m_fadeAudioFrames);

		switch(playing->m_type)
		{
			case PAT_Sample:
			{
				m_device->setVolumePan(playing->m_voice, AVK_2D, volume, 0.5f);
				break;
			}

			case PAT_3DSample:
			{
				m_device->set3DVolume(playing->m_voice, volume);
				break;
			}

			case PAT_Stream:
			{
				m_device->setVolumePan(playing->m_voice, AVK_Stream, volume, 0.5f);
				break;
			}
		}
	}

	//
	// Release audio with locking. Must not call into Miles!
	//
	releasePlayingAudioInListIfStopped(m_fadingAudio, m_fadingAudioCS);
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::shouldProcessRequestThisFrame( AudioRequest *req ) const
{
	if (req->m_pendingEvent == nullptr) {
		return true;
	}

	if (req->m_pendingEvent->getDelay() < MSEC_PER_LOGICFRAME_REAL) {
		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::adjustRequest( AudioRequest *req )
{
	if (req->m_pendingEvent == nullptr) {
		return;
	}

	req->m_pendingEvent->decrementDelay(MSEC_PER_LOGICFRAME_REAL);
	req->m_requiresCheckForSample = true;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::checkForSample( AudioRequest *req )
{
	if (req->m_pendingEvent == nullptr) {
		return true;
	}

  if ( req->m_pendingEvent->getAudioEventInfo() == nullptr )
  {
    // Fill in event info
    getInfoForAudioEvent( req->m_pendingEvent.Peek() );
  }

	if (req->m_pendingEvent->getAudioEventInfo()->m_type != AT_SoundEffect)
  {
		return true;
	}

	return m_sound->canPlayNow(req->m_pendingEvent.Peek());
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::setHardwareAccelerated(Bool accel)
{
	// Extends
	Bool retEarly = (accel == m_hardwareAccel);
	AudioManager::setHardwareAccelerated(accel);

	if (retEarly) {
		return;
	}

	if (m_hardwareAccel) {
		for (Int i = 0; i < MAX_HW_PROVIDERS; ++i) {
			UnsignedInt providerNdx = TheAudio->getProviderIndex(TheAudio->getAudioSettings()->m_preferred3DProvider[i]);
			TheAudio->selectProvider(providerNdx);
			if (getSelectedProvider() == providerNdx) {
				return;
			}
		}
	}

	// set it false
	AudioManager::setHardwareAccelerated(FALSE);
	UnsignedInt providerNdx = TheAudio->getProviderIndex(TheAudio->getAudioSettings()->m_preferred3DProvider[MAX_HW_PROVIDERS]);
	TheAudio->selectProvider(providerNdx);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::setSpeakerSurround(Bool surround)
{
	// Extends
	Bool retEarly = (surround == m_surroundSpeakers);
	AudioManager::setSpeakerSurround(surround);

	if (retEarly) {
		return;
	}

	UnsignedInt speakerType;
	if (m_surroundSpeakers) {
		speakerType = TheAudio->getAudioSettings()->m_defaultSpeakerType3D;
	} else {
		speakerType = TheAudio->getAudioSettings()->m_defaultSpeakerType2D;
	}

	TheAudio->setSpeakerType(speakerType);
}

//-------------------------------------------------------------------------------------------------
Real MilesAudioManager::getFileLengthMS( AsciiString strToLoad ) const
{
	if (strToLoad.isEmpty()) {
		return 0.0f;
	}

	// The device opens it as a stream to get the file info without actually opening the file.
	const Int retVal = m_device->fileLengthMS(strToLoad.str());
	if (retVal < 0) {
		AUDIO_LOG("LEN", "file=%s ms=-1 open_failed", strToLoad.str());
		return 0.0f;
	}
	AUDIO_LOG("LEN", "file=%s ms=%d", strToLoad.str(), retVal);

	return INT_TO_REAL(retVal);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::closeAnySamplesUsingFile( const void *fileToClose )
{
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;

	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it ) {
		playing = *it;

		if (playing->m_file == fileToClose) {
			stopPlayingAudio(playing, "closefile");
		}
	}

	for (it = m_playing3DSounds.begin(); it != m_playing3DSounds.end(); ++it ) {
		playing = *it;

		if (playing->m_file == fileToClose) {
			stopPlayingAudio(playing, "closefile");
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::setDeviceListenerPosition()
{
	if (m_hasListener) {
		m_device->setListener(m_listenerPosition, m_listenerOrientation);

		Real x = m_listenerPosition.x;
		Real y = m_listenerPosition.y;
		Real z = m_listenerPosition.z;
		if (AudioLog::g_on) {
			static UnsignedInt s_lastLoggedFrame = 0xFFFFFFFFu;
			const UnsignedInt logFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
			if (logFrame % 300 == 0 && logFrame != s_lastLoggedFrame) {
				s_lastLoggedFrame = logFrame;
				AUDIO_LOG("LISTENER", "pos=%.1f,%.1f,%.1f orient=%.3f,%.3f,%.3f", x, y, z,
					m_listenerOrientation.x, m_listenerOrientation.y, m_listenerOrientation.z);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
const Coord3D *MilesAudioManager::getCurrentPositionFromEvent( AudioEventRTS *event )
{
	if (!event->isPositionalAudio()) {
		return nullptr;
	}

	return event->getCurrentPosition();
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isOnScreen( const Coord3D *pos ) const
{
	static ICoord2D dummy;
	// WorldToScreen will return True if the point is onscreen and false if it is offscreen.
	return TheTacticalView->worldToScreen(pos, &dummy);
}

//-------------------------------------------------------------------------------------------------
Real MilesAudioManager::getEffectiveVolume(AudioEventRTS *event) const
{
	Real volume = event->getVolume() * event->getVolumeShift();

	switch (event->getAudioEventInfo()->m_soundType)
	{
	case AT_Music:
	{
		volume *= m_musicVolume;
		break;
	}
	case AT_Streaming:
	{
		volume *= m_speechVolume;
		break;
	}
	case AT_SoundEffect:
	{
		if (event->isPositionalAudio())
		{
			volume *= m_sound3DVolume;
			const Coord3D *pos = event->getCurrentPosition();
			if (pos)
			{
				Coord3D distance = m_listenerPosition;
				distance.sub(*pos);
				Real objMinDistance;
				Real objMaxDistance;

				const AudioSettings *audioSettings = TheAudio->getAudioSettings();

				if (event->getAudioEventInfo()->m_type & ST_GLOBAL)
				{
					objMinDistance = audioSettings->m_globalMinRange;
					objMaxDistance = audioSettings->m_globalMaxRange;
				}
				else
				{
					objMinDistance = event->getAudioEventInfo()->m_minDistance;
					objMaxDistance = event->getAudioEventInfo()->m_maxDistance;
				}

				const Real objDistance = distance.length();

				if( objDistance >= objMaxDistance )
				{
					volume = 0.0f;
				}
				else if( audioSettings->m_use3DSoundRangeVolumeFade && objDistance > objMinDistance )
				{
					Real attenuation = (objDistance - objMinDistance) / (objMaxDistance - objMinDistance);
					attenuation = pow(attenuation, audioSettings->m_3DSoundRangeVolumeFadeExponent);
					volume *= 1.0f - attenuation;
				}
			}
		}
		else
		{
			volume *= m_soundVolume;
		}
		break;
	}
	}

	return volume;
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::startNextLoop( PlayingAudio *playing )
{
	closeFile(playing->m_file);
	playing->m_file = nullptr;

	if (!playing->isPlaying()) {
		return false;
	}

	if (playing->m_requestStop) {
		return false;
	}

	if (playing->m_audioEventRTS->hasMoreLoops()) {
		// generate a new filename, and test to see whether we can play with it now
		playing->m_audioEventRTS->generateFilename();

		if (playing->m_audioEventRTS->getDelay() > MSEC_PER_LOGICFRAME_REAL) {
			playing->m_rerequestOnNextUpdate = true;
			InterlockedCompareExchange(reinterpret_cast<volatile long*>(&playing->m_status), PS_Stopping, PS_Playing);
			return true;
		}

		if (playing->m_type == PAT_3DSample) {
			playing->m_file = playSample3D(playing->m_audioEventRTS.Peek(), playing->m_voice);
		} else {
			playing->m_file = playSample(playing->m_audioEventRTS.Peek(), playing->m_voice);
		}
		if (playing->m_file != nullptr) {
			noteRestartGap(playing, TRUE);   // O72
			armNativeLoop(playing);
		}

		return playing->m_file != nullptr;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::playStream( AudioEventRTS *event, AudioVoice stream )
{
	// Force it to the beginning (loop count when music, end callback, start)
	m_device->startStream(stream, event->getAudioEventInfo()->m_soundType == AT_Music ? (Int)INFINITE_LOOP_COUNT : -1);
	if (event->getAudioEventInfo()->m_soundType == AT_Music) {
		// Need to stop/fade out the old music here.
	}
}

//-------------------------------------------------------------------------------------------------
void *MilesAudioManager::playSample( AudioEventRTS *event, AudioVoice sample )
{
	// pitch shift
	Real pitchShift = event->getPitchShift();
	if (pitchShift == 0.0f) {
		DEBUG_CRASH(("Invalid Pitch shift in sound: '%s'", event->getEventName().str()) );
	}

	// Init, end callback, volume, pitch shift and delay filter
	m_device->prepare2D(sample, getEffectiveVolume(event), pitchShift, event->getDelay());
	if (voiceMeterWanted() && event->getEventName().compareNoCase("DozerUSAVoiceSelect") == 0)
	{
		// O55: the in-match line, measured as the preview is
		m_device->resetVoiceMeter(sample);
		s_meteredVoice = sample;
		s_meteredInfo.format("match: %s %s: line volume %.2f x shift %.2f x sound %.2f (system %.2f, script %.2f) = %.3f",
			event->getEventName().str(), event->getFilename().str(), event->getVolume(), event->getVolumeShift(),
			getVolume(AudioAffect_Sound), getSystemSoundVolume(), getScriptSoundVolume(), getEffectiveVolume(event));
		s_meteredLengthMS = 0;
	}

	// Load the file in
	void *fileBuffer = nullptr;
	fileBuffer = loadFileForRead(event);

	// File and start, when there is a file
	AudioSample sampleToPlay;
	sampleToPlay.data = fileBuffer;
	sampleToPlay.bytes = 0;
	sampleToPlay.ownedByDevice = FALSE;
	m_device->start2D(sample, sampleToPlay);

	return fileBuffer;
}

//-------------------------------------------------------------------------------------------------
void *MilesAudioManager::playSample3D( AudioEventRTS *event, AudioVoice sample3D )
{
	const Coord3D *pos = getCurrentPositionFromEvent(event);
	if (pos) {
		// Load the file in
		void *fileBuffer = loadFileForRead(event);
		if (fileBuffer) {
			// The distances as this code always passed them (min, max); mss.h names them (max_dist, min_dist).
			Real distanceArg1;
			Real distanceArg2;
			if (event->getAudioEventInfo()->m_type & ST_GLOBAL) {
				distanceArg1 = TheAudio->getAudioSettings()->m_globalMinRange;
				distanceArg2 = TheAudio->getAudioSettings()->m_globalMaxRange;
			} else {
				distanceArg1 = event->getAudioEventInfo()->m_minDistance;
				distanceArg2 = event->getAudioEventInfo()->m_maxDistance;
			}

			// pitch shift
			Real pitchShift = event->getPitchShift();
			if (pitchShift == 0.0f) {
				DEBUG_CRASH(("Invalid Pitch shift in sound: '%s'", event->getEventName().str()) );
			}

			// Low pass filter
			const Bool useOcclusion = event->getAudioEventInfo()->m_lowPassFreq > 0 && !isOnScreen(pos);

			AudioSample sampleToPlay;
			sampleToPlay.data = fileBuffer;
			sampleToPlay.bytes = 0;
			sampleToPlay.ownedByDevice = FALSE;

			// File, end callback, distances, position, volume, pitch shift, occlusion, start
			m_device->play3D(sample3D, sampleToPlay, distanceArg1, distanceArg2, *pos, getEffectiveVolume(event),
				pitchShift, useOcclusion, 1.0f - event->getAudioEventInfo()->m_lowPassFreq);
		}
		return fileBuffer;
	}

	return nullptr;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::buildProviderList()
{
	// The device enumerated its providers when it opened; the manager keeps their names.
	UnsignedInt index = 0;
	while (index < MAXPROVIDERS && index < m_device->providerCount()) {
		m_provider3D[index].name = m_device->providerName(index);
		++index;
	}

	m_providerCount = index;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::createListener()
{
	if (!(isOn(AudioAffect_Sound3D) && isValidProvider())) {
		return;
	}

	m_hasListener = m_device->createListener(m_selectedProvider);
	// initial listener position will be (0, 0, 0)
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::initDelayFilter()
{
	m_device->initDelayFilter();
}

//-------------------------------------------------------------------------------------------------
Bool MilesAudioManager::isValidProvider()
{
	return (m_selectedProvider < m_providerCount);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::initSamplePools()
{
	if (!(isOn(AudioAffect_Sound3D) && isValidProvider())) {
		return;
	}

	m_availableSamples.reserve(getAudioSettings()->m_sampleCount2D);
	m_available3DSamples.reserve(getAudioSettings()->m_sampleCount3D);

	int i = 0;
	for (i = 0; i < getAudioSettings()->m_sampleCount2D; ++i) {
		AudioVoice sample = m_device->allocVoice(AVK_2D, m_selectedProvider, i + 1);
		DEBUG_ASSERTCRASH(sample, ("Couldn't get %d 2D samples", i + 1));
		if (sample) {
			m_availableSamples.push_back(sample);
			++m_num2DSamples;
		}
	}

	for (i = 0; i < getAudioSettings()->m_sampleCount3D; ++i) {
		AudioVoice sample = m_device->allocVoice(AVK_3D, m_selectedProvider, i + 1);
		DEBUG_ASSERTCRASH(sample, ("Couldn't get %d 3D samples", i + 1));
		if (sample) {
			m_available3DSamples.push_back(sample);
			++m_num3DSamples;
		}
	}

	// Streams are basically free, so we can just allocate the appropriate number
	m_numStreams = getAudioSettings()->m_streamCount;
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::processRequest( AudioRequest *req )
{
	switch (req->m_request)
	{
		case AR_Play:
		{
			playAudioEvent(req);
			break;
		}
		case AR_Pause:
		{
			pauseAudioEvent(req->m_handleToInteractOn);
			break;
		}
		case AR_Stop:
		{
			stopAudioEvent(req->m_handleToInteractOn);
			break;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void *MilesAudioManager::getHandleForBink()
{
	if (m_binkHandle == nullptr) {
		PlayingAudio *playing = allocatePlayingAudio();
		playing->m_audioEventRTS.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS)("BinkHandle"));
		getInfoForAudioEvent(playing->m_audioEventRTS.Peek());
		playing->m_voice = getAvailable2DSample(playing->m_audioEventRTS.Peek());
		playing->m_type = PAT_Sample;

		if (!playing->m_voice) {
			stopPlayingAudio(playing);
			releasePlayingAudio(playing);
			return nullptr;
		}

		m_binkHandle = playing;
	}

	return m_device->movieAudioHandle(m_binkHandle->m_voice);
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @feature Movie sound for the FFmpeg video player, at the volume the Bink player sets: the speech
// volume x 0.8, plus 1 percent (Bink plays volume 0 at full volume).
// With -audioLog, a MOVIESOUND line at the open (the stream the device made, or none) and one at the close.
uintptr_t MilesAudioManager::openMovieSound( UnsignedInt sampleRate, UnsignedInt channels, const char *movieName )
{
	const Int mod = (Int)((getVolume(AudioAffect_Speech) * 0.8f) * 100) + 1;
	const uintptr_t sound = m_device->openMovieSound(sampleRate, channels, mod / 100.0f);
	if (AudioLog::g_on)
	{
		AUDIO_LOG("MOVIESOUND", "open movie=%s device=%s rate=%u channels=%u volume=%.2f result=%s", movieName ? movieName : "-",
			m_device->name(), sampleRate, channels, mod / 100.0f, sound ? "ok" : "no_stream");
		if (sound)
		{
			MovieSoundLog &m = m_movieSoundLogs[sound];
			m.name = movieName ? movieName : "-";
			m.sampleRate = sampleRate;
			m.channels = channels;
			m.pushed = 0;
			m.pushCalls = 0;
			m.openMS = (UnsignedInt)::GetTickCount();
		}
	}
	return sound;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MilesAudioManager::pushMovieSound( uintptr_t sound, const Short *frames, UnsignedInt frameCount )
{
	const UnsignedInt taken = sound ? m_device->pushMovieSound(sound, frames, frameCount) : 0;
	if (AudioLog::g_on && sound)
	{
		std::map<uintptr_t, MovieSoundLog>::iterator it = m_movieSoundLogs.find(sound);
		if (it != m_movieSoundLogs.end()) {
			it->second.pushed += taken;
			++it->second.pushCalls;
		}
	}
	return taken;
}

//-------------------------------------------------------------------------------------------------
// The close line: frames pushed and played (and as ms at the sound's rate), the wall time the sound was open, the
// video frames shown of the movie's frames (and as ms at its frame rate), and the gaps: times the device's queue ran
// dry while more sound was still to come, with the frames of silence in them.
void MilesAudioManager::closeMovieSound( uintptr_t sound, Int framesShown, Int framesTotal, Real framesPerSecond )
{
	if (!sound) {
		return;
	}
	if (AudioLog::g_on)
	{
		std::map<uintptr_t, MovieSoundLog>::iterator it = m_movieSoundLogs.find(sound);
		if (it != m_movieSoundLogs.end())
		{
			const MovieSoundLog &m = it->second;
			const UnsignedInt64 played = m_device->movieSoundPlayedFrames(sound);
			UnsignedInt gaps = 0;
			UnsignedInt64 gapFrames = 0;
			m_device->movieSoundGaps(sound, &gaps, &gapFrames);
			const double rate = m.sampleRate ? (double)m.sampleRate : 1.0;
			const double shownMS = framesPerSecond > 0.0f && framesShown >= 0 ? framesShown * 1000.0 / framesPerSecond : -1.0;
			AUDIO_LOG("MOVIESOUND", "close movie=%s device=%s pushed=%llu played=%llu pushed_ms=%.0f played_ms=%.0f pushes=%u open_ms=%u "
				"shown=%d/%d fps=%.3f shown_ms=%.0f gaps=%u gap_frames=%llu gap_ms=%.0f",
				m.name.str(), m_device->name(), (unsigned long long)m.pushed, (unsigned long long)played, m.pushed * 1000.0 / rate,
				played * 1000.0 / rate, m.pushCalls, (UnsignedInt)::GetTickCount() - m.openMS, framesShown, framesTotal,
				(double)framesPerSecond, shownMS, gaps, (unsigned long long)gapFrames, gapFrames * 1000.0 / rate);
			m_movieSoundLogs.erase(it);
		}
	}
	m_device->closeMovieSound(sound);
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::releaseHandleForBink()
{
	if (m_binkHandle) {
		stopPlayingAudio(m_binkHandle);
		releasePlayingAudio(m_binkHandle);
		m_binkHandle = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
void MilesAudioManager::friend_forcePlayAudioEventRTS(const AudioEventRTS* eventToPlay)
{
	if (!eventToPlay->getAudioEventInfo()) {
		getInfoForAudioEvent(eventToPlay);
		if (!eventToPlay->getAudioEventInfo()) {
			DEBUG_CRASH(("No info for forced audio event '%s'", eventToPlay->getEventName().str()));
			return;
		}
	}

	switch (eventToPlay->getAudioEventInfo()->m_soundType)
	{
		case AT_Music:
			if (!isOn(AudioAffect_Music))
				return;
			break;
		case AT_SoundEffect:
			if (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D))
				return;
			break;
		case AT_Streaming:
			if (!isOn(AudioAffect_Speech))
				return;
			break;
	}

	AudioEventRTS event = *eventToPlay;

	event.generateFilename();
	event.generatePlayInfo();

	std::list<std::pair<AsciiString, Real>/**/>::iterator it;
	for (it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == event.getEventName()) {
			event.setVolume(it->second);
			break;
		}
	}

	AsciiString fileToPlay = event.getFilename();
	// Even though the event type is not Speech, this is used only for mission briefings, so use the
	// speech slider to adjust the volume.
	// Get the volume from the event, and pass 0.5 to play the audio in the middle. (0.0 is full left, 1.0 is full right)
	AudioVoice haud = m_device->playOneShot(fileToPlay.str(), event.getVolume() * getVolume(AudioAffect_Speech), 0.5f);
	m_audioForcePlayed.push_back(haud);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// The end of a voice, from the device (Miles: its timer thread). The kinds are the PAT_* values.
// TheSuperHackers @feature The output device's changes (lost, rerouted, null_fallback, restored, retry_failed, no_output),
// from the device's update() on the main thread.
void MilesAudioManager::onDeviceEvent( const char *event, const char *detail )
{
	AUDIO_LOG("DEVICE", "%s %s", event, detail);
	DEBUG_LOG(("Audio output device: %s %s", event, detail));
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
AudioFileCache::AudioFileCache( AudioDevice *device ) : m_maxSize(0), m_currentlyUsedSize(0), m_mutexName("AudioFileCacheMutex"), m_device(device)
{
	m_mutex = CreateMutex(nullptr, FALSE, m_mutexName);
}

//-------------------------------------------------------------------------------------------------
AudioFileCache::~AudioFileCache()
{
	{
		ScopedMutex mut(m_mutex);

		// Free all the samples that are open.
		OpenFilesHashIt it;
		for ( it = m_openFiles.begin(); it != m_openFiles.end(); ++it ) {
			if (it->second.m_openCount > 0) {
				DEBUG_CRASH(("Sample '%s' is still playing, and we're trying to quit.", it->second.m_eventInfo->m_audioName.str()));
			}

			releaseOpenAudioFile(&it->second);
			// Don't erase it from the map, cause it makes this whole process way more complicated, and
			// we're about to go away anyways.
		}
	}

	CloseHandle(m_mutex);
}

//-------------------------------------------------------------------------------------------------
void *AudioFileCache::openFile( AudioEventRTS *eventToOpenFrom )
{
	// Protect the entire openFile function
	ScopedMutex mut(m_mutex);

	AsciiString strToFind;
	switch (eventToOpenFrom->getNextPlayPortion())
	{
		case PP_Attack:
			strToFind = eventToOpenFrom->getAttackFilename();
			break;
		case PP_Sound:
			strToFind = eventToOpenFrom->getFilename();
			break;
		case PP_Decay:
			strToFind = eventToOpenFrom->getDecayFilename();
			break;
		case PP_Done:
			return nullptr;
	}

	OpenFilesHash::iterator it;
	it = m_openFiles.find(strToFind);

	if (it != m_openFiles.end()) {
		++it->second.m_openCount;
		return it->second.m_sample.data;
	}

	// Couldn't find the file, so actually open it.
	File *file = TheFileSystem->openFile(strToFind.str());
	if (!file) {
		DEBUG_ASSERTLOG(strToFind.isEmpty(), ("Missing Audio File: '%s'", strToFind.str()));
		return nullptr;
	}

	UnsignedInt fileSize = file->size();
	char* buffer = file->readEntireAndClose();

	OpenAudioFile openedAudioFile;
	openedAudioFile.m_eventInfo = eventToOpenFrom->getAudioEventInfo();

	AudioSampleInfo soundInfo;
	m_device->readSampleInfo(buffer, &soundInfo);

	if (eventToOpenFrom->isPositionalAudio()) {
		if (soundInfo.channels > 1) {
			DEBUG_CRASH(("Requested Positional Play of audio '%s', but it is in stereo.", strToFind.str()));
			delete [] buffer;
			return nullptr;
		}
	}

	// IMA ADPCM: decompressed by the device into its own memory; PCM: played from the file bytes.
	AudioSample decoded;
	if (m_device->decodeSample(&soundInfo, buffer, fileSize, &decoded)) {
		fileSize = decoded.bytes;
		if (decoded.ownedByDevice) {
			delete [] buffer;
		}
		openedAudioFile.m_sample = decoded;
		openedAudioFile.m_soundInfo = soundInfo;
		openedAudioFile.m_openCount = 1;
	} else {
		DEBUG_CRASH(("Unexpected compression type in '%s'", strToFind.str()));
		// prevent leaks
		delete [] buffer;
		return nullptr;
	}

	openedAudioFile.m_sample.bytes = fileSize;
	m_currentlyUsedSize += openedAudioFile.m_sample.bytes;
	if (m_currentlyUsedSize > m_maxSize) {
		// We need to free some samples, or we're not going to be able to play this sound.
		if (!freeEnoughSpaceForSample(openedAudioFile)) {
			m_currentlyUsedSize -= openedAudioFile.m_sample.bytes;
			releaseOpenAudioFile(&openedAudioFile);
			return nullptr;
		}
	}

	m_openFiles[strToFind] = openedAudioFile;
	return openedAudioFile.m_sample.data;
}

//-------------------------------------------------------------------------------------------------
void AudioFileCache::closeFile( void *fileToClose )
{
	if (!fileToClose) {
		return;
	}

	// Protect the entire closeFile function
	ScopedMutex mut(m_mutex);

	OpenFilesHash::iterator it;
	for ( it = m_openFiles.begin(); it != m_openFiles.end(); ++it ) {
		if ( it->second.m_sample.data == fileToClose ) {
			--it->second.m_openCount;
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void AudioFileCache::setMaxSize( UnsignedInt size )
{
	// Protect the function, in case we're trying to use this value elsewhere.
	ScopedMutex mut(m_mutex);

	m_maxSize = size;
}

//-------------------------------------------------------------------------------------------------
void AudioFileCache::releaseOpenAudioFile( OpenAudioFile *fileToRelease )
{
	if (fileToRelease->m_openCount > 0) {
		// This thing needs to be terminated IMMEDIATELY.
		TheAudio->closeAnySamplesUsingFile(fileToRelease->m_sample.data);
	}

	if (fileToRelease->m_sample.data) {
		if (fileToRelease->m_sample.ownedByDevice) {
			// Files the device decompressed must be freed by the device.
			m_device->freeSample(fileToRelease->m_sample.data);
		} else {
			// Otherwise, we read it, we own it, blow it away.
			delete [] fileToRelease->m_sample.data;
		}
		fileToRelease->m_sample.data = nullptr;
		fileToRelease->m_eventInfo = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
Bool AudioFileCache::freeEnoughSpaceForSample(const OpenAudioFile& sampleThatNeedsSpace)
{

	Int spaceRequired = m_currentlyUsedSize - m_maxSize;
	Int runningTotal = 0;

	std::list<AsciiString> filesToClose;
	// First, search for any samples that have ref counts of 0. They are low-hanging fruit, and
	// should be considered immediately.
	OpenFilesHashIt it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second.m_openCount == 0) {
			// This is said low-hanging fruit.
			filesToClose.push_back(it->first);

			runningTotal += it->second.m_sample.bytes;

			if (runningTotal >= spaceRequired) {
				break;
			}
		}
	}

	// If we don't have enough space yet, then search through the events who have a count of 1 or more
	// and who are lower priority than this sound.
	// Mical said that at this point, sounds shouldn't care if other sounds are interruptable or not.
	// Kill any files of lower priority necessary to clear our the buffer.
	if (runningTotal < spaceRequired) {
		for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
			if (it->second.m_openCount > 0) {
				if (it->second.m_eventInfo->m_priority < sampleThatNeedsSpace.m_eventInfo->m_priority) {
					filesToClose.push_back(it->first);
					runningTotal += it->second.m_sample.bytes;

					if (runningTotal >= spaceRequired) {
						break;
					}
				}
			}
		}
	}

	// We weren't able to find enough sounds to truncate. Therefore, this sound is not going to play.
	if (runningTotal < spaceRequired) {
		return FALSE;
	}

	std::list<AsciiString>::iterator ait;
	for (ait = filesToClose.begin(); ait != filesToClose.end(); ++ait) {
		OpenFilesHashIt itToErase = m_openFiles.find(*ait);
		if (itToErase != m_openFiles.end()) {
			releaseOpenAudioFile(&itToErase->second);
			m_currentlyUsedSize -= itToErase->second.m_sample.bytes;
			m_openFiles.erase(itToErase);
		}
	}

	return TRUE;
}


#if defined(RTS_DEBUG)
//-------------------------------------------------------------------------------------------------
void MilesAudioManager::dumpAllAssetsUsed()
{
	if (!TheGlobalData->m_preloadReport) {
		return;
	}

	// Dump all the audio assets we've used.
	FILE *logfile=fopen("PreloadedAssets.txt","a+");	//append to log
	if (!logfile)
		return;

	std::list<AsciiString> missingEvents;
	std::list<AsciiString> usedFiles;

	std::list<AsciiString>::iterator lit;

	fprintf(logfile, "\nAudio Asset Report - BEGIN\n");
	{
		SetAsciiStringIt it;
		std::vector<AsciiString>::iterator asIt;
		for (it = m_allEventsLoaded.begin(); it != m_allEventsLoaded.end(); ++it) {
			AsciiString astr = *it;
			AudioEventInfo *aei = findAudioEventInfo(astr);
			if (!aei) {
				missingEvents.push_back(astr);
				continue;
			}

			for (asIt = aei->m_attackSounds.begin(); asIt != aei->m_attackSounds.end(); ++asIt) {
				usedFiles.push_back(*asIt);
			}

			for (asIt = aei->m_sounds.begin(); asIt != aei->m_sounds.end(); ++asIt) {
				usedFiles.push_back(*asIt);
			}

			for (asIt = aei->m_decaySounds.begin(); asIt != aei->m_decaySounds.end(); ++asIt) {
				usedFiles.push_back(*asIt);
			}

			if (!aei->m_filename.isEmpty()) {
				usedFiles.push_back(aei->m_filename);
			}
		}

		fprintf(logfile, "\nEvents Requested that are missing information - BEGIN\n");
		for (lit = missingEvents.begin(); lit != missingEvents.end(); ++lit) {
			fprintf(logfile, "%s\n", (*lit).str());
		}
		fprintf(logfile, "\nEvents Requested that are missing information - END\n");

		fprintf(logfile, "\nFiles Used - BEGIN\n");
		for (lit = usedFiles.begin(); lit != usedFiles.end(); ++lit) {
			fprintf(logfile, "%s\n", (*lit).str());
		}
		fprintf(logfile, "\nFiles Used - END\n");
	}
	fprintf(logfile, "\nAudio Asset Report - END\n");
	fclose(logfile);
	logfile = nullptr;
}
#endif
