// TheSuperHackers @build O159 (headless port) <direct.h> on macOS and Linux: the directory calls of the C runtime.
#pragma once
#include <sys/stat.h>
#include <unistd.h>
inline int _mkdir(const char * path) { return mkdir(path, 0755); }
inline char * _getcwd(char * buffer, int size) { return getcwd(buffer, (size_t)size); }
inline int _chdir(const char * path) { return chdir(path); }
inline int _rmdir(const char * path) { return rmdir(path); }
