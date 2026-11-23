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

// TheSuperHackers @build O154 See windows_compat_o154.h: command line, number to text, wide variants, directory
// listing, date and time text and random bytes, written with POSIX.

#include "windows_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

int __argc = 0;
char ** __argv = nullptr;

namespace
{
	// The C runtime start-up passes argc and argv to the initialisers on glibc; macOS has _NSGetArgc/_NSGetArgv.
#ifdef __APPLE__
	__attribute__((constructor)) void Init_Command_Line()
	{
		__argc = *_NSGetArgc();
		__argv = *_NSGetArgv();
	}
#else
	__attribute__((constructor)) void Init_Command_Line(int argc, char ** argv, char **)
	{
		__argc = argc;
		__argv = argv;
	}
#endif

	std::wstring Widen(const char * text)
	{
		std::wstring result;
		if (!text)
			return result;
		size_t length = mbstowcs(nullptr, text, 0);
		if (length == (size_t)-1)
		{
			// Not valid in the current locale: byte by byte.
			for (const char * p = text; *p; ++p)
				result.push_back((wchar_t)(unsigned char)*p);
			return result;
		}
		result.resize(length);
		mbstowcs(&result[0], text, length);
		return result;
	}

	std::string Narrow(const wchar_t * text)
	{
		std::string result;
		if (!text)
			return result;
		size_t length = wcstombs(nullptr, text, 0);
		if (length == (size_t)-1)
		{
			for (const wchar_t * p = text; *p; ++p)
				result.push_back(*p < 0x80 ? (char)*p : '?');
			return result;
		}
		result.resize(length);
		wcstombs(&result[0], text, length);
		return result;
	}

	DWORD Copy_Wide(const std::wstring & text, wchar_t * buffer, DWORD size)
	{
		if (!buffer || size == 0)
			return 0;
		DWORD count = (DWORD)text.size();
		if (count >= size)
			count = size - 1;
		wmemcpy(buffer, text.c_str(), count);
		buffer[count] = 0;
		return count;
	}

	template <typename T> char * Unsigned_To_Text(T value, char * buffer, int radix, bool negative)
	{
		if (!buffer)
			return buffer;
		if (radix < 2 || radix > 36)
		{
			buffer[0] = 0;
			return buffer;
		}
		char digits[72];
		int count = 0;
		do
		{
			int digit = (int)(value % (T)radix);
			digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
			value /= (T)radix;
		} while (value != 0);
		char * out = buffer;
		if (negative)
			*out++ = '-';
		while (count > 0)
			*out++ = digits[--count];
		*out = 0;
		return buffer;
	}

	void To_File_Time(time_t seconds, FILETIME * out)
	{
		// 100 ns steps since 1601-01-01.
		uint64_t value = ((uint64_t)seconds + 11644473600ull) * 10000000ull;
		out->dwLowDateTime = (DWORD)value;
		out->dwHighDateTime = (DWORD)(value >> 32);
	}

	struct FindHandle
	{
		DIR * Directory;
		std::string Folder;
		std::string Pattern;
	};

	bool Fill_Find_Data(FindHandle * find, WIN32_FIND_DATAA * data)
	{
		for (;;)
		{
			struct dirent * entry = readdir(find->Directory);
			if (!entry)
				return false;
			if (fnmatch(find->Pattern.c_str(), entry->d_name, 0) != 0)
				continue;
			memset(data, 0, sizeof(*data));
			strncpy(data->cFileName, entry->d_name, MAX_PATH - 1);
			std::string path = find->Folder.empty() ? std::string(entry->d_name) : find->Folder + "/" + entry->d_name;
			struct stat info;
			if (stat(path.c_str(), &info) == 0)
			{
				data->dwFileAttributes = S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
				data->nFileSizeLow = (DWORD)info.st_size;
				data->nFileSizeHigh = (DWORD)((uint64_t)info.st_size >> 32);
				To_File_Time(info.st_mtime, &data->ftLastWriteTime);
				To_File_Time(info.st_atime, &data->ftLastAccessTime);
				To_File_Time(info.st_ctime, &data->ftCreationTime);
			}
			else
			{
				data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
			}
			return true;
		}
	}

	struct tm To_Tm(const SYSTEMTIME * time)
	{
		struct tm result;
		memset(&result, 0, sizeof(result));
		if (time)
		{
			result.tm_year = time->wYear - 1900;
			result.tm_mon = time->wMonth - 1;
			result.tm_mday = time->wDay;
			result.tm_wday = time->wDayOfWeek;
			result.tm_hour = time->wHour;
			result.tm_min = time->wMinute;
			result.tm_sec = time->wSecond;
		}
		else
		{
			time_t now = ::time(nullptr);
			localtime_r(&now, &result);
		}
		return result;
	}

