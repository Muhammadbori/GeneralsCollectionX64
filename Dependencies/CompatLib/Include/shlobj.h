// TheSuperHackers @build O154 (headless port) <shlobj.h> on macOS and Linux: the special folder calls find no folder.
#pragma once
#include "windows_compat.h"
typedef void * LPITEMIDLIST;
typedef const void * PCIDLIST_ABSOLUTE;
#define CSIDL_PERSONAL 0x0005
#define CSIDL_DESKTOPDIRECTORY 0x0010
#define CSIDL_APPDATA 0x001a
#define CSIDL_LOCAL_APPDATA 0x001c
inline BOOL SHGetSpecialFolderPathA(HWND, char * path, int, BOOL) { if (path) path[0] = 0; return FALSE; }
#define SHGetSpecialFolderPath SHGetSpecialFolderPathA
inline LONG SHGetSpecialFolderLocation(HWND, int, LPITEMIDLIST * list) { if (list) *list = nullptr; return -1; }
inline BOOL SHGetPathFromIDListA(const void *, char * path) { if (path) path[0] = 0; return FALSE; }
#define SHGetPathFromIDList SHGetPathFromIDListA
