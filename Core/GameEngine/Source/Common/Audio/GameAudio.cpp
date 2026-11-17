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

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: GameAudio.cpp
//
// Created:   5/01/01
//
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/GameAudio.h"

#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/CommandLine.h"
#include "Common/FileSystem.h"
#include "Common/GameEngine.h"
#include "Common/GameMusic.h"
#include "Common/GameSounds.h"
#include "Common/MiscAudio.h"
#include "Common/OSDisplay.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/OptionPreferences.h"

#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"

#include "WWMath/matrix3d.h"

///////////////////////////////////////////////////////////////////////////////////////////////////

static const char* TheSpeakerTypes[] =
{
	"2 Speakers",
	"Headphones",
	"Surround Sound",
	"4 Speaker",
	"5.1 Surround",
	"7.1 Surround",
	nullptr
};

static const Int TheSpeakerTypesCount = sizeof(TheSpeakerTypes) / sizeof(TheSpeakerTypes[0]);

static void parseSpeakerType( INI *ini, void *instance, void *store, const void *userData );

// Field Parse table for Audio Settings ///////////////////////////////////////////////////////////
static const FieldParse audioSettingsFieldParseTable[] =
{
	{ "AudioRoot",						INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_audioRoot) },
	{ "SoundsFolder",					INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_soundsFolder) },
	{ "MusicFolder",					INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_musicFolder) },
	{ "StreamingFolder",			INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_streamingFolder) },
	{ "SoundsExtension",			INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_soundsExtension) },

	{ "UseDigital",						INI::parseBool,											nullptr,							offsetof( AudioSettings, m_useDigital) },
	{ "UseMidi",							INI::parseBool,											nullptr,							offsetof( AudioSettings, m_useMidi) },
	{ "OutputRate",						INI::parseInt,											nullptr,							offsetof( AudioSettings, m_outputRate) },
	{ "OutputBits",						INI::parseInt,											nullptr,							offsetof( AudioSettings, m_outputBits) },
	{ "OutputChannels",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_outputChannels) },
	{ "SampleCount2D",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_sampleCount2D) },
	{ "SampleCount3D",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_sampleCount3D) },
	{ "StreamCount",					INI::parseInt,											nullptr,							offsetof( AudioSettings, m_streamCount) },

	{ "Preferred3DHW1",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[0]) },
	{ "Preferred3DHW2",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[1]) },
	{ "Preferred3DHW3",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[2]) },
	{ "Preferred3DHW4",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[3]) },

	{ "Preferred3DSW",				INI::parseAsciiString,							nullptr,							offsetof( AudioSettings, m_preferred3DProvider[4]) },

	{ "Default2DSpeakerType",		 parseSpeakerType,							nullptr,							offsetof( AudioSettings, m_defaultSpeakerType2D) },
	{ "Default3DSpeakerType",		 parseSpeakerType,							nullptr,							offsetof( AudioSettings, m_defaultSpeakerType3D) },

	{ "MinSampleVolume",			INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_minVolume) },
	{ "Use3DSoundRangeVolumeFade", INI::parseBool,								nullptr,							offsetof( AudioSettings, m_use3DSoundRangeVolumeFade) },
	{ "3DSoundRangeVolumeFadeExponent", INI::parseReal,						nullptr,							offsetof( AudioSettings, m_3DSoundRangeVolumeFadeExponent) },
	{ "GlobalMinRange",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_globalMinRange) },
	{ "GlobalMaxRange",				INI::parseInt,											nullptr,							offsetof( AudioSettings, m_globalMaxRange) },
	{ "TimeBetweenDrawableSounds", INI::parseDurationUnsignedInt, nullptr,							offsetof( AudioSettings, m_drawableAmbientFrames) },
	{ "TimeToFadeAudio",			INI::parseDurationUnsignedInt,			nullptr,							offsetof( AudioSettings, m_fadeAudioFrames) },
	{ "AudioFootprintInBytes",INI::parseUnsignedInt,							nullptr,							offsetof( AudioSettings, m_maxCacheSize) },
	{ "Relative2DVolume",			INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_relative2DVolume ) },
	{ "DefaultSoundVolume",		INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_defaultSoundVolume) },
	{ "Default3DSoundVolume",	INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_default3DSoundVolume) },
	{ "DefaultSpeechVolume",	INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_defaultSpeechVolume) },
	{ "DefaultMusicVolume",		INI::parsePercentToReal,						nullptr,							offsetof( AudioSettings, m_defaultMusicVolume) },
	{ "DefaultMoneyTransactionVolume", INI::parsePercentToReal,		nullptr,							offsetof( AudioSettings, m_defaultMoneyTransactionVolume) },
	{ "MicrophoneDesiredHeightAboveTerrain",	INI::parseReal,			nullptr,							offsetof( AudioSettings, m_microphoneDesiredHeightAboveTerrain ) },
	{ "MicrophoneMaxPercentageBetweenGroundAndCamera", INI::parsePercentToReal,	nullptr,	offsetof( AudioSettings, m_microphoneMaxPercentageBetweenGroundAndCamera ) },
  { "ZoomMinDistance",		INI::parseReal,									nullptr,							offsetof( AudioSettings, m_zoomMinDistance ) },
  { "ZoomMaxDistance",		INI::parseReal,									nullptr,							offsetof( AudioSettings, m_zoomMaxDistance ) },
  { "ZoomSoundVolumePercentageAmount",		INI::parsePercentToReal,	nullptr,		offsetof( AudioSettings, m_zoomSoundVolumePercentageAmount ) },

	{ nullptr, nullptr, nullptr, 0 }
};

