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

// FILE: TerrainTex.cpp ////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: TerrainTex.cpp
//
// Created:   John Ahlquist, April 2001
//
// Desc:      TextureClass overrides to perform custom texturing for the terrain.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------
#include <stdlib.h>

#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/TileData.h"
#include "Common/GlobalData.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/surfaceclass.h"
#include "WW3D2/Backend/DX12/dx12resource.h"

/******************************************************************************
						Direct3D 12 helpers
******************************************************************************/
// In Direct3D 12 mode these textures have no IDirect3DTexture8. The update functions fill
// level 0 through SurfaceClass, address rows with the locked pitch, and build the mip chain
// on the CPU in place of D3DXFilterTexture. Direct3D 12 has no SetLOD; texture reduction is applied as
// the texture's minimum mip level (DX12TextureClass::Set_Min_LOD), which the sampler clamps to.

//=============================================================================
// TerrainTex_Build_DX12_Mips
//=============================================================================
/** Box filters mip levels 1..n of the texture from level 0 (Direct3D 12 mode). */
//=============================================================================
void TerrainTex_Build_DX12_Mips(TextureClass *tex)
{
	if (tex == nullptr) {
		return;
	}
	SurfaceClass::Generate_DX12_Mipmaps(tex->Peek_DX12_Texture());
}

//=============================================================================
// updateTerrainTextureDX12
//=============================================================================
/** Direct3D 12 version of TerrainTextureClass::update. */
//=============================================================================
int TerrainTextureClass::updateTerrainTextureDX12(WorldHeightMap *htMap)
{
	SurfaceClass *surface_level = Get_Surface_Level(0);
	if (surface_level == nullptr) {
		return 0;
	}
	SurfaceClass::SurfaceDescription surface_desc;
	surface_level->Get_Description(surface_desc);
	if (surface_desc.Width < TEXTURE_WIDTH) {
		REF_PTR_RELEASE(surface_level);
		return 0;
	}

	int pitch = 0;
	UnsignedByte *pBits = (UnsignedByte *)surface_level->Lock(&pitch);
	if (pBits == nullptr) {
		REF_PTR_RELEASE(surface_level);
		return 0;
	}

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
#ifdef RTS_DEBUG
	Int tilesPerRow = surface_desc.Width/(2*TILE_PIXEL_EXTENT+TILE_OFFSET);
	tilesPerRow *= 2;
	DEBUG_ASSERTCRASH((Int)surface_desc.Width >= tilePixelExtent*tilesPerRow, ("Bitmap too small."));
#endif
	if (surface_desc.Format == WW3D_FORMAT_A1R5G5B5) {
		Int tileNdx;
		Int pixelBytes = 2;
		for (tileNdx=0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
			TileData *pTile = htMap->getSourceTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real tile offsets start at 2.  jba.

			Int i,j;
			for (j=0; j<tilePixelExtent; j++) {
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				Int row = position.y+j;
				UnsignedByte *pBGRX = pBits + row*pitch;

				Int column = position.x;
				pBGRX += column*pixelBytes;
				for (i=0; i<tilePixelExtent; i++) {
					*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
					pBGRX +=pixelBytes;
					pBGR +=TILE_BYTES_PER_PIXEL;
				}
			}
		}
		// Now draw the 4 pixel border around each tile class.
		Int texClass;
		for (texClass=0; texClass<htMap->m_numTextureClasses; texClass++) {
			Int width = htMap->m_textureClasses[texClass].width;
			ICoord2D origin = htMap->m_textureClasses[texClass].positionInTexture;
			if (origin.x<=0) continue;
			width *= TILE_PIXEL_EXTENT;
			// Duplicate 4 columns of pixels before and after.
			Int j;
			for (j=0; j<width; j++) {
				Int row = origin.y+j;
				UnsignedByte *pBGRX = pBits + row*pitch;

				Int column = origin.x;
				pBGRX += column*pixelBytes;
				// copy before
				memcpy(pBGRX-(4)*pixelBytes, pBGRX+(width-4)*pixelBytes, 4*pixelBytes);
				// copy after
				memcpy(pBGRX+(width*pixelBytes), pBGRX, 4*pixelBytes);
			}

			// Duplicate 4 rows of pixels before and after.
			for (j=0; j<4; j++) {
				// copy before.
				Int row = origin.y-j-1;
				UnsignedByte *pBGRX = pBits + row*pitch;
				UnsignedByte *target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target, target+width*pitch, (width+8)*pixelBytes);
				// copy after.
				row = origin.y+j;
				pBGRX = pBits + row*pitch;
				target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target+width*pitch, target, (width+8)*pixelBytes);
			}
		}
	}
	surface_level->Unlock();
	REF_PTR_RELEASE(surface_level);
	TerrainTex_Build_DX12_Mips(this);
	if (WW3D::Get_Texture_Reduction() && Peek_DX12_Texture()) {
		Peek_DX12_Texture()->Set_Min_LOD(WW3D::Get_Texture_Reduction()); // as SetLOD in Direct3D 8
	}
	return(surface_desc.Height);
}

