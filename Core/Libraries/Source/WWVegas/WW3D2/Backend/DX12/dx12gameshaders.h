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

// Game shaders: the game's Direct3D 8 vertex and pixel shaders (vs.1.1 / ps.1.1), ported by hand to
// HLSL (vs_5_0 / ps_5_0) and identified by name instead of bytecode.
//
// Selection. W3DShaderManager::LoadAndCreateD3DShader maps a shader file to a name, the file name
// without directory and extension plus the stage: "shaders\terrain.pso" is "terrain_ps",
// "shaders\Trees.vso" is "Trees_vs" (names compare without regard to case). The water shaders
// assembled at run time are "water_river_ps", "water_bump_ps" and "water_trapezoid_ps". In Direct3D
// 12 mode these call DX8Wrapper::Create_DX12_Shader, which returns a handle like Direct3D 8 (vertex
// shader handles have bit 0 set) or fails when the name is not registered, its HLSL does not compile
// or its vertex declaration is not supported; the game then falls back exactly as on hardware
// without that shader. Set_Vertex_Shader, Set_Pixel_Shader(_Raw) and the shader constant setters
// work as with Direct3D 8; a draw uses the game shaders whose handles are set.
//
// Registration. Each DX12_Register_*_Shaders function below calls, once per shader,
//	DX12Device::Register_Game_Shader(name, vertex_shader, hlsl_source, entry_point);
// The functions run once while DX12Device initializes, before the game creates any shader.
//
// HLSL contract. The registry compiles the registered source between a prologue and a wrapper; the
// source holds the ported function, named entry_point, and anything it needs (no entry point
// attributes, no resource or constant buffer declarations of its own). Every shader stage uses the
// same vertex output / pixel input struct (also the fixed-function one, so a game vertex shader works
// with the fixed-function pixel stages and the fixed-function vertex processing with a game pixel
// shader):
//
//	struct GameVSOutput
//	{
//		float4 Position : SV_Position;	// oPos, clip space
//		float4 Diffuse : COLOR0;		// oD0 / v0
//		float4 Specular : COLOR1;		// oD1 / v1
//		float4 Tex0 : TEXCOORD0;		// oT0 / t0 ... Tex7 : TEXCOORD7 / oT7 / t7
//		...
//		float4 Tex7 : TEXCOORD7;
//		float Fog : FOG;				// oFog: fog factor, 1 = no fog
//	};
//	typedef GameVSOutput GamePSInput;
//
// Pixel shaders, prologue:
//	Texture2D Texture0 : register(t0);	SamplerState Sampler0 : register(s0);	... stages 0..7
//	cbuffer GamePSConstants : register(b2)
//	{
//		float4 c[8];					// SetPixelShaderConstant c0..c7, clamped to [-1, 1] as for ps.1.x
//		float4 BumpEnvMat[8];			// D3DTSS_BUMPENVMAT00, 01, 10, 11 of each stage (for texbem)
//		float4 BumpEnvLuminance[8];		// D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET, 0, 0 of each stage
//	};
// and the function
//	float4 entry_point(GamePSInput input)	// returns r0
// input.TexN holds the texture coordinates of stage N as fixed-function vertex processing computes
// them for ps.1.x (D3DTSS_TEXCOORDINDEX set or texgen, then the texture transform), already divided
// for D3DTTFF_PROJECTED, so "tex tN" is TextureN.Sample(SamplerN, input.TexN.xy). The samplers have the
// stage's filter and address modes; a stage without a texture samples opaque white. Emulate the
// ps.1.x range with saturate() where the original clamps (texture and color inputs are [0, 1]; keep
// _x2 / _bx2 / _bias modifiers as arithmetic). The wrapper saturates the result, then applies the
// alpha test (D3DRS_ALPHATESTENABLE ...) and fog (D3DRS_FOGENABLE with the fog of the vertex
// processing), as Direct3D 8 does after the pixel shader.
//
// Vertex shaders, prologue:
//	cbuffer GameVSConstants : register(b3) { float4 c[96]; };	// SetVertexShaderConstant c0..c95
// The source declares the input struct, named GameVSInput, with one member per register of the
// Direct3D 8 vertex declaration passed at creation, semantic V<register>:
//	struct GameVSInput { float3 Position : V0; float4 Diffuse : V2; float2 Tex : V7; };
// D3DVSDT_FLOAT1..4 are float..float4, D3DVSDT_D3DCOLOR is float4 (r, g, b, a) like Direct3D 8,
// D3DVSDT_UBYTE4 is uint4 (0..255), D3DVSDT_SHORT2/4 are int2/int4. Only stream 0 and D3DVSD_REG /
// D3DVSD_SKIP / D3DVSD_NOP tokens are supported. The function
//	GameVSOutput entry_point(GameVSInput input)
// writes every output member (start from (GameVSOutput)0 and set Fog to 1 when it computes none).
// The wrapper moves the position by the Direct3D 8 to Direct3D 12 half pixel difference and
// saturates Diffuse, Specular and Fog. Direct3D 12 table fog is not emulated for game vertex
// shaders: with D3DRS_FOGENABLE the fog factor is always Fog, as with Direct3D 8 vertex fog.
//
// -dx12debug logs each registration, creation and compile (with the full source on errors) to
// dx12debug.log, and checks the registry at initialization with a small test shader pair.

