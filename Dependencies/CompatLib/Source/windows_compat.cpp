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

// TheSuperHackers @build O140 See windows_compat.h. The executable path (GetModuleFileNameA) and library loading
// follow the CompatLib of GeneralsX (https://github.com/fbraz3/GeneralsX, GPL-3.0-or-later, Felipe Braz and
// contributors).

#include "windows_compat.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace
{
	enum HandleKind { HANDLE_THREAD = 1, HANDLE_EVENT, HANDLE_MUTEX, HANDLE_FILE };

	struct CompatHandle
	{
		int Kind;
		pthread_t Thread;
		pthread_mutex_t Mutex;
		pthread_cond_t Condition;
		bool ManualReset;
		bool Signaled;
		int File;
	};

	thread_local DWORD LastError = 0;

	// O156: the Win32 error code for a failed file call, so callers that compare codes see the Windows ones.
	DWORD Error_From_Errno(int error)
	{
		switch (error)
		{
			case ENOENT: return ERROR_FILE_NOT_FOUND;
			case ENOTDIR: return ERROR_PATH_NOT_FOUND;
			case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
			case EEXIST: return ERROR_FILE_EXISTS;
			case ENOSPC: return ERROR_DISK_FULL;
			case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
			case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
			default: return ERROR_GEN_FAILURE;
		}
	}

	CompatHandle * New_Handle(int kind)
	{
		CompatHandle * handle = new CompatHandle;
		memset(handle, 0, sizeof(*handle));
		handle->Kind = kind;
		handle->File = -1;
		if (kind == HANDLE_EVENT || kind == HANDLE_MUTEX)
		{
			pthread_mutex_init(&handle->Mutex, nullptr);
			pthread_cond_init(&handle->Condition, nullptr);
		}
		return handle;
	}

	struct ThreadStart
	{
		LPTHREAD_START_ROUTINE Start;
		void * Parameter;
	};

	void * Thread_Main(void * argument)
	{
		ThreadStart start = *static_cast<ThreadStart *>(argument);
		delete static_cast<ThreadStart *>(argument);
		start.Start(start.Parameter);
		return nullptr;
	}

	void Deadline(struct timespec & deadline, DWORD milliseconds)
	{
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += milliseconds / 1000;
		deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L)
		{
			deadline.tv_sec += 1;
			deadline.tv_nsec -= 1000000000L;
		}
	}
}

void InitializeCriticalSection(CRITICAL_SECTION * section)
{
	pthread_mutexattr_t attributes;
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&section->Mutex, &attributes);
	pthread_mutexattr_destroy(&attributes);
}

void DeleteCriticalSection(CRITICAL_SECTION * section)
{
	pthread_mutex_destroy(&section->Mutex);
}

void EnterCriticalSection(CRITICAL_SECTION * section)
{
	pthread_mutex_lock(&section->Mutex);
}

void LeaveCriticalSection(CRITICAL_SECTION * section)
{
	pthread_mutex_unlock(&section->Mutex);
}

DWORD GetLastError(void)
{
	return LastError;
}

void SetLastError(DWORD error)
{
	LastError = error;
}

BOOL CloseHandle(HANDLE handle)
{
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
	{
		return FALSE;
	}
	CompatHandle * h = static_cast<CompatHandle *>(handle);
	switch (h->Kind)
	{
	case HANDLE_THREAD:
		pthread_detach(h->Thread);
		break;
	case HANDLE_EVENT:
	case HANDLE_MUTEX:
		pthread_cond_destroy(&h->Condition);
		pthread_mutex_destroy(&h->Mutex);
		break;
	case HANDLE_FILE:
		if (h->File >= 0)
			close(h->File);
		break;
	default:
		return FALSE;
	}
	delete h;
	return TRUE;
}

HANDLE CreateThread(void *, SIZE_T stack_size, LPTHREAD_START_ROUTINE start, void * parameter, DWORD, DWORD * thread_id)
{
	CompatHandle * handle = New_Handle(HANDLE_THREAD);
	ThreadStart * argument = new ThreadStart;
	argument->Start = start;
	argument->Parameter = parameter;
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	if (stack_size != 0)
		pthread_attr_setstacksize(&attributes, stack_size);
	const int result = pthread_create(&handle->Thread, &attributes, Thread_Main, argument);
	pthread_attr_destroy(&attributes);
	if (result != 0)
	{
		delete argument;
		delete handle;
		LastError = (DWORD)result;
		return nullptr;
	}
	if (thread_id)
		*thread_id = (DWORD)(uintptr_t)handle;
	return handle;
}

