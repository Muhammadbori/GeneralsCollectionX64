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

// FILE: W3DSnow.h /////////////////////////////////////////////////////////

#include "W3DDevice/GameClient/W3DSnow.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "Common/GlobalData.h"
#include "GameClient/View.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/assetmgr.h"
#include "WW3D2/ww3d.h"



#define SNOW_BATCH_SIZE	2048	//we render at most this many particles per drawprimitive call.  This number * 6 must be less than 65536 to fit into index buffer.

// TheSuperHackers @feature The snow style: Classic draws flakes with the size law of hardware point sprites, the look the
// weather INI values were made for; Large draws the point sprite emulation, flakes of SnowQuadSize world units. Both are
// drawn as quads.
static Bool isClassicSnowStyle()
{
	switch (TheGlobalData->m_snowStyle)
	{
		case SNOW_STYLE_CLASSIC: return TRUE;
		case SNOW_STYLE_LARGE: return FALSE;
		default: return TheWeatherSetting->m_usePointSprites;
	}
}

W3DSnowManager::W3DSnowManager()
{
	m_indexBuffer=nullptr;
	m_snowTexture=nullptr;
}

W3DSnowManager::~W3DSnowManager()
{
	ReleaseResources();
}

void W3DSnowManager::init()
{
	SnowManager::init();
	ReAcquireResources();
}

/** Releases all W3D/D3D assets before a reset.. */
void W3DSnowManager::ReleaseResources()
{
	REF_PTR_RELEASE(m_snowTexture);
	REF_PTR_RELEASE(m_indexBuffer);
}

/** (Re)allocates all W3D/D3D assets after a reset.. */
Bool W3DSnowManager::ReAcquireResources()
{
	ReleaseResources();

	if (!TheWeatherSetting->m_snowEnabled)
		return TRUE;	//no need for resources if snow is disabled.

	m_indexBuffer=NEW_REF(DX8IndexBufferClass,(SNOW_BATCH_SIZE *6));	//allocate 2 triangles per flake, each with 3 indices.

	// Fill up the IB with static vertex indices that will be used for all smudges.
	{
		DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexBuffer);
		UnsignedShort *ib=lockIdxBuffer.Get_Index_Array();
		//quad of 4 triangles:
		//	0-----3
		//  |\   /|
		//  |  X  |
		//	|/   \|
		//  1-----2
		Int vbCount=0;
		for (Int i=0; i<SNOW_BATCH_SIZE; i++)
		{
			//Top
			ib[0]=vbCount+3;
			ib[1]=vbCount;
			ib[2]=vbCount+2;
			//Bottom
			ib[3]=vbCount+2;
			ib[4]=vbCount;
			ib[5]=vbCount+1;

			vbCount += 4;
			ib+=6;
		}
	}

	m_snowTexture = WW3DAssetManager::Get_Instance()->Get_Texture(TheWeatherSetting->m_snowTexture.str());

	return TRUE;
}

void W3DSnowManager::updateIniSettings()
{
	//Call base class
	SnowManager::updateIniSettings();

	if (m_snowTexture && stricmp(m_snowTexture->Get_Texture_Name(),TheWeatherSetting->m_snowTexture.str()) != 0)
	{
		REF_PTR_RELEASE(m_snowTexture);
		m_snowTexture = WW3DAssetManager::Get_Instance()->Get_Texture(TheWeatherSetting->m_snowTexture.str());
	}
}

void W3DSnowManager::reset()
{
	SnowManager::reset();
}

void W3DSnowManager::update()
{
	// TheSuperHackers @tweak The snow render update is now decoupled from the logic step.
	m_time += WW3D::Get_Logic_Frame_Time_Seconds();

	//find current time offset, adjusting for overflow
	m_time=fmod(m_time,m_fullTimePeriod);
}

#define MAXIMUM_CAMERA_DISTANCE 100000	//maximum distance of camera position from world origin.
#define ISPOW2(x)  (x && (x & (x-1)) == 0)	//is a number a power of 2?
#define MODPOW2(x,y) ((x) & (y-1))		//mod '%' operator for powers of 2.

