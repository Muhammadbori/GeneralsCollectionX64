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

// Game shader registry: names, HLSL sources, handles, Direct3D 8 vertex declarations and the
// prologue and wrapper around each ported function. Compiling and caching the result is up to
// DX12Device, which owns the compiler and the pipeline states.

#include "WW3D2/Backend/DX12/dx12gameshaders.h"
#include "WW3D2/Backend/DX12/dx12ffp.h"

#include <dxgiformat.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{

struct RegisteredShader
{
	std::string Name;
	bool VertexShader;
	std::string Source;
	std::string EntryPoint;
};

struct HandleSlot
{
	HandleSlot() : Index(-1), VertexShader(false), LayoutId(0) {}

	int Index;			// registered shader, -1 when the slot is free
	bool VertexShader;
	unsigned LayoutId;
};

std::vector<RegisteredShader> Shaders;
std::vector<HandleSlot> Handles;	// slot 0 is never used, so no handle is 0
std::vector<unsigned> FreeHandleSlots;

// Direct3D 8 vertex declaration tokens (d3d8types.h D3DVSD_*), spelled out so the backend does not
// depend on d3d8.h.
enum
{
	VSD_TOKEN_TYPE_SHIFT = 29,
	VSD_TOKEN_NOP = 0,
	VSD_TOKEN_STREAM = 1,
	VSD_TOKEN_STREAMDATA = 2,
	VSD_TOKEN_TESSELLATOR = 3,
	VSD_TOKEN_CONSTMEM = 4,
	VSD_TOKEN_EXT = 5,
	VSD_TOKEN_END = 7,

	VSD_STREAM_NUMBER_MASK = 0xf,
	VSD_STREAM_TESS_BIT = 0x10000000,
	VSD_DATA_LOAD_TYPE_BIT = 0x10000000,	// set for D3DVSD_SKIP
	VSD_DATA_TYPE_SHIFT = 16,
	VSD_DATA_TYPE_MASK = 0xf,
	VSD_SKIP_COUNT_SHIFT = 16,
	VSD_SKIP_COUNT_MASK = 0xf,
	VSD_VERTEX_REG_MASK = 0x1f,
	VSD_EXT_COUNT_SHIFT = 24,
	VSD_EXT_COUNT_MASK = 0x1f,

	VSDT_FLOAT1 = 0,
	VSDT_FLOAT2 = 1,
	VSDT_FLOAT3 = 2,
	VSDT_FLOAT4 = 3,
	VSDT_D3DCOLOR = 4,
	VSDT_UBYTE4 = 5,
	VSDT_SHORT2 = 6,
	VSDT_SHORT4 = 7,

	// A declaration without D3DVSD_END is not read past this many tokens.
	VSD_MAX_TOKENS = 256,
};

const char VertexEntry[] = "GameVSMain";
const char PixelEntry[] = "GamePSMain";

void Append(std::string & text, const char * format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length > 0)
	{
		text.append(buffer, (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1);
	}
}

void Append_Vertex_Prologue(std::string & hlsl, const char * name)
{
	Append(hlsl, "// Game vertex shader %s\n", name);
	Append(hlsl, "#define MAX_STAGES %u\n", (unsigned)DX12_MAX_STAGES);
	Append(hlsl, "cbuffer GameVSConstants : register(b3)\n{\n\tfloat4 c[%u];\n};\n", (unsigned)DX12_MAX_VS_CONSTANTS);
	// The start of the fixed-function VSConstants (dx12shaders.h), for the half pixel offset.
	hlsl += "cbuffer GameFixedFunctionVSConstants : register(b0)\n{\n";
	hlsl += "\trow_major float4x4 DX12_WorldViewProjection;\n";
	hlsl += "\trow_major float4x4 DX12_WorldView;\n";
	hlsl += "\trow_major float4x4 DX12_NormalMatrix;\n";
	hlsl += "\trow_major float4x4 DX12_TextureMatrix[MAX_STAGES];\n";
	hlsl += "\tfloat4 DX12_ScreenToClip;\n";
	hlsl += "\tfloat4 DX12_DepthToClip;\n";
	hlsl += "\tfloat4 DX12_PixelOffset;\n";
	hlsl += "};\n";
	DX12FFP_Append_Interface(hlsl, "GameVSOutput");
	hlsl += "\n// Ported shader\n";
}

