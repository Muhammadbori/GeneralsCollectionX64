// TheSuperHackers @build O154 (headless port) The next part of the Win32 API the game engine uses on macOS and Linux.
// Everything here is either plain POSIX (directory listing, file position, dates, random bytes) or a stand-in that fails
// the way Windows does when the feature is missing: no processes or job objects are started, there is no clipboard,
// monitor, keyboard state or added font. None of it is called by the game logic, so the logic CRC is not affected.
#pragma once

#include <stdlib.h>
#include <wchar.h>

#ifndef VOID
#define VOID void
#endif
typedef BYTE BOOLEAN;
typedef void * PVOID;
typedef HANDLE HKL;
typedef HANDLE HGLOBAL;
typedef HANDLE HMENU;
typedef HANDLE HICON;
typedef HANDLE HCURSOR;
#ifndef NTAPI
#define NTAPI
#endif
#ifndef MAKEWORD
#define MAKEWORD(a, b) ((WORD)(((BYTE)((a) & 0xff)) | ((WORD)((BYTE)((b) & 0xff))) << 8))
#endif
#define SYNCHRONIZE 0x00100000L

// Command line of the process. The CompatLib fills these from /proc/self/cmdline (Linux) or _NSGetArgv (macOS).
extern int __argc;
extern char ** __argv;
const wchar_t * GetCommandLineW(void);

// Number to text, as in the Microsoft C runtime.
char * itoa(int value, char * buffer, int radix);
#define _itoa itoa
char * _ultoa(unsigned long value, char * buffer, int radix);
char * _i64toa(long long value, char * buffer, int radix);
char * _ui64toa(unsigned long long value, char * buffer, int radix);
#ifndef __APPLE__ // macOS has it in <wctype.h>
inline int iswascii(wint_t c) { return c < 0x80; }
#endif

// Wide variants (text converted with the current C locale).
DWORD GetModuleFileNameW(HMODULE module, wchar_t * buffer, DWORD size);
DWORD GetCurrentDirectoryW(DWORD size, wchar_t * buffer);
DWORD GetFileAttributesW(const wchar_t * path);
DWORD FormatMessageW(DWORD flags, const void * source, DWORD message_id, DWORD language_id, wchar_t * buffer, DWORD size, void * arguments);
DWORD GetFullPathNameA(const char * name, DWORD size, char * buffer, char ** file_part);
#define GetFullPathName GetFullPathNameA

// Directory listing. The pattern's last part may hold '*' and '?' (fnmatch); the directory part is taken as it is.
typedef struct _WIN32_FIND_DATAA
{
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	char cFileName[MAX_PATH];
	char cAlternateFileName[14];
} WIN32_FIND_DATAA, WIN32_FIND_DATA;
HANDLE FindFirstFileA(const char * pattern, WIN32_FIND_DATAA * data);
BOOL FindNextFileA(HANDLE find, WIN32_FIND_DATAA * data);
BOOL FindClose(HANDLE find);
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA
#ifndef ERROR_NO_MORE_FILES
#define ERROR_NO_MORE_FILES 18
#endif

