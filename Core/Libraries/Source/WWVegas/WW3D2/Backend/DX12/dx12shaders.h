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

// HLSL source of the fixed-function emulation shaders, compiled at runtime by DX12Device.
//
// The vertex shader is compiled once per vertex format. RHW, HAS_NORMAL, HAS_DIFFUSE,
// HAS_SPECULAR and TEXCOUNT describe the Direct3D 8 FVF. The constant buffer layouts must match
// DX12VSConstants and DX12PSConstants in dx12device.cpp.
//
// The pixel shader evaluates the Direct3D 8 texture stage cascade for two stages with dynamic
// branches on the stage operations, followed by the specular add and the alpha test.

#pragma once

static const char DX12VertexShaderSource[] = R"HLSL(
cbuffer VSConstants : register(b0)
{
	row_major float4x4 WorldViewProjection;
	float4 ScreenToClip;	// RHW: xy scale, zw offset from render target pixels to NDC
	float4 DepthToClip;		// RHW: x scale, y offset from viewport depth to NDC depth
	float4 PixelOffset;		// half a pixel in NDC, emulates the Direct3D 8 pixel center
	uint4 TexCoordIndex;	// texture coordinate set used by stage 0 and stage 1
	float4 MaterialDiffuse;	// color of vertices without diffuse: white, or the material diffuse when lit
};

struct VSInput
{
#if RHW
	float4 Position : POSITION;
#else
	float3 Position : POSITION;
#endif
#if HAS_NORMAL
	float3 Normal : NORMAL;
#endif
#if HAS_DIFFUSE
	float4 Diffuse : COLOR0;
#endif
#if HAS_SPECULAR
	float4 Specular : COLOR1;
#endif
#if TEXCOUNT > 0
	float4 UV0 : TEXCOORD0;
#endif
#if TEXCOUNT > 1
	float4 UV1 : TEXCOORD1;
#endif
#if TEXCOUNT > 2
	float4 UV2 : TEXCOORD2;
#endif
#if TEXCOUNT > 3
	float4 UV3 : TEXCOORD3;
#endif
#if TEXCOUNT > 4
	float4 UV4 : TEXCOORD4;
#endif
#if TEXCOUNT > 5
	float4 UV5 : TEXCOORD5;
#endif
#if TEXCOUNT > 6
	float4 UV6 : TEXCOORD6;
#endif
#if TEXCOUNT > 7
	float4 UV7 : TEXCOORD7;
#endif
};

struct VSOutput
{
	float4 Position : SV_Position;
	float4 Diffuse : COLOR0;
	float4 Specular : COLOR1;
	float2 UV0 : TEXCOORD0;
	float2 UV1 : TEXCOORD1;
};

float2 Select_UV(VSInput input, uint index)
{
	float2 uv = float2(0.0, 0.0);
#if TEXCOUNT > 0
	if (index == 0) uv = input.UV0.xy;
#endif
#if TEXCOUNT > 1
	if (index == 1) uv = input.UV1.xy;
#endif
#if TEXCOUNT > 2
	if (index == 2) uv = input.UV2.xy;
#endif
#if TEXCOUNT > 3
	if (index == 3) uv = input.UV3.xy;
#endif
#if TEXCOUNT > 4
	if (index == 4) uv = input.UV4.xy;
#endif
#if TEXCOUNT > 5
	if (index == 5) uv = input.UV5.xy;
#endif
#if TEXCOUNT > 6
	if (index == 6) uv = input.UV6.xy;
#endif
#if TEXCOUNT > 7
	if (index == 7) uv = input.UV7.xy;
#endif
	return uv;
}

VSOutput VSMain(VSInput input)
{
	VSOutput output;

#if RHW
	// Pretransformed vertices are in render target pixels. Keep w = 1/rhw so that colors and
	// texture coordinates are interpolated with perspective correction, as Direct3D 8 did.
	float w = (input.Position.w > 0.0) ? 1.0 / input.Position.w : 1.0;
	float3 ndc;
	ndc.xy = input.Position.xy * ScreenToClip.xy + ScreenToClip.zw;
	ndc.z = input.Position.z * DepthToClip.x + DepthToClip.y;
	output.Position = float4(ndc * w, w);
#else
	output.Position = mul(float4(input.Position, 1.0), WorldViewProjection);
	output.Position.xy += PixelOffset.xy * output.Position.w;
#endif

	// Direct3D 8 colors are BGRA in memory and are read as R8G8B8A8.
#if HAS_DIFFUSE
	output.Diffuse = input.Diffuse.zyxw;
#else
	output.Diffuse = saturate(MaterialDiffuse);
#endif
#if HAS_SPECULAR
	output.Specular = input.Specular.zyxw;
#else
	output.Specular = float4(0.0, 0.0, 0.0, 0.0);
#endif

	output.UV0 = Select_UV(input, TexCoordIndex.x);
	output.UV1 = Select_UV(input, TexCoordIndex.y);
	return output;
}
)HLSL";

static const char DX12PixelShaderSource[] = R"HLSL(
Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);
SamplerState Sampler0 : register(s0);
SamplerState Sampler1 : register(s1);

cbuffer PSConstants : register(b1)
{
	uint4 ColorStage[2];	// D3DTOP, arg1, arg2, arg0
	uint4 AlphaStage[2];
	uint4 Flags;			// x: alpha test D3DCMPFUNC (0 = off), y: alpha ref, z: specular enable
	float4 TextureFactor;
};

struct PSInput
{
	float4 Position : SV_Position;
	float4 Diffuse : COLOR0;
	float4 Specular : COLOR1;
	float2 UV0 : TEXCOORD0;
	float2 UV1 : TEXCOORD1;
};