HANDLE GetCurrentThread(void)
{
	// A pseudo handle, as on Windows; only SetThreadPriority takes it here.
	return (HANDLE)(intptr_t)-2;
}

BOOL SetThreadPriority(HANDLE, int)
{
	// Thread priorities are left to the system.
	return TRUE;
}

BOOL TerminateThread(HANDLE, DWORD)
{
	// Not supported: threads end by returning.
	return FALSE;
}

HANDLE CreateEventA(void *, BOOL manual_reset, BOOL initial_state, const char *)
{
	CompatHandle * handle = New_Handle(HANDLE_EVENT);
	handle->ManualReset = manual_reset != FALSE;
	handle->Signaled = initial_state != FALSE;
	return handle;
}

BOOL SetEvent(HANDLE event)
{
	CompatHandle * h = static_cast<CompatHandle *>(event);
	if (!h || h->Kind != HANDLE_EVENT)
		return FALSE;
	pthread_mutex_lock(&h->Mutex);
	h->Signaled = true;
	pthread_cond_broadcast(&h->Condition);
	pthread_mutex_unlock(&h->Mutex);
	return TRUE;
}

BOOL ResetEvent(HANDLE event)
{
	CompatHandle * h = static_cast<CompatHandle *>(event);
	if (!h || h->Kind != HANDLE_EVENT)
		return FALSE;
	pthread_mutex_lock(&h->Mutex);
	h->Signaled = false;
	pthread_mutex_unlock(&h->Mutex);
	return TRUE;
}

HANDLE CreateMutexA(void *, BOOL initial_owner, const char *)
{
	// A named mutex on Windows guards against a second game instance; here every instance gets its own.
	CompatHandle * handle = New_Handle(HANDLE_MUTEX);
	handle->Signaled = initial_owner == FALSE;
	LastError = ERROR_SUCCESS;
	return handle;
}

BOOL ReleaseMutex(HANDLE mutex)
{
	CompatHandle * h = static_cast<CompatHandle *>(mutex);
	if (!h || h->Kind != HANDLE_MUTEX)
		return FALSE;
	pthread_mutex_lock(&h->Mutex);
	h->Signaled = true;
	pthread_cond_signal(&h->Condition);
	pthread_mutex_unlock(&h->Mutex);
	return TRUE;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
	CompatHandle * h = static_cast<CompatHandle *>(handle);
	if (!h)
		return WAIT_FAILED;
	if (h->Kind == HANDLE_THREAD)
	{
		// Only an infinite wait joins; a timed wait reports a running thread.
		if (milliseconds != INFINITE)
			return WAIT_TIMEOUT;
		pthread_join(h->Thread, nullptr);
		h->Kind = 0;
		return WAIT_OBJECT_0;
	}
	if (h->Kind != HANDLE_EVENT && h->Kind != HANDLE_MUTEX)
		return WAIT_FAILED;
	struct timespec deadline;
	if (milliseconds != INFINITE)
		Deadline(deadline, milliseconds);
	pthread_mutex_lock(&h->Mutex);
	int result = 0;
	while (!h->Signaled && result == 0)
	{
		if (milliseconds == INFINITE)
			result = pthread_cond_wait(&h->Condition, &h->Mutex);
		else
			result = pthread_cond_timedwait(&h->Condition, &h->Mutex, &deadline);
	}
	const bool signaled = h->Signaled;
	if (signaled && (h->Kind == HANDLE_MUTEX || !h->ManualReset))
		h->Signaled = false;
	pthread_mutex_unlock(&h->Mutex);
	return signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

int MessageBoxA(HWND, const char * text, const char * caption, UINT type)
{
	fprintf(stderr, "[%s] %s\n", caption ? caption : "message", text ? text : "");
	fflush(stderr);
	// The answer an unattended run takes: OK, or the negative choice where there is one.
	switch (type & 0xf)
	{
	case MB_YESNO:
	case MB_YESNOCANCEL:
		return IDNO;
	case MB_ABORTRETRYIGNORE:
		return IDABORT;
	case MB_RETRYCANCEL:
	case MB_OKCANCEL:
		return IDCANCEL;
	default:
		return IDOK;
	}
}

void OutputDebugStringA(const char * text)
{
	fputs(text ? text : "", stderr);
}

DWORD GetModuleFileNameA(HMODULE, char * buffer, DWORD size)
{
	if (!buffer || size == 0)
		return 0;
#if defined(__APPLE__)
	uint32_t length = size;
	if (_NSGetExecutablePath(buffer, &length) != 0)
	{
		buffer[0] = '\0';
		return 0;
	}
	return (DWORD)strlen(buffer);
#else
	ssize_t count = readlink("/proc/self/exe", buffer, size - 1);
	if (count < 0)
	{
		buffer[0] = '\0';
		return 0;
	}
	buffer[count] = '\0';
	return (DWORD)count;
#endif
}

HMODULE LoadLibraryA(const char * name)
{
	if (!name)
		return nullptr;
	// "x.dll" becomes the platform's x.dylib / x.so.
	char path[1024];
	strncpy(path, name, sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	char * extension = strrchr(path, '.');
	if (extension && strcasecmp(extension, ".dll") == 0)
	{
		*extension = '\0';
#if defined(__APPLE__)
		strncat(path, ".dylib", sizeof(path) - strlen(path) - 1);
#else
		strncat(path, ".so", sizeof(path) - strlen(path) - 1);
#endif
	}
	return dlopen(path, RTLD_LAZY);
}

FARPROC GetProcAddress(HMODULE module, const char * name)
{
	return module ? (FARPROC)dlsym(module, name) : nullptr;
}

BOOL FreeLibrary(HMODULE module)
{
	return module && dlclose(module) == 0;
}

void ExitProcess(UINT exit_code)
{
	fflush(nullptr);
	_exit((int)exit_code);
}

DWORD GetCurrentProcessId(void)
{
	return (DWORD)getpid();
}

void GlobalMemoryStatus(MEMORYSTATUS * status)
{
	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);
	const long pages = sysconf(_SC_PHYS_PAGES);
	const long page_size = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page_size > 0)
		status->dwTotalPhys = status->dwAvailPhys = (SIZE_T)pages * (SIZE_T)page_size;
	status->dwTotalVirtual = status->dwAvailVirtual = status->dwTotalPhys;
}

