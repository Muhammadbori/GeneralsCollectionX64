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

// HLSL ports of the game's water shaders: wave_ps and wave_vs (Core/GameEngineDevice/Source/W3DDevice/GameClient/
// Water/wave.nvp, wave.nvv) and the shaders W3DWater assembles at run time, water_river_ps,
// water_bump_ps and water_trapezoid_ps.
// See dx12gameshaders.h for the contract.
//
// The shipped wave.pso / wave.vso (shaders.big) are the variants without DO_WATER_ALPHA_TEXTURE, so
// that is what is ported here.
//
// ps.1.1 arithmetic: texture and color inputs are [0, 1] and the r registers hold [-1, 1]. Every
// intermediate result below that can leave [0, 1] is an add of non-negative terms, so the
// Direct3D 8 clamp is saturate(); the final result is saturated by the wrapper.
//
// Bump maps (texbem). Direct3D 12 mode has no signed texture formats (every texture is stored as
// B8G8R8A8_UNORM), so W3DWater stores the du / dv of a Direct3D 8 V8U8 bump map in the red / green
// channels of an A8R8G8B8 texture, biased by 128 (texel byte = signed value + 128). WaterBumpDuDv
// turns that back into the signed [-1, 1] values texbem reads (s / 127, the DXGI SNORM rule, so a
// texel of 128 is exactly no perturbation). The bump stage of a ported texbem shader must therefore
// hold such a texture: an empty stage samples opaque white, which would decode as du = dv = 1.

#include "WW3D2/Backend/DX12/dx12gameshaders.h"