//=============================================================================
// updateFlatTerrainTextureDX12
//=============================================================================
/** Direct3D 12 version of TerrainTextureClass::updateFlat. */
//=============================================================================
Bool TerrainTextureClass::updateFlatTerrainTextureDX12(WorldHeightMap *htMap, Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell)
{
	SurfaceClass *surface_level = Get_Surface_Level(0);
	if (surface_level == nullptr) {
		return false;
	}
	SurfaceClass::SurfaceDescription surface_desc;
	surface_level->Get_Description(surface_desc);
	DEBUG_ASSERTCRASH((Int)surface_desc.Width == cellWidth*pixelsPerCell, ("Bitmap too small."));
	DEBUG_ASSERTCRASH((Int)surface_desc.Height == cellWidth*pixelsPerCell, ("Bitmap too small."));
	if (surface_desc.Width != cellWidth*pixelsPerCell) {
		REF_PTR_RELEASE(surface_level);
		return false;
	}

	int pitch = 0;
	UnsignedByte *pBits = (UnsignedByte *)surface_level->Lock(&pitch);
	if (pBits == nullptr) {
		REF_PTR_RELEASE(surface_level);
		return false;
	}

	if (surface_desc.Format == WW3D_FORMAT_A1R5G5B5) {
		Int pixelBytes = 2;
		Int cellX, cellY;
		for (cellX = 0; cellX < cellWidth; cellX++) {
			for (cellY = 0; cellY < cellWidth; cellY++) {
				UnsignedByte *pBGRX_data = pBits;
				UnsignedByte *pBGR = htMap->getPointerToTileData(xCell+cellX, yCell+cellY, pixelsPerCell);
				if (pBGR == nullptr) continue; // past end of defined terrain. [3/24/2003]
				Int k, l;
				for (k=pixelsPerCell-1; k>=0; k--) {
					UnsignedByte *pBGRX = pBGRX_data + (pixelsPerCell*(cellWidth-cellY-1)+k)*pitch +
						cellX*pixelsPerCell*pixelBytes;
					for (l=0; l<pixelsPerCell; l++) {
						*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
						pBGRX +=pixelBytes;
						pBGR +=TILE_BYTES_PER_PIXEL;
					}
				}
			}
		}
	}

	surface_level->Unlock();
	REF_PTR_RELEASE(surface_level);
	TerrainTex_Build_DX12_Mips(this);
	return(surface_desc.Height);
}

//=============================================================================
// updateAlphaEdgeTextureDX12
//=============================================================================
/** Direct3D 12 version of AlphaEdgeTextureClass::update. */
//=============================================================================
int AlphaEdgeTextureClass::updateAlphaEdgeTextureDX12(WorldHeightMap *htMap)
{
	SurfaceClass *surface_level = Get_Surface_Level(0);
	if (surface_level == nullptr) {
		return 0;
	}
	SurfaceClass::SurfaceDescription surface_desc;
	surface_level->Get_Description(surface_desc);

	int pitch = 0;
	UnsignedByte *pBits = (UnsignedByte *)surface_level->Lock(&pitch);
	if (pBits == nullptr) {
		REF_PTR_RELEASE(surface_level);
		return 0;
	}

	Int tilePixelExtent = TILE_PIXEL_EXTENT; // blend tiles are 1/4 tiles.

	if (surface_desc.Format == WW3D_FORMAT_A8R8G8B8) {
		Int cellX, cellY;
		for (cellX = 0; (UnsignedInt)cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; (UnsignedInt)cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = pBits + cellY*pitch + cellX*4;
				pBGR[2] = 255-cellY/2;
				pBGR[0] = cellX/2;
				pBGR[3] = cellX/2;  // alpha.
				pBGR[3] = 128;  // alpha.
			}
		}
		Int tileNdx;
		Int pixelBytes = 4;
		for (tileNdx=0; tileNdx < htMap->m_numEdgeTiles; tileNdx++) {
			TileData *pTile = htMap->getEdgeTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real edge offsets start at 4.  jba.
			Int i,j;
			Int column = position.x;
			for (j=0; j<tilePixelExtent; j++) {
				Int row = position.y+j;
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				UnsignedByte *pBGRX = pBits + row*pitch;
				pBGRX += column*pixelBytes;

				for (i=0; i<tilePixelExtent; i++) {
					pBGRX[0] = pBGR[0];  //r
					pBGRX[1] = pBGR[1];	//g
					pBGRX[2] = pBGR[2];	//b
					if (pBGR[0]==0 && pBGR[1]==0 && pBGR[2]==0) {
						pBGRX[3] = 0x80;
					} else if (pBGR[0]==0xff && pBGR[1]==0xff && pBGR[2]==0xff) {
						pBGRX[3] = 0x00;
					}	else {
						pBGRX[3] = 0xff;
					}

					pBGRX += pixelBytes;
					pBGR += TILE_BYTES_PER_PIXEL;
				}
			}
		}
	}
	surface_level->Unlock();
	REF_PTR_RELEASE(surface_level);
	TerrainTex_Build_DX12_Mips(this);
	return(surface_desc.Height);
}