BOOL GetVersionEx(OSVERSIONINFO * info)
{
	if (info)
		memset(info, 0, sizeof(*info));
	return FALSE;
}

static void Fill_System_Time(SYSTEMTIME * out, bool local)
{
	struct timeval now;
	gettimeofday(&now, nullptr);
	time_t seconds = now.tv_sec;
	struct tm parts;
	if (local)
		localtime_r(&seconds, &parts);
	else
		gmtime_r(&seconds, &parts);
	out->wYear = (WORD)(parts.tm_year + 1900);
	out->wMonth = (WORD)(parts.tm_mon + 1);
	out->wDayOfWeek = (WORD)parts.tm_wday;
	out->wDay = (WORD)parts.tm_mday;
	out->wHour = (WORD)parts.tm_hour;
	out->wMinute = (WORD)parts.tm_min;
	out->wSecond = (WORD)parts.tm_sec;
	out->wMilliseconds = (WORD)(now.tv_usec / 1000);
}

void GetLocalTime(SYSTEMTIME * time)
{
	if (time)
		Fill_System_Time(time, true);
}

void GetSystemTime(SYSTEMTIME * time)
{
	if (time)
		Fill_System_Time(time, false);
}

BOOL QueryPerformanceCounter(LARGE_INTEGER * count)
{
	if (!count)
		return FALSE;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	count->QuadPart = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
	return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER * frequency)
{
	if (!frequency)
		return FALSE;
	frequency->QuadPart = 1000000000LL;
	return TRUE;
}

DWORD FormatMessageA(DWORD, const void *, DWORD message_id, DWORD, char * buffer, DWORD size, void *)
{
	if (!buffer || size == 0)
		return 0;
	snprintf(buffer, size, "error %u", (unsigned)message_id);
	return (DWORD)strlen(buffer);
}

HANDLE CreateFileA(const char * name, DWORD access, DWORD, void *, DWORD disposition, DWORD attributes, HANDLE)
{
	int flags = 0;
	if ((access & GENERIC_READ) && (access & GENERIC_WRITE))
		flags = O_RDWR;
	else if (access & GENERIC_WRITE)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;
	switch (disposition)
	{
	case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
	case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
	case OPEN_ALWAYS: flags |= O_CREAT; break;
	case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
	default: break;
	}
	const int file = open(name, flags, 0644);
	if (file < 0)
	{
		LastError = Error_From_Errno(errno);
		return INVALID_HANDLE_VALUE;
	}
	// O156: the name goes now; the open file stays until it is closed, as with FILE_FLAG_DELETE_ON_CLOSE.
	if (attributes & FILE_FLAG_DELETE_ON_CLOSE)
		unlink(name);
	CompatHandle * handle = New_Handle(HANDLE_FILE);
	handle->File = file;
	return handle;
}

