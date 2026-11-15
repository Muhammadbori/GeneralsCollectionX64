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

// TheSuperHackers @feature See MismatchReport.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/MismatchReport.h"

#include "Common/Fingerprint.h"
#include "Common/GlobalData.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "Common/version.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"

#include <intrin.h>
#include <stdio.h>
#include <vector>

namespace
{
	struct FrameLine
	{
		UnsignedInt frame;
		UnsignedInt seed;
		UnsignedInt draws;
	};

	enum { FRAME_LINES = 1024, CRC_KEEP = 4, MAX_STAGES = 12 };

	struct CrcRecord
	{
		UnsignedInt frame;
		UnsignedInt finalCRC;
		Int stageCount;
		const char *stageNames[MAX_STAGES];
		UnsignedInt stageCRCs[MAX_STAGES];
		std::vector<UnsignedInt> objectIds;
		std::vector<UnsignedInt> objectCRCs;
		Bool done;
	};

	struct PlayerCRC
	{
		Int index;
		UnicodeString name;
		UnsignedInt crc;
	};

	FrameLine s_frames[FRAME_LINES];
	UnsignedInt s_frameCount = 0;          ///< lines written (the ring holds the last FRAME_LINES)
	CrcRecord s_crcs[CRC_KEEP];
	Int s_crcNext = 0;
	CrcRecord *s_crcOpen = nullptr;
	std::vector<PlayerCRC> s_players;
	Bool s_written = FALSE;
	Int s_perturbFrame = -1;
	Int s_reportFrame = -1;
	Bool s_perturbDone = FALSE;

	void crcClear(CrcRecord &r)
	{
		r.frame = 0;
		r.finalCRC = 0;
		r.stageCount = 0;
		r.objectIds.clear();
		r.objectCRCs.clear();
		r.done = FALSE;
	}

	void cpuName(char *out, Int size)
	{
		int regs[4] = { 0 };
		char brand[49] = { 0 };
		__cpuid(regs, 0x80000000);
		if ((unsigned)regs[0] >= 0x80000004u)
		{
			for (int i = 0; i < 3; ++i)
			{
				__cpuid(regs, 0x80000002 + i);
				memcpy(brand + i * 16, regs, 16);
			}
		}
		strlcpy(out, brand[0] ? brand : "unknown", size);
	}

