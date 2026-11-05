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

// Direct3D 8 fixed-function emulation with generated shaders.
//
// Each draw is reduced to a DX12FFPKey: the parts of the Direct3D 8 state that change the shader
// code (vertex format, lighting setup, color sources, fog modes, texture stage operations, texture
// coordinate generation, alpha test function). DX12Device caches one vertex shader per DX12VSKey
// and one pixel shader per DX12PSKey, generated as straight-line HLSL by DX12FFP_Generate_*.
// Everything that varies continuously (matrices, colors, light parameters, fog distances, alpha
// reference, texture factor) goes to the constant buffers filled by DX12FFP_Fill_Constants.
//
// Game pixel shaders (dx12gameshaders.h) replace the texture stages: with one, the vertex shader
// computes the texture coordinates of every stage, as Direct3D 8 does for the ps.1.x tN registers,
// and the pixel shader key keeps only what is applied around the game shader (projection, alpha
// test, fog). All vertex shaders, generated or ported, write the same output signature
// (DX12FFP_Append_Interface), so any vertex shader fits any pixel shader.
//
// Lighting is done in camera space, as in Direct3D 8. Normals go through the inverse transpose of
// the world-view matrix, so non-uniform scales light correctly; without D3DRS_NORMALIZENORMALS a
// uniform scale s changes the normal length by 1/s, as with the Direct3D 8 reference rasterizer.

#pragma once

#include "WW3D2/Backend/DX12/dx12device.h"

#include <string>

// Direct3D 8 FVF bits. Spelled out here so the backend does not depend on d3d8.h.
enum
{
	FVF_POSITION_MASK = 0x00e,
	FVF_XYZ = 0x002,
	FVF_XYZRHW = 0x004,
	FVF_XYZB1 = 0x006,
	FVF_NORMAL = 0x010,
	FVF_PSIZE = 0x020,
	FVF_DIFFUSE = 0x040,
	FVF_SPECULAR = 0x080,
	FVF_TEXCOUNT_MASK = 0xf00,
	FVF_TEXCOUNT_SHIFT = 8,
	FVF_MAX_TEXCOORDS = 8,
};

// D3DRENDERSTATETYPE and D3DTEXTURESTAGESTATETYPE values read by the emulation.
enum
{
	FFP_RS_ALPHATESTENABLE = 15,
	FFP_RS_ALPHAREF = 24,
	FFP_RS_ALPHAFUNC = 25,
	FFP_RS_FOGENABLE = 28,
	FFP_RS_SPECULARENABLE = 29,
	FFP_RS_FOGCOLOR = 34,
	FFP_RS_FOGTABLEMODE = 35,
	FFP_RS_FOGSTART = 36,
	FFP_RS_FOGEND = 37,
	FFP_RS_FOGDENSITY = 38,
	FFP_RS_RANGEFOGENABLE = 48,
	FFP_RS_STENCILENABLE = 52,		// the stencil states are read by DX12Device for the pipeline state
	FFP_RS_STENCILFAIL = 53,
	FFP_RS_STENCILZFAIL = 54,
	FFP_RS_STENCILPASS = 55,
	FFP_RS_STENCILFUNC = 56,
	FFP_RS_STENCILREF = 57,
	FFP_RS_STENCILMASK = 58,
	FFP_RS_STENCILWRITEMASK = 59,
	FFP_RS_TEXTUREFACTOR = 60,
	FFP_RS_LIGHTING = 137,
	FFP_RS_AMBIENT = 139,
	FFP_RS_FOGVERTEXMODE = 140,
	FFP_RS_COLORVERTEX = 141,
	FFP_RS_LOCALVIEWER = 142,
	FFP_RS_NORMALIZENORMALS = 143,
	FFP_RS_DIFFUSEMATERIALSOURCE = 145,
	FFP_RS_SPECULARMATERIALSOURCE = 146,
	FFP_RS_AMBIENTMATERIALSOURCE = 147,
	FFP_RS_EMISSIVEMATERIALSOURCE = 148,
	FFP_RS_COUNT = 256,

