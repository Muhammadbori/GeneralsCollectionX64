/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// BezierSegment.h ////////////////////////////////////////////////////////////////////////////////
// John K McDonald, Jr.
// September 2002
// DO NOT DISTRIBUTE

#pragma once

#include "Common/STLTypedefs.h"
#include "Lib/BaseDefines.h"

#define USUAL_TOLERANCE 1.0f

// TheSuperHackers @refactor The Bezier math keeps its own 4-float vector and 4x4 matrix types instead of the
// D3DX ones. They have the same memory layout: x, y, z, w and m[row][column].
struct BezierVector4
{
	BezierVector4() {}
	BezierVector4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
	float x, y, z, w;
};

struct BezierMatrix
{
	float m[4][4];
};

class BezierMath
{
public:
	// Row vector times matrix.
	static void Vec4Transform(BezierVector4* out, const BezierVector4* v, const BezierMatrix* m)
	{
#if USE_DETERMINISTIC_MATH
		const float x = v->x, y = v->y, z = v->z, w = v->w;
		out->x = ((x * m->m[0][0] + y * m->m[1][0]) + z * m->m[2][0]) + w * m->m[3][0];
		out->y = ((x * m->m[0][1] + y * m->m[1][1]) + z * m->m[2][1]) + w * m->m[3][1];
		out->z = ((x * m->m[0][2] + y * m->m[1][2]) + z * m->m[2][2]) + w * m->m[3][2];
		out->w = ((x * m->m[0][3] + y * m->m[1][3]) + z * m->m[2][3]) + w * m->m[3][3];
#else
		// The retail game called D3DXVec4Transform here. Its SSE and SSE2 versions in d3dx8.lib add the rows
		// pairwise, which is the order below. Not verified in this configuration (RETAIL_COMPATIBLE_CRC=1).
		const float x = v->x, y = v->y, z = v->z, w = v->w;
		out->x = (x * m->m[0][0] + y * m->m[1][0]) + (z * m->m[2][0] + w * m->m[3][0]);
		out->y = (x * m->m[0][1] + y * m->m[1][1]) + (z * m->m[2][1] + w * m->m[3][1]);
		out->z = (x * m->m[0][2] + y * m->m[1][2]) + (z * m->m[2][2] + w * m->m[3][2]);
		out->w = (x * m->m[0][3] + y * m->m[1][3]) + (z * m->m[2][3] + w * m->m[3][3]);
#endif
	}

	static float Vec4Dot(const BezierVector4* a, const BezierVector4* b)
	{
#if USE_DETERMINISTIC_MATH
		return ((a->x * b->x + a->y * b->y) + a->z * b->z) + a->w * b->w;
#else
		// The retail game called the D3DXVec4Dot inline of d3dx8math.inl, the same left-to-right sum.
		return a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
#endif
	}
};

class BezierSegment
{
	protected:
		static const BezierMatrix s_bezBasisMatrix;
		Coord3D m_controlPoints[4];

	public:	// Constructors
		BezierSegment();
		BezierSegment(Real x0, Real y0, Real z0,
									Real x1, Real y1, Real z1,
									Real x2, Real y2, Real z2,
									Real x3, Real y3, Real z3);

		BezierSegment(Real cp[12]);


		BezierSegment(const Coord3D& cp0,
									const Coord3D& cp1,
									const Coord3D& cp2,
									const Coord3D& cp3);

		BezierSegment(Coord3D cp[4]);

	public:
		void evaluateBezSegmentAtT(Real tValue, Coord3D *outResult) const;
		void getSegmentPoints(Int numSegments, VecCoord3D *outResult) const;
		Real getApproximateLength(Real withinTolerance = USUAL_TOLERANCE) const;

		void splitSegmentAtT(Real tValue, BezierSegment &outSeg1, BezierSegment &outSeg2) const;

	public:	// He get's friendly access.
		friend class BezFwdIterator;
};