/******************************************************************************
						TerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 16 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
TerrainTextureClass::TerrainTextureClass(int height) :
	TextureClass(TEXTURE_WIDTH, height,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_3 )
{
}

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 16 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
TerrainTextureClass::TerrainTextureClass(int height, int width) :
	TextureClass(width, height,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_ALL )
{
}


//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
int TerrainTextureClass::update(WorldHeightMap *htMap)
{
	return updateTerrainTextureDX12(htMap);
}


//=============================================================================
// TerrainTextureClass::setLOD
//=============================================================================
/** Sets the lod of the texture to be loaded into the video card.  */
//=============================================================================
void TerrainTextureClass::setLOD(Int LOD)
{
	// Direct3D 12 has no SetLOD; the texture's minimum mip level is applied by the sampler.
	if (Peek_DX12_Texture()) Peek_DX12_Texture()->Set_Min_LOD((unsigned)LOD);
}
//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
Bool TerrainTextureClass::updateFlat(WorldHeightMap *htMap, Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell)
{
	return updateFlatTerrainTextureDX12(htMap, xCell, yCell, cellWidth, pixelsPerCell);
}

//=============================================================================
// TerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup
(standard D3D setup, but beyond the scope of W3D).  */
//=============================================================================
void TerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}
	// Now setup the texture pipeline.
	if (stage==0) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);

	}
#endif
}

/******************************************************************************
						AlphaTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// AlphaTerrainTextureClass::AlphaTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to creat a throw away 8x8 texture,
then uses the base texture's D3D texture. This way the base tiles pass, drawn
using TerrainTextureClass shares the same texture with the blended edges pass,
saving lots of texture memory, and preventing seams between blended tiles. */
//=============================================================================
AlphaTerrainTextureClass::AlphaTerrainTextureClass( TextureClass *pBaseTex ):
	TextureClass(8, 8,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_1 )
{
	// Share the base texture's Direct3D 12 texture; this releases the throw away 8x8 one.
	Set_DX12_Texture(pBaseTex->Peek_DX12_Texture());
}


//=============================================================================
// AlphaTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
This may be applied in either single pass, as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the base texture in stage 0.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void AlphaTerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);

	// Set the bilinear or trilinear filtering.
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}
	// Since we are using multiple distinct tiles, the textures doesn't wrap, so clamp it.
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {
		// Modulate the diffuse color with the texture as lighting comes from diffuse.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 1 );
		// Blend the result using the alpha. (came from diffuse mod texture)
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
		// Disable stage 2.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	}	else if (stage==1) {

		if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
		{
			///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
			//This method is a backdoor specific to Nvidia based cards.  It will fail on
			//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP, D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_DIFFUSE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(2, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXCOORDINDEX, 2);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLORARG2, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(3, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, 3);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE | 0 | D3DTA_ALPHAREPLICATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(4, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_TEXCOORDINDEX, 4);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLORARG1, D3DTA_CURRENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture(5, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLOROP, D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_TEXCOORDINDEX, 5);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAOP,   D3DTOP_ADD);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(6, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLOROP, D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_TEXCOORDINDEX, 6);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLORARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLORARG2, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

			DX8Wrapper::Set_DX8_Texture(7, nullptr);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_TEXCOORDINDEX, 7);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLORARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLORARG2, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
		}
		else
		{
  			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
		}
	}
}


