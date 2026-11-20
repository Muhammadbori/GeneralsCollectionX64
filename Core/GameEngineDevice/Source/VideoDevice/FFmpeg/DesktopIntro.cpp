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

// TheSuperHackers @feature O108 (band 61) The EA logo over the desktop (DesktopIntro.h).
//
// It runs in WinMain before the game engine, the file system and the game window exist, so it uses plain Win32,
// FFmpeg and miniaudio only: no game header, no game file system (the movie is a loose file).

#include "VideoDevice/FFmpeg/DesktopIntro.h"

#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef RTS_HAS_MINIAUDIO
#include "miniaudio.h"
#endif

#pragma comment(lib, "winmm.lib")

namespace
{

const int kAudioRate = 48000;
const int kMaxAudioSeconds = 30;   // a longer sound is cut there
const double kMaxRunSeconds = 30.0; // whatever happens, the intro window is gone after this
const int kSkipFadeMs = 150;

volatile LONG s_skip = 0;

void introLog(const char *text)
{
	OutputDebugStringA("DesktopIntro: ");
	OutputDebugStringA(text);
	OutputDebugStringA("\n");
}

//-------------------------------------------------------------------------------------------------
// Command line

bool matchesSkipSwitch(const wchar_t *arg)
{
	static const wchar_t *const exact[] = {
		L"-nologo", L"-headless", L"-file", L"-skirmish", L"-skirmishAI", L"-replay", L"-loadreplay",
		L"-followReplay", L"-watchReplay", L"-map", L"-benchmark", L"-forceBenchmark", L"-quickstart",
		L"-jumpToFrame", L"-uiScript", L"-restartWait",
	};
	static const wchar_t *const prefixes[] = { L"-shot", L"-test", L"-netLab", L"-serveReplay" };

	for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i)
		if (_wcsicmp(arg, exact[i]) == 0)
			return true;
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
		if (_wcsnicmp(arg, prefixes[i], wcslen(prefixes[i])) == 0)
			return true;

	// a replay or a map handed over by a double click
	const size_t n = wcslen(arg);
	if (n > 4 && (_wcsicmp(arg + n - 4, L".rep") == 0 || _wcsicmp(arg + n - 4, L".map") == 0))
		return true;
	return false;
}

//-------------------------------------------------------------------------------------------------
// Files

bool exeDirectory(wchar_t *out, size_t outCount)
{
	DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)outCount);
	if (n == 0 || n >= outCount)
		return false;
	wchar_t *slash = wcsrchr(out, L'\\');
	if (slash == nullptr)
		return false;
	slash[1] = 0;
	return true;
}

// Fills utf8Path with the movie to play, the one closer to the monitor's shape first. FALSE when there is none.
bool findMovie(double monitorAspect, char *utf8Path, int utf8Count)
{
	wchar_t dir[MAX_PATH];
	if (!exeDirectory(dir, MAX_PATH))
		return false;

	const wchar_t *const wide = L"Data\\Intro\\EALogo_16x9.webm";
	const wchar_t *const tall = L"Data\\Intro\\EALogo_16x10.webm";
	// 16:9 is 1.78 and 16:10 is 1.6: below the middle the 16:10 movie fits better (also 4:3 and 5:4)
	const wchar_t *const order[2] = { monitorAspect < 1.69 ? tall : wide, monitorAspect < 1.69 ? wide : tall };

	for (int i = 0; i < 2; ++i)
	{
		wchar_t path[MAX_PATH * 2];
		wcscpy_s(path, dir);
		wcscat_s(path, order[i]);
		if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
			continue;
		// FFmpeg takes UTF-8 file names on Windows
		if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8Path, utf8Count, nullptr, nullptr) > 0)
			return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
// Sound: decoded whole before the start (a few seconds), then played by a miniaudio device.

struct AudioClip
{
	float *samples;          // interleaved stereo
	size_t frames;
	volatile LONG64 position;
	volatile LONG fading;    // set on a skip: a short fade, then silence
	float gain;
};

