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

// TheSuperHackers @build O140 The Win32 subset the headless game needs on macOS and Linux (CompatLib).
//
// Adapted from the CompatLib of GeneralsX by Felipe Braz and contributors (https://github.com/fbraz3/GeneralsX,
// GPL-3.0-or-later), which builds on the Linux port work of fighter19: the executable path, the uptime clock on macOS,
// library loading and the critical section emulation follow GeneralsX; the rest is written for this project. Only what
// the code compiled on these platforms uses is here. It is never used on Windows: the include directory of this library
// is added for non-Windows targets only (cmake), so <windows.h> there is the Windows SDK as always.
#pragma once

#ifdef _WIN32
#error "CompatLib is for macOS and Linux only"
#endif

#include <Utility/compat.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

// Basic types. 32-bit where Windows has 32 bits, also on LP64 platforms (macOS, Linux: long is 64-bit there), so
// file and network formats keep their sizes. bittype.h declares the same types.
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int BOOL;
typedef unsigned int UINT;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint16_t USHORT;
typedef float FLOAT;
typedef int32_t HRESULT;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;
typedef intptr_t LRESULT;
typedef void * LPVOID;
typedef const void * LPCVOID;
typedef DWORD * LPDWORD;
typedef size_t SIZE_T;
typedef DWORD COLORREF;

typedef void * HANDLE;
typedef HANDLE HWND;
typedef HANDLE HINSTANCE;
typedef HANDLE HMODULE;
typedef HANDLE HKEY;
typedef HANDLE HDC;
typedef HANDLE HBITMAP;
typedef HANDLE HFONT;
typedef HANDLE HGDIOBJ;
typedef HANDLE HMONITOR;
typedef void (*FARPROC)(void);

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define CONST const
#define FAR
#define NEAR
#define IN
#define OUT
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef MAX_PATH
#define MAX_PATH _MAX_PATH
#endif

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))
#define MAKELONG(a, b) ((LONG)(((WORD)(((uintptr_t)(a)) & 0xffff)) | ((DWORD)((WORD)(((uintptr_t)(b)) & 0xffff))) << 16))

typedef union _LARGE_INTEGER
{
	struct
	{
		DWORD LowPart;
		LONG HighPart;
	};
	int64_t QuadPart;
} LARGE_INTEGER;