	FFP_TSS_COLOROP = 1,
	FFP_TSS_COLORARG1 = 2,
	FFP_TSS_COLORARG2 = 3,
	FFP_TSS_ALPHAOP = 4,
	FFP_TSS_ALPHAARG1 = 5,
	FFP_TSS_ALPHAARG2 = 6,
	FFP_TSS_BUMPENVMAT00 = 7,
	FFP_TSS_BUMPENVMAT01 = 8,
	FFP_TSS_BUMPENVMAT10 = 9,
	FFP_TSS_BUMPENVMAT11 = 10,
	FFP_TSS_TEXCOORDINDEX = 11,
	FFP_TSS_BUMPENVLSCALE = 22,
	FFP_TSS_BUMPENVLOFFSET = 23,
	FFP_TSS_TEXTURETRANSFORMFLAGS = 24,
	FFP_TSS_COLORARG0 = 26,
	FFP_TSS_ALPHAARG0 = 27,
	FFP_TSS_COUNT = 32,
};

// DX12VSKey::Fog
enum
{
	FFP_VS_FOG_NONE = 0,
	FFP_VS_FOG_EXP = 1,			// vertex fog, D3DFOG numbering
	FFP_VS_FOG_EXP2 = 2,
	FFP_VS_FOG_LINEAR = 3,
	FFP_VS_FOG_SPECULAR = 4,	// fog factor from the specular alpha (no fog mode, or pretransformed vertices)
	FFP_VS_FOG_DEPTH = 5,		// depth for table fog in the pixel shader
};

// DX12PSStageKey::Texture
enum
{
	FFP_TEXTURE_UNUSED = 0,
	FFP_TEXTURE_SAMPLED = 1,
	FFP_TEXTURE_WHITE = 2,		// used, but no texture is bound
};

// Every member is a byte (or a whole 32-bit word) so the keys have no padding; members that do not
// matter for a draw are zero so that equivalent states share shaders.
struct DX12VSKey
{
	unsigned FVF;
	unsigned char Lighting;
	unsigned char LightType[DX12_MAX_LIGHTS];		// D3DLIGHTTYPE of the enabled lights in order, 0 = none
	unsigned char DiffuseSource;					// 0 material, 1 vertex diffuse, 2 vertex specular
	unsigned char AmbientSource;
	unsigned char SpecularSource;
	unsigned char EmissiveSource;
	unsigned char NormalizeNormals;
	unsigned char LocalViewer;
	unsigned char Specular;							// specular lighting
	unsigned char Fog;								// FFP_VS_FOG_*
	unsigned char RangeFog;
	unsigned char TexCoordSet[DX12_MAX_STAGES];		// texture coordinate set of each stage, 0xff = stage samples no texture
	unsigned char TexGen[DX12_MAX_STAGES];			// D3DTSS_TCI_* >> 16, 0 = the set itself
	unsigned char TexTransform[DX12_MAX_STAGES];	// D3DTTFF_COUNT1..4, 0 = no texture matrix
	unsigned char Pad[2];
};

struct DX12PSStageKey
{
	unsigned char ColorOp;			// 0 = stage disabled
	unsigned char ColorArg0;
	unsigned char ColorArg1;
	unsigned char ColorArg2;
	unsigned char AlphaOp;			// 0 = keeps the current alpha
	unsigned char AlphaArg0;
	unsigned char AlphaArg1;
	unsigned char AlphaArg2;
	unsigned char Texture;			// FFP_TEXTURE_*
	unsigned char Projected;		// component (1..4) the coordinates are divided by, 0 = none
	unsigned char Pad[2];
};

// With a game pixel shader only Stages[].Projected, AlphaFunc, Fog and FogTableMode are set.
struct DX12PSKey
{
	DX12PSStageKey Stages[DX12_MAX_STAGES];
	unsigned char AlphaFunc;		// D3DCMPFUNC, 0 = no alpha test
	unsigned char Specular;			// add the specular color
	unsigned char Fog;				// 0 none, 1 factor from the vertex shader, 2 table fog
	unsigned char FogTableMode;		// D3DFOGMODE for table fog
};

struct DX12FFPKey
{
	DX12VSKey VS;
	DX12PSKey PS;
};

