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

// State keys, constants and HLSL generation for the Direct3D 8 fixed-function emulation.
//
// The formulas follow the Direct3D 8 documentation:
//	diffuse  = emissive + ambient * (global ambient + sum(light ambient * attenuation * spot))
//	           + material diffuse * sum(light diffuse * N.L * attenuation * spot)
//	specular = material specular * sum(light specular * (N.H)^power * attenuation * spot), for N.L > 0
//	attenuation = 1 / (a0 + a1 * d + a2 * d^2), 0 beyond the range, 1 for directional lights.
// Vertices without normals get the ambient and emissive terms only. The viewer of a non-local
// viewer is at -z infinity in camera space, so the direction to it is (0, 0, -1).

#include "WW3D2/Backend/DX12/dx12ffp.h"
#include "WW3D2/Backend/DX12/dx12shaders.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace
{

// D3DTEXTUREOP. Operations above LERP are invalid and pass the current color through, as in M1.
enum
{
	TOP_DISABLE = 1,
	TOP_SELECTARG1 = 2,
	TOP_SELECTARG2 = 3,
	TOP_MODULATE = 4,
	TOP_MODULATE2X = 5,
	TOP_MODULATE4X = 6,
	TOP_ADD = 7,
	TOP_ADDSIGNED = 8,
	TOP_ADDSIGNED2X = 9,
	TOP_SUBTRACT = 10,
	TOP_ADDSMOOTH = 11,
	TOP_BLENDDIFFUSEALPHA = 12,
	TOP_BLENDTEXTUREALPHA = 13,
	TOP_BLENDFACTORALPHA = 14,
	TOP_BLENDTEXTUREALPHAPM = 15,
	TOP_BLENDCURRENTALPHA = 16,
	TOP_PREMODULATE = 17,
	TOP_MODULATEALPHA_ADDCOLOR = 18,
	TOP_MODULATECOLOR_ADDALPHA = 19,
	TOP_MODULATEINVALPHA_ADDCOLOR = 20,
	TOP_MODULATEINVCOLOR_ADDALPHA = 21,
	TOP_BUMPENVMAP = 22,
	TOP_BUMPENVMAPLUMINANCE = 23,
	TOP_DOTPRODUCT3 = 24,
	TOP_MULTIPLYADD = 25,
	TOP_LERP = 26,
	TOP_INVALID = 27,
};

// D3DTA
enum
{
	TA_SELECTMASK = 0x0f,
	TA_DIFFUSE = 0,
	TA_CURRENT = 1,
	TA_TEXTURE = 2,
	TA_TFACTOR = 3,
	TA_SPECULAR = 4,
	TA_TEMP = 5,
	TA_COMPLEMENT = 0x10,
	TA_ALPHAREPLICATE = 0x20,
};

enum
{
	CMP_NEVER = 1,
	CMP_LESS = 2,
	CMP_EQUAL = 3,
	CMP_LESSEQUAL = 4,
	CMP_GREATER = 5,
	CMP_NOTEQUAL = 6,
	CMP_GREATEREQUAL = 7,
	CMP_ALWAYS = 8,

	LIGHT_POINT = 1,
	LIGHT_SPOT = 2,
	LIGHT_DIRECTIONAL = 3,

	MCS_MATERIAL = 0,
	MCS_COLOR1 = 1,
	MCS_COLOR2 = 2,

	FOG_NONE = 0,
	FOG_EXP = 1,
	FOG_EXP2 = 2,
	FOG_LINEAR = 3,

	TCI_SET_MASK = 0xffff,
	TCI_GEN_SHIFT = 16,
	TCI_PASSTHRU = 0,
	TCI_CAMERASPACENORMAL = 1,
	TCI_CAMERASPACEPOSITION = 2,
	TCI_CAMERASPACEREFLECTIONVECTOR = 3,

	TTFF_COUNT_MASK = 0xff,
	TTFF_PROJECTED = 0x100,

	TEXCOORD_UNUSED = 0xff,

	USES_ARG0 = 1,
	USES_ARG1 = 2,
	USES_ARG2 = 4,
};

// Components of each D3DFVF_TEXTUREFORMAT.
const unsigned TexCoordDimensions[4] = { 2, 3, 4, 1 };

unsigned Op_Args(unsigned op)
{
	switch (op)
	{
	case TOP_SELECTARG1:
	case TOP_PREMODULATE:
		return USES_ARG1;
	case TOP_SELECTARG2:
		return USES_ARG2;
	case TOP_BUMPENVMAP:
	case TOP_BUMPENVMAPLUMINANCE:
	case TOP_INVALID:
		return 0;
	case TOP_MULTIPLYADD:
	case TOP_LERP:
		return USES_ARG0 | USES_ARG1 | USES_ARG2;
	default:
		return USES_ARG1 | USES_ARG2;
	}
}

unsigned Normalize_Op(unsigned op)
{
	if (op <= TOP_DISABLE)
		return 0;
	if (op > TOP_LERP)
		return TOP_INVALID;
	return op;
}

// Unknown sources read the current color, as in M1.
unsigned Normalize_Arg(unsigned arg)
{
	unsigned source = arg & TA_SELECTMASK;
	if (source > TA_TEMP)
		source = TA_CURRENT;
	return source | (arg & (TA_COMPLEMENT | TA_ALPHAREPLICATE));
}

bool Reads_Texture(unsigned op, unsigned arg0, unsigned arg1, unsigned arg2)
{
	if (op == 0)
		return false;
	if (op == TOP_BLENDTEXTUREALPHA || op == TOP_BLENDTEXTUREALPHAPM)
		return true;
	unsigned args = Op_Args(op);
	return ((args & USES_ARG0) && (arg0 & TA_SELECTMASK) == TA_TEXTURE)
		|| ((args & USES_ARG1) && (arg1 & TA_SELECTMASK) == TA_TEXTURE)
		|| ((args & USES_ARG2) && (arg2 & TA_SELECTMASK) == TA_TEXTURE);
}

unsigned char Color_Source(unsigned source, bool color_vertex, bool has_diffuse, bool has_specular)
{
	if (color_vertex)
	{
		if (source == MCS_COLOR1 && has_diffuse)
			return MCS_COLOR1;
		if (source == MCS_COLOR2 && has_specular)
			return MCS_COLOR2;
	}
	return MCS_MATERIAL;
}

float DWORD_To_Float(unsigned value)
{
	float result;
	memcpy(&result, &value, sizeof(result));
	return result;
}

void Color_To_Float4(unsigned argb, float * out)
{
	out[0] = ((argb >> 16) & 0xff) / 255.0f;
	out[1] = ((argb >> 8) & 0xff) / 255.0f;
	out[2] = (argb & 0xff) / 255.0f;
	out[3] = ((argb >> 24) & 0xff) / 255.0f;
}

void Copy4(float * out, const float * in)
{
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
	out[3] = in[3];
}

void Set_Identity(float * m)
{
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b for row major 4x4 matrices.
void Multiply_Matrix(const float * a, const float * b, float * out)
{
	for (int row = 0; row < 4; ++row)
	{
		for (int col = 0; col < 4; ++col)
		{
			out[row * 4 + col] =
				a[row * 4 + 0] * b[0 * 4 + col] +
				a[row * 4 + 1] * b[1 * 4 + col] +
				a[row * 4 + 2] * b[2 * 4 + col] +
				a[row * 4 + 3] * b[3 * 4 + col];
		}
	}
}

// Inverse transpose of the upper 3x3 of m, which is the cofactor matrix divided by the determinant.
// A singular matrix keeps its own 3x3.
void Normal_Matrix(const float * m, float * out)
{
	const float a = m[0], b = m[1], c = m[2];
	const float d = m[4], e = m[5], f = m[6];
	const float g = m[8], h = m[9], i = m[10];

	float cofactor[9];
	cofactor[0] = e * i - f * h;
	cofactor[1] = f * g - d * i;
	cofactor[2] = d * h - e * g;
	cofactor[3] = c * h - b * i;
	cofactor[4] = a * i - c * g;
	cofactor[5] = b * g - a * h;
	cofactor[6] = b * f - c * e;
	cofactor[7] = c * d - a * f;
	cofactor[8] = a * e - b * d;

	const float det = a * cofactor[0] + b * cofactor[1] + c * cofactor[2];

	memset(out, 0, sizeof(float) * 16);
	for (int row = 0; row < 3; ++row)
	{
		for (int col = 0; col < 3; ++col)
		{
			if (fabsf(det) > 1.0e-30f)
				out[row * 4 + col] = cofactor[row * 3 + col] / det;
			else
				out[row * 4 + col] = m[row * 4 + col];
		}
	}
	out[15] = 1.0f;
}

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

// Vertex shader output and pixel shader input. Always complete so every vertex shader fits every
// pixel shader.
void Append_Interface(std::string & text, const char * name)
{
	Append(text, "\nstruct %s\n{\n", name);
	text += "\tfloat4 Position : SV_Position;\n";
	text += "\tfloat4 Diffuse : COLOR0;\n";
	text += "\tfloat4 Specular : COLOR1;\n";
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		Append(text, "\tfloat4 TexCoord%u : TEXCOORD%u;\n", stage, stage);
	}
	Append(text, "\tfloat Fog : TEXCOORD%u;\n", (unsigned)DX12_MAX_STAGES);
	text += "};\n";
}

const char * Material_Source(unsigned source, const char * material)
{
	if (source == MCS_COLOR1)
		return "vertex_diffuse";
	if (source == MCS_COLOR2)
		return "vertex_specular";
	return material;
}

// Texture coordinate set as the four components Direct3D 8 feeds the texture matrix: the first
// missing component is 1, the others 0.
void Append_TexCoord_Set(std::string & text, unsigned fvf, unsigned set)
{
	unsigned texcount = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	if (set >= texcount || set >= FVF_MAX_TEXCOORDS)
	{
		text += "float4(0.0, 0.0, 0.0, 0.0)";
		return;
	}
	switch (TexCoordDimensions[(fvf >> (16 + set * 2)) & 3])
	{
	case 1: Append(text, "float4(input.UV%u, 1.0, 0.0, 0.0)", set); break;
	case 2: Append(text, "float4(input.UV%u, 1.0, 0.0)", set); break;
	case 3: Append(text, "float4(input.UV%u, 1.0)", set); break;
	default: Append(text, "input.UV%u", set); break;
	}
}

std::string Arg_Expression(unsigned arg, unsigned stage)
{
	std::string value;
	switch (arg & TA_SELECTMASK)
	{
	case TA_DIFFUSE: value = "diffuse"; break;
	case TA_TEXTURE: Append(value, "tex%u", stage); break;
	case TA_TFACTOR: value = "TextureFactor"; break;
	case TA_SPECULAR: value = "specular"; break;
	case TA_TEMP: value = "temp"; break;
	default: value = "current"; break;
	}
	if (arg & TA_ALPHAREPLICATE)
		value += ".aaaa";
	if (arg & TA_COMPLEMENT)
		value = "(1.0 - " + value + ")";
	return value;
}

// The operation on a0, a1 and a2 as a float4 expression, with the same semantics as M1.
std::string Op_Expression(unsigned op, const char * a0, const char * a1, const char * a2, const char * tex, const char * next_tex)
{
	std::string e;
	switch (op)
	{
	case TOP_SELECTARG1: Append(e, "%s", a1); break;
	case TOP_SELECTARG2: Append(e, "%s", a2); break;
	case TOP_MODULATE: Append(e, "%s * %s", a1, a2); break;
	case TOP_MODULATE2X: Append(e, "%s * %s * 2.0", a1, a2); break;
	case TOP_MODULATE4X: Append(e, "%s * %s * 4.0", a1, a2); break;
	case TOP_ADD: Append(e, "%s + %s", a1, a2); break;
	case TOP_ADDSIGNED: Append(e, "%s + %s - 0.5", a1, a2); break;
	case TOP_ADDSIGNED2X: Append(e, "(%s + %s - 0.5) * 2.0", a1, a2); break;
	case TOP_SUBTRACT: Append(e, "%s - %s", a1, a2); break;
	case TOP_ADDSMOOTH: Append(e, "%s + %s - %s * %s", a1, a2, a1, a2); break;
	case TOP_BLENDDIFFUSEALPHA: Append(e, "lerp(%s, %s, diffuse.a)", a2, a1); break;
	case TOP_BLENDTEXTUREALPHA: Append(e, "lerp(%s, %s, %s.a)", a2, a1, tex); break;
	case TOP_BLENDFACTORALPHA: Append(e, "lerp(%s, %s, TextureFactor.a)", a2, a1); break;
	case TOP_BLENDTEXTUREALPHAPM: Append(e, "%s + %s * (1.0 - %s.a)", a1, a2, tex); break;
	case TOP_BLENDCURRENTALPHA: Append(e, "lerp(%s, %s, current.a)", a2, a1); break;
	case TOP_PREMODULATE: Append(e, "%s * %s", a1, next_tex); break;
	case TOP_MODULATEALPHA_ADDCOLOR: Append(e, "float4(%s.rgb + %s.a * %s.rgb, %s.a)", a1, a1, a2, a1); break;
	case TOP_MODULATECOLOR_ADDALPHA: Append(e, "float4(%s.rgb * %s.rgb + %s.a, %s.a)", a1, a2, a1, a1); break;
	case TOP_MODULATEINVALPHA_ADDCOLOR: Append(e, "float4(%s.rgb + (1.0 - %s.a) * %s.rgb, %s.a)", a1, a1, a2, a1); break;
	case TOP_MODULATEINVCOLOR_ADDALPHA: Append(e, "float4((1.0 - %s.rgb) * %s.rgb + %s.a, %s.a)", a1, a2, a1, a1); break;
	case TOP_DOTPRODUCT3: Append(e, "(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5)).xxxx", a1, a2); break;
	case TOP_MULTIPLYADD: Append(e, "%s + %s * %s", a0, a1, a2); break;
	case TOP_LERP: Append(e, "lerp(%s, %s, %s)", a2, a1, a0); break;
	default: e = "current"; break;		// bump mapping and invalid operations
	}
	return e;
}

void Append_Stage_Args(std::string & text, const char * prefix, unsigned op, unsigned arg0, unsigned arg1, unsigned arg2, unsigned stage)
{
	unsigned args = Op_Args(op);
	if (args & USES_ARG0)
		Append(text, "\t\tfloat4 %s0 = %s;\n", prefix, Arg_Expression(arg0, stage).c_str());
	if (args & USES_ARG1)
		Append(text, "\t\tfloat4 %s1 = %s;\n", prefix, Arg_Expression(arg1, stage).c_str());
	if (args & USES_ARG2)
		Append(text, "\t\tfloat4 %s2 = %s;\n", prefix, Arg_Expression(arg2, stage).c_str());
}

} // namespace

