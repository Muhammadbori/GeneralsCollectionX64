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

// HLSL ports of the game's terrain, road and screen filter pixel shaders: monochrome, terrain, terrainnoise,
// terrainnoise2, roadnoise2, fterrain, fterrain0, fterrainnoise and fterrainnoise2 (shaders\*.pso,
// sources GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shaders/*.nvp; the retail .pso
// files in shaders.big and ShadersZH.big disassemble to the same instructions).
// See dx12gameshaders.h for the contract.
//
// Each port follows its ps.1.1 source instruction by instruction:
//	tex tN				TextureN.Sample(SamplerN, input.TexN.xy); texture registers are [0, 1]
//	v0					saturate(input.Diffuse); color registers are [0, 1]
//	cN					c[N], already clamped to [-1, 1] by the device
//	mov / mul / lrp / dp3	the same arithmetic, every result clamped to the [-1, 1] register range of
//						ps.1.1 hardware (MaxPixelShaderValue 1), as PS11_Range does
//	lrp d, s0, s1, s2	d = s0 * s1 + (1 - s0) * s2, per component
//	dp3 d, s0, s1		d = dot(s0.rgb, s1.rgb) in all four components (ps.1.1 writes r, g, b and a)
// The function returns r0; the device saturates it and applies the alpha test and fog afterwards.

#include "WW3D2/Backend/DX12/dx12gameshaders.h"