BOOL WriteFile(HANDLE file, const void * buffer, DWORD bytes, DWORD * written, OVERLAPPED *)
{
	CompatHandle * h = static_cast<CompatHandle *>(file);
	if (!h || file == INVALID_HANDLE_VALUE || h->Kind != HANDLE_FILE)
		return FALSE;
	const ssize_t count = write(h->File, buffer, bytes);
	if (written)
		*written = count > 0 ? (DWORD)count : 0;
	return count >= 0;
}

BOOL ReadFile(HANDLE file, void * buffer, DWORD bytes, DWORD * read_bytes, OVERLAPPED *)
{
	CompatHandle * h = static_cast<CompatHandle *>(file);
	if (!h || file == INVALID_HANDLE_VALUE || h->Kind != HANDLE_FILE)
		return FALSE;
	const ssize_t count = read(h->File, buffer, bytes);
	if (read_bytes)
		*read_bytes = count > 0 ? (DWORD)count : 0;
	return count >= 0;
}

BOOL CopyFileA(const char * existing, const char * target, BOOL fail_if_exists)
{
	FILE * in = fopen(existing, "rb");
	if (!in)
		return FALSE;
	if (fail_if_exists && access(target, F_OK) == 0)
	{
		fclose(in);
		return FALSE;
	}
	FILE * out = fopen(target, "wb");
	if (!out)
	{
		fclose(in);
		return FALSE;
	}
	char buffer[65536];
	size_t count;
	bool ok = true;
	while ((count = fread(buffer, 1, sizeof(buffer), in)) > 0)
	{
		if (fwrite(buffer, 1, count, out) != count)
		{
			ok = false;
			break;
		}
	}
	fclose(in);
	fclose(out);
	return ok;
}

BOOL CreateDirectoryA(const char * path, void *)
{
	if (mkdir(path, 0755) == 0)
		return TRUE;
	LastError = errno == EEXIST ? ERROR_ALREADY_EXISTS : Error_From_Errno(errno);
	return FALSE;
}

DWORD GetFileAttributesA(const char * path)
{
	struct stat info;
	if (stat(path, &info) != 0)
		return INVALID_FILE_ATTRIBUTES;
	return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL DeleteFileA(const char * path)
{
	return unlink(path) == 0;
}

// O153
DWORD GetCurrentDirectoryA(DWORD size, char * buffer)
{
	if (!buffer || size == 0)
		return 0;
	if (!getcwd(buffer, size))
	{
		buffer[0] = '\0';
		return 0;
	}
	return (DWORD)strlen(buffer);
}

BOOL SetCurrentDirectoryA(const char * path)
{
	return path && chdir(path) == 0;
}

// O154: file position and size of a CreateFileA handle.
DWORD SetFilePointer(HANDLE file, LONG distance, LONG * distance_high, DWORD method)
{
	CompatHandle * h = static_cast<CompatHandle *>(file);
	if (!h || file == INVALID_HANDLE_VALUE || h->Kind != HANDLE_FILE)
		return INVALID_SET_FILE_POINTER;
	off_t offset = distance_high ? (off_t)(((uint64_t)(uint32_t)*distance_high << 32) | (uint32_t)distance) : (off_t)distance;
	int whence = method == FILE_END ? SEEK_END : (method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
	off_t result = lseek(h->File, offset, whence);
	if (result < 0)
		return INVALID_SET_FILE_POINTER;
	if (distance_high)
		*distance_high = (LONG)((uint64_t)result >> 32);
	return (DWORD)result;
}

DWORD GetFileSize(HANDLE file, DWORD * size_high)
{
	CompatHandle * h = static_cast<CompatHandle *>(file);
	struct stat info;
	if (!h || file == INVALID_HANDLE_VALUE || h->Kind != HANDLE_FILE || fstat(h->File, &info) != 0)
		return (DWORD)-1;
	if (size_high)
		*size_high = (DWORD)((uint64_t)info.st_size >> 32);
	return (DWORD)info.st_size;
}