void W3DSnowManager::render(RenderInfoClass &rinfo)
{
	if (!TheWeatherSetting->m_snowEnabled || !m_isVisible)
		return;

	const Bool classic = isClassicSnowStyle();

	//make sure the noise table is powers of 2 in dimensions.
	WWASSERT(ISPOW2(SNOW_NOISE_X) && ISPOW2(SNOW_NOISE_Y));

	//CameraClass &camera=rinfo.Camera;

	const Coord3D &cPos=TheTacticalView->get3DCameraPosition();
	Vector3 camPos(cPos.x,cPos.y,cPos.z);

	//Number of emitters from cube center to edge of visible extent.
	Int mumEmittersInHalf=(Int)floor(m_boxDimensions / m_emitterSpacing * 0.5f);

	//Find origin of visible cube surrounding camera.
	Int cubeCenterX=(Int)floor(camPos.X/m_emitterSpacing);
	Int cubeCenterY=(Int)floor(camPos.Y/m_emitterSpacing);

	//Find extents of visible cube surrounding camera.
	Int cubeOriginX=cubeCenterX - mumEmittersInHalf;	//top/left extents.
	Int cubeOriginY=cubeCenterY - mumEmittersInHalf;
	Int cubeDimX=cubeCenterX + mumEmittersInHalf;		//bottom/right extents.
	Int cubeDimY=cubeCenterY + mumEmittersInHalf;

 	const FrustumClass & frustum = rinfo.Camera.Get_Frustum();
	AABoxClass bbox;

	//Get a bounding box around our visible universe.  Bounded by terrain and the sky
	//so much tighter fitting volume than what's actually visible.  This will cull
	//particles falling under the ground.

 	TheTerrainRenderObject->getMaximumVisibleBox(frustum, &bbox, TRUE);

	//Particles move outside the visible box as a result of local sine movement
	//so adjust bounding box to include them.
	bbox.Extent.X += m_amplitude+m_quadSize;
	bbox.Extent.Y += m_amplitude+m_quadSize;

	//Clip our visible snow rendering box
	if ((cubeOriginX * m_emitterSpacing ) < (bbox.Center.X - bbox.Extent.X))
		cubeOriginX = (Int)floor ((bbox.Center.X - bbox.Extent.X)/m_emitterSpacing);

	if ((cubeOriginY * m_emitterSpacing ) < (bbox.Center.Y - bbox.Extent.Y))
		cubeOriginY = (Int)floor ((bbox.Center.Y - bbox.Extent.Y)/m_emitterSpacing);

	if ((cubeDimX * m_emitterSpacing ) > (bbox.Center.X + bbox.Extent.X))
		cubeDimX = (Int)floor ((bbox.Center.X + bbox.Extent.X)/m_emitterSpacing);

	if ((cubeDimY * m_emitterSpacing ) > (bbox.Center.Y + bbox.Extent.Y))
		cubeDimY = (Int)floor ((bbox.Center.Y + bbox.Extent.Y)/m_emitterSpacing);

	if ((cubeDimY - cubeOriginY) < 0 || (cubeDimX-cubeOriginX) < 0)
		return;	//entire snow box is culled by either x or y screen boundary.

	//Find total number of particles that need rendering.
	Int totalPart=(cubeDimY-cubeOriginY)*(cubeDimX-cubeOriginX);

	if (totalPart <= 0)
		return;	//nothing to render.

	//Height at the top of the cube with camera at center.
	m_snowCeiling = camPos.Z + m_boxDimensions/2.0f;

	//Offset to allow cube extents to move with camera.
	Real cameraOffset = fmod (camPos.Z,m_boxDimensions);
	m_heightTraveled=m_time*m_velocity+cameraOffset;	//height that snow flake traveled this frame.

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);

	DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);

	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);

	//make sure we have all the resources we need
	if (!m_indexBuffer)
		ReAcquireResources();

	DX8Wrapper::Set_Texture(0,m_snowTexture);

	m_totalRendered = 0;	//keep track of how many particles were rendered.
	renderAsQuads(rinfo,cubeOriginX,cubeOriginY,cubeDimX,cubeDimY,classic);
}

