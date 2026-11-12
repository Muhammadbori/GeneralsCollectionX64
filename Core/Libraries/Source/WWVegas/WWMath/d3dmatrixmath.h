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

// TheSuperHackers @refactor Replacements for the D3DX 8 matrix functions that the renderer used, on the
// Direct3D matrix type (D3DMATRIX: row vectors, m[row][column], v' = v * M).
//
// Each function does the same float operations in the same order as the d3dx8.lib version the game ran
// on an SSE2 processor without 3DNow (d3dx8 picks its sse2_ functions there, and the c_ ones where no
// SSE2 version exists), as read from the library code:
//  - Multiply, Vec3Transform, Inverse: the sse2_ versions (Inverse with the same reciprocal estimate and
//    Newton step, so it uses the SSE instructions directly);
//  - Rotation_Z: the c_ version (x87 fsincos, results rounded to float once);
//  - Transpose, Identity, Scaling, Translation: exact element copies.
// They are exact replacements for that configuration, which covers the machines the game runs on today.

#pragma once

#include <d3d8types.h>
#include <xmmintrin.h>
#include <math.h>

namespace D3DMatrixMath
{

// out = a * b. out may be a or b.
inline D3DMATRIX* Multiply(D3DMATRIX* out, const D3DMATRIX* a, const D3DMATRIX* b)
{
	D3DMATRIX r;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			r.m[i][j] = ((a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j]) + a->m[i][2] * b->m[2][j]) + a->m[i][3] * b->m[3][j];
		}
	}
	*out = r;
	return out;
}

// out = (v.x, v.y, v.z, 1) * m.
inline void Vec3Transform(float out[4], const float v[3], const D3DMATRIX* m)
{
	const float x = v[0], y = v[1], z = v[2];
	float r[4];
	for (int j = 0; j < 4; ++j) {
		r[j] = (x * m->m[0][j] + y * m->m[1][j]) + (z * m->m[2][j] + m->m[3][j]);
	}
	out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = r[3];
}

// out may be m.
inline D3DMATRIX* Transpose(D3DMATRIX* out, const D3DMATRIX* m)
{
	D3DMATRIX r;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			r.m[i][j] = m->m[j][i];
		}
	}
	*out = r;
	return out;
}

inline D3DMATRIX* Identity(D3DMATRIX* out)
{
	out->m[0][1] = out->m[0][2] = out->m[0][3] =
	out->m[1][0] = out->m[1][2] = out->m[1][3] =
	out->m[2][0] = out->m[2][1] = out->m[2][3] =
	out->m[3][0] = out->m[3][1] = out->m[3][2] = 0.0f;
	out->m[0][0] = out->m[1][1] = out->m[2][2] = out->m[3][3] = 1.0f;
	return out;
}

inline D3DMATRIX* Scaling(D3DMATRIX* out, float sx, float sy, float sz)
{
	Identity(out);
	out->m[0][0] = sx;
	out->m[1][1] = sy;
	out->m[2][2] = sz;
	return out;
}

inline D3DMATRIX* Translation(D3DMATRIX* out, float x, float y, float z)
{
	Identity(out);
	out->m[3][0] = x;
	out->m[3][1] = y;
	out->m[3][2] = z;
	return out;
}

// Rotation about the z axis by angle radians (row vector convention).
inline D3DMATRIX* Rotation_Z(D3DMATRIX* out, float angle)
{
	float s, c;
#if defined(_MSC_VER) && defined(_M_IX86)
	__asm {
		fld angle
		fsincos
		fstp c
		fstp s
	}
#else
	c = (float)cos((double)angle);
	s = (float)sin((double)angle);
#endif
	Identity(out);
	out->m[0][0] = c;
	out->m[0][1] = s;
	out->m[1][0] = -s;
	out->m[1][1] = c;
	return out;
}