	int Copy_Text(const char * text, char * buffer, int size)
	{
		int length = (int)strlen(text) + 1;
		if (!buffer || size == 0)
			return length;
		if (length > size)
			return 0;
		memcpy(buffer, text, length);
		return length;
	}

	int Copy_Text(const char * text, wchar_t * buffer, int size)
	{
		std::wstring wide = Widen(text);
		int length = (int)wide.size() + 1;
		if (!buffer || size == 0)
			return length;
		if (length > size)
			return 0;
		wmemcpy(buffer, wide.c_str(), length);
		return length;
	}

	// The Windows picture strings the game passes (d, dd, M, MM, yy, yyyy, h, hh, H, HH, m, mm, s, ss, t, tt) in strftime terms.
	std::string Picture_To_Strftime(const std::string & picture)
	{
		std::string result;
		for (size_t i = 0; i < picture.size();)
		{
			char c = picture[i];
			size_t run = 1;
			while (i + run < picture.size() && picture[i + run] == c)
				++run;
			switch (c)
			{
				case 'd': result += run >= 4 ? "%A" : (run == 3 ? "%a" : "%d"); break;
				case 'M': result += run >= 4 ? "%B" : (run == 3 ? "%b" : "%m"); break;
				case 'y': result += run >= 3 ? "%Y" : "%y"; break;
				case 'h': result += "%I"; break;
				case 'H': result += "%H"; break;
				case 'm': result += "%M"; break;
				case 's': result += "%S"; break;
				case 't': result += "%p"; break;
				case '%': result += "%%"; break;
				default: result.append(run, c); break;
			}
			i += run;
		}
		return result;
	}

	std::string Format_Date(DWORD flags, const SYSTEMTIME * time, const char * picture)
	{
		struct tm t = To_Tm(time);
		std::string format = picture ? Picture_To_Strftime(picture) : std::string((flags & DATE_LONGDATE) ? "%x" : "%x");
		char text[256];
		size_t length = strftime(text, sizeof(text), format.c_str(), &t);
		text[length] = 0;
		return text;
	}

	std::string Format_Time(DWORD flags, const SYSTEMTIME * time, const char * picture)
	{
		struct tm t = To_Tm(time);
		std::string format;
		if (picture)
			format = Picture_To_Strftime(picture);
		else if (flags & TIME_NOMINUTESORSECONDS)
			format = (flags & TIME_FORCE24HOURFORMAT) ? "%H" : "%I %p";
		else if (flags & TIME_NOSECONDS)
			format = (flags & TIME_FORCE24HOURFORMAT) ? "%H:%M" : "%I:%M %p";
		else
			format = (flags & TIME_FORCE24HOURFORMAT) ? "%H:%M:%S" : "%X";
		char text[256];
		size_t length = strftime(text, sizeof(text), format.c_str(), &t);
		text[length] = 0;
		return text;
	}
}

const wchar_t * GetCommandLineW(void)
{
	static std::wstring line;
	if (line.empty())
	{
		for (int i = 0; i < __argc; ++i)
		{
			if (i > 0)
				line += L' ';
			std::wstring arg = Widen(__argv[i]);
			bool quote = arg.find(L' ') != std::wstring::npos;
			if (quote)
				line += L'"';
			line += arg;
			if (quote)
				line += L'"';
		}
	}
	return line.c_str();
}

char * itoa(int value, char * buffer, int radix)
{
	if (radix == 10 && value < 0)
		return Unsigned_To_Text<unsigned int>(0u - (unsigned int)value, buffer, radix, true);
	return Unsigned_To_Text<unsigned int>((unsigned int)value, buffer, radix, false);
}

char * _ultoa(unsigned long value, char * buffer, int radix)
{
	return Unsigned_To_Text<unsigned long>(value, buffer, radix, false);
}

char * _i64toa(long long value, char * buffer, int radix)
{
	if (radix == 10 && value < 0)
		return Unsigned_To_Text<unsigned long long>(0ull - (unsigned long long)value, buffer, radix, true);
	return Unsigned_To_Text<unsigned long long>((unsigned long long)value, buffer, radix, false);
}

char * _ui64toa(unsigned long long value, char * buffer, int radix)
{
	return Unsigned_To_Text<unsigned long long>(value, buffer, radix, false);
}

DWORD GetModuleFileNameW(HMODULE module, wchar_t * buffer, DWORD size)
{
	char path[PATH_MAX];
	if (GetModuleFileNameA(module, path, sizeof(path)) == 0)
		return 0;
	return Copy_Wide(Widen(path), buffer, size);
}

DWORD GetCurrentDirectoryW(DWORD size, wchar_t * buffer)
{
	char path[PATH_MAX];
	if (!getcwd(path, sizeof(path)))
		return 0;
	return Copy_Wide(Widen(path), buffer, size);
}

DWORD GetFileAttributesW(const wchar_t * path)
{
	return GetFileAttributesA(Narrow(path).c_str());
}

