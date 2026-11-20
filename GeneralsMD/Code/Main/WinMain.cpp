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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: WinMain.cpp //////////////////////////////////////////////////////////
//
// Entry point for game application
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <windows.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <eh.h>
#include <ole2.h>
#include <dbt.h>

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "WinMain.h"
#include "Common/CommandLine.h"
#include "Common/OptionPreferences.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/Packs.h"
#include "Common/GameEngine.h"
#include "Common/GameSounds.h"
#include "Common/Debug.h"
#include "Common/GameMemory.h"
#include "Common/StackDump.h"
#include "Common/MessageStream.h"
#include "Common/PlayerList.h"
#include "Common/Registry.h"
#include "Common/Team.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/InGameUI.h"
#include "GameClient/GameClient.h"
#include "GameClient/Intro.h"
#ifdef RTS_HAS_FFMPEG
#include "VideoDevice/FFmpeg/DesktopIntro.h"
#endif
#include "GameLogic/GameLogic.h"  ///< @todo for demo, remove
#include "GameClient/Mouse.h"
#include "GameClient/IMEManager.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#include "Win32Device/Common/Win32GameEngine.h"
#include "Common/version.h"
#include "BuildVersion.h"
#include "GeneratedVersion.h"
#include "resource.h"

#ifdef RTS_ENABLE_CRASHDUMP
#include "MiniDumper/MiniDumper.h"
#endif


// GLOBALS ////////////////////////////////////////////////////////////////////
HINSTANCE ApplicationHInstance = nullptr;  ///< our application instance
HWND ApplicationHWnd = nullptr;  ///< our application window handle
Win32Mouse *TheWin32Mouse = nullptr;  ///< for the WndProc() only
DWORD TheMessageTime = 0;	///< For getting the time that a message was posted from Windows.

const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
const char *gAppPrefix = ""; /// So WB can have a different debug log file name.

static Bool gInitializing = false;
static Bool gDoPaint = true;
static Bool isWinMainActive = false;

static HBITMAP gLoadScreenBitmap = nullptr;

//#define DEBUG_WINDOWS_MESSAGES