typedef struct _WIN32_FILE_ATTRIBUTE_DATA
{
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
BOOL GetFileAttributesExA(const char * path, GET_FILEEX_INFO_LEVELS level, void * info);
#define GetFileAttributesEx GetFileAttributesExA

#ifndef FILE_SHARE_DELETE
#define FILE_SHARE_DELETE 0x00000004
#endif
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
DWORD SetFilePointer(HANDLE file, LONG distance, LONG * distance_high, DWORD method);
DWORD GetFileSize(HANDLE file, DWORD * size_high);

// Dates and times in the user's format: strftime with the locale's short forms.
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define DATE_SHORTDATE 0x00000001
#define DATE_LONGDATE 0x00000002
#define TIME_NOMINUTESORSECONDS 0x00000001
#define TIME_NOSECONDS 0x00000002
#define TIME_NOTIMEMARKER 0x00000004
#define TIME_FORCE24HOURFORMAT 0x00000008
int GetDateFormatA(DWORD locale, DWORD flags, const SYSTEMTIME * time, const char * format, char * buffer, int size);
int GetTimeFormatA(DWORD locale, DWORD flags, const SYSTEMTIME * time, const char * format, char * buffer, int size);
int GetDateFormatW(DWORD locale, DWORD flags, const SYSTEMTIME * time, const wchar_t * format, wchar_t * buffer, int size);
int GetTimeFormatW(DWORD locale, DWORD flags, const SYSTEMTIME * time, const wchar_t * format, wchar_t * buffer, int size);
#define GetDateFormat GetDateFormatA
#define GetTimeFormat GetTimeFormatA

// Keyboard and mouse state: a headless game has none.
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
inline SHORT GetAsyncKeyState(int) { return 0; }
inline SHORT GetKeyState(int) { return 0; }
inline HKL GetKeyboardLayout(DWORD) { return nullptr; }
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_SWAPBUTTON 23
inline int GetSystemMetrics(int) { return 0; }
inline HWND GetForegroundWindow(void) { return nullptr; }
inline HWND GetActiveWindow(void) { return nullptr; }

// Monitors and display modes: none.
#define MONITOR_DEFAULTTONULL 0
#define MONITOR_DEFAULTTOPRIMARY 1
#define MONITOR_DEFAULTTONEAREST 2
typedef struct tagMONITORINFO { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct tagMONITORINFOEXA { DWORD cbSize; RECT rcMonitor; RECT rcWork; DWORD dwFlags; char szDevice[32]; } MONITORINFOEXA, MONITORINFOEX;
inline HMONITOR MonitorFromWindow(HWND, DWORD) { return nullptr; }
inline BOOL GetMonitorInfoA(HMONITOR, void *) { return FALSE; }
#define GetMonitorInfo GetMonitorInfoA
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
typedef struct _devicemodeA
{
	char dmDeviceName[32];
	WORD dmSpecVersion;
	WORD dmDriverVersion;
	WORD dmSize;
	WORD dmDriverExtra;
	DWORD dmFields;
	DWORD dmBitsPerPel;
	DWORD dmPelsWidth;
	DWORD dmPelsHeight;
	DWORD dmDisplayFlags;
	DWORD dmDisplayFrequency;
} DEVMODEA, DEVMODE;
inline BOOL EnumDisplaySettingsA(const char *, DWORD, DEVMODEA *) { return FALSE; }
#define EnumDisplaySettings EnumDisplaySettingsA

// Global memory (the clipboard and the GameMemory fallback): malloc.
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
inline HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes) { return (flags & GMEM_ZEROINIT) ? calloc(1, bytes ? bytes : 1) : malloc(bytes ? bytes : 1); }
inline HGLOBAL GlobalFree(HGLOBAL memory) { free(memory); return nullptr; }
inline void * GlobalLock(HGLOBAL memory) { return memory; }
inline BOOL GlobalUnlock(HGLOBAL) { return TRUE; }

// Clipboard: none (opening it fails).
#define CF_TEXT 1
#define CF_UNICODETEXT 13
inline BOOL OpenClipboard(HWND) { return FALSE; }
inline BOOL EmptyClipboard(void) { return FALSE; }
inline BOOL CloseClipboard(void) { return TRUE; }
inline HANDLE SetClipboardData(UINT, HANDLE) { return nullptr; }
inline HANDLE GetClipboardData(UINT) { return nullptr; }
inline BOOL IsClipboardFormatAvailable(UINT) { return FALSE; }

// Fonts added to the process: none.
#define FR_PRIVATE 0x10
inline int AddFontResourceExA(const char *, DWORD, void *) { return 0; }
inline BOOL RemoveFontResourceExA(const char *, DWORD, void *) { return FALSE; }
inline int AddFontResourceA(const char *) { return 0; }
inline BOOL RemoveFontResourceA(const char *) { return FALSE; }
#define AddFontResource AddFontResourceA
#define RemoveFontResource RemoveFontResourceA
#define AddFontResourceEx AddFontResourceExA
#define RemoveFontResourceEx RemoveFontResourceExA

// Processes, pipes and job objects: not started on these platforms (the calls fail).
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; void * lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct _STARTUPINFOW
{
	DWORD cb; wchar_t * lpReserved; wchar_t * lpDesktop; wchar_t * lpTitle;
	DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
	WORD wShowWindow, cbReserved2; BYTE * lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;
typedef struct _STARTUPINFOA
{
	DWORD cb; char * lpReserved; char * lpDesktop; char * lpTitle;
	DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
	WORD wShowWindow, cbReserved2; BYTE * lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, STARTUPINFO;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess; HANDLE hThread; DWORD dwProcessId; DWORD dwThreadId; } PROCESS_INFORMATION;
#define STARTF_USESHOWWINDOW 0x00000001
#define STARTF_FORCEOFFFEEDBACK 0x00000080
#define STARTF_USESTDHANDLES 0x00000100
#define CREATE_NO_WINDOW 0x08000000
#define CREATE_SUSPENDED 0x00000004
#define CREATE_UNICODE_ENVIRONMENT 0x00000400
#define HANDLE_FLAG_INHERIT 0x00000001
#define STILL_ACTIVE 259
#define SW_HIDE 0
#define SW_SHOW 5
inline BOOL CreateProcessW(const wchar_t *, wchar_t *, SECURITY_ATTRIBUTES *, SECURITY_ATTRIBUTES *, BOOL, DWORD, void *, const wchar_t *, STARTUPINFOW *, PROCESS_INFORMATION *) { return FALSE; }
inline BOOL CreateProcessA(const char *, char *, SECURITY_ATTRIBUTES *, SECURITY_ATTRIBUTES *, BOOL, DWORD, void *, const char *, STARTUPINFOA *, PROCESS_INFORMATION *) { return FALSE; }
#define CreateProcess CreateProcessA
inline BOOL GetExitCodeProcess(HANDLE, DWORD *) { return FALSE; }
inline BOOL TerminateProcess(HANDLE, UINT) { return FALSE; }
inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
inline BOOL CreatePipe(HANDLE *, HANDLE *, SECURITY_ATTRIBUTES *, DWORD) { return FALSE; }
inline BOOL PeekNamedPipe(HANDLE, void *, DWORD, DWORD *, DWORD *, DWORD *) { return FALSE; }
inline BOOL SetHandleInformation(HANDLE, DWORD, DWORD) { return FALSE; }
typedef struct _JOBOBJECT_BASIC_LIMIT_INFORMATION
{
	LARGE_INTEGER PerProcessUserTimeLimit; LARGE_INTEGER PerJobUserTimeLimit; DWORD LimitFlags;
	SIZE_T MinimumWorkingSetSize; SIZE_T MaximumWorkingSetSize; DWORD ActiveProcessLimit; uintptr_t Affinity;
	DWORD PriorityClass; DWORD SchedulingClass;
} JOBOBJECT_BASIC_LIMIT_INFORMATION;
typedef struct _IO_COUNTERS
{
	uint64_t ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
} IO_COUNTERS;
typedef struct _JOBOBJECT_EXTENDED_LIMIT_INFORMATION
{
	JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; IO_COUNTERS IoInfo;
	SIZE_T ProcessMemoryLimit; SIZE_T JobMemoryLimit; SIZE_T PeakProcessMemoryUsed; SIZE_T PeakJobMemoryUsed;
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
typedef enum _JOBOBJECTINFOCLASS { JobObjectBasicLimitInformation = 2, JobObjectExtendedLimitInformation = 9 } JOBOBJECTINFOCLASS;
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
inline HANDLE CreateJobObjectW(SECURITY_ATTRIBUTES *, const wchar_t *) { return nullptr; }
inline HANDLE CreateJobObjectA(SECURITY_ATTRIBUTES *, const char *) { return nullptr; }
inline BOOL SetInformationJobObject(HANDLE, JOBOBJECTINFOCLASS, void *, DWORD) { return FALSE; }
inline BOOL AssignProcessToJobObject(HANDLE, HANDLE) { return FALSE; }

// Overlapped (asynchronous) file reads: not used on these platforms; the calls report no result.
#ifndef ERROR_IO_PENDING
#define ERROR_IO_PENDING 997
#endif
#define ERROR_IO_INCOMPLETE 996
#define HasOverlappedIoCompleted(o) ((o)->Internal != (uintptr_t)ERROR_IO_PENDING)
inline BOOL GetOverlappedResult(HANDLE, OVERLAPPED *, DWORD * bytes, BOOL) { if (bytes) *bytes = 0; return FALSE; }
inline BOOL CancelIoEx(HANDLE, OVERLAPPED *) { return FALSE; }

// Stack capture (diagnostics only): nothing is captured.
inline WORD RtlCaptureStackBackTrace(DWORD, DWORD, void **, DWORD * hash) { if (hash) *hash = 0; return 0; }
#define CaptureStackBackTrace RtlCaptureStackBackTrace

// Random bytes (advapi32's RtlGenRandom, used by GuardLink): the system's random source.
extern "C" BOOLEAN SystemFunction036(PVOID buffer, ULONG length);

// More of the same (process handles, the pipe to a helper process, byte helpers). Processes are not opened.
typedef SECURITY_ATTRIBUTES * LPSECURITY_ATTRIBUTES;
#ifndef LOBYTE
#define LOBYTE(w) ((BYTE)(((uintptr_t)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((uintptr_t)(w)) >> 8) & 0xff))
#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
inline HANDLE OpenProcess(DWORD, BOOL, DWORD) { return nullptr; }
inline DWORD ResumeThread(HANDLE) { return (DWORD)-1; }
inline HMODULE GetModuleHandleW(const wchar_t *) { return nullptr; }
inline BOOL GetNamedPipeServerProcessId(HANDLE, ULONG * pid) { if (pid) *pid = 0; return FALSE; }
inline HANDLE CreateFileW(const wchar_t *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
inline LONG CompareFileTime(const FILETIME * a, const FILETIME * b)
{
	uint64_t x = ((uint64_t)a->dwHighDateTime << 32) | a->dwLowDateTime;
	uint64_t y = ((uint64_t)b->dwHighDateTime << 32) | b->dwLowDateTime;
	return x < y ? -1 : (x > y ? 1 : 0);
}
#ifndef FILE_FLAG_OVERLAPPED
#define FILE_FLAG_OVERLAPPED 0x40000000
#endif
typedef uintptr_t ULONG_PTR;
typedef uint64_t ULONGLONG;

// ---- O155 ----
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

// Interlocked operations for any integer width (LONG is 32 bits here, long is 64): the GCC/Clang atomic builtins,
// sequentially consistent like the Windows functions. Utility/interlocked_adapter.h keeps its exact 'long' forms.
template <typename T> inline T InterlockedIncrement(volatile T * value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
template <typename T> inline T InterlockedDecrement(volatile T * value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }
template <typename T, typename V> inline T InterlockedExchange(volatile T * target, V value) { return __atomic_exchange_n(target, (T)value, __ATOMIC_SEQ_CST); }
template <typename T, typename V> inline T InterlockedExchangeAdd(volatile T * target, V value) { return __atomic_fetch_add(target, (T)value, __ATOMIC_SEQ_CST); }
template <typename T, typename V, typename C> inline T InterlockedCompareExchange(volatile T * target, V exchange, C comparand)
{
	T expected = (T)comparand;
	__atomic_compare_exchange_n(target, &expected, (T)exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

// The process heap: malloc.
#define HEAP_ZERO_MEMORY 0x00000008
inline HANDLE GetProcessHeap(void) { return (HANDLE)(intptr_t)1; }
inline void * HeapAlloc(HANDLE, DWORD flags, SIZE_T bytes) { return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1) : malloc(bytes ? bytes : 1); }
inline void * HeapReAlloc(HANDLE, DWORD, void * memory, SIZE_T bytes) { return realloc(memory, bytes ? bytes : 1); }
inline BOOL HeapFree(HANDLE, DWORD, void * memory) { free(memory); return TRUE; }

// System information (diagnostic reports and thread counts): sysconf.
typedef struct _SYSTEM_INFO
{
	WORD wProcessorArchitecture;
	WORD wReserved;
	DWORD dwPageSize;
	void * lpMinimumApplicationAddress;
	void * lpMaximumApplicationAddress;
	uintptr_t dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
	DWORD dwProcessorType;
	DWORD dwAllocationGranularity;
	WORD wProcessorLevel;
	WORD wProcessorRevision;
} SYSTEM_INFO;
inline void GetSystemInfo(SYSTEM_INFO * info)
{
	memset(info, 0, sizeof(*info));
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	long page = sysconf(_SC_PAGESIZE);
	info->dwNumberOfProcessors = cpus > 0 ? (DWORD)cpus : 1;
	info->dwPageSize = page > 0 ? (DWORD)page : 4096;
	info->dwAllocationGranularity = info->dwPageSize;
}
#define GetNativeSystemInfo GetSystemInfo

typedef struct _MEMORYSTATUSEX
{
	DWORD dwLength;
	DWORD dwMemoryLoad;
	uint64_t ullTotalPhys;
	uint64_t ullAvailPhys;
	uint64_t ullTotalPageFile;
	uint64_t ullAvailPageFile;
	uint64_t ullTotalVirtual;
	uint64_t ullAvailVirtual;
	uint64_t ullAvailExtendedVirtual;
} MEMORYSTATUSEX;
inline BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX * status)
{
	DWORD length = status->dwLength;
	memset(status, 0, sizeof(*status));
	status->dwLength = length;
	long pages = sysconf(_SC_PHYS_PAGES);
	long page = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page > 0)
		status->ullTotalPhys = (uint64_t)pages * (uint64_t)page;
	return TRUE;
}

typedef struct _OSVERSIONINFOW
{
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	wchar_t szCSDVersion[128];
} OSVERSIONINFOW;

inline BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER * size)
{
	DWORD high = 0;
	DWORD low = GetFileSize(file, &high);
	if (low == (DWORD)-1 && GetLastError() != 0)
		return FALSE;
	size->QuadPart = (int64_t)(((uint64_t)high << 32) | low);
	return TRUE;
}

// Windows and message boxes of a headless game: none (the message box text goes to stderr, as MessageBoxA's does).
#define SW_SHOWNORMAL 1
#define SW_MINIMIZE 6
#define SW_RESTORE 9
inline BOOL ShowWindow(HWND, int) { return FALSE; }
inline BOOL SetWindowTextA(HWND, const char *) { return FALSE; }
inline BOOL SetWindowTextW(HWND, const wchar_t *) { return FALSE; }
#define SetWindowText SetWindowTextA
inline int MessageBoxW(HWND, const wchar_t * text, const wchar_t * caption, UINT)
{
	fprintf(stderr, "%ls: %ls\n", caption ? caption : L"", text ? text : L"");
	return 1; // IDOK
}

// No other programs (WorldBuilder) are started on these platforms.
#define _P_WAIT 0
#define _P_NOWAIT 1
inline intptr_t _spawnl(int, const char *, const char *, ...) { errno = ENOENT; return -1; }

#include "shlobj.h"

// ---- O156 ----
#ifndef ERROR_PATH_NOT_FOUND
#define ERROR_PATH_NOT_FOUND 3
#endif
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_GEN_FAILURE 31
#define ERROR_FILE_EXISTS 80
#define ERROR_DISK_FULL 112
#define ERROR_FILENAME_EXCED_RANGE 206
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#ifndef FILE_FLAG_DELETE_ON_CLOSE
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#endif
#ifndef FILE_FLAG_SEQUENTIAL_SCAN
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#endif

// Slim reader/writer locks (exclusive use only in the game): a pthread mutex. SRWLOCK_INIT is a static initialiser.
#include <pthread.h>
typedef struct _SRWLOCK { pthread_mutex_t Mutex; } SRWLOCK;
#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
inline void InitializeSRWLock(SRWLOCK * lock) { pthread_mutex_init(&lock->Mutex, nullptr); }
inline void AcquireSRWLockExclusive(SRWLOCK * lock) { pthread_mutex_lock(&lock->Mutex); }
inline void ReleaseSRWLockExclusive(SRWLOCK * lock) { pthread_mutex_unlock(&lock->Mutex); }
inline BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK * lock) { return pthread_mutex_trylock(&lock->Mutex) == 0; }

// Thread placement hint: the scheduler decides on these platforms.
inline DWORD SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }

// More virtual key codes (VoiceChat push-to-talk keys).
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_3 0xC0
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73

// ---- O159 ----
#include <ctype.h>
#include <string.h>
// The Win32 string calls (kernel32 lstr*), as functions so that ::lstrcpy works.
inline char * lstrcpy(char * dst, const char * src) { return strcpy(dst, src); }
inline char * lstrcat(char * dst, const char * src) { return strcat(dst, src); }
inline int lstrlen(const char * s) { return s ? (int)strlen(s) : 0; }
inline int lstrcmp(const char * a, const char * b) { return strcmp(a, b); }
inline int lstrcmpi(const char * a, const char * b) { return strcasecmp(a, b); }
inline char * lstrcpyn(char * dst, const char * src, int count)
{
	if (count <= 0) return dst;
	int i = 0;
	for (; i < count - 1 && src[i]; ++i) dst[i] = src[i];
	dst[i] = 0;
	return dst;
}
#define lstrcpyA lstrcpy
#define lstrcatA lstrcat
#define lstrlenA lstrlen
#define lstrcmpiA lstrcmpi
#define lstrcpynA lstrcpyn
#ifndef ZeroMemory
#define ZeroMemory(dst, len) memset((dst), 0, (len))
#define FillMemory(dst, len, fill) memset((dst), (fill), (len))
#define CopyMemory(dst, src, len) memcpy((dst), (src), (len))
#define MoveMemory(dst, src, len) memmove((dst), (src), (len))
#endif
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef void * LPDISPATCH;
typedef struct tagSIZE { LONG cx; LONG cy; } SIZE;

// A headless game has no window, cursor or error dialogs.
inline BOOL IsIconic(HWND) { return FALSE; }
inline BOOL IsWindowVisible(HWND) { return FALSE; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
inline BOOL GetCursorPos(POINT * point) { if (point) { point->x = 0; point->y = 0; } return FALSE; }
inline BOOL ScreenToClient(HWND, POINT *) { return FALSE; }
inline BOOL ClientToScreen(HWND, POINT *) { return FALSE; }
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000
inline UINT SetErrorMode(UINT) { return 0; }
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_INSERT 0x2D
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMPAD0 0x60

// ---- O160 ----
// HRESULT construction (WWDownload's error codes).
#ifndef MAKE_HRESULT
#define SEVERITY_SUCCESS 0
#define SEVERITY_ERROR 1
#define FACILITY_ITF 4
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#endif
// Display modes and window placement of a headless game: no display, no window (the calls fail).
#define DM_BITSPERPEL 0x00040000L
#define DM_PELSWIDTH 0x00080000L
#define DM_PELSHEIGHT 0x00100000L
#define DM_DISPLAYFREQUENCY 0x00400000L
#define CDS_FULLSCREEN 0x00000004
#define CDS_TEST 0x00000002
#define DISP_CHANGE_SUCCESSFUL 0
#define DISP_CHANGE_FAILED (-1)
inline LONG ChangeDisplaySettingsExA(const char *, DEVMODEA *, HWND, DWORD, void *) { return DISP_CHANGE_FAILED; }
inline LONG ChangeDisplaySettingsA(DEVMODEA *, DWORD) { return DISP_CHANGE_FAILED; }
#define ChangeDisplaySettingsEx ChangeDisplaySettingsExA
#define ChangeDisplaySettings ChangeDisplaySettingsA
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
inline LONG GetWindowLongA(HWND, int) { return 0; }
inline LONG SetWindowLongA(HWND, int, LONG) { return 0; }
#define GetWindowLong GetWindowLongA
#define SetWindowLong SetWindowLongA
inline BOOL GetClientRect(HWND, RECT * rect) { if (rect) { rect->left = rect->top = rect->right = rect->bottom = 0; } return FALSE; }
inline BOOL GetWindowRect(HWND, RECT * rect) { if (rect) { rect->left = rect->top = rect->right = rect->bottom = 0; } return FALSE; }
inline BOOL AdjustWindowRect(RECT *, DWORD, BOOL) { return FALSE; }
#define HWND_TOP ((HWND)0)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040
inline BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return FALSE; }