DWORD FormatMessageW(DWORD flags, const void * source, DWORD message_id, DWORD language_id, wchar_t * buffer, DWORD size, void * arguments)
{
	char text[512];
	if (FormatMessageA(flags, source, message_id, language_id, text, sizeof(text), arguments) == 0)
		return 0;
	return Copy_Wide(Widen(text), buffer, size);
}

DWORD GetFullPathNameA(const char * name, DWORD size, char * buffer, char ** file_part)
{
	if (!name)
		return 0;
	std::string full;
	if (name[0] == '/')
	{
		full = name;
	}
	else
	{
		char folder[PATH_MAX];
		if (!getcwd(folder, sizeof(folder)))
			return 0;
		full = std::string(folder) + "/" + name;
	}
	if (!buffer || full.size() + 1 > size)
		return (DWORD)full.size() + 1;
	memcpy(buffer, full.c_str(), full.size() + 1);
	if (file_part)
	{
		char * slash = strrchr(buffer, '/');
		*file_part = slash ? slash + 1 : buffer;
	}
	return (DWORD)full.size();
}

HANDLE FindFirstFileA(const char * pattern, WIN32_FIND_DATAA * data)
{
	if (!pattern || !data)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	std::string text(pattern);
	for (char & c : text)
		if (c == '\\')
			c = '/';
	size_t slash = text.rfind('/');
	FindHandle * find = new FindHandle;
	find->Folder = slash == std::string::npos ? std::string() : text.substr(0, slash);
	find->Pattern = slash == std::string::npos ? text : text.substr(slash + 1);
	if (find->Pattern == "*.*")
		find->Pattern = "*";
	find->Directory = opendir(find->Folder.empty() ? "." : find->Folder.c_str());
	if (!find->Directory || !Fill_Find_Data(find, data))
	{
		if (find->Directory)
			closedir(find->Directory);
		delete find;
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return find;
}

BOOL FindNextFileA(HANDLE handle, WIN32_FIND_DATAA * data)
{
	FindHandle * find = static_cast<FindHandle *>(handle);
	if (!find || handle == INVALID_HANDLE_VALUE || !data)
		return FALSE;
	if (!Fill_Find_Data(find, data))
	{
		SetLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	return TRUE;
}

BOOL FindClose(HANDLE handle)
{
	FindHandle * find = static_cast<FindHandle *>(handle);
	if (!find || handle == INVALID_HANDLE_VALUE)
		return FALSE;
	closedir(find->Directory);
	delete find;
	return TRUE;
}

BOOL GetFileAttributesExA(const char * path, GET_FILEEX_INFO_LEVELS, void * info)
{
	struct stat st;
	if (!path || !info || stat(path, &st) != 0)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	WIN32_FILE_ATTRIBUTE_DATA * data = static_cast<WIN32_FILE_ATTRIBUTE_DATA *>(info);
	memset(data, 0, sizeof(*data));
	data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	data->nFileSizeLow = (DWORD)st.st_size;
	data->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
	To_File_Time(st.st_mtime, &data->ftLastWriteTime);
	To_File_Time(st.st_atime, &data->ftLastAccessTime);
	To_File_Time(st.st_ctime, &data->ftCreationTime);
	return TRUE;
}

int GetDateFormatA(DWORD, DWORD flags, const SYSTEMTIME * time, const char * format, char * buffer, int size)
{
	return Copy_Text(Format_Date(flags, time, format).c_str(), buffer, size);
}

int GetTimeFormatA(DWORD, DWORD flags, const SYSTEMTIME * time, const char * format, char * buffer, int size)
{
	return Copy_Text(Format_Time(flags, time, format).c_str(), buffer, size);
}

int GetDateFormatW(DWORD, DWORD flags, const SYSTEMTIME * time, const wchar_t * format, wchar_t * buffer, int size)
{
	std::string picture = Narrow(format);
	return Copy_Text(Format_Date(flags, time, format ? picture.c_str() : nullptr).c_str(), buffer, size);
}

int GetTimeFormatW(DWORD, DWORD flags, const SYSTEMTIME * time, const wchar_t * format, wchar_t * buffer, int size)
{
	std::string picture = Narrow(format);
	return Copy_Text(Format_Time(flags, time, format ? picture.c_str() : nullptr).c_str(), buffer, size);
}

extern "C" BOOLEAN SystemFunction036(PVOID buffer, ULONG length)
{
#ifdef __APPLE__
	arc4random_buf(buffer, length);
	return TRUE;
#else
	int file = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return FALSE;
	unsigned char * out = static_cast<unsigned char *>(buffer);
	ULONG done = 0;
	while (done < length)
	{
		ssize_t n = read(file, out + done, length - done);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			close(file);
			return FALSE;
		}
		done += (ULONG)n;
	}
	close(file);
	return TRUE;
#endif
}