unsigned DX12FFP_Hash(const void * data, unsigned size)
{
	// FNV-1a
	const unsigned char * bytes = static_cast<const unsigned char *>(data);
	unsigned hash = 2166136261u;
	for (unsigned i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

void DX12FFP_Fill_Fallback_States(const DX12DrawDesc & desc, unsigned * rs, unsigned * tss)
{
	memset(rs, 0, sizeof(unsigned) * FFP_RS_COUNT);
	memset(tss, 0, sizeof(unsigned) * FFP_TSS_COUNT * DX12_MAX_STAGES);

	const float one = 1.0f;
	memcpy(&rs[FFP_RS_FOGEND], &one, sizeof(one));
	memcpy(&rs[FFP_RS_FOGDENSITY], &one, sizeof(one));
	rs[FFP_RS_ALPHATESTENABLE] = desc.AlphaTestEnable ? 1 : 0;
	rs[FFP_RS_ALPHAFUNC] = desc.AlphaFunc;
	rs[FFP_RS_ALPHAREF] = desc.AlphaRef;
	rs[FFP_RS_SPECULARENABLE] = desc.SpecularEnable ? 1 : 0;
	rs[FFP_RS_TEXTUREFACTOR] = desc.TextureFactor;
	rs[FFP_RS_COLORVERTEX] = 1;

	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		unsigned * t = tss + stage * FFP_TSS_COUNT;
		t[FFP_TSS_COLOROP] = desc.ColorOp[stage];
		t[FFP_TSS_COLORARG0] = desc.ColorArg0[stage];
		t[FFP_TSS_COLORARG1] = desc.ColorArg1[stage];
		t[FFP_TSS_COLORARG2] = desc.ColorArg2[stage];
		t[FFP_TSS_ALPHAOP] = desc.AlphaOp[stage];
		t[FFP_TSS_ALPHAARG0] = desc.AlphaArg0[stage];
		t[FFP_TSS_ALPHAARG1] = desc.AlphaArg1[stage];
		t[FFP_TSS_ALPHAARG2] = desc.AlphaArg2[stage];
		t[FFP_TSS_TEXCOORDINDEX] = desc.TexCoordIndex[stage];
	}
}

void DX12FFP_Build_Key(const DX12DrawDesc & desc, const unsigned * rs, const unsigned * tss, DX12FFPKey & key)
{
	memset(&key, 0, sizeof(key));
	DX12VSKey & vs = key.VS;
	DX12PSKey & ps = key.PS;

	const unsigned fvf = desc.FVF;
	const bool rhw = (fvf & FVF_POSITION_MASK) == FVF_XYZRHW;
	const bool has_normal = !rhw && (fvf & FVF_NORMAL) != 0;
	const bool has_diffuse = (fvf & FVF_DIFFUSE) != 0;
	const bool has_specular = (fvf & FVF_SPECULAR) != 0;
	unsigned texcount = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	if (texcount > FVF_MAX_TEXCOORDS)
		texcount = FVF_MAX_TEXCOORDS;

	vs.FVF = fvf;

	// Texture stage cascade. A disabled color stage ends it; DOTPRODUCT3 also writes the alpha.
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		const unsigned * t = tss + stage * FFP_TSS_COUNT;
		unsigned color_op = Normalize_Op(t[FFP_TSS_COLOROP]);
		if (color_op == 0)
			break;

		DX12PSStageKey & s = ps.Stages[stage];
		unsigned args = Op_Args(color_op);
		s.ColorOp = (unsigned char)color_op;
		s.ColorArg0 = (args & USES_ARG0) ? (unsigned char)Normalize_Arg(t[FFP_TSS_COLORARG0]) : 0;
		s.ColorArg1 = (args & USES_ARG1) ? (unsigned char)Normalize_Arg(t[FFP_TSS_COLORARG1]) : 0;
		s.ColorArg2 = (args & USES_ARG2) ? (unsigned char)Normalize_Arg(t[FFP_TSS_COLORARG2]) : 0;

		unsigned alpha_op = (color_op == TOP_DOTPRODUCT3) ? 0 : Normalize_Op(t[FFP_TSS_ALPHAOP]);
		if (alpha_op != 0)
		{
			args = Op_Args(alpha_op);
			s.AlphaOp = (unsigned char)alpha_op;
			s.AlphaArg0 = (args & USES_ARG0) ? (unsigned char)Normalize_Arg(t[FFP_TSS_ALPHAARG0]) : 0;
			s.AlphaArg1 = (args & USES_ARG1) ? (unsigned char)Normalize_Arg(t[FFP_TSS_ALPHAARG1]) : 0;
			s.AlphaArg2 = (args & USES_ARG2) ? (unsigned char)Normalize_Arg(t[FFP_TSS_ALPHAARG2]) : 0;
		}
	}

	// Textures and their coordinates, only for stages that read them. PREMODULATE reads the
	// texture of the next stage.
	bool uses_normal = false;
	bool uses_reflection = false;
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		vs.TexCoordSet[stage] = TEXCOORD_UNUSED;

		DX12PSStageKey & s = ps.Stages[stage];
		if (s.ColorOp == 0)
			continue;

		bool reads = Reads_Texture(s.ColorOp, s.ColorArg0, s.ColorArg1, s.ColorArg2)
			|| Reads_Texture(s.AlphaOp, s.AlphaArg0, s.AlphaArg1, s.AlphaArg2);
		if (stage > 0)
		{
			const DX12PSStageKey & previous = ps.Stages[stage - 1];
			reads = reads || previous.ColorOp == TOP_PREMODULATE || previous.AlphaOp == TOP_PREMODULATE;
		}
		if (!reads)
			continue;
		if (desc.Textures[stage] == nullptr)
		{
			s.Texture = FFP_TEXTURE_WHITE;
			continue;
		}
		s.Texture = FFP_TEXTURE_SAMPLED;

		const unsigned * t = tss + stage * FFP_TSS_COUNT;
		unsigned index = t[FFP_TSS_TEXCOORDINDEX];
		unsigned set = index & TCI_SET_MASK;
		unsigned gen = index >> TCI_GEN_SHIFT;
		if (gen > TCI_CAMERASPACEREFLECTIONVECTOR || rhw)
			gen = TCI_PASSTHRU;

		// Pretransformed vertices skip the texture matrix but still honor PROJECTED.
		unsigned flags = t[FFP_TSS_TEXTURETRANSFORMFLAGS];
		unsigned count = flags & TTFF_COUNT_MASK;
		if (count > 4)
			count = 0;

		// Texgen ignores the set; keep it out of the key so equivalent states share a shader.
		vs.TexCoordSet[stage] = gen == TCI_PASSTHRU ? (unsigned char)(set < FVF_MAX_TEXCOORDS ? set : FVF_MAX_TEXCOORDS) : 0;
		vs.TexGen[stage] = (unsigned char)gen;
		vs.TexTransform[stage] = rhw ? 0 : (unsigned char)count;

		if (flags & TTFF_PROJECTED)
		{
			unsigned divisor = count;
			if (divisor == 0 && gen == TCI_PASSTHRU && set < texcount)
				divisor = TexCoordDimensions[(fvf >> (16 + set * 2)) & 3];
			s.Projected = (unsigned char)divisor;
		}

		if (gen == TCI_CAMERASPACENORMAL || gen == TCI_CAMERASPACEREFLECTIONVECTOR)
			uses_normal = true;
		if (gen == TCI_CAMERASPACEREFLECTIONVECTOR)
			uses_reflection = true;
	}

	// Lighting. Pretransformed vertices are never lit.
	unsigned light_count = 0;
	if (!rhw && rs[FFP_RS_LIGHTING] != 0)
	{
		vs.Lighting = 1;
		for (unsigned i = 0; i < DX12_MAX_LIGHTS; ++i)
		{
			const DX12LightDesc & light = desc.Lights[i];
			if (light.Enabled && light.Type >= LIGHT_POINT && light.Type <= LIGHT_DIRECTIONAL)
			{
				vs.LightType[light_count++] = (unsigned char)light.Type;
			}
		}

		bool color_vertex = rs[FFP_RS_COLORVERTEX] != 0;
		vs.Specular = (rs[FFP_RS_SPECULARENABLE] != 0 && light_count > 0 && has_normal) ? 1 : 0;
		vs.DiffuseSource = Color_Source(rs[FFP_RS_DIFFUSEMATERIALSOURCE], color_vertex, has_diffuse, has_specular);
		vs.AmbientSource = Color_Source(rs[FFP_RS_AMBIENTMATERIALSOURCE], color_vertex, has_diffuse, has_specular);
		vs.EmissiveSource = Color_Source(rs[FFP_RS_EMISSIVEMATERIALSOURCE], color_vertex, has_diffuse, has_specular);
		if (vs.Specular)
			vs.SpecularSource = Color_Source(rs[FFP_RS_SPECULARMATERIALSOURCE], color_vertex, has_diffuse, has_specular);
		if (light_count > 0)
			uses_normal = true;
	}
	if (has_normal && uses_normal && rs[FFP_RS_NORMALIZENORMALS] != 0)
		vs.NormalizeNormals = 1;
	if ((vs.Specular || uses_reflection) && rs[FFP_RS_LOCALVIEWER] != 0)
		vs.LocalViewer = 1;

	// Fog. Table fog wins over vertex fog. Without a fog mode, and always for pretransformed
	// vertices, the fog factor is the specular alpha.
	if (rs[FFP_RS_FOGENABLE] != 0)
	{
		unsigned table = rs[FFP_RS_FOGTABLEMODE];
		unsigned vertex = rs[FFP_RS_FOGVERTEXMODE];
		if (table >= FOG_EXP && table <= FOG_LINEAR)
		{
			vs.Fog = FFP_VS_FOG_DEPTH;
			ps.Fog = 2;
			ps.FogTableMode = (unsigned char)table;
		}
		else if (!rhw && vertex >= FOG_EXP && vertex <= FOG_LINEAR)
		{
			vs.Fog = (unsigned char)vertex;
			vs.RangeFog = rs[FFP_RS_RANGEFOGENABLE] != 0 ? 1 : 0;
			ps.Fog = 1;
		}
		else if (has_specular && !vs.Lighting)
		{
			vs.Fog = FFP_VS_FOG_SPECULAR;
			ps.Fog = 1;
		}
	}

	// Only when the vertex shader can output a nonzero specular color.
	ps.Specular = (rs[FFP_RS_SPECULARENABLE] != 0 && (vs.Specular || (!vs.Lighting && has_specular))) ? 1 : 0;

	unsigned alpha_func = rs[FFP_RS_ALPHAFUNC];
	if (rs[FFP_RS_ALPHATESTENABLE] != 0 && alpha_func >= CMP_NEVER && alpha_func < CMP_ALWAYS)
		ps.AlphaFunc = (unsigned char)alpha_func;
}

