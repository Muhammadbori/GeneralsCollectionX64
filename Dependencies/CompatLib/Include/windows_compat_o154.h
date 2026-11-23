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
