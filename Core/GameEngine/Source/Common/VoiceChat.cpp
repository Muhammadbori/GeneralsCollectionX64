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

// VoiceChat.cpp ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See VoiceChat.h.
//
// Threads: the game thread (update, the rules, the talk key, the settings), the voice thread (everything with sound and
// sockets) and the audio device's threads (mixOutput, pushCapture). The game thread hands the voice thread a Roster under
// a lock; the voice thread hands back who is speaking as single words; the audio threads meet the voice thread only in
// two single-producer single-consumer rings. The voice thread uses fixed buffers and Win32 calls only, and its own random
// numbers (the loss test): the game's logic random generator is never touched.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "GameClient/DefeatedPlayer.h"

#include <winsock.h>

#include "Common/VoiceChat.h"
#include "GameNetwork/NetLab.h"

#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/LanRoom.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdarg.h>

extern HWND ApplicationHWnd;

namespace
{
	using namespace VoiceChat;

	enum
	{
		TEST_PORT_BASE = 47300,       ///< loopback test: slot N listens on 127.0.0.1:(47300 + N)
		REAL_PORT = 8094,             ///< a network match: every computer listens on this UDP port (UNSEEN tonight)
		FRAME_MS = 40,
		HEADER = 12,
		JITTER_SLOTS = 16,
		JITTER_START = 3,             ///< frames buffered before a sender's voice starts (120 ms)
		JITTER_MAX_AHEAD = 10,        ///< further ahead than this: skip forward
		SILENCE_RESET_MS = 400,       ///< no packet and nothing buffered for this long: the sender stopped talking
		SPEAKING_HOLD_MS = 300,
		RING = 16384,                 ///< samples (power of two) in the capture and the output rings
		MAX_PAYLOAD = 200,
	};

