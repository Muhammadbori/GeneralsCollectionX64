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

#pragma once

#include "GameClient/Display.h"

void W3D_TakeCompressedScreenshot(ScreenshotFormat format, Int jpegQuality);

// TheSuperHackers @feature Writes the screenshot to the given full file path, without the on screen message.
void W3D_TakeCompressedScreenshotToFile(ScreenshotFormat format, Int jpegQuality, const char* filePath);

// Returns whether a screenshot thread has not finished writing its file yet.
Bool W3D_IsScreenshotWritePending();

// Called once per frame on the main thread to show messages for screenshots
// that the screenshot thread has finished writing.
void W3D_UpdateScreenshotMessages();