void Append_Vertex_Wrapper(std::string & hlsl, const char * entry_point)
{
	Append(hlsl, "\nGameVSOutput %s(GameVSInput input)\n{\n", VertexEntry);
	Append(hlsl, "\tGameVSOutput output = %s(input);\n", entry_point);
	// Direct3D 8 puts pixel centers on integer coordinates, Direct3D 12 half a pixel further.
	hlsl += "\toutput.Position.xy += DX12_PixelOffset.xy * output.Position.w;\n";
	hlsl += "\toutput.Diffuse = saturate(output.Diffuse);\n";
	hlsl += "\toutput.Specular = saturate(output.Specular);\n";
	hlsl += "\toutput.Fog = saturate(output.Fog);\n";
	hlsl += "\treturn output;\n}\n";
}

void Append_Pixel_Prologue(std::string & hlsl, const char * name)
{
	Append(hlsl, "// Game pixel shader %s\n", name);
	Append(hlsl, "#define MAX_STAGES %u\n", (unsigned)DX12_MAX_STAGES);
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		Append(hlsl, "Texture2D Texture%u : register(t%u);\n", stage, stage);
		Append(hlsl, "SamplerState Sampler%u : register(s%u);\n", stage, stage);
	}
	Append(hlsl, "cbuffer GamePSConstants : register(b2)\n{\n\tfloat4 c[%u];\n\tfloat4 BumpEnvMat[MAX_STAGES];\n\tfloat4 BumpEnvLuminance[MAX_STAGES];\n};\n",
		(unsigned)DX12_MAX_PS_CONSTANTS);
	// The fixed-function PSConstants (dx12shaders.h), for the alpha test and fog.
	hlsl += "cbuffer GameFixedFunctionPSConstants : register(b1)\n{\n";
	hlsl += "\tfloat4 DX12_TextureFactor;\n";
	hlsl += "\tfloat4 DX12_FogColor;\n";
	hlsl += "\tfloat4 DX12_Fog;\n";
	hlsl += "\tfloat4 DX12_AlphaRef;\n";
	hlsl += "};\n";
	DX12FFP_Append_Interface(hlsl, "GameVSOutput");
	hlsl += "typedef GameVSOutput GamePSInput;\n";
	hlsl += "\n// Ported shader\n";
}

void Append_Pixel_Wrapper(std::string & hlsl, const char * entry_point, const DX12GamePSVariant & variant)
{
	static const char components[5] = { 'x', 'x', 'y', 'z', 'w' };

	Append(hlsl, "\nfloat4 %s(GamePSInput input) : SV_Target\n{\n", PixelEntry);
	hlsl += "\tGamePSInput game_input = input;\n";
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		unsigned divisor = variant.Projected[stage];
		if (divisor >= 1 && divisor <= 4)
			Append(hlsl, "\tgame_input.Tex%u.xy /= game_input.Tex%u.%c;\n", stage, stage, components[divisor]);
	}
	Append(hlsl, "\tfloat4 current = saturate(%s(game_input));\n", entry_point);

	DX12PSKey key;
	memset(&key, 0, sizeof(key));
	key.AlphaFunc = variant.AlphaFunc;
	key.Fog = variant.Fog;
	key.FogTableMode = variant.FogTableMode;
	DX12FFP_Append_Alpha_Test_And_Fog(key, "DX12_", hlsl);

	hlsl += "\treturn current;\n}\n";
}

bool Is_Valid_Index(int index)
{
	return index >= 0 && index < (int)Shaders.size();
}

} // namespace

void DX12_Register_Game_Shaders()
{
	DX12_Register_Terrain_Shaders();
	DX12_Register_Water_Shaders();
	DX12_Register_Tree_Shaders();
}

int DX12GameShaders_Register(const char * name, bool vertex_shader, const char * hlsl_source, const char * entry_point)
{
	if (name == nullptr || name[0] == '\0' || hlsl_source == nullptr || entry_point == nullptr || entry_point[0] == '\0')
	{
		return -1;
	}
	// The wrapper's own entry points cannot be the ported function.
	if (strcmp(entry_point, VertexEntry) == 0 || strcmp(entry_point, PixelEntry) == 0)
	{
		return -1;
	}

	int index = DX12GameShaders_Find(name, vertex_shader);
	if (index < 0)
	{
		index = (int)Shaders.size();
		Shaders.push_back(RegisteredShader());
	}
	RegisteredShader & shader = Shaders[index];
	shader.Name = name;
	shader.VertexShader = vertex_shader;
	shader.Source = hlsl_source;
	shader.EntryPoint = entry_point;
	return index;
}

