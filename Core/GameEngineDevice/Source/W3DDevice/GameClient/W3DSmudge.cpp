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

// W3DSmudge.cpp ////////////////////////////////////////////////////////////////////////////////
// Smudge System implementation
// Author: Mark Wilczynski, June 2003
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "Lib/BaseType.h"
#include "WWLib/always.h"
#include "W3DDevice/GameClient/W3DSmudge.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "Common/GameMemory.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"
#include "WW3D2/texture.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/sortingrenderer.h"


SmudgeManager *TheSmudgeManager=nullptr;

W3DSmudgeManager::W3DSmudgeManager()
{
}

W3DSmudgeManager::~W3DSmudgeManager()
{
	ReleaseResources();
}

void W3DSmudgeManager::init()
{
	SmudgeManager::init();
	ReAcquireResources();
}

void W3DSmudgeManager::reset ()
{
	SmudgeManager::reset();	//base
}

void W3DSmudgeManager::ReleaseResources()
{
	REF_PTR_RELEASE(m_backgroundTexture);
	REF_PTR_RELEASE(m_indexBuffer);
}


#define SMUDGE_DRAW_SIZE	500	//draw at most 50 smudges per call. Tweak value to improve CPU/GPU parallelism.

static_assert(SMUDGE_DRAW_SIZE * 5 < 0x10000, "Vertex index exceeds 16-bit limit");


void W3DSmudgeManager::ReAcquireResources()
{
	ReleaseResources();

	// Direct3D 12 mode has no Direct3D 8 back buffer to describe; the background texture is a render
	// target of the back buffer size that render() fills on the GPU.
	if (!updateBackgroundTexture())
		return;

	m_indexBuffer=NEW_REF(DX8IndexBufferClass,(SMUDGE_DRAW_SIZE*4*3));	//allocate 4 triangles per smudge, each with 3 indices.

	// Fill up the IB with static vertex indices that will be used for all smudges.
	{
		DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexBuffer);
		UnsignedShort *ib=lockIdxBuffer.Get_Index_Array();
		//quad of 4 triangles:
		//	0-----3
		//  |\   /|
		//  |  4  |
		//	|/   \|
		//  1-----2
		Int vbCount=0;
		for (Int i=0; i<SMUDGE_DRAW_SIZE; i++)
		{
			//Top
			ib[0]=vbCount;
			ib[1]=vbCount+4;
			ib[2]=vbCount+3;
			//Right
			ib[3]=vbCount+3;
			ib[4]=vbCount+4;
			ib[5]=vbCount+2;
			//Bottom
			ib[6]=vbCount+2;
			ib[7]=vbCount+4;
			ib[8]=vbCount+1;
			//Left
			ib[9]=vbCount+1;
			ib[10]=vbCount+4;
			ib[11]=vbCount+0;

			vbCount += 5;
			ib+=12;
		}
	}
}

/*Direct3D 12 mode: makes sure the background texture is a render target of the back buffer size, (re)creating it
when there is none yet or the back buffer was resized, which needs no device reset there.  Returns false, with no
background texture, if it cannot be created.*/
Bool W3DSmudgeManager::updateBackgroundTexture()
{
	const Int width = (Int)DX12Device::Get_Width();
	const Int height = (Int)DX12Device::Get_Height();

	if (m_backgroundTexture && m_backBufferWidth == width && m_backBufferHeight == height)
		return TRUE;

	REF_PTR_RELEASE(m_backgroundTexture);
	if (width <= 0 || height <= 0)
		return FALSE;

	// A8R8G8B8 like the back buffer, so the GPU copy needs no conversion.
	m_backgroundTexture = MSGNEW("TextureClass") TextureClass(width,height,WW3D_FORMAT_A8R8G8B8,MIP_LEVELS_1,TextureClass::POOL_DEFAULT, true);
	if (m_backgroundTexture->Peek_DX12_Texture() == nullptr)
	{
		REF_PTR_RELEASE(m_backgroundTexture);
		return FALSE;
	}

	m_backBufferWidth = width;
	m_backBufferHeight = height;
	return TRUE;
}

Bool W3DSmudgeManager::testHardwareSupport()
{
	if (m_hardwareSupportStatus == SMUDGE_SUPPORT_UNKNOWN)
	{
		//we have not done the test yet.

		// TheSuperHackers @bugfix When Render-To-Texture is disabled globally, we fallback
		// to copying the backbuffer to a texture. Direct3D 12 mode has no Direct3D 8 render texture, so this
		// is the only path. The old Direct3D 8 probe that compared a rendered block with a copy of it is gone.
		if (m_backgroundTexture)
		{
			m_hardwareSupportStatus = SMUDGE_SUPPORT_YES;
			return TRUE;
		}

		m_hardwareSupportStatus = SMUDGE_SUPPORT_NO;
		return FALSE;
	}

	return (SMUDGE_SUPPORT_YES == m_hardwareSupportStatus);
}

