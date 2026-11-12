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

// Registry.cpp
// Simple interface for storing/retrieving registry values
// Author: Matthew D. Campbell, December 2001

#include "Registry.h"
#include <string>
#include "WWLib/win.h"

#ifdef _WIN64
// The game's registry keys are in the 32-bit view (WOW6432Node), where the installers and the 32-bit
// executables keep them; a 64-bit process would read the native view by default.
#define GAME_REGISTRY_VIEW KEY_WOW64_32KEY
#else
#define GAME_REGISTRY_VIEW 0
#endif


// TheSuperHackers @feature -noRegistry on the command line, or an archive list next to the exe (a self-contained
// folder): the game's registry keys are neither read nor written. Same rule as CommandLine::isRegistryDisabled.
static bool registryDisabled()
{
	static int state = -1;
	if (state < 0)
	{
		state = 0;
		for (int arg = 1; arg < __argc; ++arg)
		{
			if (_stricmp(__argv[arg], "-noRegistry") == 0)
			{
				state = 1;
			}
		}
		if (state == 0)
		{
			char path[MAX_PATH];
			const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
			char *separator = (len > 0 && len < MAX_PATH) ? strrchr(path, '\\') : nullptr;
			if (separator != nullptr)
			{
				static const char *const names[] = { "Archives_Generals.txt", "Archives_ZeroHour.txt" };
				for (int i = 0; i < 2; ++i)
				{
					separator[1] = '\0';
					if (strlen(path) + strlen(names[i]) < MAX_PATH)
					{
						strcat(path, names[i]);
						if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
						{
							state = 1;
						}
					}
				}
			}
		}
	}
	return state == 1;
}


bool  getStringFromRegistry(HKEY root, std::string path, std::string key, std::string& val)
{
	if (registryDisabled())
	{
		return false;
	}

	HKEY handle;
	unsigned char buffer[256];
	unsigned long size = 256;
	unsigned long type;
	int returnValue;

	if ((returnValue = RegOpenKeyEx( root, path.c_str(), 0, KEY_READ | GAME_REGISTRY_VIEW, &handle )) == ERROR_SUCCESS)
	{
		returnValue = RegQueryValueEx(handle, key.c_str(), nullptr, &type, (unsigned char *) &buffer, &size);
		RegCloseKey( handle );
	}

	if (returnValue == ERROR_SUCCESS)
	{
		val = (char *)buffer;
		return true;
	}

	return false;
}

bool getUnsignedIntFromRegistry(HKEY root, std::string path, std::string key, unsigned int& val)
{
	if (registryDisabled())
	{
		return false;
	}

	HKEY handle;
	unsigned long buffer;
	unsigned long size = sizeof(buffer);
	unsigned long type;
	int returnValue;

	if ((returnValue = RegOpenKeyEx( root, path.c_str(), 0, KEY_READ | GAME_REGISTRY_VIEW, &handle )) == ERROR_SUCCESS)
	{
		returnValue = RegQueryValueEx(handle, key.c_str(), nullptr, &type, (unsigned char *) &buffer, &size);
		RegCloseKey( handle );
	}

	if (returnValue == ERROR_SUCCESS)
	{
		val = buffer;
		return true;
	}

	return false;
}

bool setStringInRegistry( HKEY root, std::string path, std::string key, std::string val)
{
	if (registryDisabled())
	{
		return false;
	}

	HKEY handle;
	unsigned long type;
	unsigned long returnValue;
	int size;
	char lpClass[] = "REG_NONE";

	if ((returnValue = RegCreateKeyEx( root, path.c_str(), 0, lpClass, REG_OPTION_NON_VOLATILE, KEY_WRITE | GAME_REGISTRY_VIEW, nullptr, &handle, nullptr )) == ERROR_SUCCESS)
	{
		type = REG_SZ;
		size = val.length()+1;
		returnValue = RegSetValueEx(handle, key.c_str(), 0, type, (unsigned char *)val.c_str(), size);
		RegCloseKey( handle );
	}

	return (returnValue == ERROR_SUCCESS);
}

bool setUnsignedIntInRegistry( HKEY root, std::string path, std::string key, unsigned int val)
{
	if (registryDisabled())
	{
		return false;
	}

	HKEY handle;
	unsigned long type;
	unsigned long returnValue;
	int size;
	char lpClass[] = "REG_NONE";

	if ((returnValue = RegCreateKeyEx( root, path.c_str(), 0, lpClass, REG_OPTION_NON_VOLATILE, KEY_WRITE | GAME_REGISTRY_VIEW, nullptr, &handle, nullptr )) == ERROR_SUCCESS)
	{
		type = REG_DWORD;
		size = 4;
		returnValue = RegSetValueEx(handle, key.c_str(), 0, type, (unsigned char *)&val, size);
		RegCloseKey( handle );
	}

	return (returnValue == ERROR_SUCCESS);
}

bool GetStringFromRegistry(std::string path, std::string key, std::string& val)
{
#if RTS_GENERALS
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";
#elif RTS_ZEROHOUR
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
#endif

	fullPath.append(path);
	if (getStringFromRegistry(HKEY_CURRENT_USER, fullPath.c_str(), key.c_str(), val))
	{
		return true;
	}

	return getStringFromRegistry(HKEY_LOCAL_MACHINE, fullPath.c_str(), key.c_str(), val);
}

bool GetUnsignedIntFromRegistry(std::string path, std::string key, unsigned int& val)
{
#if RTS_GENERALS
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";
#elif RTS_ZEROHOUR
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
#endif

	fullPath.append(path);
	if (getUnsignedIntFromRegistry(HKEY_CURRENT_USER, fullPath.c_str(), key.c_str(), val))
	{
		return true;
	}

	return getUnsignedIntFromRegistry(HKEY_LOCAL_MACHINE, fullPath.c_str(), key.c_str(), val);
}

bool SetStringInRegistry( std::string path, std::string key, std::string val)
{
#if RTS_GENERALS
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";
#elif RTS_ZEROHOUR
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
#endif
	fullPath.append(path);

	// TheSuperHackers @fix bobtista 12/02/2026 Always write to HKCU. Per-user settings belong
	// in HKEY_CURRENT_USER and writes there should always succeed without admin privileges.
	return setStringInRegistry( HKEY_CURRENT_USER, fullPath, key, val );
}

bool SetUnsignedIntInRegistry( std::string path, std::string key, unsigned int val)
{
#if RTS_GENERALS
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";
#elif RTS_ZEROHOUR
	std::string fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
#endif
	fullPath.append(path);

	// TheSuperHackers @fix bobtista 12/02/2026 Always write to HKCU. Per-user settings belong
	// in HKEY_CURRENT_USER and writes there should always succeed without admin privileges.
	return setUnsignedIntInRegistry( HKEY_CURRENT_USER, fullPath, key, val );
}