// Inverse by cofactors with SSE (the sse2_D3DXMatrixInverse instruction sequence). Writes *determinant when
// determinant is not null. Returns null, and leaves out unchanged, when the determinant is zero or NaN.
inline D3DMATRIX* Inverse(D3DMATRIX* out, float* determinant, const D3DMATRIX* m)
{
	__m128 xmm0 = _mm_setr_ps(m->m[0][2], m->m[0][3], m->m[1][2], m->m[1][3]);
	__m128 xmm4 = _mm_setr_ps(m->m[2][2], m->m[2][3], m->m[3][2], m->m[3][3]);
	__m128 xmm3 = _mm_setr_ps(m->m[2][0], m->m[2][1], m->m[3][0], m->m[3][1]);
	__m128 xmm1 = _mm_setr_ps(m->m[0][0], m->m[0][1], m->m[1][0], m->m[1][1]);
	__m128 xmm2, xmm5, xmm6, xmm7;
	__m128 s10, s20, s30, s40, s50, s60, s70, s80, s90, sA0, sB0, sC0, sD0, sE0;

	xmm5 = xmm0;
	xmm0 = _mm_shuffle_ps(xmm0, xmm4, 0x88);
	xmm4 = _mm_shuffle_ps(xmm4, xmm5, 0xDD);
	xmm5 = xmm4;
	xmm5 = _mm_mul_ps(xmm5, xmm0);
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0xB1);
	xmm6 = xmm5;
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0x4E);
	xmm7 = xmm3;
	xmm3 = _mm_shuffle_ps(xmm3, xmm1, 0xDD);
	xmm1 = _mm_shuffle_ps(xmm1, xmm7, 0x88);
	xmm7 = xmm3;
	xmm3 = _mm_mul_ps(xmm3, xmm5);
	xmm2 = _mm_setzero_ps();
	xmm5 = _mm_mul_ps(xmm5, xmm1);
	s30 = xmm5;
	xmm5 = xmm7;
	xmm7 = _mm_mul_ps(xmm7, xmm6);
	xmm6 = _mm_mul_ps(xmm6, xmm1);
	xmm3 = _mm_sub_ps(xmm3, xmm7);
	xmm7 = xmm5;
	xmm5 = _mm_mul_ps(xmm5, xmm0);
	xmm0 = _mm_shuffle_ps(xmm0, xmm0, 0x4E);
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0xB1);
	s40 = xmm6;
	xmm6 = xmm4;
	xmm6 = _mm_mul_ps(xmm6, xmm5);
	xmm6 = _mm_add_ps(xmm6, xmm3);
	xmm3 = xmm5;
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0x4E);
	xmm3 = _mm_mul_ps(xmm3, xmm1);
	s50 = xmm3;
	xmm3 = xmm4;
	xmm3 = _mm_mul_ps(xmm3, xmm5);
	xmm5 = _mm_mul_ps(xmm5, xmm1);
	xmm6 = _mm_sub_ps(xmm6, xmm3);
	xmm3 = xmm7;
	xmm7 = _mm_shuffle_ps(xmm7, xmm7, 0x4E);
	xmm7 = _mm_mul_ps(xmm7, xmm4);
	xmm7 = _mm_shuffle_ps(xmm7, xmm7, 0xB1);
	s60 = xmm5;
	xmm5 = xmm0;
	xmm0 = _mm_mul_ps(xmm0, xmm7);
	xmm0 = _mm_add_ps(xmm0, xmm6);
	xmm6 = xmm7;
	xmm7 = _mm_shuffle_ps(xmm7, xmm7, 0x4E);
	xmm6 = _mm_mul_ps(xmm6, xmm1);
	s70 = xmm4;
	xmm4 = xmm5;
	xmm5 = _mm_mul_ps(xmm5, xmm7);
	xmm7 = _mm_mul_ps(xmm7, xmm1);
	xmm0 = _mm_sub_ps(xmm0, xmm5);
	xmm7 = _mm_sub_ps(xmm7, xmm6);
	xmm5 = xmm1;
	xmm1 = _mm_mul_ps(xmm1, xmm0);
	xmm7 = _mm_shuffle_ps(xmm7, xmm7, 0x4E);
	xmm6 = xmm1;
	xmm1 = _mm_shuffle_ps(xmm1, xmm1, 0x4E);
	s20 = xmm0;
	xmm1 = _mm_add_ps(xmm1, xmm6);
	xmm0 = xmm1;
	xmm1 = _mm_shuffle_ps(xmm1, xmm1, 0xB1);
	xmm1 = _mm_add_ss(xmm1, xmm0);
	s10 = xmm1;
	xmm0 = xmm5;
	xmm5 = _mm_mul_ps(xmm5, xmm3);
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0xB1);
	xmm6 = xmm5;
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0x4E);
	s80 = xmm4;
	xmm4 = s70;
	s90 = xmm3;
	xmm3 = xmm4;
	xmm3 = _mm_mul_ps(xmm3, xmm6);
	xmm3 = _mm_add_ps(xmm3, xmm7);
	xmm7 = xmm4;
	xmm7 = _mm_mul_ps(xmm7, xmm5);
	xmm7 = _mm_sub_ps(xmm7, xmm3);
	xmm3 = xmm4;
	xmm3 = _mm_mul_ps(xmm3, xmm0);
	xmm3 = _mm_shuffle_ps(xmm3, xmm3, 0xB1);
	sA0 = xmm5;
	xmm5 = s90;
	sB0 = xmm6;
	xmm6 = xmm5;
	xmm5 = _mm_mul_ps(xmm5, xmm3);
	xmm5 = _mm_add_ps(xmm5, xmm7);
	xmm7 = xmm3;
	xmm3 = _mm_shuffle_ps(xmm3, xmm3, 0x4E);
	sC0 = xmm4;
	xmm4 = xmm6;
	xmm4 = _mm_mul_ps(xmm4, xmm3);
	sD0 = xmm6;
	xmm6 = xmm5;
	xmm6 = _mm_sub_ps(xmm6, xmm4);
	xmm4 = s30;
	xmm5 = s40;
	xmm4 = _mm_sub_ps(xmm4, xmm5);
	xmm4 = _mm_shuffle_ps(xmm4, xmm4, 0x4E);
	xmm5 = s80;
	xmm7 = _mm_mul_ps(xmm7, xmm5);
	xmm3 = _mm_mul_ps(xmm3, xmm5);
	xmm4 = _mm_sub_ps(xmm4, xmm7);
	xmm0 = _mm_mul_ps(xmm0, xmm5);
	xmm4 = _mm_add_ps(xmm4, xmm3);
	xmm0 = _mm_shuffle_ps(xmm0, xmm0, 0xB1);
	xmm3 = xmm0;
	xmm0 = _mm_shuffle_ps(xmm0, xmm0, 0x4E);
	xmm7 = sC0;
	sE0 = xmm6;
	xmm6 = xmm7;
	xmm7 = _mm_mul_ps(xmm7, xmm3);
	xmm6 = _mm_mul_ps(xmm6, xmm0);
	xmm4 = _mm_add_ps(xmm4, xmm7);
	xmm4 = _mm_sub_ps(xmm4, xmm6);
	xmm6 = s60;
	xmm7 = s50;
	xmm6 = _mm_sub_ps(xmm6, xmm7);
	xmm6 = _mm_shuffle_ps(xmm6, xmm6, 0x4E);
	xmm7 = sB0;
	xmm7 = _mm_mul_ps(xmm7, xmm5);
	xmm7 = _mm_sub_ps(xmm7, xmm6);
	xmm6 = sA0;
	xmm5 = _mm_mul_ps(xmm5, xmm6);
	xmm7 = _mm_sub_ps(xmm7, xmm5);
	xmm5 = sD0;
	xmm3 = _mm_mul_ps(xmm3, xmm5);
	xmm5 = _mm_mul_ps(xmm5, xmm0);
	xmm7 = _mm_sub_ps(xmm7, xmm3);
	xmm2 = xmm5;
	xmm2 = _mm_add_ps(xmm2, xmm7);
	xmm6 = sE0;

	// comiss + je: a zero or unordered determinant returns null.
	const float det = _mm_cvtss_f32(s10);
	if (det == 0.0f || det != det)
		return nullptr;
	if (determinant != nullptr)
		*determinant = det;

	xmm0 = s10;
	xmm1 = s20;
	xmm3 = xmm0;
	xmm0 = _mm_rcp_ss(xmm0);
	xmm5 = xmm0;
	xmm0 = _mm_mul_ss(xmm0, xmm0);
	xmm3 = _mm_mul_ss(xmm3, xmm0);
	xmm5 = _mm_add_ss(xmm5, xmm5);
	xmm5 = _mm_sub_ss(xmm5, xmm3);
	xmm5 = _mm_shuffle_ps(xmm5, xmm5, 0);
	xmm1 = _mm_mul_ps(xmm1, xmm5);
	xmm4 = _mm_mul_ps(xmm4, xmm5);
	xmm6 = _mm_mul_ps(xmm6, xmm5);
	xmm5 = _mm_mul_ps(xmm5, xmm2);
	_mm_storeu_ps(&out->m[0][0], xmm1);
	_mm_storeu_ps(&out->m[1][0], xmm4);
	_mm_storeu_ps(&out->m[2][0], xmm6);
	_mm_storeu_ps(&out->m[3][0], xmm5);
	return out;
}

} // namespace D3DMatrixMath

// A D3DMATRIX with the D3DXMATRIX product operators: a * b and a *= b are D3DMatrixMath::Multiply(a, b), as the
// D3DXMATRIX operators call D3DXMatrixMultiply.
struct D3DMatrix : public D3DMATRIX
{
	D3DMatrix() {}
	D3DMatrix operator*(const D3DMATRIX& b) const
	{
		D3DMatrix r;
		D3DMatrixMath::Multiply(&r, this, &b);
		return r;
	}
	D3DMatrix& operator*=(const D3DMATRIX& b)
	{
		D3DMatrixMath::Multiply(this, this, &b);
		return *this;
	}
};

// Four floats with the layout and the pointer conversion of D3DXVECTOR4 (shader constants, vertex data).
struct D3DVector4
{
	D3DVector4() {}
	D3DVector4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
	operator float*() { return &x; }
	operator const float*() const { return &x; }
	float x, y, z, w;
};