int DX12GameShaders_Find(const char * name, bool vertex_shader)
{
	if (name == nullptr)
	{
		return -1;
	}
	for (size_t i = 0; i < Shaders.size(); ++i)
	{
		if (Shaders[i].VertexShader == vertex_shader && _stricmp(Shaders[i].Name.c_str(), name) == 0)
		{
			return (int)i;
		}
	}
	return -1;
}

const char * DX12GameShaders_Get_Name(int index)
{
	return Is_Valid_Index(index) ? Shaders[index].Name.c_str() : "";
}

bool DX12GameShaders_Is_Vertex_Shader(int index)
{
	return Is_Valid_Index(index) && Shaders[index].VertexShader;
}

int DX12GameShaders_Get_Count()
{
	return (int)Shaders.size();
}

void DX12GameShaders_Generate(int index, const DX12GamePSVariant * variant, std::string & hlsl, const char ** entry, const char ** target)
{
	hlsl.clear();
	if (!Is_Valid_Index(index))
	{
		return;
	}

	const RegisteredShader & shader = Shaders[index];
	if (shader.VertexShader)
	{
		Append_Vertex_Prologue(hlsl, shader.Name.c_str());
		hlsl += shader.Source;
		Append_Vertex_Wrapper(hlsl, shader.EntryPoint.c_str());
		*entry = VertexEntry;
		*target = "vs_5_0";
	}
	else
	{
		DX12GamePSVariant none;
		memset(&none, 0, sizeof(none));
		Append_Pixel_Prologue(hlsl, shader.Name.c_str());
		hlsl += shader.Source;
		Append_Pixel_Wrapper(hlsl, shader.EntryPoint.c_str(), variant ? *variant : none);
		*entry = PixelEntry;
		*target = "ps_5_0";
	}
}

bool DX12GameShaders_Parse_Declaration(const unsigned * tokens, DX12GameInputLayout & layout, std::string & error)
{
	memset(&layout, 0, sizeof(layout));
	error.clear();
	if (tokens == nullptr)
	{
		error = "no vertex declaration";
		return false;
	}

	unsigned stream = 0;
	unsigned offset = 0;
	unsigned used_registers = 0;
	for (unsigned i = 0; i < VSD_MAX_TOKENS; ++i)
	{
		const unsigned token = tokens[i];
		if (token == 0xffffffffu)
		{
			layout.Size = offset;
			if (layout.ElementCount == 0)
			{
				error = "the vertex declaration has no stream data";
				return false;
			}
			return true;
		}

		switch (token >> VSD_TOKEN_TYPE_SHIFT)
		{
		case VSD_TOKEN_NOP:
			if (token != 0)
			{
				Append(error, "unsupported extension token 0x%08x", token);
				return false;
			}
			break;

		case VSD_TOKEN_STREAM:
			if (token & VSD_STREAM_TESS_BIT)
			{
				error = "tessellator streams are not supported";
				return false;
			}
			stream = token & VSD_STREAM_NUMBER_MASK;
			offset = 0;
			break;

		case VSD_TOKEN_STREAMDATA:
		{
			if (stream != 0)
			{
				Append(error, "stream %u is not supported, only stream 0", stream);
				return false;
			}
			if (token & VSD_DATA_LOAD_TYPE_BIT)
			{
				offset += 4 * ((token >> VSD_SKIP_COUNT_SHIFT) & VSD_SKIP_COUNT_MASK);
				break;
			}

			const unsigned reg = token & VSD_VERTEX_REG_MASK;
			const unsigned type = (token >> VSD_DATA_TYPE_SHIFT) & VSD_DATA_TYPE_MASK;
			if (reg >= DX12_GAME_MAX_INPUTS || (used_registers & (1u << reg)))
			{
				Append(error, "input register v%u is out of range or declared twice", reg);
				return false;
			}

			unsigned format;
			unsigned size;
			switch (type)
			{
			case VSDT_FLOAT1: format = DXGI_FORMAT_R32_FLOAT; size = 4; break;
			case VSDT_FLOAT2: format = DXGI_FORMAT_R32G32_FLOAT; size = 8; break;
			case VSDT_FLOAT3: format = DXGI_FORMAT_R32G32B32_FLOAT; size = 12; break;
			case VSDT_FLOAT4: format = DXGI_FORMAT_R32G32B32A32_FLOAT; size = 16; break;
			// D3DCOLOR is ARGB, BGRA in memory, and Direct3D 8 hands it to the shader as (r, g, b, a).
			case VSDT_D3DCOLOR: format = DXGI_FORMAT_B8G8R8A8_UNORM; size = 4; break;
			case VSDT_UBYTE4: format = DXGI_FORMAT_R8G8B8A8_UINT; size = 4; break;
			case VSDT_SHORT2: format = DXGI_FORMAT_R16G16_SINT; size = 4; break;
			case VSDT_SHORT4: format = DXGI_FORMAT_R16G16B16A16_SINT; size = 8; break;
			default:
				Append(error, "unsupported data type %u for v%u", type, reg);
				return false;
			}

			DX12GameInputElement & element = layout.Elements[layout.ElementCount++];
			element.Register = reg;
			element.Format = format;
			element.Offset = offset;
			offset += size;
			used_registers |= 1u << reg;
			break;
		}

		case VSD_TOKEN_EXT:
			// Extension tokens carry their own count of data tokens.
			i += (token >> VSD_EXT_COUNT_SHIFT) & VSD_EXT_COUNT_MASK;
			break;

		case VSD_TOKEN_TESSELLATOR:
			error = "tessellator data is not supported";
			return false;

		case VSD_TOKEN_CONSTMEM:
			error = "D3DVSD_CONST is not supported; set the constants with SetVertexShaderConstant";
			return false;

		default:
			Append(error, "invalid token 0x%08x", token);
			return false;
		}
	}

	error = "the vertex declaration has no D3DVSD_END";
	return false;
}