static_assert(sizeof(DX12VSKey) == 20 + 3 * DX12_MAX_STAGES, "DX12VSKey must not have padding");
static_assert(sizeof(DX12PSStageKey) == 12, "DX12PSStageKey must not have padding");
static_assert(sizeof(DX12PSKey) == 12 * DX12_MAX_STAGES + 4, "DX12PSKey must not have padding");

// Must match VSConstants and PSConstants in dx12shaders.h.
struct DX12FFPLightConstants
{
	float Diffuse[4];
	float Specular[4];
	float Ambient[4];
	float Position[4];			// camera space, w = range
	float Direction[4];			// camera space, normalized, w = falloff
	float Attenuation[4];		// a0, a1, a2
	float Spot[4];				// cos(theta / 2), cos(phi / 2), 1 / (cos(theta / 2) - cos(phi / 2))
};

struct DX12VSConstants
{
	float WorldViewProjection[16];
	float WorldView[16];
	float NormalMatrix[16];
	float TextureMatrix[DX12_MAX_STAGES][16];
	float ScreenToClip[4];
	float DepthToClip[4];
	float PixelOffset[4];
	float MaterialDiffuse[4];
	float MaterialAmbient[4];
	float MaterialSpecular[4];
	float MaterialEmissive[4];
	float GlobalAmbient[4];
	float Fog[4];				// end, 1 / (end - start), density, 1 = table fog uses eye depth (w), 0 = z
	float MaterialPower[4];
	DX12FFPLightConstants Lights[DX12_MAX_LIGHTS];
};

struct DX12PSConstants
{
	float TextureFactor[4];
	float FogColor[4];
	float Fog[4];				// end, 1 / (end - start), density
	float AlphaRef[4];			// 0..255
};

static_assert(sizeof(DX12VSConstants) == 16 * (12 + 4 * DX12_MAX_STAGES + 10 + 7 * DX12_MAX_LIGHTS), "DX12VSConstants must match the HLSL constant buffer");
static_assert(sizeof(DX12PSConstants) == 64, "DX12PSConstants must match the HLSL constant buffer");

struct DX12FFPViewport
{
	float X;
	float Y;
	float Width;
	float Height;
	float MinZ;
	float MaxZ;
};

// Fills the render states and texture stage states from the explicit DX12DrawDesc fields, for a
// desc without RenderStates or TextureStageStates. There is no lighting and no fog then.
void DX12FFP_Fill_Fallback_States(const DX12DrawDesc & desc, unsigned * render_states, unsigned * texture_stage_states);

// game_pixel_shader: the draw uses a game pixel shader instead of the texture stages.
void DX12FFP_Build_Key(const DX12DrawDesc & desc, const unsigned * render_states, const unsigned * texture_stage_states,
	bool game_pixel_shader, DX12FFPKey & key);
void DX12FFP_Fill_Constants(const DX12DrawDesc & desc, const unsigned * render_states, const DX12FFPKey & key,
	const DX12FFPViewport & viewport, DX12VSConstants & vs, DX12PSConstants & ps);

// Appends the vertex shader output and pixel shader input struct that every shader uses, named name:
//	float4 Position : SV_Position;
//	float4 Diffuse : COLOR0;		// v0
//	float4 Specular : COLOR1;		// v1
//	float4 Tex0 : TEXCOORD0; ... float4 Tex7 : TEXCOORD7;	// texture coordinates of stages 0..7
//	float Fog : FOG;				// fog factor (1 = no fog), or the depth for table fog
void DX12FFP_Append_Interface(std::string & hlsl, const char * name);

// HLSL for the vertex shader VSMain (vs_5_0) and the pixel shader PSMain (ps_5_0).
void DX12FFP_Generate_Vertex_Shader(const DX12VSKey & key, std::string & hlsl);
void DX12FFP_Generate_Pixel_Shader(const DX12PSKey & key, std::string & hlsl);

// The end of a pixel shader: the alpha test and fog of key on the float4 'current', reading the fog
// factor or depth from 'input.Fog' and the PSConstants members with names prefixed by prefix.
void DX12FFP_Append_Alpha_Test_And_Fog(const DX12PSKey & key, const char * prefix, std::string & hlsl);

unsigned DX12FFP_Hash(const void * data, unsigned size);