bool decodeAudio(const char *path, AudioClip &clip)
{
	AVFormatContext *format = nullptr;
	if (avformat_open_input(&format, path, nullptr, nullptr) < 0)
		return false;

	bool ok = false;
	const AVCodec *codec = nullptr;
	int stream = -1;
	if (avformat_find_stream_info(format, nullptr) >= 0)
		stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);

	AVCodecContext *context = (stream >= 0 && codec != nullptr) ? avcodec_alloc_context3(codec) : nullptr;
	if (context != nullptr
		&& avcodec_parameters_to_context(context, format->streams[stream]->codecpar) >= 0
		&& avcodec_open2(context, codec, nullptr) >= 0)
	{
		SwrContext *resampler = nullptr;
		AVChannelLayout stereo;
		av_channel_layout_default(&stereo, 2);
		if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT, kAudioRate,
				&context->ch_layout, context->sample_fmt, context->sample_rate, 0, nullptr) >= 0
			&& swr_init(resampler) >= 0)
		{
			const size_t capacity = (size_t)kAudioRate * kMaxAudioSeconds;
			clip.samples = (float *)malloc(capacity * 2 * sizeof(float));
			AVPacket *packet = av_packet_alloc();
			AVFrame *frame = av_frame_alloc();
			if (clip.samples != nullptr && packet != nullptr && frame != nullptr)
			{
				bool reading = true;
				while (true)
				{
					if (reading)
					{
						if (av_read_frame(format, packet) < 0)
						{
							reading = false;
							avcodec_send_packet(context, nullptr);
						}
						else
						{
							if (packet->stream_index == stream)
								avcodec_send_packet(context, packet);
							av_packet_unref(packet);
						}
					}

					while (avcodec_receive_frame(context, frame) >= 0)
					{
						const int room = (int)(capacity - clip.frames);
						if (room > 0)
						{
							uint8_t *out = (uint8_t *)(clip.samples + clip.frames * 2);
							const int got = swr_convert(resampler, &out, room,
								(const uint8_t *const *)frame->extended_data, frame->nb_samples);
							if (got > 0)
								clip.frames += (size_t)got;
						}
						av_frame_unref(frame);
					}

					if (!reading)
						break;
				}

				// what the resampler still holds
				const int room = (int)(capacity - clip.frames);
				if (room > 0)
				{
					uint8_t *out = (uint8_t *)(clip.samples + clip.frames * 2);
					const int got = swr_convert(resampler, &out, room, nullptr, 0);
					if (got > 0)
						clip.frames += (size_t)got;
				}
				ok = clip.frames > 0;
			}
			av_frame_free(&frame);
			av_packet_free(&packet);
		}
		swr_free(&resampler);
	}
	avcodec_free_context(&context);
	avformat_close_input(&format);
	return ok;
}

#ifdef RTS_HAS_MINIAUDIO
void audioCallback(ma_device *device, void *output, const void *, ma_uint32 frameCount)
{
	AudioClip *clip = (AudioClip *)device->pUserData;
	float *out = (float *)output;
	size_t position = (size_t)clip->position;
	const float fadeStep = 1.0f / (kAudioRate * kSkipFadeMs / 1000);

	for (ma_uint32 i = 0; i < frameCount; ++i)
	{
		float l = 0.0f, r = 0.0f;
		if (position < clip->frames && clip->gain > 0.0f)
		{
			l = clip->samples[position * 2] * clip->gain;
			r = clip->samples[position * 2 + 1] * clip->gain;
			++position;
			if (clip->fading)
				clip->gain = clip->gain > fadeStep ? clip->gain - fadeStep : 0.0f;
		}
		out[i * 2] = l;
		out[i * 2 + 1] = r;
	}
	InterlockedExchange64(&clip->position, (LONG64)position);
}
#endif

#ifdef RTS_HAS_MINIAUDIO
// O109: the sound may outlast the picture; it then plays on while the game starts, and this thread closes it.
AudioClip s_clip = {};
ma_device s_device;

DWORD WINAPI soundTailThread(LPVOID)
{
	const DWORD until = GetTickCount() + 5000;
	while ((size_t)s_clip.position < s_clip.frames && (LONG)(until - GetTickCount()) > 0)
		Sleep(20);
	ma_device_uninit(&s_device);
	free(s_clip.samples);
	s_clip.samples = nullptr;
	return 0;
}
#endif

//-------------------------------------------------------------------------------------------------
// Window

LRESULT CALLBACK introWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE || wParam == VK_SPACE || wParam == VK_RETURN)
			InterlockedExchange(&s_skip, 1);
		return 0;
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_CLOSE:
		InterlockedExchange(&s_skip, 1);
		return 0;
	case WM_SETCURSOR:
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

// O109: only the intro window's messages. The game window (with the splash picture) already exists; its messages
// stay queued for the game, as they did while the engine starts.
HWND s_window = nullptr;