void DX12FFP_Fill_Constants(const DX12DrawDesc & desc, const unsigned * rs, const DX12FFPKey & key,
	const DX12FFPViewport & viewport, DX12VSConstants & vs, DX12PSConstants & ps)
{
	memset(&vs, 0, sizeof(vs));
	memset(&ps, 0, sizeof(ps));

	float identity[16];
	Set_Identity(identity);
	const float * world = desc.World ? desc.World : identity;
	const float * view = desc.View ? desc.View : identity;
	const float * projection = desc.Projection ? desc.Projection : identity;

	Multiply_Matrix(world, view, vs.WorldView);
	Multiply_Matrix(vs.WorldView, projection, vs.WorldViewProjection);
	Normal_Matrix(vs.WorldView, vs.NormalMatrix);
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		memcpy(vs.TextureMatrix[stage], desc.TextureMatrices[stage] ? desc.TextureMatrices[stage] : identity, sizeof(float) * 16);
	}

	float vp_w = viewport.Width >= 1.0f ? viewport.Width : 1.0f;
	float vp_h = viewport.Height >= 1.0f ? viewport.Height : 1.0f;

	// Direct3D 8 puts pixel centers on integer coordinates, Direct3D 12 half a pixel further.
	// Pretransformed vertices: render target pixels plus half a pixel, through the viewport to NDC.
	vs.ScreenToClip[0] = 2.0f / vp_w;
	vs.ScreenToClip[1] = -2.0f / vp_h;
	vs.ScreenToClip[2] = (0.5f - viewport.X) * 2.0f / vp_w - 1.0f;
	vs.ScreenToClip[3] = 1.0f - (0.5f - viewport.Y) * 2.0f / vp_h;

	// Pretransformed depth is final; undo the viewport depth mapping.
	float depth_range = viewport.MaxZ - viewport.MinZ;
	if (depth_range > 1.0e-6f || depth_range < -1.0e-6f)
	{
		vs.DepthToClip[0] = 1.0f / depth_range;
		vs.DepthToClip[1] = -viewport.MinZ / depth_range;
	}
	else
	{
		vs.DepthToClip[0] = 1.0f;
		vs.DepthToClip[1] = 0.0f;
	}

	vs.PixelOffset[0] = 1.0f / vp_w;
	vs.PixelOffset[1] = -1.0f / vp_h;

	if (key.VS.Lighting)
	{
		Copy4(vs.MaterialDiffuse, desc.Material.Diffuse);
		Copy4(vs.MaterialAmbient, desc.Material.Ambient);
		Copy4(vs.MaterialSpecular, desc.Material.Specular);
		Copy4(vs.MaterialEmissive, desc.Material.Emissive);
		vs.MaterialPower[0] = desc.Material.Power;
		Color_To_Float4(rs[FFP_RS_AMBIENT], vs.GlobalAmbient);

		// Lights in the order of the key, moved to camera space.
		unsigned count = 0;
		for (unsigned i = 0; i < DX12_MAX_LIGHTS; ++i)
		{
			const DX12LightDesc & light = desc.Lights[i];
			if (!light.Enabled || light.Type < LIGHT_POINT || light.Type > LIGHT_DIRECTIONAL)
				continue;

			DX12FFPLightConstants & out = vs.Lights[count++];
			Copy4(out.Diffuse, light.Diffuse);
			Copy4(out.Specular, light.Specular);
			Copy4(out.Ambient, light.Ambient);

			const float * p = light.Position;
			for (int c = 0; c < 3; ++c)
			{
				out.Position[c] = p[0] * view[c] + p[1] * view[4 + c] + p[2] * view[8 + c] + view[12 + c];
			}
			out.Position[3] = light.Range;

			const float * d = light.Direction;
			float dir[3];
			for (int c = 0; c < 3; ++c)
			{
				dir[c] = d[0] * view[c] + d[1] * view[4 + c] + d[2] * view[8 + c];
			}
			float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (length > 1.0e-20f)
			{
				out.Direction[0] = dir[0] / length;
				out.Direction[1] = dir[1] / length;
				out.Direction[2] = dir[2] / length;
			}
			else
			{
				out.Direction[2] = 1.0f;
			}
			out.Direction[3] = light.Falloff;

			out.Attenuation[0] = light.Attenuation0;
			out.Attenuation[1] = light.Attenuation1;
			out.Attenuation[2] = light.Attenuation2;

			float cos_theta = cosf(light.Theta * 0.5f);
			float cos_phi = cosf(light.Phi * 0.5f);
			out.Spot[0] = cos_theta;
			out.Spot[1] = cos_phi;
			out.Spot[2] = (cos_theta - cos_phi > 1.0e-6f) ? 1.0f / (cos_theta - cos_phi) : 1.0e6f;
		}
	}

	// Fog distances. Table fog uses the eye depth for perspective projections (w fog) and the
	// depth buffer value otherwise.
	float fog_start = DWORD_To_Float(rs[FFP_RS_FOGSTART]);
	float fog_end = DWORD_To_Float(rs[FFP_RS_FOGEND]);
	float fog_range = fog_end - fog_start;
	vs.Fog[0] = fog_end;
	vs.Fog[1] = (fog_range > 1.0e-6f || fog_range < -1.0e-6f) ? 1.0f / fog_range : 1.0e6f;
	vs.Fog[2] = DWORD_To_Float(rs[FFP_RS_FOGDENSITY]);
	vs.Fog[3] = (projection[11] != 0.0f) ? 1.0f : 0.0f;

	Color_To_Float4(rs[FFP_RS_TEXTUREFACTOR], ps.TextureFactor);
	Color_To_Float4(rs[FFP_RS_FOGCOLOR], ps.FogColor);
	ps.Fog[0] = vs.Fog[0];
	ps.Fog[1] = vs.Fog[1];
	ps.Fog[2] = vs.Fog[2];
	ps.AlphaRef[0] = (float)(rs[FFP_RS_ALPHAREF] & 0xff);
}

