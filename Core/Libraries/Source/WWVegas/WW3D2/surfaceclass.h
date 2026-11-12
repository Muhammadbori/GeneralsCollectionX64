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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /VSS_Sync/ww3d2/surfaceclass.h                              $*
 *                                                                                             *
 *              Original Author:: Nathaniel Hoffman                                            *
 *                                                                                             *
 *                      $Author:: Vss_sync                                                    $*
 *                                                                                             *
 *                     $Modtime:: 8/29/01 9:32p                                               $*
 *                                                                                             *
 *                    $Revision:: 17                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "ww3dformat.h"

struct tagRECT;
class Vector2i;
class Vector3;
class DX12TextureClass;

/*************************************************************************
**                             SurfaceClass
**
** This is our surface class, which wraps one level of a Direct3D 12 texture.
** In Direct3D 12 mode it wraps one level of a DX12TextureClass instead
** (a CPU only texture for standalone surfaces).
**
** Hector Yee 2/12/01 - added in fills, blits etc for font3d class
**
*************************************************************************/
class SurfaceClass : public RefCountClass
{
	W3DMPO_CODE(SurfaceClass)
	public:
		typedef void *LockedSurfacePtr;

		struct SurfaceDescription {
			WW3DFormat		Format;	// Surface format
			unsigned int	Width;	// Surface width in pixels
			unsigned int	Height;	// Surface height in pixels
		};

		// Create surface with desired height, width and format.
		SurfaceClass(unsigned width, unsigned height, WW3DFormat format);

		// Create surface from a file.
		SurfaceClass(const char *filename);

		// Create the surface from a D3D pointer

		// Create the surface from one level of a Direct3D 12 texture (keeps a reference to it)
		SurfaceClass(DX12TextureClass *texture, unsigned level);

		virtual ~SurfaceClass() override;

		// Get surface description
		void Get_Description(SurfaceDescription &surface_desc);

		// Get the bytes per pixel count
		unsigned int Get_Bytes_Per_Pixel();

		// Lock / unlock the surface
		LockedSurfacePtr Lock(int *pitch);
		LockedSurfacePtr Lock(int *pitch, const Vector2i &min, const Vector2i &max);
		void Unlock();

		// HY -- The following functions are support functions for font3d
		// zaps the surface memory to zero
		void Clear();

		// copies the contents of one surface to another
		void Copy(
			unsigned int dstx, unsigned int dsty,
			unsigned int srcx, unsigned int srcy,
			unsigned int width, unsigned int height,
			const SurfaceClass *other);

		// support for copying from a byte array
		void Copy(const unsigned char *other);

		// support for copying from a byte array
		void Copy(const Vector2i &min, const Vector2i &max, const unsigned char *other);

		// copies the contents of one surface to another, stretches
		void Stretch_Copy(
			unsigned int dstx, unsigned int dsty, unsigned int dstwidth, unsigned int dstheight,
			unsigned int srcx, unsigned int srcy, unsigned int srcwidth, unsigned int srcheight,
			const SurfaceClass *source);

		// finds the bounding box of non-zero pixels, used in font3d
		void FindBB(Vector2i *min,Vector2i*max);

		// tests a column to see if the alpha is nonzero, used in font3d
		bool Is_Transparent_Column(unsigned int column);

		// makes a copy of the surface into a byte array
		unsigned char *CreateCopy(int *width,int *height,int*size,bool flip=false);

			// For use by TextureClass:
		DX12TextureClass *Peek_DX12_Texture() { return DX12Texture; }
		unsigned Get_DX12_Level() const { return DX12Level; }


		// draws a horizontal line
		void Draw_H_Line(const unsigned int y, const unsigned int x1, const unsigned int x2,
			unsigned int color, unsigned int bytesPerPixel, LockedSurfacePtr pBits, int pitch);

		// draws a pixel
		void Draw_Pixel(const unsigned int x, const unsigned int y, unsigned int color,
			unsigned int bytesPerPixel, LockedSurfacePtr pBits, int pitch);

		// get pixel function
		void Get_Pixel(Vector3 &rgb, int x, int y, LockedSurfacePtr pBits, int pitch);

		void Hue_Shift(const Vector3 &hsv_shift);

		bool Is_Monochrome();

		WW3DFormat Get_Surface_Format() const { return SurfaceFormat; }

		// Direct3D 12 replacement for D3DXFilterTexture(texture, nullptr, 0, D3DX_FILTER_BOX): rebuilds
		// levels first_level and below on the CPU, box filtering each level from the one above it, and
		// uploads them. The level above first_level is only read, with a new lock, so it must still have
		// its CPU data: does nothing for textures created without keep_cpu_copy, for null textures and
		// for formats the CPU cannot convert (DXT, paletted).
		static void Generate_DX12_Mipmaps(DX12TextureClass *texture, unsigned first_level = 1);

	private:

		// Lock helpers shared by the Direct3D 8 and Direct3D 12 paths. A null rect locks the whole surface.
		void *Lock_Rect(int *pitch, const tagRECT *rect, bool read_only);
		void Unlock_Rect();

		// Creates a Direct3D 12 texture holding a copy of this surface, with mipmaps generated from it.
		DX12TextureClass *Create_DX12_Texture(unsigned levels);

		// Direct3D 12 version of Copy/Stretch_Copy. The rects are already clipped to both surfaces.
		void Copy_DX12(const tagRECT &dest, const SurfaceClass *source, const tagRECT &src);

		// Direct3D surface object

		// Direct3D 12 texture and level (owned reference)
		DX12TextureClass *DX12Texture;
		unsigned DX12Level;

		WW3DFormat SurfaceFormat;
	friend class TextureClass;
};