/**Draws the flakes as quads.  With classic set, each flake gets the screen size a hardware point sprite would have,
otherwise all flakes are SnowQuadSize world units wide.*/
void W3DSnowManager::renderAsQuads(RenderInfoClass &rinfo, Int cubeOriginX, Int cubeOriginY, Int cubeDimX, Int cubeDimY, Bool classic)
{

	Matrix4x4 proj;
	Matrix3D view;
	Vector3 snowCenter;
	Vector3 snowCenterVS;

	CameraClass &camera=rinfo.Camera;

	camera.Get_View_Matrix(&view);
	camera.Get_Projection_Matrix(&proj);

	Vector3 vertex_offsets[4] = {
		Vector3(-0.5f, 0.5f, 0.0f),
		Vector3(-0.5f, -0.5f, 0.0f),
		Vector3(0.5f, -0.5f, 0.0f),
		Vector3(0.5f, 0.5f, 0.0f)
	};

	Vector2 quad_uvs[4] = {
		Vector2(0.0f, 0.0f),
		Vector2(0.0f, 1.0f),
		Vector2(1.0f, 1.0f),
		Vector2(1.0f, 0.0f)
	};


	// TheSuperHackers @feature The Classic style uses the size law of the point sprite path: Direct3D point scaling with
	// A=0, B=0, C=1 gives viewport height * SnowPointSize / eye distance pixels, clamped to SnowMinPointSize..SnowMaxPointSize.
	// A view space square of side s at depth z covers s * viewport height / (2 * tanHalfV * z) pixels, so a flake of p
	// pixels gets the side p * 2 * tanHalfV * z / viewport height.
	Real viewportHeight = 1.0f;
	Real pixelsToViewSpace = 0.0f;
	if (classic)
	{
		Vector2 viewportMin, viewportMax;
		camera.Get_Viewport(viewportMin, viewportMax);
		int width, height, bits;
		bool windowed;
		WW3D::Get_Render_Target_Resolution(width, height, bits, windowed);
		viewportHeight = (Real)(unsigned int)((viewportMax.Y - viewportMin.Y) * (float)height);	// as CameraClass::Apply sets the viewport
		if (viewportHeight < 1.0f)
			viewportHeight = 1.0f;

		Vector2 viewPlaneMin, viewPlaneMax;
		camera.Get_View_Plane(viewPlaneMin, viewPlaneMax);
		pixelsToViewSpace = (viewPlaneMax.Y - viewPlaneMin.Y) / viewportHeight;	// 2 * tanHalfV / viewport height
	}
	else
	{
		//pre-multiple the offsets by particle size
		for (Int i=0; i<4; i++)
		{
			vertex_offsets[i] *= m_quadSize;
		}
	}

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_VIEW,identity);

	DX8Wrapper::Set_Index_Buffer(m_indexBuffer,0);

	Int y=cubeOriginY;	//loop counter.
	Int cubeOriginXRemainder = cubeOriginX;	//loop counter - adjusted when not all particles fit into render buffer.

	//Find total number of particles that need rendering.
	Int totalPart=(cubeDimY-cubeOriginY)*(cubeDimX-cubeOriginX);

	m_totalRendered += totalPart;

	while (totalPart)
	{
		Int batchSize=totalPart;

		if (batchSize > SNOW_BATCH_SIZE)
			batchSize = SNOW_BATCH_SIZE;

		Int numberInBatch=0;

		DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,batchSize*4);	//allocate 4 verts per flake
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();

			for (;y<cubeDimY; y++)
			{
				for (Int x=cubeOriginXRemainder; x<cubeDimX; x++)
				{
					if (numberInBatch >= batchSize)
					{	cubeOriginXRemainder = x;
						goto flush_particles;
					}

					//Get initial height from noise table.  We add a large value to make sure it's positive.  Then
					//modulate by table dimensions to find a value.
					Int noiseOffset=MODPOW2(x+MAXIMUM_CAMERA_DISTANCE,SNOW_NOISE_X)+MODPOW2(y+MAXIMUM_CAMERA_DISTANCE,SNOW_NOISE_Y)*SNOW_NOISE_X;
					if (noiseOffset > (SNOW_NOISE_X * SNOW_NOISE_Y))
						noiseOffset = 0;	//this should never happen but check to prevent buffer over/under flow.

					//find current height
					Real h0=m_snowCeiling-fmod(m_heightTraveled+m_startingHeights[noiseOffset],m_boxDimensions);

					//find world-space position of snow flake
					snowCenter.Set(x*m_emitterSpacing,y*m_emitterSpacing,h0);

					Real flakeSide = 1.0f;
					if (classic)
					{
						// The sway is added in world space, as on the point sprite path.
						snowCenter.X += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleX + (Real)x);
						snowCenter.Y += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleY + (Real)y);

						Matrix3D::Transform_Vector(view,snowCenter,&snowCenterVS);

						const Real eyeDistance = snowCenterVS.Length();
						Real pixels = (eyeDistance > 0.001f) ? viewportHeight * m_pointSize / eyeDistance : m_maxPointSize;
						if (pixels > m_maxPointSize)
							pixels = m_maxPointSize;
						if (pixels < m_minPointSize)
							pixels = m_minPointSize;
						const Real depth = -snowCenterVS.Z;	// the camera looks along -Z in view space
						flakeSide = pixels * pixelsToViewSpace * (depth > 0.0f ? depth : 0.0f);
					}
					else
					{
						//Get view-space position
						Matrix3D::Transform_Vector(view,snowCenter,&snowCenterVS);

						//Adjust position so snow flakes don't fall straight down.
						snowCenterVS.X += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleX + (Real)x);
						snowCenterVS.Y += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleY + (Real)y);
					}

					for (Int i=0; i<4; i++)
					{
						if (classic)
							*(Vector3 *)verts=snowCenterVS + vertex_offsets[i] * flakeSide;
						else
							*(Vector3 *)verts=snowCenterVS + vertex_offsets[i];
						verts->nx=0;	//keep AGP write-combining active
						verts->ny=0;
						verts->nz=0;
						verts->diffuse=0xffffffff;	//set to opaque
						verts->u1=quad_uvs[i].X;
						verts->v1=quad_uvs[i].Y;
						verts->u2=0;	//keep AGP write-combining active
						verts->v2=0;
						verts++;
					}

					numberInBatch++;
				}
				//getting here means we did not overflow the render buffer, so reset x origin to normal.
				cubeOriginXRemainder = cubeOriginX;	//reset to normal amount
			}
flush_particles:
			numberInBatch;	//need something at goto destination - stupid c compiler.
		}

		//Render any particles that may be queued up.
		if (numberInBatch)
		{
			DX8Wrapper::Set_Vertex_Buffer(vb_access);
			DX8Wrapper::Draw_Triangles(	0,numberInBatch*2, 0, numberInBatch*4);
			totalPart -= numberInBatch;
		}
	}
}