typedef struct _FILETIME
{
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME;

typedef struct _SYSTEMTIME
{
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
} SYSTEMTIME;

typedef struct tagRECT
{
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT;

typedef struct tagPOINT
{
	LONG x;
	LONG y;
} POINT;

typedef struct _OVERLAPPED
{
	uintptr_t Internal;
	uintptr_t InternalHigh;
	DWORD Offset;
	DWORD OffsetHigh;
	HANDLE hEvent;
} OVERLAPPED;

typedef struct _MEMORYSTATUS
{
	DWORD dwLength;
	DWORD dwMemoryLoad;
	SIZE_T dwTotalPhys;
	SIZE_T dwAvailPhys;
	SIZE_T dwTotalPageFile;
	SIZE_T dwAvailPageFile;
	SIZE_T dwTotalVirtual;
	SIZE_T dwAvailVirtual;
} MEMORYSTATUS;

typedef struct _OSVERSIONINFO
{
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	char szCSDVersion[128];
} OSVERSIONINFO;
#define VER_PLATFORM_WIN32_WINDOWS 1
#define VER_PLATFORM_WIN32_NT 2

// Critical sections: recursive pthread mutexes, as Windows critical sections are recursive.
typedef struct _CRITICAL_SECTION
{
	pthread_mutex_t Mutex;
} CRITICAL_SECTION;

void InitializeCriticalSection(CRITICAL_SECTION * section);
void DeleteCriticalSection(CRITICAL_SECTION * section);
void EnterCriticalSection(CRITICAL_SECTION * section);
void LeaveCriticalSection(CRITICAL_SECTION * section);

// Handles: CloseHandle releases what the functions below create (threads, events, mutexes, files).
BOOL CloseHandle(HANDLE handle);
DWORD GetLastError(void);
void SetLastError(DWORD error);
#define ERROR_SUCCESS 0
#define ERROR_ALREADY_EXISTS 183
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffffu

typedef DWORD (*LPTHREAD_START_ROUTINE)(void * parameter);
HANDLE CreateThread(void * attributes, SIZE_T stack_size, LPTHREAD_START_ROUTINE start, void * parameter, DWORD flags, DWORD * thread_id);
HANDLE GetCurrentThread(void);
BOOL SetThreadPriority(HANDLE thread, int priority);
BOOL TerminateThread(HANDLE thread, DWORD exit_code);
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15

HANDLE CreateEventA(void * attributes, BOOL manual_reset, BOOL initial_state, const char * name);
#define CreateEvent CreateEventA
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
HANDLE CreateMutexA(void * attributes, BOOL initial_owner, const char * name);
#define CreateMutex CreateMutexA
BOOL ReleaseMutex(HANDLE mutex);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);

// Message boxes and debug output go to the standard error stream: a headless game has no window.
int MessageBoxA(HWND window, const char * text, const char * caption, UINT type);
#define MessageBox MessageBoxA
void OutputDebugStringA(const char * text);
#define MB_OK 0x00000000
#define MB_OKCANCEL 0x00000001
#define MB_ABORTRETRYIGNORE 0x00000002
#define MB_YESNOCANCEL 0x00000003
#define MB_YESNO 0x00000004
#define MB_RETRYCANCEL 0x00000005
#define MB_ICONERROR 0x00000010
#define MB_ICONHAND 0x00000010
#define MB_ICONQUESTION 0x00000020
#define MB_ICONWARNING 0x00000030
#define MB_ICONEXCLAMATION 0x00000030
#define MB_ICONINFORMATION 0x00000040
#define MB_SYSTEMMODAL 0x00001000
#define MB_TASKMODAL 0x00002000
#define MB_SETFOREGROUND 0x00010000
#define MB_TOPMOST 0x00040000
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

// Modules.
DWORD GetModuleFileNameA(HMODULE module, char * buffer, DWORD size);
#define GetModuleFileName GetModuleFileNameA
HMODULE LoadLibraryA(const char * name);
#define LoadLibrary LoadLibraryA
FARPROC GetProcAddress(HMODULE module, const char * name);
BOOL FreeLibrary(HMODULE module);

// Process and system.
void ExitProcess(UINT exit_code);
DWORD GetCurrentProcessId(void);
void GlobalMemoryStatus(MEMORYSTATUS * status);
BOOL GetVersionEx(OSVERSIONINFO * info);
void GetLocalTime(SYSTEMTIME * time);
void GetSystemTime(SYSTEMTIME * time);
BOOL QueryPerformanceCounter(LARGE_INTEGER * count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER * frequency);
DWORD FormatMessageA(DWORD flags, const void * source, DWORD message_id, DWORD language_id, char * buffer, DWORD size, void * arguments);
#define FormatMessage FormatMessageA
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define LANG_NEUTRAL 0x00
#define SUBLANG_DEFAULT 0x01
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))

// Files: the few Win32 file calls outside the game's own file system.
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
HANDLE CreateFileA(const char * name, DWORD access, DWORD share, void * security, DWORD disposition, DWORD flags, HANDLE templ);
#define CreateFile CreateFileA
BOOL WriteFile(HANDLE file, const void * buffer, DWORD bytes, DWORD * written, OVERLAPPED * overlapped);
BOOL ReadFile(HANDLE file, void * buffer, DWORD bytes, DWORD * read, OVERLAPPED * overlapped);
BOOL CopyFileA(const char * existing, const char * target, BOOL fail_if_exists);
#define CopyFile CopyFileA
BOOL CreateDirectoryA(const char * path, void * security);
#define CreateDirectory CreateDirectoryA
DWORD GetFileAttributesA(const char * path);
#define GetFileAttributes GetFileAttributesA
BOOL DeleteFileA(const char * path);
#define DeleteFile DeleteFileA
