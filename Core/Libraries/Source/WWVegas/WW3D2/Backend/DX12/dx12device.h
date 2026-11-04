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
// emulates the fixed-function pipeline with generated shaders (dx12ffp.h) and a pipeline state cache.
//
// The Direct3D 12 path is selected with -dx12 while the port is in progress. Once it covers
// everything the Direct3D 8 path is deleted. -dx12debug sets DX12Device_DebugLayer, which enables
// the Direct3D 12 debug layer and writes its messages to dx12debug.log.

#pragma once

#include "WWLib/always.h"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct tagRECT;
class DX12TextureClass;

extern bool DX12Device_DebugLayer;

enum
{
	DX12_MAX_STAGES = 2,
	DX12_MAX_LIGHTS = 4,
};

// D3DMATERIAL8, colors as rgba.
struct DX12MaterialDesc
{
	float Diffuse[4];
	float Ambient[4];
	float Specular[4];
	float Emissive[4];
	float Power;
};

// D3DLIGHT8 in world space, colors as rgba. Type is D3DLIGHTTYPE.
struct DX12LightDesc
{
	bool Enabled;
	unsigned Type;
	float Diffuse[4];
	float Specular[4];
	float Ambient[4];
	float Position[3];
	float Direction[3];
	float Range;
	float Falloff;
	float Attenuation0;
	float Attenuation1;
	float Attenuation2;
	float Theta;
	float Phi;
};

// One draw call, described with plain values so this header does not depend on Direct3D 8 types.
// Enumerations keep their Direct3D 8 numbering (D3DPRIMITIVETYPE, D3DBLEND, D3DCMPFUNC, D3DTOP, ...).
//
// IndexAddress 0 draws VertexCount vertices from StartVertex without indices; otherwise IndexCount
// 16-bit indices from StartIndex, offset by BaseVertex. Point, line and triangle lists and strips
// are supported; DX8Wrapper converts triangle fans to triangle lists. Draws that would read past
// the end of a buffer are skipped.
struct DX12DrawDesc
{
	unsigned PrimitiveType;
	unsigned IndexCount;
	unsigned StartIndex;
	int BaseVertex;
	unsigned VertexCount;
	unsigned StartVertex;

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

	// The Direct3D 8 device state the fixed-function emulation is derived from. Without
	// RenderStates or TextureStageStates the explicit fields above are used, with no lighting, fog
	// or stencil. The stencil states (D3DRS_STENCILENABLE ... D3DRS_STENCILWRITEMASK) are read from
	// RenderStates only.
	const unsigned * RenderStates;			// 256 entries, D3DRENDERSTATETYPE indexed
	const unsigned * TextureStageStates;	// [stage * 32 + D3DTEXTURESTAGESTATETYPE], DX12_MAX_STAGES stages
	const float * TextureMatrices[DX12_MAX_STAGES];	// D3DTS_TEXTURE0 + stage, 4x4 row major; null is identity
	DX12MaterialDesc Material;
	DX12LightDesc Lights[DX12_MAX_LIGHTS];
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
	// Ends the frame and presents it. Without present, like Direct3D 8 EndScene without Present
	// (render to texture passes before the main scene), a frame being recorded continues in the same
	// command list and ends with the next present; outside a frame, pending uploads are executed.
	static void End_Frame(bool present);
	// Clears the current viewport of the current render target and, when one is bound, its depth
	// stencil buffer.
	static void Clear(bool clear_color, bool clear_z_stencil, unsigned argb, float z, unsigned stencil);
	static void Set_Viewport(unsigned x, unsigned y, unsigned width, unsigned height, float min_z, float max_z);
	static void Draw(const DX12DrawDesc & desc);

	// Render to texture. color is a DX12TextureClass::Create_Render_Target texture, or null for the
	// back buffer. depth is a DX12TextureClass::Create_Depth_Target texture; without one,
	// use_default_depth binds the default depth buffer. As with Direct3D 8, a depth buffer is only
	// bound when it is at least as large as the color target; otherwise the target has no depth
	// buffer, and depth and stencil tests are off while drawing into it. Textures that are not render
	// or depth targets select the back buffer or no depth buffer instead. The back buffer always uses
	// the default depth buffer; depth and use_default_depth only apply to a color target. The
	// viewport is reset to the whole target, as Direct3D 8 SetRenderTarget does. The selection stays until it is changed, also
	// across frames and resolution changes; the device keeps a reference to the selected textures.
	static void Set_Render_Target(DX12TextureClass * color, DX12TextureClass * depth, bool use_default_depth);
	// The selected color target, null for the back buffer.
	static DX12TextureClass * Peek_Render_Target();
	static bool Is_Render_To_Texture() { return Peek_Render_Target() != nullptr; }
	// Size of the selected color target (the back buffer size when none is selected).
	static unsigned Get_Target_Width();
	static unsigned Get_Target_Height();

	// Copies a rectangle of the current color target (the back buffer, or the selected render target)
	// into level 0 of dest at dest_x, dest_y, on the GPU in the frame being recorded. A null src_rect
	// copies the whole target. The rectangle is clipped to both textures. dest must be a B8G8R8A8
	// texture (any non-DXT GPU texture, including render targets) other than the current target; it
	// is left in the copy destination state and moved to PIXEL_SHADER_RESOURCE when it is sampled.
	// Returns false when nothing was copied.
	static bool Copy_Target_To_Texture(DX12TextureClass * dest, const tagRECT * src_rect, int dest_x, int dest_y);

	// Reads the back buffer back to the CPU: during a frame, the back buffer as drawn so far (the
	// commands recorded so far are executed and waited for, and the frame continues); between frames,
	// the last presented back buffer. Returns a cpu_only A8R8G8B8 texture of the back buffer size with
	// the pixels in level 0 (B8G8R8A8 bytes, alpha as rendered), or null on failure. Slow: it waits for
	// the GPU. Main thread only.
	static DX12TextureClass * Read_Back_Buffer();

	// Resource support for DX12TextureClass and DX12BufferClass.
	static ID3D12Device * Get_D3D12_Device() { return Device; }
	static unsigned Allocate_SRV_Slot();
	static void Free_SRV_Slot(unsigned slot);
	// Render target and depth stencil view slots for render and depth targets. 0 means none was free.
	static unsigned Allocate_RTV_Slot();
	static void Free_RTV_Slot(unsigned slot);
	static unsigned Allocate_DSV_Slot();
	static void Free_DSV_Slot(unsigned slot);
	static void Release_When_Idle(ID3D12Resource * resource);
	static void Queue_Texture_Upload(DX12TextureClass * texture, unsigned level, ID3D12Resource * upload);
	static bool Allocate_Frame_Memory(unsigned size, void ** cpu, unsigned long long * gpu);
	static unsigned long long Get_Frame_Number() { return FrameNumber; }

private:
	// Executes the commands recorded so far (starting a frame for pending uploads when none is being
	// recorded), presents when asked and moves to the next frame slot.
	static void Submit_Frame(bool present);

	static bool Requested;
	static ID3D12Device * Device;
	static unsigned Width;
	static unsigned Height;
	static unsigned long long FrameNumber;
};