void DX12FFP_Generate_Vertex_Shader(const DX12VSKey & key, std::string & hlsl)
{
	const unsigned fvf = key.FVF;
	const bool rhw = (fvf & FVF_POSITION_MASK) == FVF_XYZRHW;
	const bool has_normal = !rhw && (fvf & FVF_NORMAL) != 0;
	const bool has_diffuse = (fvf & FVF_DIFFUSE) != 0;
	const bool has_specular = (fvf & FVF_SPECULAR) != 0;
	unsigned texcount = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	if (texcount > FVF_MAX_TEXCOORDS)
		texcount = FVF_MAX_TEXCOORDS;

	unsigned light_count = 0;
	while (key.Lighting && light_count < DX12_MAX_LIGHTS && key.LightType[light_count] != 0)
		++light_count;

	bool uses_normal = key.Lighting && light_count > 0;
	bool uses_eye = key.Lighting && light_count > 0;
	bool uses_reflection = false;
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		if (key.TexCoordSet[stage] == TEXCOORD_UNUSED)
			continue;
		if (key.TexGen[stage] == TCI_CAMERASPACENORMAL)
			uses_normal = true;
		if (key.TexGen[stage] == TCI_CAMERASPACEPOSITION)
			uses_eye = true;
		if (key.TexGen[stage] == TCI_CAMERASPACEREFLECTIONVECTOR)
			uses_normal = uses_eye = uses_reflection = true;
	}
	if (key.Fog >= FFP_VS_FOG_EXP && key.Fog <= FFP_VS_FOG_LINEAR)
		uses_eye = true;
	if (key.Fog == FFP_VS_FOG_DEPTH)
		uses_eye = true;
	if (rhw)
		uses_eye = uses_normal = false;

	hlsl.clear();
	Append(hlsl, "#define MAX_STAGES %u\n#define MAX_LIGHTS %u\n", (unsigned)DX12_MAX_STAGES, (unsigned)DX12_MAX_LIGHTS);
	hlsl += DX12VertexShaderPrologue;
	Append_Interface(hlsl, "VSOutput");

	hlsl += "\nstruct VSInput\n{\n";
	hlsl += rhw ? "\tfloat4 Position : POSITION;\n" : "\tfloat3 Position : POSITION;\n";
	if (has_normal)
		hlsl += "\tfloat3 Normal : NORMAL;\n";
	if (has_diffuse)
		hlsl += "\tfloat4 Diffuse : COLOR0;\n";
	if (has_specular)
		hlsl += "\tfloat4 Specular : COLOR1;\n";
	for (unsigned set = 0; set < texcount; ++set)
	{
		static const char * const types[5] = { "", "float", "float2", "float3", "float4" };
		Append(hlsl, "\t%s UV%u : TEXCOORD%u;\n", types[TexCoordDimensions[(fvf >> (16 + set * 2)) & 3]], set, set);
	}
	hlsl += "};\n\nVSOutput VSMain(VSInput input)\n{\n\tVSOutput output;\n\n";

	if (rhw)
	{
		// Pretransformed vertices are in render target pixels. Keep w = 1/rhw so that colors and
		// texture coordinates are interpolated with perspective correction, as Direct3D 8 did.
		hlsl += "\tfloat w = (input.Position.w > 0.0) ? 1.0 / input.Position.w : 1.0;\n";
		hlsl += "\tfloat3 ndc;\n";
		hlsl += "\tndc.xy = input.Position.xy * ScreenToClip.xy + ScreenToClip.zw;\n";
		hlsl += "\tndc.z = input.Position.z * DepthToClip.x + DepthToClip.y;\n";
		hlsl += "\toutput.Position = float4(ndc * w, w);\n";
	}
	else
	{
		hlsl += "\toutput.Position = mul(float4(input.Position, 1.0), WorldViewProjection);\n";
		hlsl += "\toutput.Position.xy += PixelOffset.xy * output.Position.w;\n";
	}
	if (uses_eye)
		hlsl += "\tfloat3 eye = mul(float4(input.Position, 1.0), WorldView).xyz;\n";
	if (uses_normal)
	{
		if (has_normal)
		{
			hlsl += "\tfloat3 normal = mul(input.Normal, (float3x3)NormalMatrix);\n";
			if (key.NormalizeNormals)
				hlsl += "\tnormal *= rsqrt(max(dot(normal, normal), 1.0e-30));\n";
		}
		else
		{
			hlsl += "\tfloat3 normal = float3(0.0, 0.0, 0.0);\n";
		}
	}

	// Direct3D 8 colors are BGRA in memory and are read as R8G8B8A8.
	hlsl += has_diffuse ? "\tfloat4 vertex_diffuse = input.Diffuse.zyxw;\n" : "\tfloat4 vertex_diffuse = float4(1.0, 1.0, 1.0, 1.0);\n";
	hlsl += has_specular ? "\tfloat4 vertex_specular = input.Specular.zyxw;\n" : "\tfloat4 vertex_specular = float4(0.0, 0.0, 0.0, 0.0);\n";

	if (!key.Lighting)
	{
		hlsl += "\toutput.Diffuse = vertex_diffuse;\n";
		hlsl += "\toutput.Specular = vertex_specular;\n";
	}
	else
	{
		Append(hlsl, "\n\tfloat4 material_diffuse = %s;\n", Material_Source(key.DiffuseSource, "MaterialDiffuse"));
		Append(hlsl, "\tfloat4 material_ambient = %s;\n", Material_Source(key.AmbientSource, "MaterialAmbient"));
		Append(hlsl, "\tfloat4 material_emissive = %s;\n", Material_Source(key.EmissiveSource, "MaterialEmissive"));
		hlsl += "\tfloat3 ambient = GlobalAmbient.rgb;\n";
		hlsl += "\tfloat3 diffuse = float3(0.0, 0.0, 0.0);\n";
		if (key.Specular)
		{
			Append(hlsl, "\tfloat4 material_specular = %s;\n", Material_Source(key.SpecularSource, "MaterialSpecular"));
			hlsl += "\tfloat3 specular = float3(0.0, 0.0, 0.0);\n";
			if (key.LocalViewer)
				hlsl += "\tfloat3 to_viewer = -eye * rsqrt(max(dot(eye, eye), 1.0e-30));\n";
			else
				hlsl += "\tfloat3 to_viewer = float3(0.0, 0.0, -1.0);\n";
		}

		for (unsigned i = 0; i < light_count; ++i)
		{
			unsigned type = key.LightType[i];
			Append(hlsl, "\n\t// Light %u: %s\n\t{\n", i,
				type == LIGHT_POINT ? "point" : (type == LIGHT_SPOT ? "spot" : "directional"));
			if (type == LIGHT_DIRECTIONAL)
			{
				Append(hlsl, "\t\tfloat3 light_dir = -Lights[%u].Direction.xyz;\n", i);
				hlsl += "\t\tfloat attenuation = 1.0;\n";
			}
			else
			{
				Append(hlsl, "\t\tfloat3 to_light = Lights[%u].Position.xyz - eye;\n", i);
				hlsl += "\t\tfloat light_distance = length(to_light);\n";
				hlsl += "\t\tfloat3 light_dir = to_light / max(light_distance, 1.0e-20);\n";
				Append(hlsl, "\t\tfloat attenuation = (light_distance <= Lights[%u].Position.w)"
					" ? 1.0 / max(dot(Lights[%u].Attenuation.xyz, float3(1.0, light_distance, light_distance * light_distance)), 1.0e-20) : 0.0;\n", i, i);
				if (type == LIGHT_SPOT)
				{
					Append(hlsl, "\t\tfloat rho = dot(Lights[%u].Direction.xyz, -light_dir);\n", i);
					Append(hlsl, "\t\tfloat spot = pow(saturate((rho - Lights[%u].Spot.y) * Lights[%u].Spot.z), Lights[%u].Direction.w);\n", i, i, i);
					Append(hlsl, "\t\tattenuation *= (rho > Lights[%u].Spot.x) ? 1.0 : ((rho > Lights[%u].Spot.y) ? spot : 0.0);\n", i, i);
				}
			}
			Append(hlsl, "\t\tambient += Lights[%u].Ambient.rgb * attenuation;\n", i);
			if (has_normal)
			{
				hlsl += "\t\tfloat n_dot_l = dot(normal, light_dir);\n";
				hlsl += "\t\tif (n_dot_l > 0.0)\n\t\t{\n";
				Append(hlsl, "\t\t\tdiffuse += Lights[%u].Diffuse.rgb * (n_dot_l * attenuation);\n", i);
				if (key.Specular)
				{
					hlsl += "\t\t\tfloat3 half_vector = light_dir + to_viewer;\n";
					hlsl += "\t\t\thalf_vector *= rsqrt(max(dot(half_vector, half_vector), 1.0e-30));\n";
					hlsl += "\t\t\tfloat n_dot_h = dot(normal, half_vector);\n";
					hlsl += "\t\t\tif (n_dot_h > 0.0)\n";
					Append(hlsl, "\t\t\t\tspecular += Lights[%u].Specular.rgb * (pow(abs(n_dot_h), MaterialPower.x) * attenuation);\n", i);
				}
				hlsl += "\t\t}\n";
			}
			hlsl += "\t}\n";
		}

		hlsl += "\n\toutput.Diffuse = float4(saturate(material_emissive.rgb + material_ambient.rgb * ambient + material_diffuse.rgb * diffuse), saturate(material_diffuse.a));\n";
		if (key.Specular)
			hlsl += "\toutput.Specular = float4(saturate(material_specular.rgb * specular), saturate(material_specular.a));\n";
		else
			hlsl += "\toutput.Specular = float4(0.0, 0.0, 0.0, 0.0);\n";
	}

	// Texture coordinates: a vertex set or a generated camera space vector, through the texture matrix.
	hlsl += "\n";
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		if (key.TexCoordSet[stage] == TEXCOORD_UNUSED)
		{
			Append(hlsl, "\toutput.TexCoord%u = float4(0.0, 0.0, 0.0, 0.0);\n", stage);
			continue;
		}

		Append(hlsl, "\tfloat4 tex_coord%u = ", stage);
		switch (key.TexGen[stage])
		{
		case TCI_CAMERASPACENORMAL:
			hlsl += "float4(normal, 1.0)";
			break;
		case TCI_CAMERASPACEPOSITION:
			hlsl += "float4(eye, 1.0)";
			break;
		case TCI_CAMERASPACEREFLECTIONVECTOR:
			if (key.LocalViewer)
				hlsl += "float4(reflect(eye * rsqrt(max(dot(eye, eye), 1.0e-30)), normal), 1.0)";
			else
				hlsl += "float4(reflect(float3(0.0, 0.0, 1.0), normal), 1.0)";
			break;
		default:
			Append_TexCoord_Set(hlsl, fvf, key.TexCoordSet[stage]);
			break;
		}
		hlsl += ";\n";
		if (key.TexTransform[stage] != 0)
			Append(hlsl, "\ttex_coord%u = mul(tex_coord%u, TextureMatrix[%u]);\n", stage, stage, stage);
		Append(hlsl, "\toutput.TexCoord%u = tex_coord%u;\n", stage, stage);
	}

	// Fog factor (1 = no fog), or the depth for table fog.
	switch (key.Fog)
	{
	case FFP_VS_FOG_EXP:
	case FFP_VS_FOG_EXP2:
	case FFP_VS_FOG_LINEAR:
		hlsl += key.RangeFog ? "\tfloat fog_distance = length(eye);\n" : "\tfloat fog_distance = abs(eye.z);\n";
		if (key.Fog == FFP_VS_FOG_LINEAR)
			hlsl += "\toutput.Fog = saturate((Fog.x - fog_distance) * Fog.y);\n";
		else if (key.Fog == FFP_VS_FOG_EXP)
			hlsl += "\toutput.Fog = saturate(exp(-Fog.z * fog_distance));\n";
		else
			hlsl += "\tfloat fog_density = Fog.z * fog_distance;\n\toutput.Fog = saturate(exp(-fog_density * fog_density));\n";
		break;
	case FFP_VS_FOG_SPECULAR:
		hlsl += "\toutput.Fog = output.Specular.a;\n";
		break;
	case FFP_VS_FOG_DEPTH:
		if (rhw)
			hlsl += "\toutput.Fog = (Fog.w != 0.0) ? w : input.Position.z;\n";
		else
			hlsl += "\toutput.Fog = (Fog.w != 0.0) ? output.Position.w : output.Position.z / output.Position.w;\n";
		break;
	default:
		hlsl += "\toutput.Fog = 1.0;\n";
		break;
	}

	hlsl += "\treturn output;\n}\n";
}