unsigned DX12GameShaders_Allocate_Handle(int index, bool vertex_shader, unsigned layout_id)
{
	if (!Is_Valid_Index(index) || Shaders[index].VertexShader != vertex_shader)
	{
		return 0;
	}
	if (Handles.empty())
	{
		Handles.push_back(HandleSlot());
	}

	unsigned slot;
	if (!FreeHandleSlots.empty())
	{
		slot = FreeHandleSlots.back();
		FreeHandleSlots.pop_back();
	}
	else
	{
		// Handles keep bit 31 clear; a billion live shaders is not a concern.
		if (Handles.size() >= 0x40000000u)
		{
			return 0;
		}
		slot = (unsigned)Handles.size();
		Handles.push_back(HandleSlot());
	}

	HandleSlot & entry = Handles[slot];
	entry.Index = index;
	entry.VertexShader = vertex_shader;
	entry.LayoutId = layout_id;
	// Like Direct3D 8, vertex shader handles have bit 0 set, which no FVF code has.
	return (slot << 1) | (vertex_shader ? 1u : 0u);
}

bool DX12GameShaders_Resolve_Handle(unsigned handle, bool vertex_shader, int * index, unsigned * layout_id)
{
	if (handle == 0 || ((handle & 1u) != 0) != vertex_shader)
	{
		return false;
	}
	const unsigned slot = handle >> 1;
	if (slot == 0 || slot >= Handles.size() || Handles[slot].Index < 0 || Handles[slot].VertexShader != vertex_shader)
	{
		return false;
	}
	if (index)
		*index = Handles[slot].Index;
	if (layout_id)
		*layout_id = Handles[slot].LayoutId;
	return true;
}

void DX12GameShaders_Free_Handle(unsigned handle)
{
	const bool vertex_shader = (handle & 1u) != 0;
	if (!DX12GameShaders_Resolve_Handle(handle, vertex_shader, nullptr, nullptr))
	{
		return;
	}
	const unsigned slot = handle >> 1;
	Handles[slot] = HandleSlot();
	FreeHandleSlots.push_back(slot);
}

void DX12GameShaders_Clear()
{
	Shaders.clear();
	Handles.clear();
	FreeHandleSlots.clear();
}