void W3DSmudgeManager::render(RenderInfoClass &rinfo)
{
	//Verify that the card supports the effect.
	if (!testHardwareSupport())
		return;

	// TheSuperHackers @performance stephanmeesters 14/08/2026 Early return when we have no smudge sets
	// or if the global smudge set is the only set and contains no smudges.
	if (m_usedSmudgeSetList.empty() || (m_usedSmudgeSetList.size() == 1 && m_usedSmudgeSetList.front()->getUsedSmudgeCount() == 0))
	{
		m_smudgeCountLastFrame = 0;
		return;
	}

	SurfaceClass::SurfaceDescription surface_desc;

	// Direct3D 12 mode has no Direct3D 8 back buffer to lock or copy. The background texture is a render
	// target of the back buffer size (resized with it) that is filled on the GPU below.
	if (!updateBackgroundTexture())
		return;

	surface_desc.Format = WW3D_FORMAT_A8R8G8B8;
	surface_desc.Width = m_backBufferWidth;
	surface_desc.Height = m_backBufferHeight;

	CameraClass &camera=rinfo.Camera;
	Vector3 vsVert;
	Vector4 ssVert;
	Real uvSpanX,uvSpanY;
	Vector3 vertex_offsets[4] = {
		Vector3(-0.5f, 0.5f, 0.0f),
		Vector3(-0.5f, -0.5f, 0.0f),
		Vector3(0.5f, -0.5f, 0.0f),
		Vector3(0.5f, 0.5f, 0.0f)
	};

#define THE_COLOR (0x00ffeedd)

	UnsignedInt vertexDiffuse[5]={THE_COLOR,THE_COLOR,THE_COLOR,THE_COLOR,THE_COLOR};

	Matrix4x4 proj;
	Matrix3D view;

	camera.Get_View_Matrix(&view);
	camera.Get_Projection_Matrix(&proj);

	Real texClampX = (Real)TheTacticalView->getWidth()/(Real)surface_desc.Width;
	Real texClampY = (Real)TheTacticalView->getHeight()/(Real)surface_desc.Height;

	Real texScaleX = texClampX*0.5f;
	Real texScaleY = texClampY*0.5f;

	//Do a first pass over the smudges to determine how many are visible
	//and to fill in their world-space positions and screen uv coordinates.
	//TODO: Optimize out this extra pass!
	//TODO: Find size of screen rectangle that actually needs copying.

	SmudgeSetDeque::iterator setIt=m_usedSmudgeSetList.begin();	//first set that didn't fit into render batch.
	Int count = 0;

	// make sure background particles have finished drawing.
	SortingRendererClass::Flush();	//draw sorted translucent polys like particles.

	for(; setIt != m_usedSmudgeSetList.end(); ++setIt)
	{
		SmudgeSet* set=*setIt;
		SmudgeDeque::iterator smudgeIt=set->getUsedSmudgeList().begin();

		for (; smudgeIt != set->getUsedSmudgeList().end(); ++smudgeIt)
		{
			Smudge* smudge=*smudgeIt;
			if (!smudge->m_draw)
				continue;

			//Get view-space center
			Matrix3D::Transform_Vector(view,smudge->m_pos,&vsVert);

			//Get 5 view-space vertices
			Smudge::smudgeVertex *verts=smudge->m_verts;

			//Do center vertex outside 'for' loop since it's different.
			verts[4].pos = vsVert;

			Vector2 offset = smudge->m_offset;

			for (Int i=0; i<4; i++)
			{
				verts[i].pos = vsVert + vertex_offsets[i] * smudge->m_size;
				//Ge uv coordinates for each vertex
				ssVert = proj * verts[i].pos;
				Real oow = 1.0f/ssVert.W;
				ssVert *= oow;	//returned in camera space which is -1,-1 (bottom-left) to 1,1 (top-right)
				//convert camera space to uv space: 0,0 (top-left), 1,1 (bottom-right)
				verts[i].uv.Set((ssVert.X+1.0f)*texScaleX,(1.0f-ssVert.Y)*texScaleY);

				Vector2 &thisUV=verts[i].uv;

				// Zero coordinates that fall outside valid texel bounds
				if (thisUV.X < 0 || thisUV.X > texClampX)
					offset.X = 0;

				if (thisUV.Y < 0 || thisUV.Y > texClampY)
					offset.Y = 0;
			}

			//Finish center vertex
			//Ge uv coordinates by interpolating corner uv coordinates and applying desired offset.
			uvSpanX=verts[3].uv.X - verts[0].uv.X;
			uvSpanY=verts[1].uv.Y - verts[0].uv.Y;
			verts[4].uv.X=verts[0].uv.X+uvSpanX*(0.5f+offset.X);
			verts[4].uv.Y=verts[0].uv.Y+uvSpanY*(0.5f+offset.Y);

			count++;	//increment visible smudge count.
		}
	}

	m_smudgeCountLastFrame = count;

	if (!count)
	{
		return;	//nothing to render.
	}

	//Copy the area of backbuffer occupied by smudges into an alternate buffer.
	// Direct3D 12 mode copies the current render target on the GPU at this point of the frame: the back
	// buffer, or the texture a screen filter redirected the rendering into.
	if (!DX8Wrapper::Copy_Back_Buffer_To_Texture(m_backgroundTexture))
		return;

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);
	DX8Wrapper::Set_Transform(D3DTS_VIEW,identity);

	DX8Wrapper::Set_Index_Buffer(m_indexBuffer,0);
	//DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueSpriteShader);

	DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);

	DX8Wrapper::Set_Texture(0,m_backgroundTexture);
	//Need these states in case texture is non-power-of-2
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSW, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);
	DX8Wrapper::Apply_Render_State_Changes();

	//Disable reading texture alpha since it's undefined.
	//DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG2);

	Int smudgesRemaining=count;
	setIt=m_usedSmudgeSetList.begin();	//first smudge set that needs rendering.
	SmudgeDeque::iterator smudgeIt = (*setIt)->getUsedSmudgeList().begin();	//first smudge that needs rendering.

	while (smudgesRemaining)	//keep drawing smudges until we run out.
	{
		//Now that we know how many smudges need rendering, allocate vertex buffer space and copy verts.
		count=smudgesRemaining;

		if (count > SMUDGE_DRAW_SIZE)
			count = SMUDGE_DRAW_SIZE;

		Int smudgesInRenderBatch=0;

		DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,count*5);	//allocate 5 verts per smudge.
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();

			while (setIt != m_usedSmudgeSetList.end())
			{
				SmudgeDeque& smudgeList = (*setIt)->getUsedSmudgeList();

				for(; smudgeIt != smudgeList.end(); ++smudgeIt)
				{
					Smudge* smudge = *smudgeIt;
					if (!smudge->m_draw)
					{
						continue;
					}

					Smudge::smudgeVertex *smVerts = smudge->m_verts;

					//Check if we exceeded maximum number of smudges allowed per draw call.
					if (smudgesInRenderBatch >= count)
					{
						goto flushSmudges;
					}

					//Set center vertex opacity.
					vertexDiffuse[4] = ((Int)(smudge->m_opacity * 255.0f) << 24) | THE_COLOR;

					for (Int i=0; i<5; i++)
					{
						verts->x=smVerts->pos.X;
						verts->y=smVerts->pos.Y;
						verts->z=smVerts->pos.Z;
						verts->nx=0;	//keep AGP write-combining active
						verts->ny=0;
						verts->nz=0;
						verts->diffuse=vertexDiffuse[i];	//set to transparent
						verts->u1=smVerts->uv.X;
						verts->v1=smVerts->uv.Y;
						verts->u2=0;	//keep AGP write-combining active
						verts->v2=0;
						verts++;
						smVerts++;
					}

					smudgesInRenderBatch++;
				}

				++setIt;	//advance to next node.

				if (setIt != m_usedSmudgeSetList.end())	//start next batch at beginning of set.
					smudgeIt = (*setIt)->getUsedSmudgeList().begin();
			}
		}

flushSmudges:
		DX8Wrapper::Set_Vertex_Buffer(vb_access);

		DX8Wrapper::Draw_Triangles(0,smudgesInRenderBatch*4, 0, smudgesInRenderBatch*5);

//Debug Code which draws outline around smudge
/*		DX8Wrapper::Set_DX8_Render_State_Raw(D3DRS_FILLMODE,D3DFILL_WIREFRAME);
		DX8Wrapper::Set_DX8_Render_State_Raw(D3DRS_ALPHABLENDENABLE,FALSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_COLOROP,D3DTOP_SELECTARG2);
		DX8Wrapper::Draw_Triangles(	0,smudgesInRenderBatch*4, 0, smudgesInRenderBatch*5);
		DX8Wrapper::Set_DX8_Render_State_Raw(D3DRS_FILLMODE,D3DFILL_SOLID);
		DX8Wrapper::Set_DX8_Render_State_Raw(D3DRS_ALPHABLENDENABLE,TRUE);
		DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
*/
		smudgesRemaining -= smudgesInRenderBatch;
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_COLOROP,D3DTOP_MODULATE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);

}
