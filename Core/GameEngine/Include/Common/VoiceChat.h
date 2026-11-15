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

// VoiceChat.h //////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature Team voice chat in a match (items 19 and 20).
//
// Rules (computed on the game thread from the game's slots each update, enforced by the voice thread):
//   - a player talks only while the push-to-talk key is held (VoicePushToTalk: CapsLock (default), Mouse4, Mouse5, Grave,
//     RightCtrl; the key is read directly, so Space and every game key keep their functions);
//   - voice goes only to the human team-mates' computers (same team number, not an observer slot): an enemy's computer
//     never receives it; a player without a team has no team-mates;
//   - observers (spectator slots) and defeated players never send; an observer slot receives nothing;
//   - the receiving side checks again: a packet is played only from a team-mate's slot, from that slot's address, and
//     not from an observer or a defeated player.
// Per team-mate volume and mute (the Tab list), the master volume of incoming voice (VoiceVolume), the own microphone off
// (VoiceMicOff) and the microphone device (VoiceInputDevice) are in Options.ini; the settings have rows for them (Sound).
//
// Voice is its own small UDP channel beside the game connection, never inside the lockstep stream. The voice thread
// (below normal priority) captures, encodes, sends, receives, buffers, decodes and mixes; the game thread only hands it
// the rules and reads who is speaking. Mixing into the sound output happens in the audio device's callback (miniaudio).
//
// Codec (VoiceCodec): IMA ADPCM, 8 kHz mono, 4 bits per sample, 40 ms frames (320 samples): 160 bytes of voice and a
// 12 byte header per packet, 25 packets per second while talking: 32 kbit/s of voice, about 40 kbit/s with UDP/IP, per
// receiving team-mate. Each packet carries its own decoder state, so a lost packet costs only its 40 ms. The receiver
// keeps a jitter buffer of 3 frames (120 ms) per sender, conceals a lost frame by repeating the last one at half level
// (twice, then silence) and never asks for a retransmission. Another codec can be put behind VoiceCodec later.
//
// Test path on one PC (silent, loopback): -voiceTest <slot> with -voiceTestRoster "0:1,1:1,2:2" (slot:team[:o observer]
// [:d defeated]) runs a voice session without a match on 127.0.0.1:(47300 + slot); the input is -voiceTestTone (a
// pattern of tones) or -voiceTestWav <file>, the talk key is held by -voiceTestTalk <start>,<seconds>, the mixed incoming
// voice is written to -voiceTestOut <file.wav> (8 kHz 16 bit), -voiceTestLoss <percent> drops incoming packets,
// -voiceTestVolume <slot>:<percent> sets a sender's volume (0 mutes). Every voice session writes VoiceChat.log.
// No real microphone is opened in a test session; the microphone is opened only in a network match.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

#include <vector>

namespace VoiceChat
{
	enum { MAX_PEERS = 8, SAMPLE_RATE = 8000, FRAME_SAMPLES = 320 };

	//--- command line (test) -----------------------------------------------------------------------
	void configureTest(Int slot);
	void configureTestRoster(const char *roster);
	void configureTestTone();
	void configureTestWav(const char *path);
	void configureTestTalk(const char *startAndSeconds);
	void configureTestOut(const char *path);
	void configureTestLoss(Int percent);
	void configureTestVolume(const char *slotAndPercent);

	//--- game thread -------------------------------------------------------------------------------
	/// Every client update: reads the settings and the slots, the talk key, starts or stops the voice session.
	void update();
	/// After the options were saved.
	void reloadSettings();
	Bool isActive();
	/// The slots whose voice this computer may receive (a row in the Tab list gets a volume button).
	Bool isVoiceMate(Int slot);
	Bool isSpeaking(Int slot);
	Bool isLocalTalking();     ///< the talk key is held and voice is being sent
	Bool isLocalBlocked();     ///< the talk key is held but this player may not talk (observer, defeated, microphone off)
	Int volumeOf(Int slot);    ///< 0..100; 0 = muted
	void setVolumeOf(Int slot, Int percent);
	UnicodeString nameOf(Int slot);

	//--- audio device ------------------------------------------------------------------------------
	/// From the sound output callback: adds the incoming voice (float samples, interleaved).
	void mixOutput(float *out, UnsignedInt frames, UnsignedInt channels, UnsignedInt sampleRate);

	typedef Bool (*CaptureOpen)(const char *deviceName);   ///< opens the capture at 8 kHz mono 16 bit; it calls pushCapture
	typedef void (*CaptureClose)();
	typedef void (*InputLister)(std::vector<AsciiString> &names);
	void setCapture(CaptureOpen open, CaptureClose close, InputLister lister);
	/// From the capture callback: 8 kHz mono samples.
	void pushCapture(const Short *samples, UnsignedInt count);
	/// The microphone names for the settings (only enumerated, none is opened).
	void listInputDevices(std::vector<AsciiString> &names);

	void shutdown();
}

/// The speech codec behind the voice chat: one frame of FRAME_SAMPLES samples in, one packet payload out.
class VoiceCodec
{
public:
	virtual ~VoiceCodec() {}
	virtual const char *name() const = 0;
	virtual Int bitsPerSecond() const = 0;
	virtual Int maxPayload() const = 0;
	/// Encodes one frame; returns the payload length.
	virtual Int encode(const Short *pcm, UnsignedByte *out) = 0;
	/// Decodes one payload into one frame; FALSE when it is not a valid payload.
	virtual Bool decode(const UnsignedByte *in, Int length, Short *pcm) = 0;
};
