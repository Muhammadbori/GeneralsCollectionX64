// TheSuperHackers @build O155 (headless port) <knownfolders.h> on macOS and Linux: the folder ids the game names. The
// Windows call that resolves them (SHGetKnownFolderPath in shell32) is not there, so the lookups find no folder.
#pragma once
#include "windows_compat.h"
#include <stdlib.h>
typedef GUID KNOWNFOLDERID;
static const GUID FOLDERID_Documents = { 0xFDD39AD0, 0x238F, 0x46AF, { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } };
static const GUID FOLDERID_LocalAppData = { 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };
inline void CoTaskMemFree(void * memory) { free(memory); }
