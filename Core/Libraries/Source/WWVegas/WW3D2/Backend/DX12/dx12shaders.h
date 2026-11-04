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

// Fixed parts of the fixed-function emulation shaders. dx12ffp.cpp defines MAX_STAGES and
// MAX_LIGHTS and adds the texture declarations, the vertex input and interface structs and the
// generated main function. The constant buffer layouts must match DX12VSConstants and
// DX12PSConstants in dx12ffp.h.

#pragma once

static const char DX12VertexShaderPrologue[] = R"HLSL(
struct Light
{
	float4 Diffuse;
	float4 Specular;
	float4 Ambient;
	float4 Position;		// camera space, w = range
	float4 Direction;		// camera space, normalized, w = falloff
	float4 Attenuation;		// a0, a1, a2
	float4 Spot;			// cos(theta / 2), cos(phi / 2), 1 / (cos(theta / 2) - cos(phi / 2))
};

cbuffer VSConstants : register(b0)
{
	row_major float4x4 WorldViewProjection;
	row_major float4x4 WorldView;
	row_major float4x4 NormalMatrix;		// inverse transpose of WorldView
	row_major float4x4 TextureMatrix[MAX_STAGES];
	float4 ScreenToClip;	// RHW: xy scale, zw offset from render target pixels to NDC
	float4 DepthToClip;		// RHW: x scale, y offset from viewport depth to NDC depth
	float4 PixelOffset;		// half a pixel in NDC, emulates the Direct3D 8 pixel center
	float4 MaterialDiffuse;
	float4 MaterialAmbient;
	float4 MaterialSpecular;
	float4 MaterialEmissive;
	float4 GlobalAmbient;
	float4 Fog;				// end, 1 / (end - start), density, 1 = table fog uses eye depth
	float4 MaterialPower;
	Light Lights[MAX_LIGHTS];
};
)HLSL";

static const char DX12PixelShaderPrologue[] = R"HLSL(
cbuffer PSConstants : register(b1)
{
	float4 TextureFactor;
	float4 FogColor;
	float4 Fog;				// end, 1 / (end - start), density
	float4 AlphaRef;		// 0..255
};
)HLSL";