void DX12FFP_Generate_Pixel_Shader(const DX12PSKey & key, std::string & hlsl)
{
	hlsl.clear();
	Append(hlsl, "#define MAX_STAGES %u\n", (unsigned)DX12_MAX_STAGES);
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		Append(hlsl, "Texture2D Texture%u : register(t%u);\n", stage, stage);
		Append(hlsl, "SamplerState Sampler%u : register(s%u);\n", stage, stage);
	}
	hlsl += DX12PixelShaderPrologue;
	Append_Interface(hlsl, "PSInput");

	hlsl += "\nfloat4 PSMain(PSInput input) : SV_Target\n{\n";
	hlsl += "\tfloat4 diffuse = input.Diffuse;\n";
	hlsl += "\tfloat4 specular = input.Specular;\n";
	hlsl += "\tfloat4 temp = float4(0.0, 0.0, 0.0, 0.0);\n";
	hlsl += "\tfloat4 current = diffuse;\n";

	static const char components[5] = { 'x', 'x', 'y', 'z', 'w' };
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		const DX12PSStageKey & s = key.Stages[stage];
		if (s.Texture == FFP_TEXTURE_SAMPLED)
		{
			if (s.Projected >= 1 && s.Projected <= 4)
				Append(hlsl, "\tfloat2 uv%u = input.TexCoord%u.xy / input.TexCoord%u.%c;\n", stage, stage, stage, components[s.Projected]);
			else
				Append(hlsl, "\tfloat2 uv%u = input.TexCoord%u.xy;\n", stage, stage);
			Append(hlsl, "\tfloat4 tex%u = Texture%u.Sample(Sampler%u, uv%u);\n", stage, stage, stage, stage);
		}
		else if (s.Texture == FFP_TEXTURE_WHITE)
		{
			Append(hlsl, "\tfloat4 tex%u = float4(1.0, 1.0, 1.0, 1.0);\n", stage);
		}
	}

	// Texture stage cascade. Every stage writes CURRENT.
	for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		const DX12PSStageKey & s = key.Stages[stage];
		if (s.ColorOp == 0)
			break;

		char tex[16];
		char next_tex[32];
		snprintf(tex, sizeof(tex), "tex%u", stage);
		if (stage + 1 < DX12_MAX_STAGES && key.Stages[stage + 1].Texture != FFP_TEXTURE_UNUSED)
			snprintf(next_tex, sizeof(next_tex), "tex%u", stage + 1);
		else
			snprintf(next_tex, sizeof(next_tex), "float4(1.0, 1.0, 1.0, 1.0)");

		Append(hlsl, "\n\t// Stage %u\n\t{\n", stage);
		Append_Stage_Args(hlsl, "color_arg", s.ColorOp, s.ColorArg0, s.ColorArg1, s.ColorArg2, stage);
		Append(hlsl, "\t\tfloat4 color = %s;\n", Op_Expression(s.ColorOp, "color_arg0", "color_arg1", "color_arg2", tex, next_tex).c_str());
		if (s.ColorOp == TOP_DOTPRODUCT3)
		{
			hlsl += "\t\tfloat alpha = color.a;\n";
		}
		else if (s.AlphaOp != 0)
		{
			Append_Stage_Args(hlsl, "alpha_arg", s.AlphaOp, s.AlphaArg0, s.AlphaArg1, s.AlphaArg2, stage);
			Append(hlsl, "\t\tfloat alpha = (%s).a;\n", Op_Expression(s.AlphaOp, "alpha_arg0", "alpha_arg1", "alpha_arg2", tex, next_tex).c_str());
		}
		else
		{
			hlsl += "\t\tfloat alpha = current.a;\n";
		}
		hlsl += "\t\tcurrent = saturate(float4(color.rgb, alpha));\n\t}\n";
	}

	if (key.Specular)
		hlsl += "\n\tcurrent.rgb = saturate(current.rgb + specular.rgb);\n";

	if (key.AlphaFunc != 0)
	{
		static const char * const compare[8] = { "", "", "<", "==", "<=", ">", "!=", ">=" };
		if (key.AlphaFunc == CMP_NEVER)
		{
			hlsl += "\n\tclip(-1.0);\n";
		}
		else if (key.AlphaFunc < CMP_ALWAYS)
		{
			hlsl += "\n\tfloat alpha_test = round(current.a * 255.0);\n";
			Append(hlsl, "\tif (!(alpha_test %s AlphaRef.x))\n\t\tdiscard;\n", compare[key.AlphaFunc]);
		}
	}

	// Fog is applied last, as in Direct3D 8.
	if (key.Fog == 1)
	{
		hlsl += "\n\tfloat fog = input.Fog;\n";
	}
	else if (key.Fog == 2)
	{
		hlsl += "\n\tfloat fog_depth = input.Fog;\n";
		if (key.FogTableMode == FOG_LINEAR)
			hlsl += "\tfloat fog = saturate((Fog.x - fog_depth) * Fog.y);\n";
		else if (key.FogTableMode == FOG_EXP)
			hlsl += "\tfloat fog = saturate(exp(-Fog.z * fog_depth));\n";
		else
			hlsl += "\tfloat fog_density = Fog.z * fog_depth;\n\tfloat fog = saturate(exp(-fog_density * fog_density));\n";
	}
	if (key.Fog != 0)
		hlsl += "\tcurrent.rgb = lerp(FogColor.rgb, current.rgb, fog);\n";

	hlsl += "\treturn current;\n}\n";
}