#ifdef DEBUG_WINDOWS_MESSAGES
static const char *messageToString(unsigned int message)
{
	static char name[32];

	switch (message)
	{
	case WM_NULL: return "WM_NULL";
	case WM_CREATE: return  "WM_CREATE";
	case WM_DESTROY: return  "WM_DESTROY";
	case WM_MOVE: return  "WM_MOVE";
	case WM_SIZE: return  "WM_SIZE";
	case WM_ACTIVATE: return  "WM_ACTIVATE";
	case WM_SETFOCUS: return  "WM_SETFOCUS";
	case WM_KILLFOCUS: return  "WM_KILLFOCUS";
	case WM_ENABLE: return  "WM_ENABLE";
	case WM_SETREDRAW: return  "WM_SETREDRAW";
	case WM_SETTEXT: return  "WM_SETTEXT";
	case WM_GETTEXT: return  "WM_GETTEXT";
	case WM_GETTEXTLENGTH: return  "WM_GETTEXTLENGTH";
	case WM_PAINT: return  "WM_PAINT";
	case WM_CLOSE: return  "WM_CLOSE";
	case WM_QUERYENDSESSION: return  "WM_QUERYENDSESSION";
	case WM_QUIT: return  "WM_QUIT";
	case WM_QUERYOPEN: return  "WM_QUERYOPEN";
	case WM_ERASEBKGND: return  "WM_ERASEBKGND";
	case WM_SYSCOLORCHANGE: return  "WM_SYSCOLORCHANGE";
	case WM_ENDSESSION: return  "WM_ENDSESSION";
	case WM_SHOWWINDOW: return  "WM_SHOWWINDOW";
	case WM_WININICHANGE: return "WM_WININICHANGE";
	case WM_DEVMODECHANGE: return  "WM_DEVMODECHANGE";
	case WM_ACTIVATEAPP: return  "WM_ACTIVATEAPP";
	case WM_FONTCHANGE: return  "WM_FONTCHANGE";
	case WM_TIMECHANGE: return  "WM_TIMECHANGE";
	case WM_CANCELMODE: return  "WM_CANCELMODE";
	case WM_SETCURSOR: return  "WM_SETCURSOR";
	case WM_MOUSEACTIVATE: return  "WM_MOUSEACTIVATE";
	case WM_CHILDACTIVATE: return  "WM_CHILDACTIVATE";
	case WM_QUEUESYNC: return  "WM_QUEUESYNC";
	case WM_GETMINMAXINFO: return  "WM_GETMINMAXINFO";
	case WM_PAINTICON: return  "WM_PAINTICON";
	case WM_ICONERASEBKGND: return  "WM_ICONERASEBKGND";
	case WM_NEXTDLGCTL: return  "WM_NEXTDLGCTL";
	case WM_SPOOLERSTATUS: return  "WM_SPOOLERSTATUS";
	case WM_DRAWITEM: return  "WM_DRAWITEM";
	case WM_MEASUREITEM: return  "WM_MEASUREITEM";
	case WM_DELETEITEM: return  "WM_DELETEITEM";
	case WM_VKEYTOITEM: return  "WM_VKEYTOITEM";
	case WM_CHARTOITEM: return  "WM_CHARTOITEM";
	case WM_SETFONT: return  "WM_SETFONT";
	case WM_GETFONT: return  "WM_GETFONT";
	case WM_SETHOTKEY: return  "WM_SETHOTKEY";
	case WM_GETHOTKEY: return  "WM_GETHOTKEY";
	case WM_QUERYDRAGICON: return  "WM_QUERYDRAGICON";
	case WM_COMPAREITEM: return  "WM_COMPAREITEM";
	case WM_COMPACTING: return  "WM_COMPACTING";
	case WM_COMMNOTIFY: return  "WM_COMMNOTIFY";
	case WM_WINDOWPOSCHANGING: return  "WM_WINDOWPOSCHANGING";
	case WM_WINDOWPOSCHANGED: return  "WM_WINDOWPOSCHANGED";
	case WM_POWER: return  "WM_POWER";
	case WM_COPYDATA: return  "WM_COPYDATA";
	case WM_CANCELJOURNAL: return  "WM_CANCELJOURNAL";
	case WM_NOTIFY: return  "WM_NOTIFY";
	case WM_INPUTLANGCHANGEREQUEST: return  "WM_INPUTLANGCHANGEREQUES";
	case WM_INPUTLANGCHANGE: return  "WM_INPUTLANGCHANGE";
	case WM_TCARD: return  "WM_TCARD";
	case WM_HELP: return  "WM_HELP";
	case WM_USERCHANGED: return  "WM_USERCHANGED";
	case WM_NOTIFYFORMAT: return  "WM_NOTIFYFORMAT";
	case WM_CONTEXTMENU: return  "WM_CONTEXTMENU";
	case WM_STYLECHANGING: return  "WM_STYLECHANGING";
	case WM_STYLECHANGED: return  "WM_STYLECHANGED";
	case WM_DISPLAYCHANGE: return  "WM_DISPLAYCHANGE";
	case WM_GETICON: return  "WM_GETICON";
	case WM_SETICON: return  "WM_SETICON";
	case WM_NCCREATE: return  "WM_NCCREATE";
	case WM_NCDESTROY: return  "WM_NCDESTROY";
	case WM_NCCALCSIZE: return  "WM_NCCALCSIZE";
	case WM_NCHITTEST: return  "WM_NCHITTEST";
	case WM_NCPAINT: return  "WM_NCPAINT";
	case WM_NCACTIVATE: return  "WM_NCACTIVATE";
	case WM_GETDLGCODE: return  "WM_GETDLGCODE";
	case WM_SYNCPAINT: return  "WM_SYNCPAINT";
	case WM_NCMOUSEMOVE: return  "WM_NCMOUSEMOVE";
	case WM_NCLBUTTONDOWN: return  "WM_NCLBUTTONDOWN";
	case WM_NCLBUTTONUP: return  "WM_NCLBUTTONUP";
	case WM_NCLBUTTONDBLCLK: return  "WM_NCLBUTTONDBLCLK";
	case WM_NCRBUTTONDOWN: return  "WM_NCRBUTTONDOWN";
	case WM_NCRBUTTONUP: return  "WM_NCRBUTTONUP";
	case WM_NCRBUTTONDBLCLK: return  "WM_NCRBUTTONDBLCLK";
	case WM_NCMBUTTONDOWN: return  "WM_NCMBUTTONDOWN";
	case WM_NCMBUTTONUP: return  "WM_NCMBUTTONUP";
	case WM_NCMBUTTONDBLCLK: return  "WM_NCMBUTTONDBLCLK";
	case WM_KEYDOWN: return  "WM_KEYDOWN";
	case WM_KEYUP: return  "WM_KEYUP";
	case WM_CHAR: return  "WM_CHAR";
	case WM_DEADCHAR: return  "WM_DEADCHAR";
	case WM_SYSKEYDOWN: return  "WM_SYSKEYDOWN";
	case WM_SYSKEYUP: return  "WM_SYSKEYUP";
	case WM_SYSCHAR: return  "WM_SYSCHAR";
	case WM_SYSDEADCHAR: return  "WM_SYSDEADCHAR";
	case WM_KEYLAST: return  "WM_KEYLAST";
	case WM_IME_STARTCOMPOSITION: return  "WM_IME_STARTCOMPOSITION";
	case WM_IME_ENDCOMPOSITION: return  "WM_IME_ENDCOMPOSITION";
	case WM_IME_COMPOSITION: return  "WM_IME_COMPOSITION";
	case WM_INITDIALOG: return  "WM_INITDIALOG";
	case WM_COMMAND: return  "WM_COMMAND";
	case WM_SYSCOMMAND: return  "WM_SYSCOMMAND";
	case WM_TIMER: return  "WM_TIMER";
	case WM_HSCROLL: return  "WM_HSCROLL";
	case WM_VSCROLL: return  "WM_VSCROLL";
	case WM_INITMENU: return  "WM_INITMENU";
	case WM_INITMENUPOPUP: return  "WM_INITMENUPOPUP";
	case WM_MENUSELECT: return  "WM_MENUSELECT";
	case WM_MENUCHAR: return  "WM_MENUCHAR";
	case WM_ENTERIDLE: return  "WM_ENTERIDLE";
	case WM_CTLCOLORMSGBOX: return  "WM_CTLCOLORMSGBOX";
	case WM_CTLCOLOREDIT: return  "WM_CTLCOLOREDIT";
	case WM_CTLCOLORLISTBOX: return  "WM_CTLCOLORLISTBOX";
	case WM_CTLCOLORBTN: return  "WM_CTLCOLORBTN";
	case WM_CTLCOLORDLG: return  "WM_CTLCOLORDLG";
	case WM_CTLCOLORSCROLLBAR: return  "WM_CTLCOLORSCROLLBAR";
	case WM_CTLCOLORSTATIC: return  "WM_CTLCOLORSTATIC";
	case WM_MOUSEMOVE: return  "WM_MOUSEMOVE";
	case WM_LBUTTONDOWN: return  "WM_LBUTTONDOWN";
	case WM_LBUTTONUP: return  "WM_LBUTTONUP";
	case WM_LBUTTONDBLCLK: return  "WM_LBUTTONDBLCLK";
	case WM_RBUTTONDOWN: return  "WM_RBUTTONDOWN";
	case WM_RBUTTONUP: return  "WM_RBUTTONUP";
	case WM_RBUTTONDBLCLK: return  "WM_RBUTTONDBLCLK";
	case WM_MBUTTONDOWN: return  "WM_MBUTTONDOWN";
	case WM_MBUTTONUP: return  "WM_MBUTTONUP";
	case WM_MBUTTONDBLCLK: return  "WM_MBUTTONDBLCLK";
//	case WM_MOUSEWHEEL: return  "WM_MOUSEWHEEL";
	case WM_PARENTNOTIFY: return  "WM_PARENTNOTIFY";
	case WM_ENTERMENULOOP: return  "WM_ENTERMENULOOP";
	case WM_EXITMENULOOP: return  "WM_EXITMENULOOP";
	case WM_NEXTMENU: return  "WM_NEXTMENU";
	case WM_SIZING: return  "WM_SIZING";
	case WM_CAPTURECHANGED: return  "WM_CAPTURECHANGED";
	case WM_MOVING: return  "WM_MOVING";
	case WM_POWERBROADCAST: return  "WM_POWERBROADCAST";
	case WM_DEVICECHANGE: return  "WM_DEVICECHANGE";
	case WM_MDICREATE: return  "WM_MDICREATE";
	case WM_MDIDESTROY: return  "WM_MDIDESTROY";
	case WM_MDIACTIVATE: return  "WM_MDIACTIVATE";
	case WM_MDIRESTORE: return  "WM_MDIRESTORE";
	case WM_MDINEXT: return  "WM_MDINEXT";
	case WM_MDIMAXIMIZE: return  "WM_MDIMAXIMIZE";
	case WM_MDITILE: return  "WM_MDITILE";
	case WM_MDICASCADE: return  "WM_MDICASCADE";
	case WM_MDIICONARRANGE: return  "WM_MDIICONARRANGE";
	case WM_MDIGETACTIVE: return  "WM_MDIGETACTIVE";
	case WM_MDISETMENU: return  "WM_MDISETMENU";
	case WM_ENTERSIZEMOVE: return  "WM_ENTERSIZEMOVE";
	case WM_EXITSIZEMOVE: return  "WM_EXITSIZEMOVE";
	case WM_DROPFILES: return  "WM_DROPFILES";
	case WM_MDIREFRESHMENU: return  "WM_MDIREFRESHMENU";
	case WM_IME_SETCONTEXT: return  "WM_IME_SETCONTEXT";
	case WM_IME_NOTIFY: return  "WM_IME_NOTIFY";
	case WM_IME_CONTROL: return  "WM_IME_CONTROL";
	case WM_IME_COMPOSITIONFULL: return  "WM_IME_COMPOSITIONFULL";
	case WM_IME_SELECT: return  "WM_IME_SELECT";
	case WM_IME_CHAR: return  "WM_IME_CHAR";
	case WM_IME_KEYDOWN: return  "WM_IME_KEYDOWN";
	case WM_IME_KEYUP: return  "WM_IME_KEYUP";
//	case WM_MOUSEHOVER: return  "WM_MOUSEHOVER";
//	case WM_MOUSELEAVE: return  "WM_MOUSELEAVE";
	case WM_CUT: return  "WM_CUT";
	case WM_COPY: return  "WM_COPY";
	case WM_PASTE: return  "WM_PASTE";
	case WM_CLEAR: return  "WM_CLEAR";
	case WM_UNDO: return  "WM_UNDO";
	case WM_RENDERFORMAT: return  "WM_RENDERFORMAT";
	case WM_RENDERALLFORMATS: return  "WM_RENDERALLFORMATS";
	case WM_DESTROYCLIPBOARD: return  "WM_DESTROYCLIPBOARD";
	case WM_DRAWCLIPBOARD: return  "WM_DRAWCLIPBOARD";
	case WM_PAINTCLIPBOARD: return  "WM_PAINTCLIPBOARD";
	case WM_VSCROLLCLIPBOARD: return  "WM_VSCROLLCLIPBOARD";
	case WM_SIZECLIPBOARD: return  "WM_SIZECLIPBOARD";
	case WM_ASKCBFORMATNAME: return  "WM_ASKCBFORMATNAME";
	case WM_CHANGECBCHAIN: return  "WM_CHANGECBCHAIN";
	case WM_HSCROLLCLIPBOARD: return  "WM_HSCROLLCLIPBOARD";
	case WM_QUERYNEWPALETTE: return  "WM_QUERYNEWPALETTE";
	case WM_PALETTEISCHANGING: return  "WM_PALETTEISCHANGING";
	case WM_PALETTECHANGED: return  "WM_PALETTECHANGED";
	case WM_HOTKEY: return  "WM_HOTKEY";
	case WM_PRINT: return  "WM_PRINT";
	case WM_PRINTCLIENT: return  "WM_PRINTCLIENT";
	case WM_HANDHELDFIRST: return  "WM_HANDHELDFIRST";
	case WM_HANDHELDLAST: return  "WM_HANDHELDLAST";
	case WM_AFXFIRST: return  "WM_AFXFIRST";
	case WM_AFXLAST: return  "WM_AFXLAST";
	case WM_PENWINFIRST: return  "WM_PENWINFIRST";
	case WM_PENWINLAST: return  "WM_PENWINLAST";
	default: return "WM_UNKNOWN";
	};
}
#endif

