// TheSuperHackers @build O159 (headless port) <dxgiformat.h> on macOS and Linux: the DXGI formats the renderer's
// shader descriptions name (dx12gameshaders.cpp), with the values of the Windows SDK.
#pragma once
typedef enum DXGI_FORMAT
{
	DXGI_FORMAT_UNKNOWN = 0,
	DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
	DXGI_FORMAT_R32G32B32_FLOAT = 6,
	DXGI_FORMAT_R16G16B16A16_SINT = 14,
	DXGI_FORMAT_R32G32_FLOAT = 16,
	DXGI_FORMAT_R8G8B8A8_UNORM = 28,
	DXGI_FORMAT_R8G8B8A8_UINT = 30,
	DXGI_FORMAT_R16G16_SINT = 38,
	DXGI_FORMAT_R32_FLOAT = 41,
	DXGI_FORMAT_B8G8R8A8_UNORM = 87,
} DXGI_FORMAT;