	//=============================================================================================
	// IMA ADPCM codec
	//=============================================================================================
	const Int kStepTable[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
		130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
		1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
		7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
	const Int kIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

	Int clampInt(Int v, Int lo, Int hi) { return v < lo ? lo : (v > hi ? hi : v); }

	Int decodeNibble(Int nibble, Int &predictor, Int &index)
	{
		const Int step = kStepTable[index];
		Int diff = step >> 3;
		if (nibble & 4) diff += step;
		if (nibble & 2) diff += step >> 1;
		if (nibble & 1) diff += step >> 2;
		predictor += (nibble & 8) ? -diff : diff;
		predictor = clampInt(predictor, -32768, 32767);
		index = clampInt(index + kIndexTable[nibble], 0, 88);
		return predictor;
	}

	class ImaAdpcmCodec : public VoiceCodec
	{
	public:
		ImaAdpcmCodec() : m_predictor(0), m_index(0) {}
		virtual const char *name() const override { return "IMA ADPCM 8 kHz 4 bit"; }
		virtual Int bitsPerSecond() const override { return SAMPLE_RATE * 4; }
		virtual Int maxPayload() const override { return 4 + FRAME_SAMPLES / 2; }
		/// payload: predictor (16 bit), step index, 0, then two samples per byte (low nibble first)
		virtual Int encode(const Short *pcm, UnsignedByte *out) override
		{
			out[0] = (UnsignedByte)(m_predictor & 0xFF);
			out[1] = (UnsignedByte)((m_predictor >> 8) & 0xFF);
			out[2] = (UnsignedByte)m_index;
			out[3] = 0;
			for (Int i = 0; i < FRAME_SAMPLES; i += 2)
			{
				const Int a = encodeSample(pcm[i]);
				const Int b = encodeSample(pcm[i + 1]);
				out[4 + i / 2] = (UnsignedByte)(a | (b << 4));
			}
			return 4 + FRAME_SAMPLES / 2;
		}
		virtual Bool decode(const UnsignedByte *in, Int length, Short *pcm) override
		{
			if (length != 4 + FRAME_SAMPLES / 2 || in[2] > 88) {
				return FALSE;
			}
			Int predictor = (Short)(in[0] | (in[1] << 8));
			Int index = in[2];
			for (Int i = 0; i < FRAME_SAMPLES; i += 2)
			{
				const Int byte = in[4 + i / 2];
				pcm[i] = (Short)decodeNibble(byte & 15, predictor, index);
				pcm[i + 1] = (Short)decodeNibble(byte >> 4, predictor, index);
			}
			return TRUE;
		}
	private:
		Int encodeSample(Int sample)
		{
			const Int step = kStepTable[m_index];
			Int diff = sample - m_predictor;
			Int nibble = 0;
			if (diff < 0) { nibble = 8; diff = -diff; }
			if (diff >= step) { nibble |= 4; diff -= step; }
			if (diff >= step >> 1) { nibble |= 2; diff -= step >> 1; }
			if (diff >= step >> 2) { nibble |= 1; }
			decodeNibble(nibble, m_predictor, m_index);   // the encoder follows the decoder
			return nibble;
		}
		Int m_predictor;
		Int m_index;
	};

	//=============================================================================================
	// Shared state
	//=============================================================================================
	struct Peer
	{
		Bool present;
		Bool sendTo;        ///< a team-mate this computer sends to
		Bool acceptFrom;    ///< a team-mate this computer plays
		u_long ip;          ///< network order
		unsigned short port;
		Int volume;         ///< 0..100
	};

	struct Roster
	{
		Bool active;
		Bool real;          ///< a network match (the real addresses); else the loopback test
		u_long localIp;     ///< network order: this computer's address in the match (the voice socket binds to it)
		Int localSlot;
		Bool mayTalk;
		Bool talkKey;
		Int masterVolume;
		Peer peers[MAX_PEERS];
	};

	CRITICAL_SECTION s_lock;
	Bool s_lockReady = FALSE;
	Roster s_roster;                         ///< written by the game thread, copied by the voice thread (under s_lock)
	volatile LONG s_speakingMask = 0;        ///< written by the voice thread
	volatile LONG s_sendingNow = 0;

	// game thread state
	Roster s_wanted;
	Int s_volumes[MAX_PEERS] = { 100, 100, 100, 100, 100, 100, 100, 100 };
	Int s_masterVolume = 100;
	Bool s_micOff = FALSE;
	Int s_talkVk = VK_CAPITAL;
	AsciiString s_inputDevice;
	Bool s_settingsRead = FALSE;
	Bool s_blocked = FALSE;
	UnicodeString s_names[MAX_PEERS];

	// test configuration
	Int s_testSlot = -1;
	struct TestMember { Bool present; Int team; Bool observer; Bool defeated; };
	TestMember s_testRoster[MAX_PEERS];
	Bool s_testTone = FALSE;
	char s_testWav[_MAX_PATH] = { 0 };
	Int s_testTalkStartMs = -1, s_testTalkLengthMs = 0;
	char s_testOut[_MAX_PATH] = { 0 };
	Int s_testLoss = 0;
	Int s_testVolume[MAX_PEERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	DWORD s_sessionStartMs = 0;

	// thread
	HANDLE s_thread = nullptr;
	volatile LONG s_stop = 0;

	// capture glue
	CaptureOpen s_captureOpen = nullptr;
	CaptureClose s_captureClose = nullptr;
	InputLister s_inputLister = nullptr;
	Bool s_captureIsOpen = FALSE;

	//--- single-producer single-consumer rings ----------------------------------------------------
	struct Ring
	{
		Short data[RING];
		volatile LONG writeAt;
		volatile LONG readAt;
		UnsignedInt available() const { return (UnsignedInt)(writeAt - readAt); }
		/// producer: drops what does not fit
		void push(const Short *samples, UnsignedInt count)
		{
			const LONG r = readAt;
			LONG w = writeAt;
			for (UnsignedInt i = 0; i < count && (UnsignedInt)(w - r) < RING; ++i) {
				data[w & (RING - 1)] = samples[i];
				++w;
			}
			InterlockedExchange(&writeAt, w);
		}
		/// consumer
		UnsignedInt pop(Short *out, UnsignedInt count)
		{
			const LONG w = writeAt;
			LONG r = readAt;
			UnsignedInt n = 0;
			while (n < count && r != w) {
				out[n++] = data[r & (RING - 1)];
				++r;
			}
			InterlockedExchange(&readAt, r);
			return n;
		}
		void clear() { InterlockedExchange(&readAt, writeAt); }
	};
	Ring s_captureRing;
	Ring s_outputRing;
	double s_outputPhase = 0.0;      ///< audio thread: the position between two voice samples
	Short s_outputPrev = 0, s_outputNext = 0;
	Bool s_outputHaveNext = FALSE;

	//--- log ----------------------------------------------------------------------------------------
	FILE *s_log = nullptr;
	CRITICAL_SECTION s_logLock;   ///< initialised with the log file, before the voice thread starts
	void logLine(const char *format, ...)
	{
		if (s_log == nullptr) {
			return;
		}
		EnterCriticalSection(&s_logLock);
		fprintf(s_log, "[%8.3f] ", (timeGetTime() - s_sessionStartMs) / 1000.0);
		va_list args;
		va_start(args, format);
		vfprintf(s_log, format, args);
		va_end(args);
		fputc('\n', s_log);
		fflush(s_log);
		LeaveCriticalSection(&s_logLock);
	}

	//=============================================================================================
	// Voice thread
	//=============================================================================================
	struct JitterFrame
	{
		Bool valid;
		UnsignedShort seq;
		Int length;
		UnsignedByte payload[MAX_PAYLOAD];
	};

	struct Sender
	{
		JitterFrame frames[JITTER_SLOTS];
		Bool started;
		UnsignedShort next;
		Short last[FRAME_SAMPLES];
		Int misses;
		DWORD lastPacketMs;
		DWORD lastSpeakMs;
		Bool haveSeq;
		UnsignedShort highestSeq;
		// statistics
		UnsignedInt received, played, concealed, late, lost, rejected;
	};

	struct VoiceThread
	{
		SOCKET socket;
		ImaAdpcmCodec encoder;
		ImaAdpcmCodec decoder;
		Sender senders[MAX_PEERS];
		UnsignedShort sendSeq;
		UnsignedInt sentPackets[MAX_PEERS];
		UnsignedInt rejectedUnknown, rejectedNotMate, rejectedAddress, rejectedBroken, droppedByTest;
		UnsignedInt lossState;
		UnsignedInt framesSent;
		// input
		UnsignedInt toneSample;
		Short *wav;
		UnsignedInt wavSamples, wavAt;
		// test output
		FILE *outFile;
		UnsignedInt outSamples;
	};
	VoiceThread *s_vt = nullptr;

	Short sat(Int v) { return (Short)clampInt(v, -32768, 32767); }

	/// The test tone pattern: 200 ms segments of 400, 600 ... 1800 Hz and one silent segment, repeated (dx12-tools\voicecmp.py
	/// makes the same).
	void makeTone(UnsignedInt &at, Short *out)
	{
		static const Int kFreq[9] = { 400, 600, 800, 1000, 1200, 1400, 1600, 1800, 0 };
		for (Int i = 0; i < FRAME_SAMPLES; ++i, ++at)
		{
			const Int segment = (Int)((at / 1600) % 9);
			const Int f = kFreq[segment];
			out[i] = f == 0 ? 0 : (Short)(12000.0 * sin(6.283185307179586 * f * (double)(at % 1600) / (double)SAMPLE_RATE));
		}
	}

	/// A 16 bit PCM WAV file, mono or several channels, any rate: 8 kHz mono (linear interpolation).
	Bool loadWav(const char *path, Short **samples, UnsignedInt *count)
	{
		FILE *fp = fopen(path, "rb");
		if (fp == nullptr) return FALSE;
		unsigned char head[12];
		if (fread(head, 1, 12, fp) != 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) { fclose(fp); return FALSE; }
		Int channels = 0, rate = 0, bits = 0;
		unsigned char chunk[8];
		while (fread(chunk, 1, 8, fp) == 8)
		{
			const UnsignedInt size = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | ((UnsignedInt)chunk[7] << 24);
			if (memcmp(chunk, "fmt ", 4) == 0)
			{
				unsigned char fmt[16];
				if (size < 16 || fread(fmt, 1, 16, fp) != 16) break;
				channels = fmt[2] | (fmt[3] << 8);
				rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
				bits = fmt[14] | (fmt[15] << 8);
				fseek(fp, (long)(size - 16 + (size & 1)), SEEK_CUR);
			}
			else if (memcmp(chunk, "data", 4) == 0 && channels > 0 && rate > 0 && bits == 16)
			{
				const UnsignedInt frames = size / (2 * channels);
				Short *raw = (Short *)HeapAlloc(GetProcessHeap(), 0, (size_t)frames * channels * 2 + 2);
				if (raw == nullptr) break;
				const size_t got = fread(raw, 2 * channels, frames, fp);
				const UnsignedInt outCount = (UnsignedInt)((double)got * (double)SAMPLE_RATE / rate);
				Short *out = (Short *)HeapAlloc(GetProcessHeap(), 0, (size_t)outCount * 2 + 2);
				if (out == nullptr) { HeapFree(GetProcessHeap(), 0, raw); break; }
				for (UnsignedInt i = 0; i < outCount; ++i)
				{
					const double pos = (double)i * rate / (double)SAMPLE_RATE;
					const UnsignedInt a = (UnsignedInt)pos;
					const UnsignedInt b = a + 1 < got ? a + 1 : a;
					const double t = pos - a;
					Int sa = 0, sb = 0;
					for (Int c = 0; c < channels; ++c) { sa += raw[a * channels + c]; sb += raw[b * channels + c]; }
					out[i] = sat((Int)((sa * (1.0 - t) + sb * t) / channels));
				}
				HeapFree(GetProcessHeap(), 0, raw);
				fclose(fp);
				*samples = out;
				*count = outCount;
				return outCount > 0;
			}
			else
			{
				fseek(fp, (long)(size + (size & 1)), SEEK_CUR);
			}
		}
		fclose(fp);
		return FALSE;
	}

	void writeWavHeader(FILE *fp, UnsignedInt samples)
	{
		unsigned char h[44];
		const UnsignedInt dataBytes = samples * 2;
		memcpy(h, "RIFF", 4);
		const UnsignedInt riff = 36 + dataBytes;
		h[4] = (unsigned char)riff; h[5] = (unsigned char)(riff >> 8); h[6] = (unsigned char)(riff >> 16); h[7] = (unsigned char)(riff >> 24);
		memcpy(h + 8, "WAVEfmt ", 8);
		h[16] = 16; h[17] = h[18] = h[19] = 0;
		h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
		h[24] = (unsigned char)(SAMPLE_RATE & 0xFF); h[25] = (unsigned char)(SAMPLE_RATE >> 8); h[26] = h[27] = 0;
		const UnsignedInt byteRate = SAMPLE_RATE * 2;
		h[28] = (unsigned char)byteRate; h[29] = (unsigned char)(byteRate >> 8); h[30] = (unsigned char)(byteRate >> 16); h[31] = 0;
		h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
		memcpy(h + 36, "data", 4);
		h[40] = (unsigned char)dataBytes; h[41] = (unsigned char)(dataBytes >> 8); h[42] = (unsigned char)(dataBytes >> 16); h[43] = (unsigned char)(dataBytes >> 24);
		fseek(fp, 0, SEEK_SET);
		fwrite(h, 1, 44, fp);
		fseek(fp, 0, SEEK_END);
	}

	Bool talkingInTest(DWORD now)
	{
		if (s_testTalkStartMs < 0) return FALSE;
		const Int t = (Int)(now - s_sessionStartMs);
		return (Int)(t - s_testTalkStartMs) >= 0 && (Int)(t - s_testTalkStartMs) < (Int)s_testTalkLengthMs;   // O73: signed
	}

	void sendFrame(VoiceThread &vt, const Roster &r, const Short *pcm)
	{
		UnsignedByte packet[HEADER + MAX_PAYLOAD];
		packet[0] = 'G'; packet[1] = 'V'; packet[2] = 1; packet[3] = (UnsignedByte)r.localSlot;
		packet[4] = (UnsignedByte)(vt.sendSeq & 0xFF); packet[5] = (UnsignedByte)(vt.sendSeq >> 8);
		packet[6] = 0; packet[7] = 0;
		const Int length = vt.encoder.encode(pcm, packet + HEADER);
		packet[8] = (UnsignedByte)length; packet[9] = (UnsignedByte)(length >> 8); packet[10] = 0; packet[11] = 0;
		for (Int i = 0; i < MAX_PEERS; ++i)
		{
			const Peer &p = r.peers[i];
			if (!p.present || !p.sendTo || i == r.localSlot) {
				continue;   // rule: only team-mates' computers get the voice
			}
			sockaddr_in to;
			memset(&to, 0, sizeof(to));
			to.sin_family = AF_INET;
			to.sin_addr.s_addr = p.ip;
			to.sin_port = htons(p.port);
			if (sendto(vt.socket, (const char *)packet, HEADER + length, 0, (sockaddr *)&to, sizeof(to)) > 0) {
				++vt.sentPackets[i];
			}
		}
		++vt.sendSeq;
		++vt.framesSent;
	}

	void receivePackets(VoiceThread &vt, const Roster &r, DWORD now)
	{
		UnsignedByte packet[HEADER + MAX_PAYLOAD + 64];
		for (;;)
		{
			sockaddr_in from;
			int fromLen = sizeof(from);
			const int got = recvfrom(vt.socket, (char *)packet, sizeof(packet), 0, (sockaddr *)&from, &fromLen);
			if (got <= 0) {
				return;
			}
			if (got < HEADER || packet[0] != 'G' || packet[1] != 'V' || packet[2] != 1) { ++vt.rejectedBroken; continue; }
			const Int slot = packet[3];
			const Int length = packet[8] | (packet[9] << 8);
			if (slot >= MAX_PEERS || length > MAX_PAYLOAD || HEADER + length != got) { ++vt.rejectedBroken; continue; }
			const Peer &p = r.peers[slot];
			// rule, receiving side: a team-mate's slot, not an observer or a defeated player, from that slot's address
			if (!p.present) { ++vt.rejectedUnknown; continue; }
			if (!p.acceptFrom || slot == r.localSlot) { ++vt.rejectedNotMate; ++vt.senders[slot].rejected; continue; }
			if (from.sin_addr.s_addr != p.ip || ntohs(from.sin_port) != p.port) { ++vt.rejectedAddress; ++vt.senders[slot].rejected; continue; }
			if (s_testLoss > 0)
			{
				vt.lossState = vt.lossState * 1664525u + 1013904223u;   // the test's own numbers, not the game's
				if ((Int)((vt.lossState >> 16) % 100) < s_testLoss) { ++vt.droppedByTest; continue; }
			}
			Sender &snd = vt.senders[slot];
			const UnsignedShort seq = (UnsignedShort)(packet[4] | (packet[5] << 8));
			++snd.received;
			snd.lastPacketMs = now;
			if (snd.started && (Short)(seq - snd.next) < 0) { ++snd.late; continue; }
			if (!snd.haveSeq || (Short)(seq - snd.highestSeq) > 0) { snd.highestSeq = seq; snd.haveSeq = TRUE; }
			JitterFrame &f = snd.frames[seq % JITTER_SLOTS];
			f.valid = TRUE;
			f.seq = seq;
			f.length = length;
			memcpy(f.payload, packet + HEADER, length);
		}
	}

	Int bufferedAhead(const Sender &snd)
	{
		Int n = 0;
		for (Int i = 0; i < JITTER_SLOTS; ++i) {
			if (snd.frames[i].valid) ++n;
		}
		return n;
	}

	/// One frame of a sender: decoded, concealed or nothing. TRUE when something was added to 'mix'.
	Bool playSender(VoiceThread &vt, Int slot, Int volume, Int *mix, DWORD now)
	{
		Sender &snd = vt.senders[slot];
		if (!snd.started)
		{
			if (bufferedAhead(snd) < JITTER_START) {
				return FALSE;
			}
			// start at the oldest buffered frame
			UnsignedShort oldest = snd.highestSeq;
			for (Int i = 0; i < JITTER_SLOTS; ++i) {
				if (snd.frames[i].valid && (Short)(snd.frames[i].seq - oldest) < 0) oldest = snd.frames[i].seq;
			}
			snd.next = oldest;
			snd.started = TRUE;
			snd.misses = 0;
		}
		if ((Short)(snd.highestSeq - snd.next) > JITTER_MAX_AHEAD)
		{
			// far behind the sender (a burst after a stall): skip forward, keep the buffer at its start size
			const UnsignedShort to = (UnsignedShort)(snd.highestSeq - (JITTER_START - 1));
			for (Int i = 0; i < JITTER_SLOTS; ++i) {
				if (snd.frames[i].valid && (Short)(snd.frames[i].seq - to) < 0) snd.frames[i].valid = FALSE;
			}
			snd.lost += (UnsignedShort)(to - snd.next);
			snd.next = to;
		}
		JitterFrame &f = snd.frames[snd.next % JITTER_SLOTS];
		Short pcm[FRAME_SAMPLES];
		Bool have = FALSE;
		if (f.valid && f.seq == snd.next && vt.decoder.decode(f.payload, f.length, pcm))
		{
			have = TRUE;
			f.valid = FALSE;
			memcpy(snd.last, pcm, sizeof(pcm));
			snd.misses = 0;
			++snd.played;
			Int energy = 0;
			for (Int i = 0; i < FRAME_SAMPLES; i += 8) energy += abs(pcm[i]);
			if (energy / (FRAME_SAMPLES / 8) > 300) snd.lastSpeakMs = now;
		}
		else
		{
			if (f.valid && f.seq != snd.next) { /* a newer frame waits in this slot */ }
			if (bufferedAhead(snd) == 0 && now - snd.lastPacketMs > SILENCE_RESET_MS)
			{
				snd.started = FALSE;   // the sender stopped talking: not a loss
				return FALSE;
			}
			if (bufferedAhead(snd) > 0) {
				++snd.lost;   // a later frame is here: this one is missing (else it may be the end of talking)
			}
			if (snd.misses < 2)
			{
				// conceal: the last frame again, at half the level each time
				++snd.misses;
				for (Int i = 0; i < FRAME_SAMPLES; ++i) snd.last[i] = (Short)(snd.last[i] / 2);
				memcpy(pcm, snd.last, sizeof(pcm));
				have = TRUE;
				++snd.concealed;
			}
		}
		++snd.next;
		if (!have) {
			return FALSE;
		}
		for (Int i = 0; i < FRAME_SAMPLES; ++i) {
			mix[i] += pcm[i] * volume / 100;
		}
		return TRUE;
	}

	void logStats(VoiceThread &vt, const char *what)
	{
		logLine("%s: frames sent %u", what, vt.framesSent);
		for (Int i = 0; i < MAX_PEERS; ++i)
		{
			const Sender &s = vt.senders[i];
			if (vt.sentPackets[i] > 0) {
				logLine("  sent to slot %d: %u packets", i, vt.sentPackets[i]);
			}
			if (s.received + s.rejected > 0) {
				logLine("  from slot %d: received %u, played %u, concealed %u, lost %u, late %u, rejected %u", i, s.received, s.played,
					s.concealed, s.lost, s.late, s.rejected);
			}
		}
		logLine("  rejected: unknown slot %u, not a team-mate %u, wrong address %u, broken %u; dropped by -voiceTestLoss %u",
			vt.rejectedUnknown, vt.rejectedNotMate, vt.rejectedAddress, vt.rejectedBroken, vt.droppedByTest);
	}

	DWORD WINAPI voiceMain(LPVOID)
	{
		VoiceThread &vt = *s_vt;
		DWORD nextTick = timeGetTime();
		DWORD lastStatsMs = nextTick;
		Roster r;
		while (InterlockedCompareExchange(&s_stop, 0, 0) == 0)
		{
			DWORD now = timeGetTime();
			if ((LONG)(now - nextTick) < 0)
			{
				Sleep(min((DWORD)5, nextTick - now));
				continue;
			}
			nextTick += FRAME_MS;
			if ((LONG)(now - nextTick) > 200) {
				nextTick = now + FRAME_MS;   // far behind (the PC stalled): do not race to catch up
			}
			EnterCriticalSection(&s_lock);
			r = s_roster;
			LeaveCriticalSection(&s_lock);

			receivePackets(vt, r, now);

			// talk
			const Bool talk = r.active && r.mayTalk && (r.real ? r.talkKey : talkingInTest(now));
			Short input[FRAME_SAMPLES];
			Bool haveInput = FALSE;
			if (r.real)
			{
				if (s_captureRing.available() >= FRAME_SAMPLES)
				{
					s_captureRing.pop(input, FRAME_SAMPLES);
					haveInput = TRUE;
				}
				if (!talk) {
					s_captureRing.clear();
				}
			}
			else if (talk)
			{
				if (vt.wav != nullptr)
				{
					for (Int i = 0; i < FRAME_SAMPLES; ++i) {
						input[i] = vt.wav[vt.wavAt];
						vt.wavAt = (vt.wavAt + 1) % vt.wavSamples;
					}
					haveInput = TRUE;
				}
				else if (s_testTone)
				{
					makeTone(vt.toneSample, input);
					haveInput = TRUE;
				}
			}
			if (talk && haveInput) {
				sendFrame(vt, r, input);
			}
			InterlockedExchange(&s_sendingNow, (talk && haveInput) ? 1 : 0);

			// play
			Int mix[FRAME_SAMPLES];
			memset(mix, 0, sizeof(mix));
			LONG speaking = 0;
			for (Int i = 0; i < MAX_PEERS; ++i)
			{
				const Peer &p = r.peers[i];
				if (!r.active || !p.present || !p.acceptFrom) {
					continue;
				}
				playSender(vt, i, p.volume, mix, now);
				if (p.volume > 0 && now - vt.senders[i].lastSpeakMs < SPEAKING_HOLD_MS && vt.senders[i].lastSpeakMs != 0) {
					speaking |= (1 << i);
				}
			}
			InterlockedExchange(&s_speakingMask, speaking);
			Short out[FRAME_SAMPLES];
			for (Int i = 0; i < FRAME_SAMPLES; ++i) {
				out[i] = sat(mix[i] * r.masterVolume / 100);
			}
			s_outputRing.push(out, FRAME_SAMPLES);
			if (vt.outFile != nullptr)
			{
				fwrite(out, 2, FRAME_SAMPLES, vt.outFile);
				vt.outSamples += FRAME_SAMPLES;
			}
			if (now - lastStatsMs >= 5000)
			{
				lastStatsMs = now;
				logStats(vt, "every 5 s");
			}
		}
		logStats(vt, "at the end");
		return 0;
	}

	//=============================================================================================
	// Session (game thread)
	//=============================================================================================
	Bool s_wsaReady = FALSE;

	Bool startSession(const Roster &r)
	{
		if (!s_wsaReady)
		{
			WSADATA data;
			s_wsaReady = WSAStartup(MAKEWORD(1, 1), &data) == 0;
			if (!s_wsaReady) return FALSE;
		}
		if (!s_lockReady)
		{
			InitializeCriticalSection(&s_lock);
			s_lockReady = TRUE;
		}
		s_sessionStartMs = timeGetTime();
		if (s_log == nullptr && TheGlobalData != nullptr)
		{
			char path[_MAX_PATH];
			snprintf(path, sizeof(path), "%sVoiceChat.log", TheGlobalData->getPath_UserData().str());
			InitializeCriticalSection(&s_logLock);
			s_log = fopen(path, "w");
		}
		s_vt = (VoiceThread *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VoiceThread));
		if (s_vt == nullptr) return FALSE;
		new (&s_vt->encoder) ImaAdpcmCodec();
		new (&s_vt->decoder) ImaAdpcmCodec();
		s_vt->lossState = 12345u;
		s_vt->socket = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		if (r.real)
		{
			// A network match: this computer's own address of the match (its slot's IP), never the wildcard; in a NetLab
			// match on one PC that is a 127.0.0.x address (O19). A LAN address raises the Windows firewall question once.
			addr.sin_addr.s_addr = r.localIp != 0 ? r.localIp : htonl(INADDR_ANY);
			addr.sin_port = htons((unsigned short)REAL_PORT);
		}
		else
		{
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = htons((unsigned short)(TEST_PORT_BASE + r.localSlot));
		}
		if (s_vt->socket == INVALID_SOCKET || bind(s_vt->socket, (sockaddr *)&addr, sizeof(addr)) != 0)
		{
			logLine("cannot open the voice socket (%d)", WSAGetLastError());
			if (s_vt->socket != INVALID_SOCKET) closesocket(s_vt->socket);
			HeapFree(GetProcessHeap(), 0, s_vt);
			s_vt = nullptr;
			return FALSE;
		}
		u_long on = 1;
		ioctlsocket(s_vt->socket, FIONBIO, &on);
		if (s_testWav[0] != '\0' && !r.real)
		{
			if (!loadWav(s_testWav, &s_vt->wav, &s_vt->wavSamples)) {
				logLine("cannot read %s (16 bit PCM WAV)", s_testWav);
			}
		}
		if (s_testOut[0] != '\0' && !r.real)
		{
			s_vt->outFile = fopen(s_testOut, "wb");
			if (s_vt->outFile != nullptr) writeWavHeader(s_vt->outFile, 0);
		}
		ImaAdpcmCodec codec;
		logLine("voice session: %s, slot %d, codec %s (%d bit/s of voice, %d ms frames)", r.real ? "network match (real addresses)" : "loopback test",
			r.localSlot, codec.name(), codec.bitsPerSecond(), FRAME_MS);
		for (Int i = 0; i < MAX_PEERS; ++i)
		{
			const Peer &p = r.peers[i];
			if (p.present) {
				logLine("  slot %d: send %s, accept %s, volume %d", i, p.sendTo ? "yes" : "no", p.acceptFrom ? "yes" : "no", p.volume);
			}
		}
		s_roster = r;
		InterlockedExchange(&s_stop, 0);
		s_thread = CreateThread(nullptr, 0, voiceMain, nullptr, 0, nullptr);
		if (s_thread != nullptr)
		{
			SetThreadPriority(s_thread, THREAD_PRIORITY_ABOVE_NORMAL);   // short work every 40 ms; sound must not stutter
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			if (si.dwNumberOfProcessors > 1) {
				SetThreadIdealProcessor(s_thread, si.dwNumberOfProcessors - 1);
			}
		}
		return s_thread != nullptr;
	}

	void stopSession()
	{
		if (s_thread != nullptr)
		{
			InterlockedExchange(&s_stop, 1);
			WaitForSingleObject(s_thread, 3000);
			CloseHandle(s_thread);
			s_thread = nullptr;
		}
		if (s_captureIsOpen && s_captureClose != nullptr)
		{
			s_captureClose();
			s_captureIsOpen = FALSE;
		}
		if (s_vt != nullptr)
		{
			closesocket(s_vt->socket);
			if (s_vt->outFile != nullptr)
			{
				writeWavHeader(s_vt->outFile, s_vt->outSamples);
				fclose(s_vt->outFile);
			}
			if (s_vt->wav != nullptr) HeapFree(GetProcessHeap(), 0, s_vt->wav);
			HeapFree(GetProcessHeap(), 0, s_vt);
			s_vt = nullptr;
			logLine("voice session ended");
		}
		InterlockedExchange(&s_speakingMask, 0);
		InterlockedExchange(&s_sendingNow, 0);
		s_wanted.active = FALSE;
	}

	AsciiString prefValue(OptionPreferences &p, const char *key, const char *fallback)
	{
		OptionPreferences::const_iterator it = p.find(key);
		return it != p.end() ? it->second : AsciiString(fallback);
	}

	void readSettings()
	{
		OptionPreferences pref;
		const AsciiString key = prefValue(pref, "VoicePushToTalk", "CapsLock");
		if (stricmp(key.str(), "Mouse4") == 0) s_talkVk = VK_XBUTTON1;
		else if (stricmp(key.str(), "Mouse5") == 0) s_talkVk = VK_XBUTTON2;
		else if (stricmp(key.str(), "Grave") == 0) s_talkVk = VK_OEM_3;
		else if (stricmp(key.str(), "RightCtrl") == 0) s_talkVk = VK_RCONTROL;
		else s_talkVk = VK_CAPITAL;
		s_micOff = stricmp(prefValue(pref, "VoiceMicOff", "no").str(), "yes") == 0;
		s_masterVolume = clampInt(atoi(prefValue(pref, "VoiceVolume", "100").str()), 0, 100);
		s_inputDevice = prefValue(pref, "VoiceInputDevice", "");
		s_settingsRead = TRUE;
	}

	Bool talkKeyDown()
	{
		if (ApplicationHWnd == nullptr || GetForegroundWindow() != ApplicationHWnd) {
			return FALSE;   // another program has the keyboard
		}
		return (GetAsyncKeyState(s_talkVk) & 0x8000) != 0;
	}

	/// The rules from the game's slots (a network match).
	Bool buildRealRoster(Roster &r)
	{
		memset(&r, 0, sizeof(r));
		if (TheNetwork == nullptr || TheGameInfo == nullptr || TheGameLogic == nullptr || !TheGameLogic->isInGame()
			|| TheGameLogic->isInShellGame() || TheGameLogic->isInReplayGame() || TheGameLogic->isLoadingMap()) {
			return FALSE;
		}
		const Int local = TheGameInfo->getLocalSlotNum();
		if (local < 0 || local >= MAX_PEERS) {
			return FALSE;
		}
		// O59 (row 47): a defeated player who joined the observers speaks on the observers' roster only
		if (DefeatedPlayer::localJoinedObservers()) {
			return FALSE;
		}
		// O59: in a match of several people a defeated player stays with his team (talks to and hears his team-mates)
		const Bool teamMatch = DefeatedPlayer::teamMatch();
		struct Info { Bool human; Int team; Bool observer; Bool defeated; };
		Info info[MAX_PEERS];
		for (Int i = 0; i < MAX_PEERS; ++i)
		{
			const GameSlot *slot = TheGameInfo->getConstSlot(i);
			info[i].human = slot != nullptr && slot->isHuman();
			info[i].team = slot != nullptr ? slot->getTeamNumber() : -1;
			info[i].observer = slot != nullptr && (slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER || slot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER);
			Player *player = info[i].human ? ThePlayerList->getPlayerFromSlotIndex(i) : nullptr;
			info[i].defeated = player != nullptr && TheVictoryConditions != nullptr && TheVictoryConditions->hasSinglePlayerBeenDefeated(player);
			if (player != nullptr && player->isPlayerObserver()) {
				info[i].observer = TRUE;
			}
			r.peers[i].present = info[i].human;
			r.peers[i].ip = slot != nullptr ? htonl(slot->getIP()) : 0;
			r.peers[i].port = (unsigned short)REAL_PORT;
			s_names[i] = slot != nullptr ? slot->getName() : UnicodeString::TheEmptyString;
		}
		r.real = TRUE;
		r.localIp = r.peers[local].ip;
		r.localSlot = local;
		const Info &me = info[local];
		const Bool meOut = me.defeated && !teamMatch;
		r.mayTalk = me.human && !me.observer && !meOut && !s_micOff;
		s_blocked = me.observer || meOut || s_micOff;
		for (Int i = 0; i < MAX_PEERS; ++i)
		{
			// O59: a player who has left the match (gone to the observers, or quit) is no longer a team-mate here
			const Bool connected = i == local || TheNetwork->isPlayerConnected(i);
			const Bool mate = i != local && info[i].human && !info[i].observer && me.team >= 0 && info[i].team == me.team && connected;
			r.peers[i].sendTo = mate && !me.observer;
			r.peers[i].acceptFrom = mate && (!info[i].defeated || teamMatch) && !me.observer;
			r.peers[i].volume = s_volumes[i];
		}
		r.active = TRUE;
		return TRUE;
	}

	/// O34: a LAN room observer watching its stream: voice among the room's observers only (the host's list at Start,
	/// the players left out), in the same order on every observer (by address); at most MAX_PEERS of them. A player is
	/// never in this roster, so nothing is sent to a player and nothing from a player's address is played; an outside
	/// viewer never gets a roster (no voice at all).
	Bool buildObserverRoster(Roster &r)
	{
		memset(&r, 0, sizeof(r));
		// O59 (row 47): also a defeated player of a live match who joined the room's observers
		if (TheGameLogic == nullptr || (!TheGameLogic->isInReplayGame() && !DefeatedPlayer::localJoinedObservers())) {
			return FALSE;
		}
		UnsignedInt ips[MAX_PEERS];
		UnicodeString names[MAX_PEERS];
		Int local = -1;
		const Int n = LanRoom::observerVoicePeers(ips, names, MAX_PEERS, &local);
		if (n < 2 || local < 0) {
			return FALSE;
		}
		for (Int i = 0; i < n; ++i)
		{
			r.peers[i].present = TRUE;
			r.peers[i].ip = htonl(ips[i]);
			r.peers[i].port = (unsigned short)REAL_PORT;
			r.peers[i].sendTo = i != local;
			r.peers[i].acceptFrom = i != local;
			r.peers[i].volume = s_volumes[i];
			s_names[i] = names[i];
		}
		r.real = TRUE;
		r.localIp = r.peers[local].ip;
		r.localSlot = local;
		r.mayTalk = !s_micOff;
		s_blocked = s_micOff;
		r.active = TRUE;
		return TRUE;
	}

	Bool buildTestRoster(Roster &r)
	{
		memset(&r, 0, sizeof(r));
		const Int local = s_testSlot;
		if (local < 0 || local >= MAX_PEERS || !s_testRoster[local].present) {
			return FALSE;
		}
		const TestMember &me = s_testRoster[local];
		r.localSlot = local;
		// O59 (row 47): the test roster is a match of several people: a defeated member stays with the team
		r.mayTalk = !me.observer && !s_micOff;
		s_blocked = me.observer || s_micOff;
		for (Int i = 0; i < MAX_PEERS; ++i)
		{
			const TestMember &m = s_testRoster[i];
			r.peers[i].present = m.present;
			r.peers[i].ip = htonl(INADDR_LOOPBACK);
			r.peers[i].port = (unsigned short)(TEST_PORT_BASE + i);
			const Bool mate = m.present && i != local && !m.observer && me.team >= 0 && m.team == me.team;
			r.peers[i].sendTo = mate && !me.observer;
			r.peers[i].acceptFrom = mate && !me.observer;   // O59: a defeated team-mate too
			r.peers[i].volume = s_testVolume[i] >= 0 ? s_testVolume[i] : s_volumes[i];
			UnicodeString name;
			name.format(L"Slot %d", i);
			s_names[i] = name;
		}
		r.active = TRUE;
		return TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
void VoiceChat::configureTest(Int slot) { s_testSlot = slot; }

void VoiceChat::configureTestRoster(const char *roster)
{
	memset(s_testRoster, 0, sizeof(s_testRoster));
	const char *p = roster;
	while (p != nullptr && *p != '\0')
	{
		Int slot = -1, team = -1;
		char flags[8] = { 0 };
		const Int n = sscanf(p, "%d:%d:%7[od]", &slot, &team, flags);
		if (n >= 2 && slot >= 0 && slot < MAX_PEERS)
		{
			s_testRoster[slot].present = TRUE;
			s_testRoster[slot].team = team;
			s_testRoster[slot].observer = strchr(flags, 'o') != nullptr;
			s_testRoster[slot].defeated = strchr(flags, 'd') != nullptr;
		}
		p = strchr(p, ',');
		if (p != nullptr) ++p;
	}
}

void VoiceChat::configureTestTone() { s_testTone = TRUE; }
void VoiceChat::configureTestWav(const char *path) { strlcpy(s_testWav, path, sizeof(s_testWav)); }
void VoiceChat::configureTestOut(const char *path) { strlcpy(s_testOut, path, sizeof(s_testOut)); }
void VoiceChat::configureTestLoss(Int percent) { s_testLoss = clampInt(percent, 0, 100); }

void VoiceChat::configureTestTalk(const char *startAndSeconds)
{
	Int start = 0, seconds = 0;
	if (sscanf(startAndSeconds, "%d,%d", &start, &seconds) == 2)
	{
		s_testTalkStartMs = start * 1000;
		s_testTalkLengthMs = seconds * 1000;
	}
}

void VoiceChat::configureTestVolume(const char *slotAndPercent)
{
	Int slot = -1, percent = 100;
	if (sscanf(slotAndPercent, "%d:%d", &slot, &percent) == 2 && slot >= 0 && slot < MAX_PEERS) {
		s_testVolume[slot] = clampInt(percent, 0, 100);
	}
}

void VoiceChat::reloadSettings()
{
	s_settingsRead = FALSE;   // read again at the next update, after the options menu has written Options.ini
}

void VoiceChat::update()
{
	if (!s_settingsRead) {
		readSettings();
	}
	Roster r;
	Bool want = FALSE;
	if (s_testSlot >= 0) {
		want = buildTestRoster(r);
	} else {
		want = buildRealRoster(r) || buildObserverRoster(r);   // O34: a room observer's channel
	}
	if (!want)
	{
		if (s_thread != nullptr || s_vt != nullptr) {
			stopSession();
		}
		s_blocked = FALSE;
		return;
	}
	r.masterVolume = s_masterVolume;
	const Bool key = r.real ? talkKeyDown() : FALSE;
	r.talkKey = key;
	if (!r.mayTalk && key) {
		s_blocked = TRUE;
	} else if (!key && r.real) {
		s_blocked = FALSE;
	}
	if (s_thread == nullptr)
	{
		if (!startSession(r)) {
			s_testSlot = r.real ? s_testSlot : -1;   // a test session that cannot start is not retried every frame
			return;
		}
	}
	// the microphone: opened only in a network match, while this player may talk (never in a test session)
	// (and never in a NetLab test match on one PC, O19: no real microphone in tests)
	// O34: nor on a loopback address (the -lanLoopback tests on one PC)
	const Bool loopbackTest = (ntohl(r.localIp) >> 24) == 127;
	if (r.real && r.mayTalk && !s_captureIsOpen && s_captureOpen != nullptr && !NetLab::isConfigured() && !loopbackTest)
	{
		s_captureIsOpen = s_captureOpen(s_inputDevice.str());
		if (s_captureIsOpen) logLine("microphone opened: %s", s_inputDevice.isEmpty() ? "(system default)" : s_inputDevice.str());
	}
	else if ((!r.real || !r.mayTalk || loopbackTest) && s_captureIsOpen && s_captureClose != nullptr)
	{
		s_captureClose();
		s_captureIsOpen = FALSE;
		logLine("microphone closed");
	}
	EnterCriticalSection(&s_lock);
	s_roster = r;
	LeaveCriticalSection(&s_lock);
	s_wanted = r;
}

Bool VoiceChat::isActive() { return s_thread != nullptr; }

Bool VoiceChat::isVoiceMate(Int slot)
{
	return slot >= 0 && slot < MAX_PEERS && s_wanted.active && s_wanted.peers[slot].present && s_wanted.peers[slot].acceptFrom;
}

Bool VoiceChat::isSpeaking(Int slot)
{
	return slot >= 0 && slot < MAX_PEERS && (InterlockedCompareExchange(&s_speakingMask, 0, 0) & (1 << slot)) != 0;
}

Bool VoiceChat::isLocalTalking() { return InterlockedCompareExchange(&s_sendingNow, 0, 0) != 0; }

Bool VoiceChat::isLocalBlocked()
{
	return s_wanted.active && s_wanted.real && s_blocked && talkKeyDown();
}

Int VoiceChat::volumeOf(Int slot)
{
	return (slot >= 0 && slot < MAX_PEERS) ? (s_testSlot >= 0 && s_testVolume[slot] >= 0 ? s_testVolume[slot] : s_volumes[slot]) : 100;
}

void VoiceChat::setVolumeOf(Int slot, Int percent)
{
	if (slot < 0 || slot >= MAX_PEERS) return;
	s_volumes[slot] = clampInt(percent, 0, 100);
	if (s_testSlot >= 0) s_testVolume[slot] = s_volumes[slot];
	logLine("volume of slot %d set to %d%% (the Tab list)", slot, s_volumes[slot]);
}

UnicodeString VoiceChat::nameOf(Int slot)
{
	return (slot >= 0 && slot < MAX_PEERS) ? s_names[slot] : UnicodeString::TheEmptyString;
}

void VoiceChat::mixOutput(float *out, UnsignedInt frames, UnsignedInt channels, UnsignedInt sampleRate)
{
	if (s_outputRing.available() == 0 && !s_outputHaveNext) {
		return;
	}
	const double step = (double)SAMPLE_RATE / (double)sampleRate;
	for (UnsignedInt f = 0; f < frames; ++f)
	{
		while (s_outputPhase >= 1.0 || !s_outputHaveNext)
		{
			Short sample;
			if (s_outputRing.pop(&sample, 1) == 0)
			{
				s_outputHaveNext = FALSE;
				return;   // nothing more yet: the rest of this buffer is the game's sound alone
			}
			s_outputPrev = s_outputHaveNext ? s_outputNext : sample;
			s_outputNext = sample;
			s_outputHaveNext = TRUE;
			if (s_outputPhase >= 1.0) s_outputPhase -= 1.0;
		}
		const float v = (float)((s_outputPrev * (1.0 - s_outputPhase) + s_outputNext * s_outputPhase) / 32768.0);
		for (UnsignedInt c = 0; c < channels; ++c) {
			out[f * channels + c] += v;
		}
		s_outputPhase += step;
	}
}

void VoiceChat::setCapture(CaptureOpen open, CaptureClose close, InputLister lister)
{
	s_captureOpen = open;
	s_captureClose = close;
	s_inputLister = lister;
}

void VoiceChat::pushCapture(const Short *samples, UnsignedInt count)
{
	s_captureRing.push(samples, count);
}

void VoiceChat::listInputDevices(std::vector<AsciiString> &names)
{
	names.clear();
	if (s_inputLister != nullptr) {
		s_inputLister(names);
	}
}

void VoiceChat::shutdown()
{
	stopSession();
	if (s_log != nullptr)
	{
		fclose(s_log);
		s_log = nullptr;
		DeleteCriticalSection(&s_logLock);
	}
	for (Int i = 0; i < MAX_PEERS; ++i) {
		s_names[i] = UnicodeString::TheEmptyString;   // game strings: freed before the memory manager goes
	}
	s_inputDevice = AsciiString::TheEmptyString;
	if (s_wsaReady)
	{
		WSACleanup();
		s_wsaReady = FALSE;
	}
}