namespace
{

// Helpers shared by every port (each registered source is compiled on its own).
#define TERRAIN_PS_HELPERS \
	"float4 PS11_Range(float4 value)\n" \
	"{\n" \
	"\treturn clamp(value, -1.0, 1.0);\n" \
	"}\n" \
	"\n" \
	"float4 PS11_Lrp(float4 s0, float4 s1, float4 s2)\n" \
	"{\n" \
	"\treturn PS11_Range(s0 * s1 + (1.0 - s0) * s2);\n" \
	"}\n" \
	"\n" \
	"float4 PS11_Dp3(float4 s0, float4 s1)\n" \
	"{\n" \
	"\treturn PS11_Range(dot(s0.rgb, s1.rgb).xxxx);\n" \
	"}\n" \
	"\n"

// monochrome.nvp: black and white screen filter (ScreenBWFilter). c0 holds the luminance weights, c1 the
// filter color and c2 the fade amount.
const char MonochromeSource[] =
	TERRAIN_PS_HELPERS
	"float4 MonochromePS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 r1 = PS11_Dp3(t0, c[0]);\t\t\t\t// dp3 r1, t0, c0\n"
	"\tr1 = PS11_Range(r1 * c[1]);\t\t\t\t// mul r1, r1, c1\n"
	"\tfloat4 r0 = PS11_Lrp(c[2], r1, t0);\t\t// lrp r0, c2, r1, t0\n"
	"\treturn r0;\n"
	"}\n";

// terrain.nvp: blends the two terrain tiles by the vertex alpha and lights the result.
const char TerrainSource[] =
	TERRAIN_PS_HELPERS
	"float4 TerrainPS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 r0 = PS11_Lrp(v0.aaaa, t1, t0);\t\t// lrp r0, v0.a, t1, t0\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\treturn r0;\n"
	"}\n";

// terrainnoise.nvp: terrain.nvp, then modulated by one noise texture (cloud or light map).
const char TerrainNoiseSource[] =
	TERRAIN_PS_HELPERS
	"float4 TerrainNoisePS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 r0 = PS11_Lrp(v0.aaaa, t1, t0);\t\t// lrp r0, v0.a, t1, t0\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\tr0 = PS11_Range(r0 * t2);\t\t\t\t\t// mul r0, r0, t2\n"
	"\treturn r0;\n"
	"}\n";

// terrainnoise2.nvp: terrain.nvp, then modulated by the cloud and the light map textures.
const char TerrainNoise2Source[] =
	TERRAIN_PS_HELPERS
	"float4 TerrainNoise2PS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 t3 = Texture3.Sample(Sampler3, input.Tex3.xy);\n"
	"\tfloat4 r0 = PS11_Lrp(v0.aaaa, t1, t0);\t\t// lrp r0, v0.a, t1, t0\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\tr0 = PS11_Range(r0 * t2);\t\t\t\t\t// mul r0, r0, t2\n"
	"\tr0 = PS11_Range(r0 * t3);\t\t\t\t\t// mul r0, r0, t3\n"
	"\treturn r0;\n"
	"}\n";

// roadnoise2.nvp: road texture modulated by the cloud and the light map textures, then lit.
const char RoadNoise2Source[] =
	TERRAIN_PS_HELPERS
	"float4 RoadNoise2PS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 r0 = PS11_Range(t0 * t1);\t\t\t// mul r0, t0, t1\n"
	"\tr0 = PS11_Range(r0 * t2);\t\t\t\t\t// mul r0, r0, t2\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\treturn r0;\n"
	"}\n";

// fterrain.nvp: flat terrain; stage 0 (shroud or cloud) multiplied into the terrain of stage 1, then lit.
const char FlatTerrainSource[] =
	TERRAIN_PS_HELPERS
	"float4 FlatTerrainPS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 r0 = PS11_Range(t1 * t0);\t\t\t// mul r0, t1, t0\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\treturn r0;\n"
	"}\n";

// fterrain0.nvp: flat terrain of stage 1 only, lit (stage 0 is not sampled).
const char FlatTerrain0Source[] =
	TERRAIN_PS_HELPERS
	"float4 FlatTerrain0PS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 r0 = t1;\t\t\t\t\t\t\t\t// mov r0, t1\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\treturn r0;\n"
	"}\n";

// fterrainnoise.nvp: fterrain.nvp, then modulated by the texture of stage 2.
const char FlatTerrainNoiseSource[] =
	TERRAIN_PS_HELPERS
	"float4 FlatTerrainNoisePS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 r0 = PS11_Range(t1 * t0);\t\t\t// mul r0, t1, t0\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\tr0 = PS11_Range(r0 * t2);\t\t\t\t\t// mul r0, r0, t2\n"
	"\treturn r0;\n"
	"}\n";

// fterrainnoise2.nvp: fterrain.nvp, then modulated by the textures of stages 2 and 3.
const char FlatTerrainNoise2Source[] =
	TERRAIN_PS_HELPERS
	"float4 FlatTerrainNoise2PS(GamePSInput input)\n"
	"{\n"
	"\tfloat4 v0 = saturate(input.Diffuse);\n"
	"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
	"\tfloat4 t1 = Texture1.Sample(Sampler1, input.Tex1.xy);\n"
	"\tfloat4 t2 = Texture2.Sample(Sampler2, input.Tex2.xy);\n"
	"\tfloat4 t3 = Texture3.Sample(Sampler3, input.Tex3.xy);\n"
	"\tfloat4 r0 = PS11_Range(t1 * t0);\t\t\t// mul r0, t1, t0\n"
	"\tr0 = PS11_Range(r0 * v0);\t\t\t\t\t// mul r0, r0, v0\n"
	"\tr0 = PS11_Range(r0 * t2);\t\t\t\t\t// mul r0, r0, t2\n"
	"\tr0 = PS11_Range(r0 * t3);\t\t\t\t\t// mul r0, r0, t3\n"
	"\treturn r0;\n"
	"}\n";

#undef TERRAIN_PS_HELPERS

} // namespace

void DX12_Register_Terrain_Shaders()
{
	DX12Device::Register_Game_Shader("monochrome_ps", false, MonochromeSource, "MonochromePS");
	DX12Device::Register_Game_Shader("terrain_ps", false, TerrainSource, "TerrainPS");
	DX12Device::Register_Game_Shader("terrainnoise_ps", false, TerrainNoiseSource, "TerrainNoisePS");
	DX12Device::Register_Game_Shader("terrainnoise2_ps", false, TerrainNoise2Source, "TerrainNoise2PS");
	DX12Device::Register_Game_Shader("roadnoise2_ps", false, RoadNoise2Source, "RoadNoise2PS");
	DX12Device::Register_Game_Shader("fterrain_ps", false, FlatTerrainSource, "FlatTerrainPS");
	DX12Device::Register_Game_Shader("fterrain0_ps", false, FlatTerrain0Source, "FlatTerrain0PS");
	DX12Device::Register_Game_Shader("fterrainnoise_ps", false, FlatTerrainNoiseSource, "FlatTerrainNoisePS");
	DX12Device::Register_Game_Shader("fterrainnoise2_ps", false, FlatTerrainNoise2Source, "FlatTerrainNoise2PS");
}
