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

// Direct3D 12 renderer. DX8Wrapper keeps its render state, texture stage state, transform and
// material caches as before, and in Direct3D 12 mode hands each draw to DX12Device, which
// emulates the fixed-function pipeline with one uber shader and a pipeline state cache.
//
// The Direct3D 12 path is selected with -dx12 while the port is in progress. Once it covers
// everything the Direct3D 8 path is deleted.

#pragma once

#include "WWLib/always.h"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
class DX12TextureClass;

enum
{
	DX12_MAX_STAGES = 2,
};

// One draw call, described with plain values so this header does not depend on Direct3D 8 types.
// Enumerations keep their Direct3D 8 numbering (D3DPRIMITIVETYPE, D3DBLEND, D3DCMPFUNC, D3DTOP, ...).
struct DX12DrawDesc
{
	unsigned PrimitiveType;
	unsigned IndexCount;
	unsigned StartIndex;
	int BaseVertex;

	unsigned long long VertexAddress;
	unsigned VertexBufferSize;
	unsigned VertexStride;
	unsigned FVF;

	unsigned long long IndexAddress;
	unsigned IndexBufferSize;

	const float * World;		// 4x4, row major, row vectors
	const float * View;
	const float * Projection;

	bool AlphaBlendEnable;
	unsigned SrcBlend;
	unsigned DestBlend;
	unsigned BlendOp;
	unsigned ColorWriteMask;
	bool ZEnable;
	bool ZWriteEnable;
	unsigned ZFunc;
	unsigned CullMode;
	unsigned FillMode;
	int DepthBias;
	bool AlphaTestEnable;
	unsigned AlphaFunc;
	unsigned AlphaRef;
	bool SpecularEnable;
	unsigned TextureFactor;
	float MaterialDiffuse[4];

	unsigned ColorOp[DX12_MAX_STAGES];
	unsigned ColorArg0[DX12_MAX_STAGES];
	unsigned ColorArg1[DX12_MAX_STAGES];
	unsigned ColorArg2[DX12_MAX_STAGES];
	unsigned AlphaOp[DX12_MAX_STAGES];
	unsigned AlphaArg0[DX12_MAX_STAGES];
	unsigned AlphaArg1[DX12_MAX_STAGES];
	unsigned AlphaArg2[DX12_MAX_STAGES];
	unsigned TexCoordIndex[DX12_MAX_STAGES];
	unsigned MinFilter[DX12_MAX_STAGES];
	unsigned MagFilter[DX12_MAX_STAGES];
	unsigned MipFilter[DX12_MAX_STAGES];
	unsigned AddressU[DX12_MAX_STAGES];
	unsigned AddressV[DX12_MAX_STAGES];
	unsigned MaxAnisotropy[DX12_MAX_STAGES];
	DX12TextureClass * Textures[DX12_MAX_STAGES];
};

class DX12Device
{
public:
	struct DisplayMode
	{
		unsigned Width;
		unsigned Height;
	};

	// -dx12 on the command line. Read by DX8Wrapper::Init.
	static bool Is_Requested() { return Requested; }
	static void Set_Requested(bool requested) { Requested = requested; }
	static bool Is_Active() { return Device != nullptr; }

	static bool Init(void * hwnd);
	static void Shutdown();

	static const char * Get_Adapter_Name();
	static unsigned Get_Vendor_Id();
	static unsigned Get_Device_Id();
	static unsigned long long Get_Dedicated_Video_Memory();
	static int Get_Display_Mode_Count();
	static DisplayMode Get_Display_Mode(int index);

	// Creates the swap chain on first use and resizes it afterwards.
	static bool Set_Resolution(unsigned width, unsigned height);
	static unsigned Get_Width() { return Width; }
	static unsigned Get_Height() { return Height; }

	static void Begin_Frame();
	static void End_Frame(bool present);
	static void Clear(bool clear_color, bool clear_z_stencil, unsigned argb, float z, unsigned stencil);
	static void Set_Viewport(unsigned x, unsigned y, unsigned width, unsigned height, float min_z, float max_z);
	static void Draw(const DX12DrawDesc & desc);

	// Resource support for DX12TextureClass and DX12BufferClass.
	static ID3D12Device * Get_D3D12_Device() { return Device; }
	static unsigned Allocate_SRV_Slot();
	static void Free_SRV_Slot(unsigned slot);
	static void Release_When_Idle(ID3D12Resource * resource);
	static void Queue_Texture_Upload(DX12TextureClass * texture, unsigned level, ID3D12Resource * upload);
	static bool Allocate_Frame_Memory(unsigned size, void ** cpu, unsigned long long * gpu);
	static unsigned long long Get_Frame_Number() { return FrameNumber; }

private:
	static bool Requested;
	static ID3D12Device * Device;
	static unsigned Width;
	static unsigned Height;
	static unsigned long long FrameNumber;
};
