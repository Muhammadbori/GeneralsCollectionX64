// TheSuperHackers @build O154 (headless port) <intrin.h> on macOS and Linux (x64 and arm64): __cpuid for the CPU name in
// the mismatch report (diagnostics only). It reports no brand string, so the report says "unknown".
#pragma once
#include <string.h>
inline void __cpuid(int regs[4], int) { memset(regs, 0, 4 * sizeof(int)); }
