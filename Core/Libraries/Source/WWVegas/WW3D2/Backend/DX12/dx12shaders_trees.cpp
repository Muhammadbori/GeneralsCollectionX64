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

// HLSL ports of the game's tree shaders: Trees_vs and Trees_ps (GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/
// GameClient/Shaders/Trees.nvv, Trees.nvp).
// See dx12gameshaders.h for the contract.

#include "WW3D2/Backend/DX12/dx12gameshaders.h"

namespace
{

// Trees.nvv (vs.1.1): breeze sway, diffuse darkening and the shroud texture coordinates of the trees,
// bushes and grass W3DTreeBuffer draws. Vertex declaration (W3DTreeBuffer::allocateTreeBuffers, the
// DX8_FVF_XYZNDUV1 vertex):
//	v0	position (FLOAT3)
//	v1	"normal" (FLOAT3): x = sway type (index of the wave constant), y = color scale for darkening,
//		z = z coordinate of the base of the tree
//	v2	diffuse color (D3DCOLOR)
//	v7	texture coordinates (FLOAT2)
// Constants (W3DTreeBuffer::drawTrees):
//	c4..c7	transposed world * view * projection matrix
//	c8		no sway (0, 0, 0, 0), sway type 0
//	c9..c18	sway (x, y, z, 0) of sway types 1..MAX_SWAY_TYPES
//	c32		shroud offset (x, y, 0, 0)
//	c33		shroud scale (x, y, 1, 1)
// The inputs are declared float4 so that the components the declaration does not provide read as
// (0, 0, 0, 1), as for Direct3D 8 input registers: v0.w and v1.w are 1.
//
//	mov  r2, v1.wwzw        r2 = (1, 1, base z, 1)
//	sub  r2, v0, r2         r2.z = height above the base
//	mov  a0.x, v1           sway type
//	mov  r0, c[a0.x+8]      its wave entry
//	mad  r1, r2.zzzw, r0, v0
//	m4x4 oPos, r1, c4
//	mov  r2, v1.yyyw
//	mul  oD0, v2, r2        rgb darkened, alpha unchanged
//	mov  oT0, v7
//	add  r1, v0, c32
//	mul  oT1, r1, c33       shroud texture coordinates
//
// The relative constant read goes through Trees_Relative_Constant: vs.1.1 converts a0.x from v1.x
// by rounding down (the game writes whole numbers), and a register outside c0..c95 reads as zero;
// the index is clamped before the read so that it never reads past the root constant buffer. Outputs the shader does not write: oD1 is zero,
// oT2..oT7 are (0, 0, 0, 1) and oFog is 1 (no fog; the tree shader disables fog).
const char TreesVertexShader[] =
	"struct GameVSInput\n"
	"{\n"
	"\tfloat4 Position : V0;\n"
	"\tfloat4 Normal : V1;\n"
	"\tfloat4 Diffuse : V2;\n"
	"\tfloat4 Tex : V7;\n"
	"};\n"
	"\n"
	"float4 Trees_Relative_Constant(float address, int base)\n"
	"{\n"
	"\tint index = (int)floor(address) + base;\n"
	"\tfloat4 value = c[clamp(index, 0, 95)];\n"
	"\treturn (index >= 0 && index <= 95) ? value : float4(0.0, 0.0, 0.0, 0.0);\n"
	"}\n"
	"\n"
	"GameVSOutput TreesVS(GameVSInput input)\n"
	"{\n"
	"\tGameVSOutput output = (GameVSOutput)0;\n"
	"\tfloat4 v0 = input.Position;\n"
	"\tfloat4 v1 = input.Normal;\n"
	"\n"
	"\tfloat4 r2 = v0 - v1.wwzw;\n"
	"\tfloat4 r0 = Trees_Relative_Constant(v1.x, 8);\n"
	"\tfloat4 r1 = r2.zzzw * r0 + v0;\n"
	"\toutput.Position = float4(dot(r1, c[4]), dot(r1, c[5]), dot(r1, c[6]), dot(r1, c[7]));\n"
	"\toutput.Diffuse = input.Diffuse * v1.yyyw;\n"
	"\toutput.Tex0 = input.Tex;\n"
	"\toutput.Tex1 = (v0 + c[32]) * c[33];\n"
	"\n"
	"\tconst float4 unused = float4(0.0, 0.0, 0.0, 1.0);\n"
	"\toutput.Specular = float4(0.0, 0.0, 0.0, 0.0);\n"
	"\toutput.Tex2 = unused;\n"
	"\toutput.Tex3 = unused;\n"
	"\toutput.Tex4 = unused;\n"
	"\toutput.Tex5 = unused;\n"
	"\toutput.Tex6 = unused;\n"
	"\toutput.Tex7 = unused;\n"
	"\toutput.Fog = 1.0;\n"
	"\treturn output;\n"
	"}\n";

// Trees.nvp (ps.1.1), "not actually used at this time" (W3DTreeBuffer sets it only in an #if 0
// block), ported so that it is created like with Direct3D 8:
//	tex t0
//	mov r0, v0
//	mul r0, r0, t0
//	mul_x2 r0.rgb, r0, c1   modulate 2 * c1 (c1 is 0.5, or 1 with overbright)
//	mov r0.a, t0.a
// ps.1.1 keeps registers in [-1, 1] (MaxPixelShaderValue 1); the wrapper saturates the result.
const char TreesPixelShader[] =
	"float4 TreesPS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 r0 = input.Diffuse * t0;\n"
	"\tr0.rgb = clamp(r0.rgb * c[1].rgb * 2.0, -1.0, 1.0);\n"
	"\tr0.a = t0.a;\n"
	"\treturn r0;\n"
	"}\n";

} // namespace

void DX12_Register_Tree_Shaders()
{
	DX12Device::Register_Game_Shader("Trees_vs", true, TreesVertexShader, "TreesVS");
	DX12Device::Register_Game_Shader("Trees_ps", false, TreesPixelShader, "TreesPS");
}
