// TheSuperHackers @build O154 (headless port) <mbstring.h> on macOS and Linux: the multibyte character count the IME
// code uses (IMEManager; there is no input method editor on these platforms).
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <wchar.h>
inline size_t _mbsnccnt(const unsigned char * text, size_t bytes)
{
	size_t count = 0;
	mbstate_t state = mbstate_t();
	size_t i = 0;
	while (i < bytes && text[i])
	{
		size_t n = mbrlen((const char *)text + i, bytes - i, &state);
		if (n == (size_t)-1 || n == (size_t)-2 || n == 0)
			n = 1;
		i += n;
		++count;
	}
	return count;
}