// D3DTA_DIFFUSE 0, CURRENT 1, TEXTURE 2, TFACTOR 3, SPECULAR 4, TEMP 5, COMPLEMENT 0x10, ALPHAREPLICATE 0x20.
// Direct3D 8 stages always write to CURRENT here, so TEMP stays zero.
float4 Stage_Arg(uint arg, float4 diffuse, float4 current, float4 tex, float4 specular)
{
	float4 value;
	uint source = arg & 0x0f;
	if (source == 0)
		value = diffuse;
	else if (source == 2)
		value = tex;
	else if (source == 3)
		value = TextureFactor;
	else if (source == 4)
		value = specular;
	else if (source == 5)
		value = float4(0.0, 0.0, 0.0, 0.0);
	else
		value = current;

	if (arg & 0x20)
		value = value.aaaa;
	if (arg & 0x10)
		value = 1.0 - value;
	return value;
}

// D3DTEXTUREOP. The bump mapping operations pass the current value through.
float4 Stage_Op(uint op, float4 arg0, float4 arg1, float4 arg2, float4 diffuse, float4 current, float4 tex, float4 next_tex)
{
	switch (op)
	{
	case 2: return arg1;
	case 3: return arg2;
	case 4: return arg1 * arg2;
	case 5: return arg1 * arg2 * 2.0;
	case 6: return arg1 * arg2 * 4.0;
	case 7: return arg1 + arg2;
	case 8: return arg1 + arg2 - 0.5;
	case 9: return (arg1 + arg2 - 0.5) * 2.0;
	case 10: return arg1 - arg2;
	case 11: return arg1 + arg2 - arg1 * arg2;
	case 12: return lerp(arg2, arg1, diffuse.a);
	case 13: return lerp(arg2, arg1, tex.a);
	case 14: return lerp(arg2, arg1, TextureFactor.a);
	case 15: return arg1 + arg2 * (1.0 - tex.a);
	case 16: return lerp(arg2, arg1, current.a);
	case 17: return arg1 * next_tex;
	case 18: return float4(arg1.rgb + arg1.a * arg2.rgb, arg1.a);
	case 19: return float4(arg1.rgb * arg2.rgb + arg1.a, arg1.a);
	case 20: return float4(arg1.rgb + (1.0 - arg1.a) * arg2.rgb, arg1.a);
	case 21: return float4((1.0 - arg1.rgb) * arg2.rgb + arg1.a, arg1.a);
	case 24: return (4.0 * dot(arg1.rgb - 0.5, arg2.rgb - 0.5)).xxxx;
	case 25: return arg0 + arg1 * arg2;
	case 26: return lerp(arg2, arg1, arg0);
	default: return current;
	}
}

float4 Apply_Stage(uint4 color_stage, uint4 alpha_stage, float4 diffuse, float4 current, float4 tex, float4 next_tex, float4 specular)
{
	float4 c0 = Stage_Arg(color_stage.w, diffuse, current, tex, specular);
	float4 c1 = Stage_Arg(color_stage.y, diffuse, current, tex, specular);
	float4 c2 = Stage_Arg(color_stage.z, diffuse, current, tex, specular);
	float4 color = Stage_Op(color_stage.x, c0, c1, c2, diffuse, current, tex, next_tex);

	// D3DTOP_DOTPRODUCT3 as color operation also writes alpha. A disabled alpha stage keeps the current alpha.
	float alpha = current.a;
	if (color_stage.x == 24)
	{
		alpha = color.a;
	}
	else if (alpha_stage.x > 1)
	{
		float4 a0 = Stage_Arg(alpha_stage.w, diffuse, current, tex, specular);
		float4 a1 = Stage_Arg(alpha_stage.y, diffuse, current, tex, specular);
		float4 a2 = Stage_Arg(alpha_stage.z, diffuse, current, tex, specular);
		alpha = Stage_Op(alpha_stage.x, a0, a1, a2, diffuse, current, tex, next_tex).a;
	}

	return saturate(float4(color.rgb, alpha));
}

float4 PSMain(PSInput input) : SV_Target
{
	float4 tex0 = Texture0.Sample(Sampler0, input.UV0);
	float4 tex1 = Texture1.Sample(Sampler1, input.UV1);
	float4 diffuse = input.Diffuse;
	float4 specular = input.Specular;
	float4 white = float4(1.0, 1.0, 1.0, 1.0);

	// D3DTOP_DISABLE (1) on a color stage ends the cascade. CURRENT starts as the diffuse color.
	float4 current = diffuse;
	if (ColorStage[0].x > 1)
	{
		current = Apply_Stage(ColorStage[0], AlphaStage[0], diffuse, current, tex0, tex1, specular);
		if (ColorStage[1].x > 1)
		{
			current = Apply_Stage(ColorStage[1], AlphaStage[1], diffuse, current, tex1, white, specular);
		}
	}

	if (Flags.z != 0)
	{
		current.rgb = saturate(current.rgb + specular.rgb);
	}

	if (Flags.x != 0)
	{
		float alpha = round(current.a * 255.0);
		float reference = (float)Flags.y;
		bool passed;
		switch (Flags.x)
		{
		case 1: passed = false; break;
		case 2: passed = alpha < reference; break;
		case 3: passed = alpha == reference; break;
		case 4: passed = alpha <= reference; break;
		case 5: passed = alpha > reference; break;
		case 6: passed = alpha != reference; break;
		case 7: passed = alpha >= reference; break;
		default: passed = true; break;
		}
		if (!passed)
			discard;
	}

	return current;
}
)HLSL";