void pumpMessages()
{
	if (s_window == nullptr)
		return;
	MSG msg;
	while (PeekMessageW(&msg, s_window, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

struct Surface
{
	HWND window;
	HDC memoryDC;
	HBITMAP bitmap;
	HGDIOBJ oldBitmap;
	uint8_t *bits;   // top-down BGRA, premultiplied alpha
	int x, y, width, height;
};

bool createSurface(HINSTANCE instance, Surface &s)
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = introWindowProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"GeneralsDesktopIntro";
	RegisterClassW(&wc);

	// WS_EX_TOOLWINDOW: no taskbar button for these few seconds
	s.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"",
		WS_POPUP, s.x, s.y, s.width, s.height, nullptr, nullptr, instance, nullptr);
	if (s.window == nullptr)
		return false;
	s_window = s.window;

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = s.width;
	info.bmiHeader.biHeight = -s.height; // top-down
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	HDC screen = GetDC(nullptr);
	s.memoryDC = CreateCompatibleDC(screen);
	void *bits = nullptr;
	s.bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (s.memoryDC == nullptr || s.bitmap == nullptr)
		return false;
	s.bits = (uint8_t *)bits;
	s.oldBitmap = SelectObject(s.memoryDC, s.bitmap);
	return true;
}

void present(Surface &s, BYTE opacity)
{
	GdiFlush();
	POINT position = { s.x, s.y };
	SIZE size = { s.width, s.height };
	POINT origin = { 0, 0 };
	BLENDFUNCTION blend = { AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA };
	UpdateLayeredWindow(s.window, nullptr, &position, &size, s.memoryDC, &origin, 0, &blend, ULW_ALPHA);
}

void destroySurface(HINSTANCE instance, Surface &s)
{
	if (s.window != nullptr)
		DestroyWindow(s.window);
	if (s.memoryDC != nullptr)
	{
		if (s.oldBitmap != nullptr)
			SelectObject(s.memoryDC, s.oldBitmap);
		DeleteDC(s.memoryDC);
	}
	if (s.bitmap != nullptr)
		DeleteObject(s.bitmap);
	UnregisterClassW(L"GeneralsDesktopIntro", instance);
	s_window = nullptr;
}

// UpdateLayeredWindow wants premultiplied colours; swscale gives straight alpha. Returns the highest alpha.
unsigned premultiply(uint8_t *bits, size_t pixelCount)
{
	unsigned highest = 0;
	for (size_t i = 0; i < pixelCount; ++i, bits += 4)
	{
		const unsigned a = bits[3];
		if (a > highest)
			highest = a;
		if (a == 255)
			continue;
		if (a == 0)
		{
			bits[0] = bits[1] = bits[2] = 0;
			continue;
		}
		bits[0] = (uint8_t)((bits[0] * a + 127) / 255);
		bits[1] = (uint8_t)((bits[1] * a + 127) / 255);
		bits[2] = (uint8_t)((bits[2] * a + 127) / 255);
	}
	return highest;
}

//-------------------------------------------------------------------------------------------------
// Clock

double secondsSince(const LARGE_INTEGER &start)
{
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

// Waits until the clock reaches target, keeping the window alive. FALSE when skipped.
bool waitUntil(const LARGE_INTEGER &start, double target)
{
	while (!s_skip)
	{
		pumpMessages();
		const double left = target - secondsSince(start);
		if (left <= 0.0)
			return true;
		// short sleeps: the game window's queued messages must not wake a wait over and over
		const DWORD ms = (DWORD)(left * 1000.0);
		Sleep(ms < 4 ? ms : 4);
	}
	return false;
}

// a decoder that keeps the alpha channel: FFmpeg's own vp8/vp9 decoders drop it, libvpx keeps it
const AVCodec *findVideoDecoder(AVCodecID id)
{
	const AVCodec *codec = nullptr;
	if (id == AV_CODEC_ID_VP9)
		codec = avcodec_find_decoder_by_name("libvpx-vp9");
	else if (id == AV_CODEC_ID_VP8)
		codec = avcodec_find_decoder_by_name("libvpx");
	return codec != nullptr ? codec : avcodec_find_decoder(id);
}

} // namespace

//-------------------------------------------------------------------------------------------------
bool DesktopIntro::isSkippedByCommandLine()
{
	// split the command line like the C runtime does (quotes group, the first word is the program)
	const wchar_t *p = GetCommandLineW();
	if (p == nullptr)
		return false;

	bool first = true;
	wchar_t arg[1024];
	while (*p != 0)
	{
		while (*p == L' ' || *p == L'\t')
			++p;
		if (*p == 0)
			break;
		size_t n = 0;
		bool quoted = false;
		while (*p != 0 && (quoted || (*p != L' ' && *p != L'\t')))
		{
			if (*p == L'"')
				quoted = !quoted;
			else if (n + 1 < sizeof(arg) / sizeof(arg[0]))
				arg[n++] = *p;
			++p;
		}
		arg[n] = 0;
		if (!first && matchesSkipSwitch(arg))
			return true;
		first = false;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
bool DesktopIntro::play(HINSTANCE instance, HWND splashWindow, DWORD splashShownAt, DWORD splashMinMs)
{
	InterlockedExchange(&s_skip, 0);

	// the primary monitor
	MONITORINFO monitor = {};
	monitor.cbSize = sizeof(monitor);
	const POINT origin = { 0, 0 };
	if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor))
		return false;
	const int monitorWidth = monitor.rcMonitor.right - monitor.rcMonitor.left;
	const int monitorHeight = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
	if (monitorWidth <= 0 || monitorHeight <= 0)
		return false;

	char path[MAX_PATH * 3];
	if (!findMovie((double)monitorWidth / monitorHeight, path, sizeof(path)))
	{
		introLog("no Data\\Intro\\EALogo_16x9.webm or EALogo_16x10.webm");
		return false;
	}

	// open the picture
	AVFormatContext *format = nullptr;
	if (avformat_open_input(&format, path, nullptr, nullptr) < 0)
	{
		introLog("cannot open the movie");
		return false;
	}

	bool shown = false;
	AVCodecContext *video = nullptr;
	SwsContext *scaler = nullptr;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	Surface surface = {};
#ifdef RTS_HAS_MINIAUDIO
	AudioClip &clip = s_clip;
	ma_device &device = s_device;
	bool deviceStarted = false;
	bool soundHandedOff = false;
#else
	AudioClip clip = {};
#endif
	clip.gain = 1.0f;
	timeBeginPeriod(1);

	int stream = -1;
	if (avformat_find_stream_info(format, nullptr) >= 0)
		stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (stream >= 0 && packet != nullptr && frame != nullptr)
	{
		const AVCodecParameters *parameters = format->streams[stream]->codecpar;
		const AVCodec *codec = findVideoDecoder(parameters->codec_id);
		video = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
		if (video != nullptr && avcodec_parameters_to_context(video, parameters) >= 0)
			video->thread_count = 0; // as many as the processor has
		if (video == nullptr || avcodec_open2(video, codec, nullptr) < 0)
		{
			introLog("no decoder for the movie");
			avcodec_free_context(&video);
		}
	}

	if (video != nullptr)
	{
		const AVRational timeBase = format->streams[stream]->time_base;
		const AVRational rate = av_guess_frame_rate(format, format->streams[stream], nullptr);
		const double frameSeconds = (rate.num > 0 && rate.den > 0) ? av_q2d(av_inv_q(rate)) : 1.0 / 25.0;
		int64_t firstPts = AV_NOPTS_VALUE;
		int64_t frameIndex = 0;
		LARGE_INTEGER start = {};
		bool reading = true;
		bool failed = false;
		// O109: the movie's last part is see-through; when the picture has gone, the start goes on at once
		bool seenPicture = false;
		bool pictureGone = false;
		int emptyFrames = 0;

		while (!s_skip && !failed && !pictureGone)
		{
			if (reading)
			{
				if (av_read_frame(format, packet) < 0)
				{
					reading = false;
					avcodec_send_packet(video, nullptr);
				}
				else
				{
					if (packet->stream_index == stream)
						avcodec_send_packet(video, packet);
					av_packet_unref(packet);
				}
			}

			while (!s_skip && !failed && !pictureGone && avcodec_receive_frame(video, frame) >= 0)
			{
				const int64_t pts = frame->best_effort_timestamp;
				if (firstPts == AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE)
					firstPts = pts;
				const double at = (pts != AV_NOPTS_VALUE && firstPts != AV_NOPTS_VALUE)
					? (double)(pts - firstPts) * av_q2d(timeBase) : (double)frameIndex * frameSeconds;
				++frameIndex;

				if (!shown)
				{
					// the first picture: it must have an alpha channel, else nothing is shown at all
					const AVPixFmtDescriptor *pixel = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
					if (pixel == nullptr || !(pixel->flags & AV_PIX_FMT_FLAG_ALPHA) || frame->width <= 0 || frame->height <= 0)
					{
						introLog("the movie has no alpha channel (or the decoder dropped it)");
						failed = true;
						break;
					}

					// as large as fits on the monitor, centred
					const double scaleX = (double)monitorWidth / frame->width;
					const double scaleY = (double)monitorHeight / frame->height;
					const double scale = scaleX < scaleY ? scaleX : scaleY;
					surface.width = (int)(frame->width * scale + 0.5) > 0 ? (int)(frame->width * scale + 0.5) : 1;
					surface.height = (int)(frame->height * scale + 0.5) > 0 ? (int)(frame->height * scale + 0.5) : 1;
					surface.x = monitor.rcMonitor.left + (monitorWidth - surface.width) / 2;
					surface.y = monitor.rcMonitor.top + (monitorHeight - surface.height) / 2;

					scaler = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
						surface.width, surface.height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
					if (scaler == nullptr || !createSurface(instance, surface))
					{
						introLog("cannot make the window");
						failed = true;
						break;
					}

#ifdef RTS_HAS_MINIAUDIO
					const bool haveSound = decodeAudio(path, clip);
#endif
					// O111: the splash picture stays alone for a moment, then goes away before the logo comes, so the
					// logo plays over the bare desktop
					if (splashWindow != nullptr)
					{
						while (GetTickCount() - splashShownAt < splashMinMs)
							Sleep(10);
						ShowWindow(splashWindow, SW_HIDE);
					}
#ifdef RTS_HAS_MINIAUDIO
					if (haveSound)
					{
						ma_device_config config = ma_device_config_init(ma_device_type_playback);
						config.playback.format = ma_format_f32;
						config.playback.channels = 2;
						config.sampleRate = kAudioRate;
						config.dataCallback = audioCallback;
						config.pUserData = &clip;
						if (ma_device_init(nullptr, &config, &device) == MA_SUCCESS)
						{
							deviceStarted = ma_device_start(&device) == MA_SUCCESS;
							if (!deviceStarted)
								ma_device_uninit(&device);
						}
					}
#endif
					QueryPerformanceCounter(&start);
					shown = true;
				}

				// late by more than a frame: drop it, keep the clock
				if (secondsSince(start) > at + frameSeconds)
				{
					av_frame_unref(frame);
					continue;
				}

				uint8_t *destination[4] = { surface.bits, nullptr, nullptr, nullptr };
				const int strides[4] = { surface.width * 4, 0, 0, 0 };
				sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, strides);
				const unsigned highest = premultiply(surface.bits, (size_t)surface.width * surface.height);
				av_frame_unref(frame);
				if (highest > 3)
				{
					seenPicture = true;
					emptyFrames = 0;
				}
				else if (seenPicture && ++emptyFrames >= 2)
				{
					pictureGone = true;
					break;
				}

				if (!waitUntil(start, at))
					break;
				present(surface, 255);
				if (frameIndex == 1)
				{
					ShowWindow(surface.window, SW_SHOW);
					SetForegroundWindow(surface.window);
				}
				if (secondsSince(start) > kMaxRunSeconds)
					InterlockedExchange(&s_skip, 1);
			}

			if (!reading && !s_skip && !pictureGone)
				break; // the decoder is drained
			pumpMessages();
		}

		if (shown)
		{
			if (s_skip)
			{
				// a short fade of the last picture and of the sound
				InterlockedExchange(&clip.fading, 1);
				const int steps = 6;
				for (int i = steps - 1; i >= 0; --i)
				{
					present(surface, (BYTE)(255 * i / steps));
					Sleep(kSkipFadeMs / steps);
				}
			}
#ifdef RTS_HAS_MINIAUDIO
			else if (deviceStarted && (size_t)clip.position < clip.frames)
			{
				// the picture is over, the sound is not: it plays on during the start (O109)
				HANDLE tail = CreateThread(nullptr, 0, soundTailThread, nullptr, 0, nullptr);
				if (tail != nullptr)
				{
					CloseHandle(tail);
					soundHandedOff = true;
				}
			}
#endif
		}
	}

#ifdef RTS_HAS_MINIAUDIO
	if (!soundHandedOff)
	{
		if (deviceStarted)
			ma_device_uninit(&device);
		free(clip.samples);
		clip.samples = nullptr;
	}
#else
	free(clip.samples);
#endif
	destroySurface(instance, surface);
	sws_freeContext(scaler);
	avcodec_free_context(&video);
	av_frame_free(&frame);
	av_packet_free(&packet);
	avformat_close_input(&format);
	timeEndPeriod(1);

	return shown;
}