/******************************************************************************
						LightMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// LightMapTerrainTextureClass::LightMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
LightMapTerrainTextureClass::LightMapTerrainTextureClass(AsciiString name, MipCountType mipLevelCount) :
TextureClass(name.isEmpty()?"TSNoiseUrb.tga":name.str(),name.isEmpty()?"TSNoiseUrb.tga":name.str(), mipLevelCount )
{
	Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
}

#define STRETCH_FACTOR ((float)(1/(63.0*MAP_XY_FACTOR/2))) /* covers 63/2 tiles */

//=============================================================================
// LightMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The LightMapTerrainTextureClass may be applied by itself, or with the
CloudMapTerrainTextureClass.  This may be applied in either single pass,
as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the cloud map texture in stage 0.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void LightMapTerrainTextureClass::Apply(unsigned int stage)
{
	TextureClass::Apply(stage);
}









/******************************************************************************
						AlphaEdgeTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

/**
* AlphaEdgeTextureClass - Generates the alpha edge blending for terrain.
*
*/
AlphaEdgeTextureClass::AlphaEdgeTextureClass( int height, MipCountType mipLevelCount) :
//	TextureClass("EdgingTemplate.tga","EdgingTemplate.tga", mipLevelCount )
	TextureClass(TEXTURE_WIDTH, height, WW3D_FORMAT_A8R8G8B8, mipLevelCount )
{

}

int AlphaEdgeTextureClass::update256(WorldHeightMap *htMap)
{
	return 1;
}

int AlphaEdgeTextureClass::update(WorldHeightMap *htMap)
{
	return updateAlphaEdgeTextureDX12(htMap);
}

void AlphaEdgeTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {

		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1,   D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 1 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	} else if (stage==1) {
		// Drawing texture through the mask.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1,   D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1,   D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2,   D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_ONE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);

	}
#endif
}


/******************************************************************************
						CloudMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// CloudMapTerrainTextureClass::CloudMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture, and sets
up the "sliding" parameters for the clouds to slide over the terrain. */
//=============================================================================
//@todo - Allow adjustment of the cloud slide rate, and lose the hard coded "cloudmap.tga"
CloudMapTerrainTextureClass::CloudMapTerrainTextureClass(MipCountType mipLevelCount) :
	TextureClass("TSCloudMed.tga","TSCloudMed.tga", mipLevelCount )
{
	Get_Filter().Set_Mip_Mapping( TextureFilterClass::FILTER_TYPE_FAST );
	m_xSlidePerSecond = -0.02f;
	m_ySlidePerSecond =  1.50f * m_xSlidePerSecond;
	m_curTick = 0;
	m_xOffset = 0;
	m_yOffset = 0;

}

//=============================================================================
// CloudMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The CloudMapTerrainTextureClass may be applied by itself, or with the
LightMapTerrainTexture.  This may be applied in either single pass,
as the first texture in the pipe with LightMapTerrainTextureClass as the
second stage of the pape, or multipass.  We setup for stage 0, assuming that
we are the only texture, as LightMapTerrainTexture will adjust for multitexture
if it is applied to stage 1.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void CloudMapTerrainTextureClass::Apply(unsigned int stage)
{


	// Do the base apply.
	TextureClass::Apply(stage);
}

//=============================================================================
// CloudMapTerrainTextureClass::restore
//=============================================================================
/** Cleans up any custom settings to the texturing pipeline that may not be
understood by w3d. */
//=============================================================================
void CloudMapTerrainTextureClass::restore()
{
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);


	if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
	{
		///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
		//This method is a backdoor specific to Nvidia based cards.  It will fail on
		//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
		Int i;
		for (i=0; i<8; i++) {
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLOROP, D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_TEXCOORDINDEX, i);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture(i, nullptr);
		}
	}
}

/******************************************************************************
						ScorchTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// ScorchTextureClass::ScorchTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
/// @todo - get "EXScorch01.tga" from not hard coded location.
ScorchTextureClass::ScorchTextureClass(MipCountType mipLevelCount) :
	TextureClass("EXScorch01.tga","EXScorch01.tga", mipLevelCount )
// Hack to disable texture reduction.
//	TextureClass("EXScorch01.tga","EXScorch01.tga", mipLevelCount,WW3D_FORMAT_UNKNOWN,true,false)
{
}

//=============================================================================
// ScorchTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The ScorchTextureClass is applied by iteself, as it's mesh is a subset of the
terrain mesh.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void ScorchTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
	// Setup bilinear or trilinear filtering as specified in global data.
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
}