// WndProc ====================================================================
/** Window Procedure */
//=============================================================================
// TheSuperHackers @tweak O119/O122 The opening picture fitted to the window's client area, centred, black around it.
// Used by WM_PAINT and, after the EA logo (gSplashRefit), again whenever the window's size or the display mode changes
// while the engine loads (S66: full screen changes the display mode and the picture stayed small in a corner).
static Bool gSplashRefit = false;

static void paintSplashFitted(HWND hWnd, HDC dc)
{
	if (gLoadScreenBitmap == nullptr || dc == nullptr)
		return;
	BITMAP bm;
	RECT client;
	::GetClientRect(hWnd, &client);
	if (::GetObject(gLoadScreenBitmap, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0
		|| client.right <= 0 || client.bottom <= 0)
		return;
	Int savContext = ::SaveDC(dc);
	HDC tmpDC = ::CreateCompatibleDC(dc);
	HBITMAP savBitmap = (HBITMAP)::SelectObject(tmpDC, gLoadScreenBitmap);
	Int w = client.right;
	Int h = (Int)((Int64)bm.bmHeight * client.right / bm.bmWidth);
	if (h > client.bottom)
	{
		h = client.bottom;
		w = (Int)((Int64)bm.bmWidth * client.bottom / bm.bmHeight);
	}
	const Int x = (client.right - w) / 2;
	const Int y = (client.bottom - h) / 2;
	::FillRect(dc, &client, (HBRUSH)::GetStockObject(BLACK_BRUSH));
	::SetStretchBltMode(dc, HALFTONE);
	::SetBrushOrgEx(dc, 0, 0, nullptr);
	::StretchBlt(dc, x, y, w, h, tmpDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
	::SelectObject(tmpDC, savBitmap);
	::DeleteDC(tmpDC);
	::RestoreDC(dc, savContext);
}

/// O122/O124: TRUE while the picture should stand in for the game: after the EA logo, before the engine's first frame
static Bool splashActive()
{
	if (!gSplashRefit || gLoadScreenBitmap == nullptr)
		return false;
	return TheGameClient == nullptr || TheGameClient->getFrame() == 0;
}

/// O124 (S67): what the window and the display really are at each size, mode or DPI change while the picture stands in
/// (<user data>\StartWindow.log, at most 200 lines a run): the window and client rectangles, GetDpiForWindow, the
/// monitor rectangle and the current display mode.
static void logStartWindow(HWND hWnd, const char *what)
{
	if (!splashActive() || TheGlobalData == nullptr || TheGlobalData->getPath_UserData().isEmpty())
		return;
	static Int s_lines = 0;
	if (s_lines >= 200)
		return;
	AsciiString path = TheGlobalData->getPath_UserData();
	path.concat("StartWindow.log");
	FILE *fp = fopen(path.str(), s_lines == 0 ? "w" : "a");
	++s_lines;
	if (fp == nullptr)
		return;
	RECT wr = { 0 }, cr = { 0 };
	::GetWindowRect(hWnd, &wr);
	::GetClientRect(hWnd, &cr);
	typedef UINT (WINAPI *DpiForWindowFunc)(HWND);
	HMODULE user = ::GetModuleHandleA("user32.dll");
	DpiForWindowFunc dpiForWindow = user != nullptr ? (DpiForWindowFunc)(void *)::GetProcAddress(user, "GetDpiForWindow") : nullptr;
	const UINT dpi = dpiForWindow != nullptr ? dpiForWindow(hWnd) : 0;
	MONITORINFO mi = { sizeof(MONITORINFO) };
	::GetMonitorInfo(::MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
	DEVMODEA dm;
	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	::EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
	fprintf(fp, "%lu %s: window %ld,%ld %ldx%ld client %ldx%ld dpi %u monitor %ld,%ld %ldx%ld mode %lux%lu\n",
		(unsigned long)::GetTickCount(), what, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
		cr.right, cr.bottom, dpi, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
		mi.rcMonitor.bottom - mi.rcMonitor.top, (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight);
	fclose(fp);
}

/// O122: the picture again at the new size, only while the engine has not drawn its first frame. O124: also logged,
/// and the window is invalidated so a later WM_PAINT (now also handled while the picture stands in) paints it again.
static void refitSplash(HWND hWnd, const char *what)
{
	logStartWindow(hWnd, what);
	if (!splashActive())
		return;
	HDC dc = ::GetDC(hWnd);
	paintSplashFitted(hWnd, dc);
	::ReleaseDC(hWnd, dc);
	::InvalidateRect(hWnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc( HWND hWnd, UINT message,
													WPARAM wParam, LPARAM lParam )
{

	try
	{
		// First let the IME manager do it's stuff.
		if ( TheIMEManager )
		{
			if ( TheIMEManager->serviceIMEMessage( hWnd, message, wParam, lParam ) )
			{
				// The manager intercepted an IME message so return the result
				return TheIMEManager->result();
			}
		}

#ifdef	DEBUG_WINDOWS_MESSAGES
		static msgCount=0;
		char testString[256];
		sprintf(testString,"\n%d: %s (%X,%X)", msgCount++,messageToString(message), wParam, lParam);
		OutputDebugString(testString);
#endif

		// handle all window messages
		switch( message )
		{
			//-------------------------------------------------------------------------
			case WM_NCHITTEST:
				// Prevent the user from selecting the menu in fullscreen mode
				if( !TheGlobalData->m_windowed )
					return HTCLIENT;
				break;

			//-------------------------------------------------------------------------
			case WM_WINDOWPOSCHANGING:
				// The fullscreen window is a borderless window covering the monitor. Keep it at the
				// monitor's corner when something outside the game moves it back to its old position.
				if( TheGlobalData && !TheGlobalData->m_windowed )
				{
					WINDOWPOS *pos = (WINDOWPOS *)lParam;
					if( !(pos->flags & SWP_NOMOVE) )
					{
						MONITORINFO mi = { sizeof(MONITORINFO) };
						GetMonitorInfo( MonitorFromWindow( hWnd, MONITOR_DEFAULTTOPRIMARY ), &mi );
						pos->x = mi.rcMonitor.left;
						pos->y = mi.rcMonitor.top;
					}
				}
				break;

			//-------------------------------------------------------------------------
			case WM_POWERBROADCAST:
				switch( wParam )
				{
					#ifndef PBT_APMQUERYSUSPEND
						#define PBT_APMQUERYSUSPEND 0x0000
					#endif
					case PBT_APMQUERYSUSPEND:
						// At this point, the app should save any data for open
						// network connections, files, etc., and prepare to go into
						// a suspended mode.
						return TRUE;

					#ifndef PBT_APMRESUMESUSPEND
						#define PBT_APMRESUMESUSPEND 0x0007
					#endif
					case PBT_APMRESUMESUSPEND:
						// At this point, the app should recover any data, network
						// connections, files, etc., and resume running from when
						// the app was suspended.
						return TRUE;
				}
				break;
			//-------------------------------------------------------------------------
			case WM_SYSCOMMAND:
				// Prevent moving/sizing and power loss in fullscreen mode
				switch( wParam & 0xFFF0 )
				{
					case SC_KEYMENU:
						// TheSuperHackers @bugfix Mauller 10/05/2025 Always handle this command to prevent halting the game when left Alt is pressed.
						return 1;
					case SC_MOVE:
					case SC_SIZE:
					case SC_MAXIMIZE:
					case SC_MONITORPOWER:
						if( !TheGlobalData->m_windowed )
							return 1;
						break;
				}
				break;

			case WM_QUERYENDSESSION:
			{
				if (TheGameEngine && !TheGameEngine->getQuitting())
				{
					if (TheMessageStream && TheMessageStream->isReadyForMessages())
					{
						TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_INSTANT_QUIT);
					}
					else
					{
						TheGameEngine->setQuitting(TRUE);
					}
				}
				return 0;	//don't allow Windows to shutdown while game is running.
			}

			// ------------------------------------------------------------------------
			case WM_CLOSE:
				// TheSuperHackers @feature Intercept Alt+F4/Close to show the quit menu in-game. 
				// Repeating the command when the menu is visible triggers a Self-Destruct followed by a sequenced quit.
				// If not in a match (e.g. main menu), the command instantly closes the application.
				if (TheGameEngine && !TheGameEngine->getQuitting())
				{
					if (TheMessageStream && TheMessageStream->isReadyForMessages())
					{
						TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_INSTANT_QUIT);
					}
					else
					{
						TheGameEngine->setQuitting(TRUE);
					}
				}
				return 0;

			//-------------------------------------------------------------------------
			case WM_MOVE:
			{
				if (TheMouse)
					TheMouse->refreshCursorCapture();

				break;
			}

			//-------------------------------------------------------------------------
			case WM_SIZE:
			{
				// When W3D initializes, it resizes the window.  So stop repainting.
				if (!gInitializing)
					gDoPaint = false;

				if (TheMouse)
					TheMouse->refreshCursorCapture();

				refitSplash(hWnd, "WM_SIZE");   // O122

				break;
			}

			// O122: the engine's full screen changes the display mode: the picture is fitted again
			case WM_DISPLAYCHANGE:
			{
				refitSplash(hWnd, "WM_DISPLAYCHANGE");
				break;
			}

			// O124: a display mode change can change the monitor's DPI too
			case 0x02E0: // WM_DPICHANGED
			{
				refitSplash(hWnd, "WM_DPICHANGED");
				break;
			}

			// O124: every later move or size, whatever made it (the engine, Windows after a mode change)
			case WM_WINDOWPOSCHANGED:
			{
				logStartWindow(hWnd, "WM_WINDOWPOSCHANGED");
				break;
			}

			// ------------------------------------------------------------------------
			case WM_SETFOCUS:
			{
				//
				// reset the state of our keyboard cause we haven't been paying
				// attention to the keys while focus was away
				//
				if (TheKeyboard)
					TheKeyboard->resetKeys();

				if (TheMouse)
					TheMouse->regainFocus();

				break;
			}

			//-------------------------------------------------------------------------
			case WM_KILLFOCUS:
			{
				if (TheKeyboard)
					TheKeyboard->resetKeys();

				if (TheMouse)
				{
					TheMouse->loseFocus();

					if (TheMouse->isCursorInside())
					{
						TheMouse->onCursorMovedOutside();
					}
				}

				break;
			}

			//-------------------------------------------------------------------------
			case WM_ACTIVATEAPP:
			{
				if ((bool) wParam != isWinMainActive)
				{
					// TheSuperHackers @bugfix xezon 11/05/2025 This event originally called DX8Wrapper::Reset_Device,
					// intended to clear resources on a lost device in fullscreen, but effectively also in
					// windowed mode, if the DXMaximizedWindowedMode shim was applied in newer versions of Windows,
					// which lead to unfortunate application crashing. Resetting the device on WM_ACTIVATEAPP instead
					// of TestCooperativeLevel() == D3DERR_DEVICENOTRESET is not a requirement. There are other code
					// paths that take care of that.

					isWinMainActive = (BOOL) wParam;

					if (TheGameEngine)
						TheGameEngine->setIsActive(isWinMainActive);

					if (isWinMainActive)
					{
						//restore mouse cursor to our custom version.
						if (TheWin32Mouse)
							TheWin32Mouse->setCursor(TheWin32Mouse->getMouseCursor());
					}
				}
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_ACTIVATE:
			{
				Int active = LOWORD( wParam );

				if( active == WA_INACTIVE )
				{
					// TheSuperHackers @feature O57 -audioIgnoreFocus (a *uitest*.exe only): tests in a background window
					// keep their audio
					if (TheAudio && !CommandLine::isAudioIgnoreFocus())
						TheAudio->muteAudio(AudioManager::MuteAudioReason_WindowFocus);
				}
				else
				{
					if (TheAudio)
						TheAudio->unmuteAudio(AudioManager::MuteAudioReason_WindowFocus);

					// Cursor can only be captured after one of the activation events.
					if (TheMouse)
						TheMouse->refreshCursorCapture();
				}
				break;
			}

			//-------------------------------------------------------------------------
			case WM_KEYDOWN:
			{
				Int key = (Int)wParam;

				switch( key )
				{
					case VK_ESCAPE:
					{
						PostQuitMessage( 0 );
						break;
					}
				}
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:

			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MBUTTONDBLCLK:

			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_RBUTTONDBLCLK:
			{
				if( TheWin32Mouse )
					TheWin32Mouse->addWin32Event( message, wParam, lParam, TheMessageTime );

				return 0;
			}

			//-------------------------------------------------------------------------
			case 0x020A: // WM_MOUSEWHEEL
			{
				if( TheWin32Mouse == nullptr )
					return 0;

				long x = (long) LOWORD(lParam);
				long y = (long) HIWORD(lParam);
				RECT rect;

				// ignore when outside of client area
				GetWindowRect( ApplicationHWnd, &rect );
				if( x < rect.left || x > rect.right || y < rect.top || y > rect.bottom )
					return 0;

				TheWin32Mouse->addWin32Event( message, wParam, lParam, TheMessageTime );
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_MOUSEMOVE:
			{
				if( TheWin32Mouse == nullptr )
					return 0;

				// ignore when window is not active
				if( !isWinMainActive )
					return 0;

				Int x = (Int)LOWORD( lParam );
				Int y = (Int)HIWORD( lParam );
				RECT rect;

				// ignore when outside of client area
				GetClientRect( ApplicationHWnd, &rect );
				if( x < rect.left || x > rect.right || y < rect.top || y > rect.bottom )
				{
					if ( TheMouse->isCursorInside() )
					{
						TheMouse->onCursorMovedOutside();
					}
					return 0;
				}

				if( !TheMouse->isCursorInside() )
				{
					TheMouse->onCursorMovedInside();
				}

				TheWin32Mouse->addWin32Event( message, wParam, lParam, TheMessageTime );
				return 0;
			}

			//-------------------------------------------------------------------------
			case WM_SETCURSOR:
			{
				if (TheWin32Mouse && (HWND)wParam == ApplicationHWnd)
					TheWin32Mouse->setCursor(TheWin32Mouse->getMouseCursor());
				return TRUE;	//tell Windows not to reset mouse cursor image to default.
			}

			case WM_PAINT:
			{
				if (gDoPaint || splashActive()) {   // O124
					PAINTSTRUCT paint;
					HDC dc = ::BeginPaint(hWnd, &paint);
#if 0
					::SetTextColor(dc, RGB(255,255,255));
					::SetBkColor(dc, RGB(0,0,0));
					::TextOut(dc, 30, 30, "Loading Command & Conquer Generals...", 37);
#endif
					paintSplashFitted(hWnd, dc);   // O119/O122
					::EndPaint(hWnd, &paint);
					return TRUE;
				}
				break;
			}

			case WM_ERASEBKGND:
			{
				if (!gDoPaint)
					return TRUE;	//we don't need to erase the background because we always draw entire window.
				break;
			}

// Well, it was a nice idea, but we don't get a message for an ejection.
// (Really unfortunate, actually.) I'm leaving this in in-case some one wants
// to trap a different device change (for instance, removal of a mouse) - jkmcd
#if 0
			case WM_DEVICECHANGE:
			{
				if (((UINT) wParam) == DBT_DEVICEREMOVEPENDING)
				{
					DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR*) lParam;
					if (!hdr) {
						break;
					}

					if (hdr->dbch_devicetype != DBT_DEVTYP_VOLUME)  {
						break;
					}

					// Lets discuss how Windows is a flaming pile of poo. I'm now casting the header
					// directly into the structure, because its the one I want, and this is just how
					// its done. I hate Windows. - jkmcd
					DEV_BROADCAST_VOLUME *vol = (DEV_BROADCAST_VOLUME*) (hdr);

					return TRUE;
				}
				break;
			}
#endif
		}

	}
	catch (...)
	{
		RELEASE_CRASH(("Uncaught exception in Main::WndProc... probably should not happen"));
		// no rethrow
	}

//In full-screen mode, only pass these messages onto the default windows handler.
//Appears to fix issues with dual monitor systems but doesn't seem safe?
///@todo: Look into proper support for dual monitor systems.
/*	if (!TheGlobalData->m_windowed)
	switch (message)
	{
		case WM_PAINT:
		case WM_NCCREATE:
		case WM_NCDESTROY:
		case WM_NCCALCSIZE:
		case WM_NCPAINT:
				return DefWindowProc( hWnd, message, wParam, lParam );
	}
	return 0;*/

	return DefWindowProc( hWnd, message, wParam, lParam );

}

// initializeAppWindows =======================================================
/** Register windows class and create application windows. */
//=============================================================================
static Bool initializeAppWindows( HINSTANCE hInstance, Int nCmdShow, Bool runWindowed )
{
	DWORD windowStyle;
	Int startWidth = DEFAULT_DISPLAY_WIDTH,
			startHeight = DEFAULT_DISPLAY_HEIGHT;

	// register the window class

  WNDCLASS wndClass = { CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, WndProc, 0, 0, hInstance,
                       LoadIcon (hInstance, MAKEINTRESOURCE(IDI_ApplicationIcon)),
                       nullptr/*LoadCursor(nullptr, IDC_ARROW)*/,
                       (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr,
	                     TEXT("Game Window") };
  RegisterClass( &wndClass );

   // Create our main window
	windowStyle =  WS_POPUP|WS_VISIBLE;
	if (runWindowed)
		windowStyle |= WS_MINIMIZEBOX | WS_SYSMENU | WS_DLGFRAME | WS_CAPTION;
	else
		windowStyle |= WS_EX_TOPMOST | WS_SYSMENU;

	RECT rect;
	rect.left = 0;
	rect.top = 0;
	rect.right = startWidth;
	rect.bottom = startHeight;
	AdjustWindowRect (&rect, windowStyle, FALSE);
	if (runWindowed) {
		// Makes the normal debug 800x600 window center in the screen.
		startWidth = DEFAULT_DISPLAY_WIDTH;
		startHeight= DEFAULT_DISPLAY_HEIGHT;
	}

	gInitializing = true;

  HWND hWnd = CreateWindow( TEXT("Game Window"),
                            TEXT("Command and Conquer Generals"),
                            windowStyle,
														(GetSystemMetrics( SM_CXSCREEN ) / 2) - (startWidth / 2), // original position X
														(GetSystemMetrics( SM_CYSCREEN ) / 2) - (startHeight / 2),// original position Y
														// Lorenzen nudged the window higher
														// so the constantdebug report would
														// not get obliterated by assert windows, thank you.
														//(GetSystemMetrics( SM_CXSCREEN ) / 2) - (startWidth / 2),   //this works with any screen res
														//(GetSystemMetrics( SM_CYSCREEN ) / 25) - (startHeight / 25),//this works with any screen res
														rect.right-rect.left,
														rect.bottom-rect.top,
														nullptr,
														nullptr,
														hInstance,
														nullptr );


	if (!runWindowed)
	{	SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,SWP_NOSIZE |SWP_NOMOVE);
	}
	else
		SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0,SWP_NOSIZE |SWP_NOMOVE);

	SetFocus(hWnd);

	SetForegroundWindow(hWnd);
	ShowWindow( hWnd, nCmdShow );
	UpdateWindow( hWnd );

	// save our application window handle for future use
	ApplicationHWnd = hWnd;
	gInitializing = false;
	if (!runWindowed) {
		gDoPaint = false;
	}

	return true;  // success

}

// Necessary to allow memory managers and such to have useful critical sections
static CriticalSection critSec1, critSec2, critSec3, critSec4;

// UnHandledExceptionFilter ===================================================
/** Handler for unhandled win32 exceptions. */
//=============================================================================
static LONG WINAPI UnHandledExceptionFilter( struct _EXCEPTION_POINTERS* e_info )
{
	DumpExceptionInfo( e_info->ExceptionRecord->ExceptionCode, e_info );
#ifdef RTS_ENABLE_CRASHDUMP
	if (TheMiniDumper && TheMiniDumper->IsInitialized())
	{
		// Create both minimal and full memory dumps
		TheMiniDumper->TriggerMiniDumpForException(e_info, DumpType_Minimal);
		TheMiniDumper->TriggerMiniDumpForException(e_info, DumpType_Full);
	}

	MiniDumper::shutdownMiniDumper();
#endif
	return EXCEPTION_EXECUTE_HANDLER;
}

// WinMain ====================================================================
/** Application entry point */
//=============================================================================
Int APIENTRY WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, Int nCmdShow )
{
	Int exitcode = 1;

	// TheSuperHackers @feature O48 (row 33) The quick restart: the new process waits until the old one is gone (its
	// instance, its anti-cheat, its files) before anything else.
	Packs::waitForRestartedParent();

	// TheSuperHackers @tweak O126 Per-monitor (V2) DPI aware from the first line. The manifest (app.manifest, RTS.RC)
	// already says so and S69's StartWindow.log shows dpi 144 on the 150 % panel; this only covers an exe copy that lost
	// its manifest. Fails harmlessly when the manifest already set it.
	{
		typedef BOOL(WINAPI *SetDpiContextFunc)(HANDLE);
		HMODULE user = GetModuleHandleA("user32.dll");
		SetDpiContextFunc setDpiContext = user != nullptr ? (SetDpiContextFunc)(void *)GetProcAddress(user, "SetProcessDpiAwarenessContext") : nullptr;
		if (setDpiContext != nullptr)
			setDpiContext((HANDLE)(INT_PTR)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
	}

#ifdef RTS_PROFILE_LEGACY
  Profile::StartRange("init");
#endif

	try {

		SetUnhandledExceptionFilter( UnHandledExceptionFilter );
		//
		// there is something about checkin in and out the .dsp and .dsw files
		// that blows the working directory information away on each of the
		// developers machines so we're going to hack it for a while and set our
		// working directory to the directory with the .exe since that's not the
		// default in a DevStudio project
		//

		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;

		// initialize the memory manager early
		initMemoryManager();

		CommandLine::parseCommandLineForStartup();

		// TheSuperHackers @feature Windowed of Options.ini chooses a window or the full screen (borderless); -win and
		// -fullscreen on the command line win over it.
		if (!CommandLine::isWindowModeGiven())
		{
			OptionPreferences pref;
			const Int windowed = pref.getWindowed();
			if (windowed >= 0)
				TheWritableGlobalData->m_windowed = (windowed == 1);
		}


		#ifdef RTS_DEBUG
			// Turn on Memory heap tracking
			int tmpFlag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG );
			tmpFlag |= (_CRTDBG_LEAK_CHECK_DF|_CRTDBG_ALLOC_MEM_DF);
			tmpFlag &= ~_CRTDBG_CHECK_CRT_DF;
			_CrtSetDbgFlag( tmpFlag );
		#endif



		// install debug callbacks
	//	WWDebug_Install_Message_Handler(WWDebug_Message_Callback);
	//	WWDebug_Install_Assert_Handler(WWAssert_Callback);


// Force "splash image" to be loaded from a file, not a resource so same exe can be used in different localizations.
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY

			// check both localized directory and root dir
		char filePath[_MAX_PATH];
		const char *fileName = "Install_Final.bmp";
		static const char *localizedPathFormat = "Data/%s/";
		sprintf(filePath,localizedPathFormat, GetRegistryLanguage().str());
		strlcat(filePath, fileName, ARRAY_SIZE(filePath));
		FILE *fileImage = fopen(filePath, "r");
		if (fileImage) {
			fclose(fileImage);
			gLoadScreenBitmap = (HBITMAP)LoadImage(hInstance, filePath, IMAGE_BITMAP, 0, 0, LR_SHARED|LR_LOADFROMFILE);
		}
		else {
			gLoadScreenBitmap = (HBITMAP)LoadImage(hInstance, fileName, IMAGE_BITMAP, 0, 0, LR_SHARED|LR_LOADFROMFILE);
		}
#else

		// in release, the file only ever lives in the root dir
		// TheSuperHackers @feature A folder shared with Generals keeps one splash picture per game.
		// TheSuperHackers @feature The chosen text pack can bring its own splash picture (Packs.h).
		char packSplash[_MAX_PATH];
		const char *splashFile = Packs::findSplash("Install_Final_ZeroHour.bmp", packSplash, ARRAY_SIZE(packSplash)) ? packSplash
			: (GetFileAttributes("Install_Final_ZeroHour.bmp") != INVALID_FILE_ATTRIBUTES) ? "Install_Final_ZeroHour.bmp" : "Install_Final.bmp";
		gLoadScreenBitmap = (HBITMAP)LoadImage(hInstance, splashFile, IMAGE_BITMAP, 0, 0, LR_SHARED|LR_LOADFROMFILE);
#endif

#ifdef RTS_ENABLE_CRASHDUMP
		// Initialize minidump facilities - requires TheGlobalData so performed after parseCommandLineForStartup
		MiniDumper::initMiniDumper(TheGlobalData->getPath_UserData().str(), 'Z');
#endif

		// register windows class and create application window
		if(!TheGlobalData->m_headless && initializeAppWindows(hInstance, nCmdShow, TheGlobalData->m_windowed) == false)
		{
			return exitcode;
		}

		// save our application instance for future use
		ApplicationHInstance = hInstance;

#ifdef RTS_HAS_FFMPEG
		// TheSuperHackers @feature O108 (band 61) The EA logo with a see-through background over the desktop.
		// O111: the game window with the splash picture comes first, alone, for a moment; it is hidden while the logo
		// plays over the bare desktop and shown again when the logo ends or is skipped, then the start goes on. -nologo, every test or automatic run
		// (DesktopIntro.h), -headless and DesktopIntro = no in Options.ini leave it out. When it played, the old EA logo
		// movie step of Intro is left out; when it could not play (no movie file, ...), that step plays as before.
		if (!TheGlobalData->m_headless && !DesktopIntro::isSkippedByCommandLine())
		{
			OptionPreferences introPref;
			if (introPref.getDesktopIntro())
			{
				// O115 (S61): the picture is painted now and while the logo gets ready, also in full screen where
				// gDoPaint is already off after initializeAppWindows
				const Bool splashDoPaint = gDoPaint;
				gDoPaint = true;
				if (ApplicationHWnd != nullptr)
				{
					InvalidateRect(ApplicationHWnd, nullptr, TRUE);
					UpdateWindow(ApplicationHWnd);
				}
				const DWORD splashShownAt = GetTickCount();
				Intro::s_desktopIntroPlayed = DesktopIntro::play(hInstance, ApplicationHWnd, splashShownAt, 1500);
				if (ApplicationHWnd != nullptr && !IsWindowVisible(ApplicationHWnd))
				{
					// TheSuperHackers @tweak O119 The window comes back at once at the size the engine will give it
					// (dx8wrapper.cpp: the resolution of Options.ini; full screen at the monitor's corner, a window
					// centred in the work area), so no small box shows in between; it shows the opening picture, fitted,
					// black around it, until the engine draws its first frame (painted once, even in full screen, where
					// gDoPaint is already off). Without a Resolution in Options.ini, full screen takes the monitor's size
					// and a window keeps its size.
					{
						// O122 (S66): the size from the same source the engine uses: -xres/-yres first (parsed only later
						// by the engine, so read here from the raw arguments), then Options.ini, then the default
						Int xres = 0, yres = 0;
						Bool haveRes = FALSE;
						for (Int a = 1; a + 1 < __argc; ++a)
						{
							if (_stricmp(__argv[a], "-xres") == 0)
								xres = atoi(__argv[a + 1]);
							else if (_stricmp(__argv[a], "-yres") == 0)
								yres = atoi(__argv[a + 1]);
						}
						haveRes = xres > 0 && yres > 0;
						if (!haveRes)
						{
							OptionPreferences resPref;
							OptionPreferences::const_iterator resIt = resPref.find("Resolution");
							if (resIt != resPref.end() && sscanf(resIt->second.str(), "%d%d", &xres, &yres) == 2 && xres > 0 && yres > 0)
								haveRes = TRUE;
						}
						if (!haveRes && TheGlobalData->m_windowed && TheGlobalData->m_xResolution > 0 && TheGlobalData->m_yResolution > 0)
						{
							xres = TheGlobalData->m_xResolution;
							yres = TheGlobalData->m_yResolution;
							haveRes = TRUE;
						}
						MONITORINFO mi = { sizeof(MONITORINFO) };
						if (GetMonitorInfo(MonitorFromWindow(ApplicationHWnd, MONITOR_DEFAULTTOPRIMARY), &mi))
						{
							if (!TheGlobalData->m_windowed)
							{
								if (!haveRes)
								{
									xres = mi.rcMonitor.right - mi.rcMonitor.left;
									yres = mi.rcMonitor.bottom - mi.rcMonitor.top;
								}
								RECT r = { 0, 0, xres, yres };
								AdjustWindowRect(&r, (DWORD)GetWindowLong(ApplicationHWnd, GWL_STYLE), FALSE);
								SetWindowPos(ApplicationHWnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
									r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
							}
							else if (haveRes)
							{
								RECT r = { 0, 0, xres, yres };
								AdjustWindowRect(&r, (DWORD)GetWindowLong(ApplicationHWnd, GWL_STYLE), FALSE);
								const Int w = r.right - r.left;
								const Int h = r.bottom - r.top;
								SetWindowPos(ApplicationHWnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2,
									(mi.rcWork.top + mi.rcWork.bottom - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
							}
						}
					}
					gSplashRefit = Intro::s_desktopIntroPlayed;   // O122
					const Bool doPaint = gDoPaint;
					gDoPaint = true;
					ShowWindow(ApplicationHWnd, SW_SHOW);
					UpdateWindow(ApplicationHWnd);
					gDoPaint = doPaint;
				}
				gDoPaint = splashDoPaint;
				if (Intro::s_desktopIntroPlayed && ApplicationHWnd != nullptr)
				{
					SetForegroundWindow(ApplicationHWnd);
					SetFocus(ApplicationHWnd);
				}
			}
		}
#endif

		// O122: after the EA logo the picture stays for the re-fits while the engine loads (freed by Windows at exit)
		if (gLoadScreenBitmap!=nullptr && !gSplashRefit) {
			::DeleteObject(gLoadScreenBitmap);
			gLoadScreenBitmap = nullptr;
		}


		// BGC - initialize COM
	//	OleInitialize(nullptr);



		// Set up version info
		TheVersion = NEW Version;
		TheVersion->setVersion(VERSION_MAJOR, VERSION_MINOR, VERSION_BUILDNUM, VERSION_LOCALBUILDNUM,
			AsciiString(VERSION_BUILDUSER), AsciiString(VERSION_BUILDLOC),
			AsciiString(__TIME__), AsciiString(__DATE__));

		// TheSuperHackers @refactor The instance mutex now lives in its own class.

		if (!rts::ClientInstance::initialize())
		{
			HWND ccwindow = FindWindow(rts::ClientInstance::getFirstInstanceName(), nullptr);
			if (ccwindow)
			{
				SetForegroundWindow(ccwindow);
				ShowWindow(ccwindow, SW_RESTORE);
			}

			DEBUG_LOG(("Generals is already running...Bail!"));
			delete TheVersion;
			TheVersion = nullptr;
			shutdownMemoryManager();
			return exitcode;
		}
		DEBUG_LOG(("Create Generals Mutex okay."));

		DEBUG_LOG(("CRC message is %d", GameMessage::MSG_LOGIC_CRC));

		// run the game main loop
		exitcode = GameMain();

		delete TheVersion;
		TheVersion = nullptr;

	#ifdef MEMORYPOOL_DEBUG
		TheMemoryPoolFactory->debugMemoryReport(REPORT_POOLINFO | REPORT_POOL_OVERFLOW | REPORT_SIMPLE_LEAKS, 0, 0);
	#endif
	#if defined(RTS_DEBUG)
		TheMemoryPoolFactory->memoryPoolUsageReport("AAAMemStats");
	#endif

		shutdownMemoryManager();

		// BGC - shut down COM
	//	OleUninitialize();
	}
	catch (...)
	{

	}

#ifdef RTS_ENABLE_CRASHDUMP
	MiniDumper::shutdownMiniDumper();
#endif
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;

	// TheSuperHackers @feature "Restart now" of the options: the game starts again once this process is done.
	Packs::restartIfRequested();

	return exitcode;

}

// CreateGameEngine ===========================================================
/** Create the Win32 game engine we're going to use */
//=============================================================================
GameEngine *CreateGameEngine()
{
	Win32GameEngine *engine;

	engine = NEW Win32GameEngine;
	//game engine may not have existed when app got focus so make sure it
	//knows about current focus state.
	engine->setIsActive(isWinMainActive);

	return engine;

}
