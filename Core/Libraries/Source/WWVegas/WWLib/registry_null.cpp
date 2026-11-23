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

// TheSuperHackers @build O162 (headless port) RegistryClass on macOS and Linux, in place of registry.cpp: there is no
// registry, so every key is invalid, reads give the defaults and writes are dropped (as on Windows when the key cannot
// be opened, or with -noRegistry).

#include "registry.h"

#include <string.h>

bool RegistryClass::IsLocked = false;

bool RegistryClass::Exists(const char *) { return false; }

RegistryClass::RegistryClass(const char *, bool) : Key(0), IsValid(false) {}
RegistryClass::~RegistryClass() {}

int RegistryClass::Get_Int(const char *, int def_value) { return def_value; }
void RegistryClass::Set_Int(const char *, int) {}
bool RegistryClass::Get_Bool(const char *, bool def_value) { return def_value; }
void RegistryClass::Set_Bool(const char *, bool) {}
float RegistryClass::Get_Float(const char *, float def_value) { return def_value; }
void RegistryClass::Set_Float(const char *, float) {}

char * RegistryClass::Get_String(const char *, char * value, int value_size, const char * default_string)
{
	if (value && value_size > 0) {
		value[0] = 0;
		if (default_string) {
			strncpy(value, default_string, value_size - 1);
			value[value_size - 1] = 0;
		}
	}
	return value;
}

void RegistryClass::Get_String(const char *, StringClass & string, const char * default_string)
{
	string = default_string ? default_string : "";
}

void RegistryClass::Set_String(const char *, const char *) {}

void RegistryClass::Get_String(const WCHAR *, WideStringClass & string, const WCHAR * default_string)
{
	string = default_string ? default_string : L"";
}

void RegistryClass::Set_String(const WCHAR *, const WCHAR *) {}
void RegistryClass::Get_Bin(const char *, void *, int) {}
int RegistryClass::Get_Bin_Size(const char *) { return 0; }
void RegistryClass::Set_Bin(const char *, const void *, int) {}
void RegistryClass::Get_Value_List(DynamicVectorClass<StringClass> &) {}
void RegistryClass::Delete_Value(const char *) {}
void RegistryClass::Deleta_All_Values() {}
void RegistryClass::Delete_Registry_Tree(char *) {}
void RegistryClass::Load_Registry(const char *, char *, char *) {}
void RegistryClass::Save_Registry(const char *, char *) {}