// Singleton TheAudio /////////////////////////////////////////////////////////////////////////////
AudioManager *TheAudio = nullptr;

const char *const AudioManager::MuteAudioReasonNames[] =
{
	"MuteAudioReason_WindowFocus",
};

// AudioManager Device Independent functions //////////////////////////////////////////////////////
AudioManager::AudioManager() :
	m_soundOn(TRUE),
	m_sound3DOn(TRUE),
	m_musicOn(TRUE),
	m_speechOn(TRUE),
	m_music(nullptr),
	m_sound(nullptr),
	m_surroundSpeakers(FALSE),
	m_hardwareAccel(FALSE)
{
	static_assert(ARRAY_SIZE(AudioManager::MuteAudioReasonNames) == MuteAudioReason_Count, "Incorrect array size");

	m_adjustedVolumes.clear();
	m_audioRequests.clear();
	m_listenerPosition.zero();
	m_musicTracks.clear();
	m_musicVolume = 0.0f;
	m_sound3DVolume = 0.0f;
	m_soundVolume   = 0.0f;
	m_speechVolume  = 0.0f;
	m_systemMusicVolume   = 0.0f;
	m_systemSound3DVolume = 0.0f;
	m_systemSoundVolume   = 0.0f;
	m_systemSpeechVolume  = 0.0f;
	m_volumeHasChanged			= FALSE;
	m_listenerOrientation.set(0.0, 1.0, 0.0);
	theAudioHandlePool = AHSV_FirstHandle;
	m_audioSettings = NEW AudioSettings;
	m_miscAudio = NEW MiscAudio;
	m_silentAudioEvent = NEW AudioEventRTS;
	m_savedValues = nullptr;
	m_muteReasonBits = 0;
	m_disallowSpeech = FALSE;
}

//-------------------------------------------------------------------------------------------------
AudioManager::~AudioManager()
{
	// cleanup all of the loaded AudioEventInfos
	AudioEventInfoHashIt it;
	for (it = m_allAudioEventInfo.begin(); it != m_allAudioEventInfo.end(); ++it) {
		AudioEventInfo *eventInfo = (*it).second;
		deleteInstance(eventInfo);
	}
	m_allAudioEventInfo.clear();

	delete m_silentAudioEvent;
	m_silentAudioEvent = nullptr;

	delete m_music;
	m_music = nullptr;

	delete m_sound;
	m_sound = nullptr;

	delete m_miscAudio;
	m_miscAudio = nullptr;

	delete m_audioSettings;
	m_audioSettings = nullptr;

	delete [] m_savedValues;
}

//-------------------------------------------------------------------------------------------------
// Audio event log (-audioLog)
//-------------------------------------------------------------------------------------------------
namespace AudioLog
{
	Bool g_on = FALSE;
	static FILE *s_file = nullptr;
	static UnsignedInt s_seq = 0;

	void init()
	{
		if (s_file || !CommandLine::isAudioLogEnabled())
			return;

		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat("AudioEvents.log");
		s_file = fopen(path.str(), "w");
		if (s_file)
		{
			g_on = TRUE;
			fprintf(s_file, "# AudioEvents.log version 1\n");
		}
	}

	static const char *kindOf(const AudioEventInfo *info)
	{
		if (!info)
			return "?";
		switch (info->m_soundType)
		{
			case AT_Music: return "music";
			case AT_SoundEffect: return "sound";
			case AT_Streaming: return "speech";
		}
		return "?";
	}

	static void vwrite(const char *tag, const char *text)
	{
		if (!s_file)
			return;
		const UnsignedInt frame = TheGameLogic ? TheGameLogic->getFrame() : 0;
		fprintf(s_file, "%u f=%u %s %s\n", ++s_seq, frame, tag, text);
		fflush(s_file);
	}

	void log(const char *tag, const char *fmt, ...)
	{
		char buf[768];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		buf[sizeof(buf) - 1] = 0;
		vwrite(tag, buf);
	}

	/// TheSuperHackers @bugfix O78 -audioLog in the product exe: no requests and no refusals (they name the sounds of
	/// units the player may not see or hear, e.g. "why=shroud", with their place) and no place or owner of any sound
	/// (pos=, obj=, drw=): the speakers give a direction, the log would give a map position. A *uitest*.exe logs all.
	static Bool fullDetail()
	{
		static int s_state = -1;
		if (s_state < 0) {
			s_state = CommandLine::isTestExe() ? 1 : 0;
		}
		return s_state == 1;
	}

