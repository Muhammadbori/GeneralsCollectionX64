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

#ifdef PROFILER_ENABLED

#include "../../../Include/W3DDevice/GameClient/W3DProfilerFrameCapture.h"

#include "WW3D2/dx8wrapper.h"
#include "WW3D2/surfaceclass.h"
#include "WW3D2/texture.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/ww3dformat.h"
#include "WWMath/wwmath.h"
#include <cstring>

// Read the back buffer back and scale it down to PROFILER_FRAME_IMAGE_SIZE pixels wide with the display aspect
// ratio (point sampled) and swizzle it from BGRA to RGBA on the CPU. Reading back waits for the GPU.
static bool CaptureBackBufferDX12(std::vector<UnsignedByte> &pixels, UnsignedInt &imageHeight, UnsignedInt displayWidth, UnsignedInt displayHeight)
{
	SurfaceClass *backBuffer = DX8Wrapper::Read_Back_Buffer();
	if (!backBuffer)
		return false;

	SurfaceClass::SurfaceDescription backBufferDesc;
	backBuffer->Get_Description(backBufferDesc);
	if ((backBufferDesc.Format != WW3D_FORMAT_A8R8G8B8 && backBufferDesc.Format != WW3D_FORMAT_X8R8G8B8) ||
		backBufferDesc.Width == 0 || backBufferDesc.Height == 0)
	{
		REF_PTR_RELEASE(backBuffer);
		return false;
	}

	int pitch = 0;
	const UnsignedByte *bits = static_cast<const UnsignedByte *>(backBuffer->Lock(&pitch));
	if (!bits)
	{
		REF_PTR_RELEASE(backBuffer);
		return false;
	}

	const Real aspectRatio = (Real)displayHeight / (Real)displayWidth;
	const UnsignedInt height = max(min((int)WWMath::Roundf(PROFILER_FRAME_IMAGE_SIZE * aspectRatio), PROFILER_FRAME_IMAGE_SIZE), 1);
	const size_t rowBytes = (size_t)PROFILER_FRAME_IMAGE_SIZE * 4;
	pixels.resize(rowBytes * height);

	for (UnsignedInt row = 0; row < height; ++row)
	{
		const UnsignedInt sourceRow = min((UnsignedInt)((row + 0.5f) * backBufferDesc.Height / height), backBufferDesc.Height - 1);
		const UnsignedByte *source = bits + sourceRow * pitch;
		UnsignedByte *destination = pixels.data() + row * rowBytes;
		for (UnsignedInt column = 0; column < PROFILER_FRAME_IMAGE_SIZE; ++column)
		{
			const UnsignedInt sourceColumn = min((UnsignedInt)((column + 0.5f) * backBufferDesc.Width / PROFILER_FRAME_IMAGE_SIZE), backBufferDesc.Width - 1);
			const UnsignedByte *pixel = source + sourceColumn * 4;
			destination[0] = pixel[2];
			destination[1] = pixel[1];
			destination[2] = pixel[0];
			destination[3] = pixel[3];
			destination += 4;
		}
	}

	backBuffer->Unlock();
	REF_PTR_RELEASE(backBuffer);
	imageHeight = height;
	return true;
}

W3DProfilerFrameCapture::W3DProfilerFrameCapture()
{
}

W3DProfilerFrameCapture::~W3DProfilerFrameCapture()
{
}

bool W3DProfilerFrameCapture::ShouldReuseLastCapture(UnsignedInt currentTimeMs) const
{
	return PROFILER_FRAME_IMAGE_INTERVAL_MS > 0 &&
		currentTimeMs - m_lastCaptureTimeMs < PROFILER_FRAME_IMAGE_INTERVAL_MS &&
		!m_lastCapturePixels.empty();
}

void W3DProfilerFrameCapture::Capture(UnsignedInt displayWidth, UnsignedInt displayHeight)
{
	if (!PROFILER_IS_CONNECTED)
		return;

	// the profiler expects an image every render frame. resend the last capture if we're inside the capture interval.
	const UnsignedInt currentTimeMs = WW3D::Get_Logic_Time_Milliseconds();
	if (ShouldReuseLastCapture(currentTimeMs))
	{
		PROFILER_FRAME_IMAGE(m_lastCapturePixels.data(), PROFILER_FRAME_IMAGE_SIZE, m_lastCaptureHeight, 0, false);
		return;
	}

	if (CaptureBackBufferDX12(m_lastCapturePixels, m_lastCaptureHeight, displayWidth, displayHeight))
	{
		PROFILER_FRAME_IMAGE(m_lastCapturePixels.data(), PROFILER_FRAME_IMAGE_SIZE, m_lastCaptureHeight, 0, false);
		m_lastCaptureTimeMs = currentTimeMs;
	}
}

#endif // PROFILER_ENABLED