	void osVersion(char *out, Int size)
	{
		typedef LONG (WINAPI *RtlGetVersionFunc)(OSVERSIONINFOW *);
		OSVERSIONINFOW info;
		memset(&info, 0, sizeof(info));
		info.dwOSVersionInfoSize = sizeof(info);
		HMODULE ntdll = GetModuleHandleA("ntdll.dll");
		RtlGetVersionFunc fn = ntdll ? (RtlGetVersionFunc)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
		if (fn != nullptr && fn(&info) == 0) {
			snprintf(out, size, "Windows %lu.%lu build %lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
		} else {
			strlcpy(out, "Windows (unknown)", size);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MismatchReport::setPerturbFrame(Int frame)
{
	s_perturbFrame = frame;
	s_perturbDone = FALSE;
}

void MismatchReport::setReportFrame(Int frame)
{
	s_reportFrame = frame;
}

Bool MismatchReport::perturbNow(UnsignedInt frame)
{
	if (s_perturbFrame < 0 || s_perturbDone || (Int)frame != s_perturbFrame) {
		return FALSE;
	}
	if (TheRecorder == nullptr || !TheRecorder->isPlaybackMode()) {
		return FALSE;   // never in a real game
	}
	s_perturbDone = TRUE;
	return TRUE;
}

Bool MismatchReport::recording()
{
	if (TheRecorder == nullptr || TheGameLogic == nullptr) {
		return FALSE;
	}
	const GameMode mode = TheGameLogic->getGameMode();
	if (mode == GAME_SHELL || mode == GAME_NONE) {
		return FALSE;
	}
	return TheRecorder->isPlaybackMode() || TheRecorder->isMultiplayer();
}

MismatchReport::FrameScope::FrameScope()
	: m_frame(0), m_draws(0), m_on(FALSE)
{
	if (TheGameLogic == nullptr) {
		return;
	}
	m_on = recording();
	if (m_on)
	{
		m_frame = TheGameLogic->getFrame();
		if (m_frame == 0 && s_frameCount > 0) {
			reset();   // a new game
		}
		m_draws = TheGameLogicRandomDrawCount;
	}
}

MismatchReport::FrameScope::~FrameScope()
{
	if (!m_on) {
		return;
	}
	FrameLine &line = s_frames[s_frameCount % FRAME_LINES];
	line.frame = m_frame;
	line.seed = GetGameLogicRandomSeedCRC();
	line.draws = TheGameLogicRandomDrawCount - m_draws;
	++s_frameCount;
	if (s_reportFrame >= 0 && (Int)m_frame == s_reportFrame && TheRecorder != nullptr && TheRecorder->isPlaybackMode()) {
		write("test", m_frame, 0, 0);
	}
}

void MismatchReport::crcBegin(UnsignedInt frame)
{
	CrcRecord &r = s_crcs[s_crcNext];
	s_crcNext = (s_crcNext + 1) % CRC_KEEP;
	crcClear(r);
	r.frame = frame;
	s_crcOpen = &r;
}

void MismatchReport::crcStage(const char *name, UnsignedInt crc)
{
	if (s_crcOpen == nullptr || s_crcOpen->stageCount >= MAX_STAGES) {
		return;
	}
	s_crcOpen->stageNames[s_crcOpen->stageCount] = name;
	s_crcOpen->stageCRCs[s_crcOpen->stageCount] = crc;
	++s_crcOpen->stageCount;
}

void MismatchReport::crcObject(UnsignedInt id, UnsignedInt crc)
{
	if (s_crcOpen == nullptr) {
		return;
	}
	s_crcOpen->objectIds.push_back(id);
	s_crcOpen->objectCRCs.push_back(crc);
}

void MismatchReport::crcEnd(UnsignedInt crc)
{
	if (s_crcOpen == nullptr) {
		return;
	}
	s_crcOpen->finalCRC = crc;
	s_crcOpen->done = TRUE;
	s_crcOpen = nullptr;
}

void MismatchReport::notePlayerCRC(Int playerIndex, const UnicodeString &name, UnsignedInt crc)
{
	PlayerCRC p;
	p.index = playerIndex;
	p.name = name;
	p.crc = crc;
	s_players.push_back(p);
}

void MismatchReport::write(const char *reason, UnsignedInt frame, UnsignedInt crcA, UnsignedInt crcB)
{
	if (s_written || TheGlobalData == nullptr) {
		return;
	}
	s_written = TRUE;

	SYSTEMTIME now;
	GetLocalTime(&now);
	AsciiString folder = TheGlobalData->getPath_UserData();
	folder.concat("MismatchReports\\");
	CreateDirectoryA(folder.str(), nullptr);
	AsciiString sub;
	sub.format("%04d%02d%02d-%02d%02d%02d_f%u_p%lu\\", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
		frame, GetCurrentProcessId());
	folder.concat(sub);
	CreateDirectoryA(folder.str(), nullptr);

	// the replay so far (the recording, or the replay being played)
	if (TheRecorder != nullptr)
	{
		AsciiString replay;
		if (TheRecorder->isPlaybackMode()) {
			replay = RecorderClass::getReplayPathForRead(TheRecorder->getCurrentReplayFilename());
		} else {
			replay = RecorderClass::getReplayDir();
			replay.concat(RecorderClass::getLastReplayFileName());
			replay.concat(RecorderClass::getReplayExtention());
		}
		AsciiString to = folder;
		to.concat("replay.rep");
		CopyFileA(replay.str(), to.str(), FALSE);
	}

	// the fingerprint
	{
		AsciiString path = folder;
		path.concat("fingerprint.txt");
		if (FILE *fp = fopen(path.str(), "w"))
		{
			fputs(Fingerprint::text().str(), fp);
			fclose(fp);
		}
	}

	AsciiString path = folder;
	path.concat("report.txt");
	FILE *fp = fopen(path.str(), "w");
	if (fp == nullptr) {
		return;
	}
	char cpu[64], os[64];
	cpuName(cpu, ARRAY_SIZE(cpu));
	osVersion(os, ARRAY_SIZE(os));
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	MEMORYSTATUSEX mem;
	mem.dwLength = sizeof(mem);
	GlobalMemoryStatusEx(&mem);

	fprintf(fp, "# mismatch report version 1\n");
	fprintf(fp, "reason %s\n", reason);
	fprintf(fp, "detected_frame %u\n", frame);
	fprintf(fp, "crc_a %08X\ncrc_b %08X\n", crcA, crcB);
	fprintf(fp, "logic_frame_now %u\n", TheGameLogic ? TheGameLogic->getFrame() : 0);
	fprintf(fp, "game_mode %d\n", TheGameLogic ? (Int)TheGameLogic->getGameMode() : -1);
	fprintf(fp, "playback %d\n", (TheRecorder && TheRecorder->isPlaybackMode()) ? 1 : 0);
	if (TheGameInfo != nullptr) {
		fprintf(fp, "map %s\nseed %d\nlocal_slot %d\n", TheGameInfo->getMap().str(), TheGameInfo->getSeed(), TheGameInfo->getLocalSlotNum());
	}
	fprintf(fp, "exe_crc %08X\nini_crc %08X\nfingerprint %08X\n", TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC, Fingerprint::overall());
	if (TheVersion != nullptr) {
		fprintf(fp, "version %s\nbuild_time %s\n", TheVersion->getAsciiVersion().str(), TheVersion->getAsciiBuildTime().str());
	}
	fprintf(fp, "build %s\n", sizeof(void *) == 8 ? "64-bit" : "32-bit");
	fprintf(fp, "cpu %s\nlogical_processors %lu\nmemory_mb %llu\nos %s\n", cpu, si.dwNumberOfProcessors,
		(unsigned long long)(mem.ullTotalPhys / (1024 * 1024)), os);
	fprintf(fp, "time %04d-%02d-%02d %02d:%02d:%02d\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

	for (size_t i = 0; i < s_players.size(); ++i)
	{
		AsciiString name;
		name.translate(s_players[i].name);
		fprintf(fp, "player %d %08X %s\n", s_players[i].index, s_players[i].crc, name.str());
	}

	// the rolling frames, oldest first: F <frame> <seed crc> <draws>
	const UnsignedInt count = s_frameCount < FRAME_LINES ? s_frameCount : FRAME_LINES;
	for (UnsignedInt i = 0; i < count; ++i)
	{
		const FrameLine &line = s_frames[(s_frameCount - count + i) % FRAME_LINES];
		fprintf(fp, "F %u %08X %u\n", line.frame, line.seed, line.draws);
	}

	// the CRCs, oldest first: C <frame> <final>, S <frame> <part> <crc after it>, O <frame> <object id> <crc after it>
	for (Int k = 0; k < CRC_KEEP; ++k)
	{
		const CrcRecord &r = s_crcs[(s_crcNext + k) % CRC_KEEP];
		if (!r.done) {
			continue;
		}
		fprintf(fp, "C %u %08X\n", r.frame, r.finalCRC);
		for (Int i = 0; i < r.stageCount; ++i) {
			fprintf(fp, "S %u %s %08X\n", r.frame, r.stageNames[i], r.stageCRCs[i]);
		}
		for (size_t i = 0; i < r.objectIds.size(); ++i) {
			fprintf(fp, "O %u %u %08X\n", r.frame, r.objectIds[i], r.objectCRCs[i]);
		}
	}
	fclose(fp);
}

void MismatchReport::reset()
{
	s_frameCount = 0;
	for (Int k = 0; k < CRC_KEEP; ++k) {
		crcClear(s_crcs[k]);
	}
	s_crcNext = 0;
	s_crcOpen = nullptr;
	s_players.clear();
	s_written = FALSE;
}

void MismatchReport::shutdown()
{
	reset();
	// give the memory back before the memory manager goes (these live in static storage)
	for (Int k = 0; k < CRC_KEEP; ++k)
	{
		std::vector<UnsignedInt>().swap(s_crcs[k].objectIds);
		std::vector<UnsignedInt>().swap(s_crcs[k].objectCRCs);
	}
	std::vector<PlayerCRC>().swap(s_players);
}