	void logEvent(const char *tag, AudioEventRTS *ev, const char *fmt, ...)
	{
		if (!fullDetail() && (strcmp(tag, "REQ") == 0 || strcmp(tag, "QUE") == 0 || strcmp(tag, "REJ") == 0)) {
			return;
		}
		char head[384];
		const AudioEventInfo *info = ev->getAudioEventInfo();
		int n = _snprintf(head, sizeof(head), "ev=%s kind=%s h=%u prio=%d vol=%.3f pl=%d",
			ev->getEventName().str(), kindOf(info), (UnsignedInt)ev->getPlayingHandle(),
			info ? (int)info->m_priority : -1, ev->getVolume(), ev->getPlayerIndex());
		if (n < 0 || n >= (int)sizeof(head))
			n = (int)sizeof(head) - 1;
		switch (fullDetail() ? ev->getOwnerType() : OT_INVALID)   // O78: no place in the product exe
		{
			case OT_Positional:
			{
				const Coord3D *pos = ev->getPosition();
				if (pos)
					n += _snprintf(head + n, sizeof(head) - n, " pos=%.1f,%.1f,%.1f", pos->x, pos->y, pos->z);
				break;
			}
			case OT_Object:
				n += _snprintf(head + n, sizeof(head) - n, " obj=%u", (UnsignedInt)ev->getObjectID());
				break;
			case OT_Drawable:
				n += _snprintf(head + n, sizeof(head) - n, " drw=%u", (UnsignedInt)ev->getDrawableID());
				break;
			case OT_Dead:
				n += _snprintf(head + n, sizeof(head) - n, " dead");
				break;
			default:
				break;
		}
		head[sizeof(head) - 1] = 0;

		char tail[320];
		va_list args;
		va_start(args, fmt);
		vsnprintf(tail, sizeof(tail), fmt, args);
		va_end(args);
		tail[sizeof(tail) - 1] = 0;

		char all[768];
		_snprintf(all, sizeof(all), "%s %s", head, tail);
		all[sizeof(all) - 1] = 0;
		vwrite(tag, all);
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::init()
{
	AudioLog::init();

	INI ini;
	ini.loadFileDirectory( "Data\\INI\\AudioSettings", INI_LOAD_OVERWRITE, nullptr);

	ini.loadFileDirectory( "Data\\INI\\Default\\Music", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\Music", INI_LOAD_OVERWRITE, nullptr );

	ini.loadFileDirectory( "Data\\INI\\Default\\SoundEffects", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\SoundEffects", INI_LOAD_OVERWRITE, nullptr );

	ini.loadFileDirectory( "Data\\INI\\Default\\Speech", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\Speech", INI_LOAD_OVERWRITE, nullptr );

	ini.loadFileDirectory( "Data\\INI\\Default\\Voice", INI_LOAD_OVERWRITE, nullptr );
	ini.loadFileDirectory( "Data\\INI\\Voice", INI_LOAD_OVERWRITE, nullptr );

	// do the miscellaneous sound files last so that we find the AudioEventRTS associated with the events.
	ini.loadFileDirectory( "Data\\INI\\MiscAudio", INI_LOAD_OVERWRITE, nullptr);

	m_music = NEW MusicManager;
	m_sound = NEW SoundManager;

	// Set our system volumes from the user's preferred settings, not the defaults.
	m_systemMusicVolume = m_audioSettings->m_preferredMusicVolume;
	m_systemSoundVolume = m_audioSettings->m_preferredSoundVolume;
	m_systemSound3DVolume = m_audioSettings->m_preferred3DSoundVolume;
	m_systemSpeechVolume = m_audioSettings->m_preferredSpeechVolume;

	m_scriptMusicVolume = 1.0f;
	m_scriptSoundVolume = 1.0f;
	m_scriptSound3DVolume = 1.0f;
	m_scriptSpeechVolume = 1.0f;

	m_musicVolume = m_systemMusicVolume;
	m_soundVolume = m_systemSoundVolume;
	m_sound3DVolume = m_systemSound3DVolume;
	m_speechVolume = m_systemSpeechVolume;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::postProcessLoad()
{

}

//-------------------------------------------------------------------------------------------------
void AudioManager::reset()
{
	// clear out any adjusted volumes we might have set.
	m_adjustedVolumes.clear();

	// adjust the scripted volumes, and reset the
	m_scriptMusicVolume = 1.0f;
	m_scriptSoundVolume = 1.0f;
	m_scriptSound3DVolume = 1.0f;
	m_scriptSpeechVolume = 1.0f;

	// restore the final values to the
	m_musicVolume = m_systemMusicVolume;
	m_soundVolume = m_systemSoundVolume;
	m_sound3DVolume = m_systemSound3DVolume;
	m_speechVolume = m_systemSpeechVolume;

	m_disallowSpeech = FALSE;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::update()
{
	Coord3D cameraPivot = TheTacticalView->getPosition();
	Real angle = TheTacticalView->getAngle();
	Matrix3D rot = Matrix3D::Identity;
	rot.Rotate_Z( angle );
	Vector3 forward( 0, 1, 0 );
	rot.mulVector3( forward );

	const Real desiredHeightRel = m_audioSettings->m_microphoneDesiredHeightAboveTerrain;
	const Real desiredHeightAbs = desiredHeightRel + cameraPivot.z;
	const Real maxPercentage = m_audioSettings->m_microphoneMaxPercentageBetweenGroundAndCamera;

	Coord3D lookTo;
	lookTo.set(forward.X, forward.Y, forward.Z);

	//Kris: At this point, the microphone is calculated to be at the ground position where the camera is looking at.
	//Instead we want to move the microphone towards the camera. Hopefully, it'll be a desired altitude, but if it
	//gets too close to the camera (or even past it), that would be undesirable. Therefore, we have a backup method
	//of making sure we only go a certain percentage towards the camera or the desired height, whichever occurs first.
	Coord3D cameraPos = TheTacticalView->get3DCameraPosition();
	Coord3D groundToCameraVector;
	groundToCameraVector.set( cameraPos );
	groundToCameraVector.sub( cameraPivot );
	Real bestScaleFactor;

	if( cameraPos.z <= desiredHeightAbs || groundToCameraVector.z <= 0.0f )
	{
		//Use the percentage calculation!
		bestScaleFactor = maxPercentage;
	}
	else
	{
		//Calculate the stopping position of the groundToCameraVector when we force z to be m_microphoneDesiredHeightAboveTerrain
		Real zScale = desiredHeightRel / groundToCameraVector.z;

		//Use the smallest of the two scale calculations
		bestScaleFactor = MIN( maxPercentage, zScale );
	}

	//Now apply the best scalar to the ground-to-camera vector.
	groundToCameraVector.scale( bestScaleFactor );

	//Set the microphone to be the ground position adjusted for terrain plus the vector we just calculated.
	Coord3D microphonePos;
	microphonePos.set( cameraPivot );
	microphonePos.add( groundToCameraVector );

	//Viola! A properly placed microphone.
	setListenerPosition( &microphonePos, &lookTo );


	//Now determine if we would like to boost the volume based on the camera being close to the microphone!
	Real maxBoostScalar = m_audioSettings->m_zoomSoundVolumePercentageAmount;
	Real minDist = m_audioSettings->m_zoomMinDistance;
	Real maxDist = m_audioSettings->m_zoomMaxDistance;

	//We can't boost a sound above 100%, instead reduce the normal sound level.
	m_zoomVolume = 1.0f - maxBoostScalar;

	//Are we even using a boost?
	if( maxBoostScalar > 0.0f )
	{
		//How far away is the camera from the microphone?
		Coord3D vector = cameraPos;
		vector.sub( microphonePos );
		Real dist = vector.length();

		if( dist < minDist )
		{
			//Max volume!
			m_zoomVolume = 1.0f;
		}
		else if( dist < maxDist )
		{
			//Determine what the boost amount will be.
			Real scalar = (dist - minDist) / (maxDist - minDist);
			m_zoomVolume = 1.0f - scalar * maxBoostScalar;
		}
	}

	set3DVolumeAdjustment( m_zoomVolume );

}

//-------------------------------------------------------------------------------------------------
void AudioManager::getInfoForAudioEvent( const AudioEventRTS *eventToFindAndFill ) const
{
	if (!eventToFindAndFill) {
		return;
	}

	if (eventToFindAndFill->getAudioEventInfo()) {
		// already done
		return;
	}

	eventToFindAndFill->setAudioEventInfo(findAudioEventInfo(eventToFindAndFill->getEventName()));
}

//-------------------------------------------------------------------------------------------------
AudioHandle AudioManager::addAudioEvent(const AudioEventRTS *eventToAdd)
{
	if (eventToAdd->getEventName().isEmpty() || eventToAdd->getEventName() == "NoSound") {
		return AHSV_NoSound;
	}

#ifdef INTENSIVE_AUDIO_DEBUG
	DEBUG_LOG(("AUDIO (%d): Received addAudioEvent('%s')", TheGameLogic->getFrame(), eventToAdd->getEventName().str()));
#endif
	if (!eventToAdd->getAudioEventInfo()) {
		getInfoForAudioEvent(eventToAdd);
		if (!eventToAdd->getAudioEventInfo()) {
			DEBUG_CRASH(("No info for requested audio event '%s'", eventToAdd->getEventName().str()));
			return AHSV_Error;
		}
	}

	const AudioType soundType = eventToAdd->getAudioEventInfo()->m_soundType;

	AUDIO_LOG_EVENT("REQ", const_cast<AudioEventRTS *>(eventToAdd), "");

	// Check if audio type is on
	// TheSuperHackers @info Zero audio volume is not a fail condition, because music, speech and sounds
	// still need to be in flight in case the user raises the volume on runtime after the audio was already triggered.
	switch (soundType)
	{
		case AT_Music:
			if (!isOn(AudioAffect_Music))
			{
				AUDIO_LOG_EVENT("REJ", const_cast<AudioEventRTS *>(eventToAdd), "why=category_off");
				return AHSV_NoSound;
			}
			break;
		case AT_SoundEffect:
			if (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D))
			{
				AUDIO_LOG_EVENT("REJ", const_cast<AudioEventRTS *>(eventToAdd), "why=category_off");
				return AHSV_NoSound;
			}
			break;
		case AT_Streaming:
			// if we're currently playing uninterruptable speech, then disallow the addition of this sample
			if (getDisallowSpeech())
			{
				AUDIO_LOG_EVENT("REJ", const_cast<AudioEventRTS *>(eventToAdd), "why=speech_disallowed");
				return AHSV_NoSound;
			}
			if (!isOn(AudioAffect_Speech))
			{
				AUDIO_LOG_EVENT("REJ", const_cast<AudioEventRTS *>(eventToAdd), "why=category_off");
				return AHSV_NoSound;
			}
			break;
	}

	// TheSuperHackers @info Scripted audio events are logical, i.e. synchronized across clients.
	// In retail mode this early return cannot be taken for such audio events as it skips code that changes the logical game seed values.
	// In non-retail mode logical audio events are decoupled from the CRC computation, so this early return is allowed.
#if RETAIL_COMPATIBLE_CRC
	const Bool logicalAudio = eventToAdd->getIsLogicalAudio();
#else
	const Bool logicalAudio = FALSE;
#endif
	const Bool notForLocal = !eventToAdd->getUninterruptible() && !shouldPlayLocally(eventToAdd);

	if (!logicalAudio && notForLocal)
	{
		AUDIO_LOG_EVENT("REJ", const_cast<AudioEventRTS *>(eventToAdd), "why=not_for_local");
		return AHSV_NotForLocal;
	}

	RefCountPtr<DynamicAudioEventRTS> audioEvent;
	audioEvent.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS)(*eventToAdd));
	audioEvent->setPlayingHandle( allocateNewHandle() );
	audioEvent->generateFilename();	// which file are we actually going to play?
	eventToAdd->setPlayingAudioIndex( audioEvent->getPlayingAudioIndex() );
	audioEvent->generatePlayInfo();	// generate pitch shift and volume shift now as well

	std::list<std::pair<AsciiString, Real>/**/>::iterator it;
	for (it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == audioEvent->getEventName()) {
			audioEvent->setVolume(it->second);
			break;
		}
	}

#if RETAIL_COMPATIBLE_CRC
	if (notForLocal)
	{
		AUDIO_LOG_EVENT("REJ", audioEvent.Peek(), "why=not_for_local");
		return AHSV_NotForLocal;
	}
#endif

	// cull muted audio
	if (audioEvent->getVolume() < m_audioSettings->m_minVolume) {
#ifdef INTENSIVE_AUDIO_DEBUG
		DEBUG_LOG((" - culled due to muting (%d).", audioEvent->getVolume()));
#endif
		AUDIO_LOG_EVENT("REJ", audioEvent.Peek(), "why=volume_zero min=%.3f", m_audioSettings->m_minVolume);
		return AHSV_Muted;
	}

	if (soundType == AT_Music)
	{
		AUDIO_LOG_EVENT("QUE", audioEvent.Peek(), "pool=stream");
		m_music->addAudioEvent(audioEvent.Peek());
	}
	else
	{
		if (!m_sound->addAudioEvent(audioEvent.Peek()))
		{
			audioEvent.Clear();
		}
	}

	if( audioEvent != nullptr )
	{
		return audioEvent->getPlayingHandle();
	}
	return AHSV_NoSound;
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isValidAudioEvent(const AudioEventRTS *eventToCheck) const
{
	if (eventToCheck->getEventName().isEmpty()) {
		return false;
	}

	getInfoForAudioEvent(eventToCheck);

	return (eventToCheck->getAudioEventInfo() != nullptr);
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isValidAudioEvent( AudioEventRTS *eventToCheck ) const
{
	if( eventToCheck->getEventName().isEmpty() )
	{
		return false;
	}

	getInfoForAudioEvent( eventToCheck );

	return( eventToCheck->getAudioEventInfo() );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::addTrackName( const AsciiString& trackName )
{
	m_musicTracks.push_back(trackName);
}

//-------------------------------------------------------------------------------------------------
AsciiString AudioManager::nextTrackName(const AsciiString& currentTrack )
{
	std::vector<AsciiString>::iterator it;
	for (it = m_musicTracks.begin(); it != m_musicTracks.end(); ++it) {
		if (*it == currentTrack) {
			break;
		}
	}

	if (it != m_musicTracks.end()) {
		++it;
	}

	if (it == m_musicTracks.end()) {
		it = m_musicTracks.begin();
		if (it == m_musicTracks.end()) {
			return AsciiString::TheEmptyString;
		}
	}

	return *it;
}

//-------------------------------------------------------------------------------------------------
AsciiString AudioManager::prevTrackName(const AsciiString& currentTrack )
{
	std::vector<AsciiString>::reverse_iterator rit;
	for (rit = m_musicTracks.rbegin(); rit != m_musicTracks.rend(); ++rit) {
		if (*rit == currentTrack) {
			break;
		}
	}

	if (rit != m_musicTracks.rend()) {
		++rit;
	}

	if (rit == m_musicTracks.rend()) {
		rit = m_musicTracks.rbegin();
		if (rit == m_musicTracks.rend()) {
			return AsciiString::TheEmptyString;
		}
	}

	return *rit;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::removeAudioEvent(AudioHandle audioEvent)
{
	if (audioEvent == AHSV_StopTheMusic || audioEvent == AHSV_StopTheMusicFade) {
		AUDIO_LOG("STOPCALL", "h=%s", audioEvent == AHSV_StopTheMusic ? "StopTheMusic" : "StopTheMusicFade");
		m_music->removeAudioEvent(audioEvent);
		return;
	}

	if (audioEvent < AHSV_FirstHandle) {
		return;
	}

	AUDIO_LOG("STOPCALL", "h=%u", (UnsignedInt)audioEvent);

	AudioRequest *req = allocateAudioRequest();
	req->m_handleToInteractOn = audioEvent;
	req->m_request = AR_Stop;
	appendAudioRequest( req );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setAudioEventEnabled( AsciiString eventToAffect, Bool enable )
{
	setAudioEventVolumeOverride(eventToAffect, (enable ? -1.0f : 0.0f) );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setAudioEventVolumeOverride( AsciiString eventToAffect, Real newVolume )
{
	if (eventToAffect == AsciiString::TheEmptyString) {
		m_adjustedVolumes.clear();
		return;
	}

	AUDIO_LOG("OVERRIDE", "ev=%s vol=%.3f", eventToAffect.str(), newVolume);

	// Find any playing audio events and adjust their volume accordingly.
	if (newVolume != -1.0f) {
		adjustVolumeOfPlayingAudio(eventToAffect, newVolume);
	}

	std::list<std::pair<AsciiString, Real>/**/>::iterator it;
	for (it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == eventToAffect) {
			if (newVolume == -1.0f) {
				m_adjustedVolumes.erase(it);
				return;
			} else {
				it->second = newVolume;
				return;
			}
		}
	}

	if (newVolume != -1.0f) {
		std::pair<AsciiString, Real> newPair;
		newPair.first = eventToAffect;
		newPair.second = newVolume;
		m_adjustedVolumes.push_front(newPair);
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::removeAudioEvent( AsciiString eventToRemove )
{
	removePlayingAudio( eventToRemove );
}

//-------------------------------------------------------------------------------------------------
void AudioManager::removeDisabledEvents()
{
	removeAllDisabledAudio();
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isCurrentlyPlaying( AudioHandle audioEvent )
{
	return true;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt AudioManager::translateSpeakerTypeToUnsignedInt( const AsciiString& speakerType )
{
	for (UnsignedInt i = 0; TheSpeakerTypes[i]; ++i) {
		if (TheSpeakerTypes[i] == speakerType) {
			return i;
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------------------
AsciiString AudioManager::translateUnsignedIntToSpeakerType( UnsignedInt speakerType )
{
	if (speakerType >= TheSpeakerTypesCount) {
		return TheSpeakerTypes[0];
	}

	return TheSpeakerTypes[speakerType];
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isOn( AudioAffect whichToGet ) const
{
	if (whichToGet & AudioAffect_Music) {
		return m_musicOn;
	} else if (whichToGet & AudioAffect_Sound) {
		return m_soundOn;
	} else if (whichToGet & AudioAffect_Sound3D) {
		return m_sound3DOn;
	}

	// Speech
	return m_speechOn;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setOn( Bool turnOn, AudioAffect whichToAffect )
{
	AUDIO_LOG("ON", "on=%d affect=0x%x", turnOn ? 1 : 0, (unsigned)whichToAffect);
	if (whichToAffect & AudioAffect_Music) {
		m_musicOn = turnOn;
	}

	if (whichToAffect & AudioAffect_Sound) {
		m_soundOn = turnOn;
	}

	if (whichToAffect & AudioAffect_Sound3D) {
		m_sound3DOn = turnOn;
	}

	if (whichToAffect & AudioAffect_Speech) {
		m_speechOn = turnOn;
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setVolume( Real volume, AudioAffect whichToAffect )
{
	if (whichToAffect & AudioAffect_Music) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemMusicVolume = volume;
		} else {
			m_scriptMusicVolume = volume;
		}

		const Real newVolume = m_scriptMusicVolume * m_systemMusicVolume;
		if (m_musicVolume != newVolume)
		{
			m_musicVolume = newVolume;
			m_volumeHasChanged = true;
			AUDIO_LOG("VOL", "cat=music total=%.4f system=%d", newVolume, (whichToAffect & AudioAffect_SystemSetting) ? 1 : 0);
		}
	}

	if (whichToAffect & AudioAffect_Sound) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemSoundVolume = volume;
		} else {
			m_scriptSoundVolume = volume;
		}

		const Real newVolume = m_scriptSoundVolume * m_systemSoundVolume;
		if (m_soundVolume != newVolume)
		{
			m_soundVolume = newVolume;
			m_volumeHasChanged = true;
			AUDIO_LOG("VOL", "cat=sound total=%.4f system=%d", newVolume, (whichToAffect & AudioAffect_SystemSetting) ? 1 : 0);
		}
	}

	if (whichToAffect & AudioAffect_Sound3D) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemSound3DVolume = volume;
		} else {
			m_scriptSound3DVolume = volume;
		}

		const Real newVolume = m_scriptSound3DVolume * m_systemSound3DVolume;
		if (m_sound3DVolume != newVolume)
		{
			m_sound3DVolume = newVolume;
			m_volumeHasChanged = true;
			AUDIO_LOG("VOL", "cat=sound3d total=%.4f system=%d", newVolume, (whichToAffect & AudioAffect_SystemSetting) ? 1 : 0);
		}
	}

	if (whichToAffect & AudioAffect_Speech) {
		if (whichToAffect & AudioAffect_SystemSetting) {
			m_systemSpeechVolume = volume;
		} else {
			m_scriptSpeechVolume = volume;
		}

		const Real newVolume = m_scriptSpeechVolume * m_systemSpeechVolume;
		if (m_speechVolume != newVolume)
		{
			m_speechVolume = newVolume;
			m_volumeHasChanged = true;
			AUDIO_LOG("VOL", "cat=speech total=%.4f system=%d", newVolume, (whichToAffect & AudioAffect_SystemSetting) ? 1 : 0);
		}
	}
}

//-------------------------------------------------------------------------------------------------
Real AudioManager::getVolume( AudioAffect whichToGet )
{
	if (whichToGet & AudioAffect_Music) {
		return m_musicVolume;
	} else if (whichToGet & AudioAffect_Sound) {
		return m_soundVolume;
	} else if (whichToGet & AudioAffect_Sound3D) {
		return m_sound3DVolume;
	}

	// Speech
	return m_speechVolume;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::set3DVolumeAdjustment( Real volumeAdjustment )
{
	const Real newVolume = clamp(0.0f, volumeAdjustment * m_scriptSound3DVolume * m_systemSound3DVolume, 1.0f);

	if (m_sound3DVolume != newVolume)
	{
		m_sound3DVolume = newVolume;
		m_volumeHasChanged = true;
	}
}

//-------------------------------------------------------------------------------------------------
void AudioManager::setListenerPosition( const Coord3D *newListenerPos, const Coord3D *newListenerOrientation )
{
	m_listenerPosition = *newListenerPos;
	m_listenerOrientation = *newListenerOrientation;
}

//-------------------------------------------------------------------------------------------------
const Coord3D *AudioManager::getListenerPosition() const
{
	return &m_listenerPosition;
}

//-------------------------------------------------------------------------------------------------
AudioRequest *AudioManager::allocateAudioRequest()
{
	AudioRequest *audioReq = newInstance(AudioRequest);
	audioReq->m_requiresCheckForSample = false;
	return audioReq;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::releaseAudioRequest( AudioRequest *requestToRelease )
{
	deleteInstance(requestToRelease);
}

//-------------------------------------------------------------------------------------------------
void AudioManager::appendAudioRequest( AudioRequest *request )
{
	m_audioRequests.push_back(request);
}

//-------------------------------------------------------------------------------------------------
// Remove all pending audio requests
void AudioManager::removeAllAudioRequests()
{
	std::list<AudioRequest*>::iterator it;
	for ( it = m_audioRequests.begin(); it != m_audioRequests.end(); ++it ) {
		releaseAudioRequest( *it );
	}
	m_audioRequests.clear();
}

//-------------------------------------------------------------------------------------------------
void AudioManager::processRequestList()
{

}

//-------------------------------------------------------------------------------------------------
AudioEventInfo *AudioManager::newAudioEventInfo( AsciiString audioName )
{
	AudioEventInfo *eventInfo = findAudioEventInfo(audioName);
	if (eventInfo) {
		DEBUG_CRASH(("Requested add of '%s' multiple times. Is this intentional? - jkmcd", audioName.str()));
		return eventInfo;
	}

	m_allAudioEventInfo[audioName] = newInstance(AudioEventInfo);
	return m_allAudioEventInfo[audioName];
}

//-------------------------------------------------------------------------------------------------
// Add an AudioEventInfo structure allocated elsewhere to the audio event list
void AudioManager::addAudioEventInfo( AudioEventInfo * newEvent )
{
  // Warning: Don't try to copy the structure. It may be a derived class
  AudioEventInfo *eventInfo = findAudioEventInfo( newEvent->m_audioName );
  if (eventInfo)
  {
    DEBUG_CRASH(("Requested add of '%s' multiple times. Is this intentional? - jkmcd", newEvent->m_audioName.str()));
    *eventInfo = *newEvent;
  }
  else
  {
    m_allAudioEventInfo[newEvent->m_audioName] = newEvent;
  }
}

//-------------------------------------------------------------------------------------------------
AudioEventInfo *AudioManager::findAudioEventInfo( AsciiString eventName ) const
{
	AudioEventInfoHash::const_iterator it;
	it = m_allAudioEventInfo.find(eventName);
	if (it == m_allAudioEventInfo.end()) {
		return nullptr;
	}

	return (*it).second;
}

//-------------------------------------------------------------------------------------------------
// Remove all AudioEventInfo's with the m_isLevelSpecific flag
void AudioManager::removeLevelSpecificAudioEventInfos()
{
  AudioEventInfoHash::iterator it = m_allAudioEventInfo.begin();

  while ( it != m_allAudioEventInfo.end() )
  {
    AudioEventInfoHash::iterator next = it; // Make sure erase doesn't cause problems
    next++;

    if ( it->second->isLevelSpecific() )
    {
      deleteInstance(it->second);
      m_allAudioEventInfo.erase( it );
    }

    it = next;
  }

}

//-------------------------------------------------------------------------------------------------
const AudioSettings *AudioManager::getAudioSettings() const
{
	return m_audioSettings;
}

//-------------------------------------------------------------------------------------------------
AudioSettings *AudioManager::friend_getAudioSettings()
{
	return m_audioSettings;
}

//-------------------------------------------------------------------------------------------------
const MiscAudio *AudioManager::getMiscAudio() const
{
	return m_miscAudio;
}

//-------------------------------------------------------------------------------------------------
MiscAudio *AudioManager::friend_getMiscAudio()
{
	return m_miscAudio;
}

//-------------------------------------------------------------------------------------------------
const FieldParse *AudioManager::getFieldParseTable() const
{
	return audioSettingsFieldParseTable;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::refreshCachedVariables()
{
	m_hardwareAccel = isCurrentProviderHardwareAccelerated();
	m_surroundSpeakers = isCurrentSpeakerTypeSurroundSound();
}

//-------------------------------------------------------------------------------------------------
Real AudioManager::getAudioLengthMS( const AudioEventRTS *event )
{
	if (!event->getAudioEventInfo()) {
		getInfoForAudioEvent(event);
		if (!event->getAudioEventInfo()) {
			return 0.0f;
		}
	}

	AudioEventRTS tmpEvent = *event;

	tmpEvent.generateFilename();
	tmpEvent.generatePlayInfo();
	const Real totalMS = getFileLengthMS(tmpEvent.getAttackFilename()) +
				 getFileLengthMS(tmpEvent.getFilename()) +
				 getFileLengthMS(tmpEvent.getDecayFilename());
	AUDIO_LOG("LENEV", "ev=%s total_ms=%.1f", event->getEventName().str(), totalMS);
	return totalMS;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::findAllAudioEventsOfType( AudioType audioType, std::vector<AudioEventInfo*>& allEvents )
{
	AudioEventInfoHashIt it;
	for (it = m_allAudioEventInfo.begin(); it != m_allAudioEventInfo.end(); ++it) {
		AudioEventInfo *aud = (*it).second;
		if (aud->m_soundType == audioType) {
			allEvents.push_back(aud);
		}
	}
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isCurrentProviderHardwareAccelerated()
{
	for (Int i = 0; i < MAX_HW_PROVIDERS; ++i) {
		if (getProviderName(getSelectedProvider()) == m_audioSettings->m_preferred3DProvider[i]) {
			return TRUE;
		}
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::isCurrentSpeakerTypeSurroundSound()
{
	return (getSpeakerType() == m_audioSettings->m_defaultSpeakerType3D);
}

//-------------------------------------------------------------------------------------------------
Bool AudioManager::shouldPlayLocally(const AudioEventRTS *audioEvent)
{
	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if( !localPlayer->isPlayerActive() )
	{
		//We are dead, thus are observing. Get the player we are observing. It's
		//possible that we're not looking at any player, therefore it can be null.
		localPlayer = TheControlBar->getObserverLookAtPlayer();
	}

	const AudioEventInfo *ei = audioEvent->getAudioEventInfo();

	// Music should always play locally.
	if (ei->m_soundType == AT_Music) {
		return TRUE;
	}

	if (!BitIsSet(ei->m_type, (ST_PLAYER | ST_ALLIES | ST_ENEMIES | ST_EVERYONE))) {
		DEBUG_CRASH(("No player restrictions specified for '%s'. Using Everyone.", ei->m_audioName.str()));
		return TRUE;
	}

	if (BitIsSet(ei->m_type, ST_EVERYONE)) {
		return TRUE;
	}

	Player *owningPlayer = ThePlayerList->getNthPlayer(audioEvent->getPlayerIndex());

	if (BitIsSet(ei->m_type, ST_PLAYER) && BitIsSet(ei->m_type, ST_UI) && owningPlayer == nullptr) {
		DEBUG_ASSERTCRASH(!TheGameLogic->isInGameLogicUpdate(), ("Playing %s sound -- player-based UI sound without specifying a player.", ei->m_audioName.str()));
		return TRUE;
	}

	if (owningPlayer == nullptr) {
		DEBUG_CRASH(("Sound '%s' expects an owning player, but the audio event that created it didn't specify one.", ei->m_audioName.str()));
		return FALSE;
	}

	if( !localPlayer )
	{
		return FALSE;
	}

	const Team *localTeam = localPlayer->getDefaultTeam();
	if (localTeam == nullptr) {
		return FALSE;
	}

	if (BitIsSet(ei->m_type, ST_PLAYER))  {
		return owningPlayer == localPlayer;
	}

	if (BitIsSet(ei->m_type, ST_ALLIES)) {
		// We have to also check that the owning player isn't the local player, because PLAYER
		// wasn't specified, or we wouldn't have gotten here.
		return (owningPlayer != localPlayer) && owningPlayer->getRelationship(localTeam) == ALLIES;
	}

	if (BitIsSet(ei->m_type, ST_ENEMIES)) {
		return owningPlayer->getRelationship(localTeam) == ENEMIES;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
AudioHandle AudioManager::allocateNewHandle()
{
	// note, intentionally a post increment rather than a pre increment.
	return theAudioHandlePool++;
}

//-------------------------------------------------------------------------------------------------
void AudioManager::muteAudio( MuteAudioReason reason )
{
	m_muteReasonBits |= 1u << reason;

	AUDIO_LOG("MUTE", "reason=%s bits=%u", MuteAudioReasonNames[reason], m_muteReasonBits);
	DEBUG_LOG(("AudioManager::muteAudio(%s): m_muteReason=%u muted=%d",
		MuteAudioReasonNames[reason], m_muteReasonBits, (int)(m_muteReasonBits != 0)));

	if (m_muteReasonBits == 0 || m_savedValues)
		return;

	// Make all the audio go silent.
	m_savedValues = NEW Real[NUM_VOLUME_TYPES];
	m_savedValues[VOLUME_TYPE_MUSIC] = m_systemMusicVolume;
	m_savedValues[VOLUME_TYPE_SOUND] = m_systemSoundVolume;
	m_savedValues[VOLUME_TYPE_SOUND3D] = m_systemSound3DVolume;
	m_savedValues[VOLUME_TYPE_SPEECH] = m_systemSpeechVolume;

	// Now, set them all to 0.
	setVolume(0.0f, (AudioAffect) (AudioAffect_All | AudioAffect_SystemSetting));
}

//-------------------------------------------------------------------------------------------------
void AudioManager::unmuteAudio( MuteAudioReason reason )
{
	m_muteReasonBits &= ~(1u << reason);

	AUDIO_LOG("UNMUTE", "reason=%s bits=%u", MuteAudioReasonNames[reason], m_muteReasonBits);
	DEBUG_LOG(("AudioManager::unmuteAudio(%s): m_muteReason=%u muted=%d",
		MuteAudioReasonNames[reason], m_muteReasonBits, (int)(m_muteReasonBits != 0)));

	if (m_muteReasonBits != 0 || !m_savedValues)
		return;

	// Restore the previous audio values.
	setVolume(m_savedValues[VOLUME_TYPE_MUSIC], (AudioAffect) (AudioAffect_Music | AudioAffect_SystemSetting));
	setVolume(m_savedValues[VOLUME_TYPE_SOUND], (AudioAffect) (AudioAffect_Sound | AudioAffect_SystemSetting));
	setVolume(m_savedValues[VOLUME_TYPE_SOUND3D], (AudioAffect) (AudioAffect_Sound3D | AudioAffect_SystemSetting));
	setVolume(m_savedValues[VOLUME_TYPE_SPEECH], (AudioAffect) (AudioAffect_Speech | AudioAffect_SystemSetting));

	// Now, blow away the old volumes.
	delete [] m_savedValues;
	m_savedValues = nullptr;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void INI::parseAudioSettingsDefinition( INI *ini )
{
	ini->initFromINI(TheAudio->friend_getAudioSettings(), TheAudio->getFieldParseTable());

	// time to override the volume settings, default 3-D provider, and speaker setup with the
	// ones stored from the prefs
	OptionPreferences prefs;

	TheAudio->setPreferredProvider(prefs.getPreferred3DProvider());
	TheAudio->setPreferredSpeaker(prefs.getSpeakerType());

	Real relative2DVolume = TheAudio->getAudioSettings()->m_relative2DVolume;
	relative2DVolume = MIN( 1.0f, MAX( -1.0f, relative2DVolume ) );

	TheAudio->friend_getAudioSettings()->m_preferredSoundVolume		= prefs.getSoundVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferred3DSoundVolume	= prefs.get3DSoundVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferredSpeechVolume	= prefs.getSpeechVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferredMusicVolume		= prefs.getMusicVolume() / 100.0f;
	TheAudio->friend_getAudioSettings()->m_preferredMoneyTransactionVolume = prefs.getMoneyTransactionVolume() / 100.0f;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
void parseSpeakerType( INI *ini, void *instance, void *store, const void* userData )
{
	AsciiString str;
	ini->parseAsciiString( ini, instance, &str, userData );

	(*(UnsignedInt*)store) = TheAudio->translateSpeakerTypeToUnsignedInt(str);
}

