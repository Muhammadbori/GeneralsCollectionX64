// TheSuperHackers @build O159 (headless port) <ddraw.h> on macOS and Linux: the DDS header flags the DDS reader uses
// (ddsfile.cpp keeps its own copy of the DDS header structures).
#pragma once
#include "windows_compat.h"
#define DDSCAPS2_CUBEMAP 0x00000200L
#define DDSCAPS2_VOLUME 0x00200000L