namespace
{

// texbem tDest, tSrc: the coordinates of stage Dest perturbed by the bump matrix of stage Dest,
//	u' = u + BUMPENVMAT00 * du + BUMPENVMAT10 * dv
//	v' = v + BUMPENVMAT01 * du + BUMPENVMAT11 * dv
// BumpEnvMat[stage] is (00, 01, 10, 11).
#define WATER_BUMP_HELPERS \
	"float2 WaterBumpDuDv(float4 bump)\n" \
	"{\n" \
	"\treturn (bump.rg * 255.0 - 128.0) / 127.0;\n" \
	"}\n" \
	"\n" \
	"float2 WaterTexBem(float2 uv, float4 bumpMatrix, float2 dudv)\n" \
	"{\n" \
	"\treturn uv + dudv.x * bumpMatrix.xy + dudv.y * bumpMatrix.zw;\n" \
	"}\n" \
	"\n"

// wave.vso (wave.nvv), declaration v0 = FLOAT3 position, v1 = D3DCOLOR diffuse, v2 = FLOAT2 bump
// map coordinates. c2..c5 world-view-projection (transposed), c6 = screen projection scale / offset.
const char WaveVS[] =
	"struct GameVSInput\n"
	"{\n"
	"\tfloat4 Position : V0;\n"
	"\tfloat4 Diffuse : V1;\n"
	"\tfloat4 Tex : V2;\n"
	"};\n"
	"\n"
	"GameVSOutput WaveVS(GameVSInput input)\n"
	"{\n"
	"\tGameVSOutput output = (GameVSOutput)0;\n"
	// dp4 r0.x..w, v0, c2..c5 / mov oPos, r0
	"\tfloat4 r0 = float4(dot(input.Position, c[2]), dot(input.Position, c[3]), dot(input.Position, c[4]), dot(input.Position, c[5]));\n"
	"\toutput.Position = r0;\n"
	// rcp r1.w, r0.w / mul r1.xy, r0.xy, r1.w / mad oT1.xy, r1.xy, c6.xy, c6.zw
	"\tfloat2 r1 = r0.xy * (1.0 / r0.w);\n"
	"\toutput.Tex1 = float4(r1 * c[6].xy + c[6].zw, 0.0, 1.0);\n"
	// mov oT0, v2 / mov oD0, v1
	"\toutput.Tex0 = input.Tex;\n"
	"\toutput.Diffuse = input.Diffuse;\n"
	// No oFog.
	"\toutput.Fog = 1.0;\n"
	"\treturn output;\n"
	"}\n";

// wave.pso (wave.nvp): tex t0 / texbem t1, t0 / mul r0, t1, v0
// Stage 0 is the bump map, stage 1 the reflection texture with stage 1's bump matrix.
const char WavePS[] =
	WATER_BUMP_HELPERS
	"float4 WavePS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, WaterTexBem(input.Tex1.xy, BumpEnvMat[1], WaterBumpDuDv(t0)));\n"
	"\treturn t1 * input.Diffuse;\n"
	"}\n";

// W3DWater m_riverWaterPixelShader:
//	tex t0..t3
//	mul r0.rgb, v0, t0
//	mov r0.a, t0
//	mul r1, t1, t2
//	add r1.rgb, r1, t3
//	mul r1.rgb, r1, v0.a
//	+mul r0.a, r0, t3
//	add r0.rgb, r0, r1
const char WaterRiverPS[] =
	"float4 WaterRiverPS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 t3 = Texture3.Sample(Sampler3, input.Tex3.xy);\n"
	"\tfloat4 r0;\n"
	"\tr0.rgb = input.Diffuse.rgb * t0.rgb;\n"
	"\tr0.a = t0.a;\n"
	"\tfloat3 r1 = saturate(t1.rgb * t2.rgb + t3.rgb);\n"
	"\tr1 = r1 * input.Diffuse.a;\n"
	"\tr0.a = r0.a * t3.a;\n"
	"\tr0.rgb = r0.rgb + r1;\n"
	"\treturn r0;\n"
	"}\n";

// W3DWater m_waterPixelShader:
//	tex t0
//	tex t1
//	texbem t2, t1
//	mul r0, v0, t0
//	mul r1.rgb, t2, c0
//	add r0.rgb, r0, r1
// Stage 1 is the bump map (see above), stage 2 the environment map with stage 2's bump matrix.
const char WaterBumpPS[] =
	WATER_BUMP_HELPERS
	"float4 WaterBumpPS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, WaterTexBem(input.Tex2.xy, BumpEnvMat[2], WaterBumpDuDv(t1)));\n"
	"\tfloat4 r0 = input.Diffuse * t0;\n"
	"\tfloat3 r1 = t2.rgb * c[0].rgb;\n"
	"\tr0.rgb = r0.rgb + r1;\n"
	"\treturn r0;\n"
	"}\n";

// W3DWater m_trapezoidWaterPixelShader:
//	tex t0..t3
//	mul r0, v0, t0
//	mad r0.rgb, t1, t2, r0
//	mul r0.rgb, r0, t3
const char WaterTrapezoidPS[] =
	"float4 WaterTrapezoidPS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 t3 = Texture3.Sample(Sampler3, input.Tex3.xy);\n"
	"\tfloat4 r0 = input.Diffuse * t0;\n"
	"\tr0.rgb = saturate(t1.rgb * t2.rgb + r0.rgb);\n"
	"\tr0.rgb = r0.rgb * t3.rgb;\n"
	"\treturn r0;\n"
	"}\n";

#undef WATER_BUMP_HELPERS

} // namespace

void DX12_Register_Water_Shaders()
{
	DX12Device::Register_Game_Shader("wave_vs", true, WaveVS, "WaveVS");
	DX12Device::Register_Game_Shader("wave_ps", false, WavePS, "WavePS");
	DX12Device::Register_Game_Shader("water_river_ps", false, WaterRiverPS, "WaterRiverPS");
	DX12Device::Register_Game_Shader("water_bump_ps", false, WaterBumpPS, "WaterBumpPS");
	DX12Device::Register_Game_Shader("water_trapezoid_ps", false, WaterTrapezoidPS, "WaterTrapezoidPS");
}