#pragma once

#include "WW3D2/Backend/DX12/dx12device.h"

#include <string>

// One function per group of ported shaders, each in its own file (dx12shaders_*.cpp).
void DX12_Register_Terrain_Shaders();
void DX12_Register_Water_Shaders();
void DX12_Register_Tree_Shaders();

// Single switch for the game shaders. With 0, Direct3D 12 mode reports no shader support and
// creates no game shaders, so the game keeps its fixed-function paths everywhere.
#define DX12_GAME_SHADERS_ENABLED 1

// ---------------------------------------------------------------------------------------------------
// Interface between the registry (dx12gameshaders.cpp) and DX12Device.

// Calls the DX12_Register_*_Shaders functions.
void DX12_Register_Game_Shaders();

// Constant buffer contents, matching the cbuffers above.
struct DX12GameVSConstants
{
	float C[DX12_MAX_VS_CONSTANTS][4];
};

struct DX12GamePSConstants
{
	float C[DX12_MAX_PS_CONSTANTS][4];
	float BumpEnvMat[DX12_MAX_STAGES][4];
	float BumpEnvLuminance[DX12_MAX_STAGES][4];
};

static_assert(sizeof(DX12GameVSConstants) == 16 * DX12_MAX_VS_CONSTANTS, "DX12GameVSConstants must match the HLSL constant buffer");
static_assert(sizeof(DX12GamePSConstants) == 16 * (DX12_MAX_PS_CONSTANTS + 2 * DX12_MAX_STAGES), "DX12GamePSConstants must match the HLSL constant buffer");

// The vertex input of a game vertex shader, from its Direct3D 8 vertex declaration. Formats are
// DXGI_FORMAT values; every element is per vertex data of input slot 0 with semantic V<Register>.
enum
{
	DX12_GAME_MAX_INPUTS = 16,		// Direct3D 8 vertex shader input registers v0..v15
};

struct DX12GameInputElement
{
	unsigned Register;
	unsigned Format;
	unsigned Offset;
};

struct DX12GameInputLayout
{
	DX12GameInputElement Elements[DX12_GAME_MAX_INPUTS];
	unsigned ElementCount;
	unsigned Size;					// bytes of the stream 0 vertex the declaration describes
};

// Wrapper variant of a game pixel shader: what is applied around the ported function. All bytes, no
// padding; zero means none.
struct DX12GamePSVariant
{
	unsigned char Projected[DX12_MAX_STAGES];	// component (1..4) stage N's coordinates are divided by
	unsigned char AlphaFunc;					// D3DCMPFUNC of the alpha test
	unsigned char Fog;							// 1 factor from the vertex shader, 2 table fog
	unsigned char FogTableMode;					// D3DFOGMODE for table fog
	unsigned char Pad;
};

static_assert(sizeof(DX12GamePSVariant) == DX12_MAX_STAGES + 4, "DX12GamePSVariant must not have padding");

// The registry. Shaders are numbered from 0 in registration order; registering a name again
// replaces that shader's source and keeps its number. Returns the number, or -1 for invalid input.
int DX12GameShaders_Register(const char * name, bool vertex_shader, const char * hlsl_source, const char * entry_point);
int DX12GameShaders_Find(const char * name, bool vertex_shader);
const char * DX12GameShaders_Get_Name(int index);
bool DX12GameShaders_Is_Vertex_Shader(int index);
int DX12GameShaders_Get_Count();

// The complete HLSL of a registered shader: prologue, the registered source and the wrapper, whose
// entry point (GameVSMain or GamePSMain) and target are returned. variant is for pixel shaders only.
void DX12GameShaders_Generate(int index, const DX12GamePSVariant * variant, std::string & hlsl, const char ** entry, const char ** target);

// Parses Direct3D 8 vertex declaration tokens. On failure error says why.
bool DX12GameShaders_Parse_Declaration(const unsigned * tokens, DX12GameInputLayout & layout, std::string & error);

// Handles. A handle names a registered shader and, for a vertex shader, an input layout id the device
// assigns. Free_Handle ignores handles that are not live.
unsigned DX12GameShaders_Allocate_Handle(int index, bool vertex_shader, unsigned layout_id);
bool DX12GameShaders_Resolve_Handle(unsigned handle, bool vertex_shader, int * index, unsigned * layout_id);
void DX12GameShaders_Free_Handle(unsigned handle);

// Forgets every registration and handle.
void DX12GameShaders_Clear();
