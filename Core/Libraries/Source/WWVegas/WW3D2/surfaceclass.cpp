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
 *                     $Archive:: /Commando/Code/ww3d2/surfaceclass.cpp                       $*
 *                                                                                             *
 *              Original Author:: Nathaniel Hoffman                                            *
 *                                                                                             *
 *                      $Author:: Greg_h2                                                     $*
 *                                                                                             *
 *                     $Modtime:: 8/30/01 2:01p                                               $*
 *                                                                                             *
 *                    $Revision:: 25                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   SurfaceClass::Clear -- Clears a surface to 0                                              *
 *   SurfaceClass::Copy -- Copies a region from one surface to another of the same format      *
 *   SurfaceClass::FindBBAlpha -- Finds the bounding box of non zero pixels in the region (x0, *
 *   SurfaceClass::Is_Transparent_Column -- Tests to see if the column is transparent or not   *
 *   SurfaceClass::Copy -- Copies from a byte array to the surface                             *
 *   SurfaceClass::CreateCopy -- Creates a byte array copy of the surface                      *
 *   SurfaceClass::DrawHLine -- draws a horizontal line                                        *
 *   SurfaceClass::DrawPixel -- draws a pixel                                                  *
 *   SurfaceClass::Copy -- Copies a block of system ram to the surface                         *
 *   SurfaceClass::Hue_Shift -- changes the hue of the surface                                 *
 *   SurfaceClass::Is_Monochrome -- Checks if surface is monochrome or not                     *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "surfaceclass.h"
#include "formconv.h"
#include "dx8wrapper.h"
#include "WWMath/vector2i.h"
#include "colorspace.h"
#include "WWLib/bound.h"
#include "bitmaphandler.h"
#include "textureloader.h"
#include "Backend/DX12/dx12device.h"
#include "Backend/DX12/dx12resource.h"

void Convert_Pixel(Vector3 &rgb, const SurfaceClass::SurfaceDescription &sd, const unsigned char * pixel)
{
	const float scale=1/255.0f;
	switch (sd.Format)
	{
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R8G8B8:
		{
			rgb.X=pixel[2]; // R
			rgb.Y=pixel[1]; // G
			rgb.Z=pixel[0]; // B
		}
		break;
	case WW3D_FORMAT_A4R4G4B4:
		{
			unsigned short tmp;
			tmp=*(unsigned short*)&pixel[0];
			rgb.X=((tmp&0x0f00)>>4);   // R
			rgb.Y=((tmp&0x00f0));		// G
			rgb.Z=((tmp&0x000f)<<4);	// B
		}
		break;
	case WW3D_FORMAT_A1R5G5B5:
		{
			unsigned short tmp;
			tmp=*(unsigned short*)&pixel[0];
			rgb.X=(tmp>>7)&0xf8; // R
			rgb.Y=(tmp>>2)&0xf8; // G
			rgb.Z=(tmp<<3)&0xf8; // B
		}
		break;
	case WW3D_FORMAT_R5G6B5:
		{
			unsigned short tmp;
			tmp=*(unsigned short*)&pixel[0];
			rgb.X=(tmp>>8)&0xf8;
			rgb.Y=(tmp>>3)&0xfc;
			rgb.Z=(tmp<<3)&0xf8;
		}
		break;

	default:
		// TODO: Implement other pixel formats
		WWASSERT(0);
	}
	rgb*=scale;
}

// Note: This function must never overwrite the original alpha
void Convert_Pixel(unsigned char * pixel,const SurfaceClass::SurfaceDescription &sd, const Vector3 &rgb)
{
	unsigned char r,g,b;
	r=(unsigned char) (rgb.X*255.0f);
	g=(unsigned char) (rgb.Y*255.0f);
	b=(unsigned char) (rgb.Z*255.0f);
	switch (sd.Format)
	{
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R8G8B8:
		pixel[0]=b;
		pixel[1]=g;
		pixel[2]=r;
		break;
	case WW3D_FORMAT_A4R4G4B4:
		{
			unsigned short tmp;
			tmp=*(unsigned short*)&pixel[0];
			tmp&=0xF000;
			tmp|=(r&0xF0) << 4;
			tmp|=(g&0xF0);
			tmp|=(b&0xF0) >> 4;
			*(unsigned short*)&pixel[0]=tmp;
		}
		break;
	case WW3D_FORMAT_A1R5G5B5:
		{
			unsigned short tmp;
			tmp=*(unsigned short*)&pixel[0];
			tmp&=0x8000;
			tmp|=(r&0xF8) << 7;
			tmp|=(g&0xF8) << 2;
			tmp|=(b&0xF8) >> 3;
			*(unsigned short*)&pixel[0]=tmp;
		}
		break;
	case WW3D_FORMAT_R5G6B5:
		{
			unsigned short tmp;
			tmp=(r&0xf8) << 8;
			tmp|=(g&0xfc) << 3;
			tmp|=(b&0xf8) >> 3;
			*(unsigned short*)&pixel[0]=tmp;
		}
		break;
	default:
		// TODO: Implement other pixel formats
		WWASSERT(0);
	}
}

static bool Is_DXT_Format(WW3DFormat format)
{
	switch (format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		return true;
	default:
		return false;
	}
}

// Formats BitmapHandlerClass can read and write without a palette.
static bool Is_Convertible_Format(WW3DFormat format)
{
	switch (format)
	{
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R8G8B8:
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_R5G6B5:
	case WW3D_FORMAT_R3G3B2:
	case WW3D_FORMAT_L8:
	case WW3D_FORMAT_A8:
		return true;
	default:
		return false;
	}
}

// BitmapHandlerClass::Write_B8G8R8A8 truncates 8 bit channels to the 4, 5 and 6 bit channels of
// 16 bit formats, and its reads expand them with a plain shift, so a filtered texel would come out
// darker than D3DX makes it. Adding half a step first rounds each channel to the nearest value.
static void Round_To_Format_Precision(unsigned char *bgra, WW3DFormat format)
{
	unsigned half[4] = { 0, 0, 0, 0 };
	switch (format)
	{
	case WW3D_FORMAT_A4R4G4B4:
		half[0] = half[1] = half[2] = half[3] = 8;
		break;
	case WW3D_FORMAT_A1R5G5B5:
		half[0] = half[1] = half[2] = 4;
		break;
	case WW3D_FORMAT_R5G6B5:
		half[0] = half[2] = 4;
		half[1] = 2;
		break;
	default:
		return;
	}
	for (int i = 0; i < 4; ++i)
	{
		const unsigned value = bgra[i] + half[i];
		bgra[i] = (unsigned char)((value > 255) ? 255 : value);
	}
}

// CPU copy of a rectangle between two locked surfaces, used in Direct3D 12 mode in place of
// CopyRects and D3DXLoadSurfaceFromSurface. Both pointers point at the top left texel of their
// surface. Same size and format copies rows (DXT formats copy whole 4x4 blocks); anything else
// converts each texel and box filters when the sizes differ.
static void Copy_Texels
(
	unsigned char *dst, int dst_pitch, WW3DFormat dst_format,
	unsigned dstx, unsigned dsty, unsigned dstwidth, unsigned dstheight,
	const unsigned char *src, int src_pitch, WW3DFormat src_format,
	unsigned srcx, unsigned srcy, unsigned srcwidth, unsigned srcheight
)
{
	if (dst == nullptr || src == nullptr || dstwidth == 0 || dstheight == 0 || srcwidth == 0 || srcheight == 0)
	{
		return;
	}

	if (dst_format == src_format && dstwidth == srcwidth && dstheight == srcheight)
	{
		if (Is_DXT_Format(src_format))
		{
			const unsigned block_size = (src_format == WW3D_FORMAT_DXT1) ? 8 : 16;
			const unsigned row_size = ((srcwidth + 3) / 4) * block_size;
			const unsigned row_count = (srcheight + 3) / 4;
			for (unsigned row = 0; row < row_count; ++row)
			{
				memcpy
				(
					dst + (dsty / 4 + row) * dst_pitch + (dstx / 4) * block_size,
					src + (srcy / 4 + row) * src_pitch + (srcx / 4) * block_size,
					row_size
				);
			}
		}
		else
		{
			const unsigned bpp = ::Get_Bytes_Per_Pixel(src_format);
			for (unsigned row = 0; row < srcheight; ++row)
			{
				memcpy(dst + (dsty + row) * dst_pitch + dstx * bpp, src + (srcy + row) * src_pitch + srcx * bpp, srcwidth * bpp);
			}
		}
		return;
	}

	if (!Is_Convertible_Format(src_format) || !Is_Convertible_Format(dst_format))
	{
		WWDEBUG_SAY(("SurfaceClass: cannot convert surface format %d to %d in Direct3D 12 mode", (int)src_format, (int)dst_format));
		return;
	}

	const unsigned src_bpp = ::Get_Bytes_Per_Pixel(src_format);
	const unsigned dst_bpp = ::Get_Bytes_Per_Pixel(dst_format);

	for (unsigned y = 0; y < dstheight; ++y)
	{
		unsigned sy0 = srcy + (y * srcheight) / dstheight;
		unsigned sy1 = srcy + ((y + 1) * srcheight) / dstheight;
		if (sy1 <= sy0) sy1 = sy0 + 1;

		unsigned char *dst_row = dst + (dsty + y) * dst_pitch;

		for (unsigned x = 0; x < dstwidth; ++x)
		{
			unsigned sx0 = srcx + (x * srcwidth) / dstwidth;
			unsigned sx1 = srcx + ((x + 1) * srcwidth) / dstwidth;
			if (sx1 <= sx0) sx1 = sx0 + 1;

			unsigned sum[4] = { 0, 0, 0, 0 };
			unsigned count = 0;
			for (unsigned sy = sy0; sy < sy1; ++sy)
			{
				for (unsigned sx = sx0; sx < sx1; ++sx)
				{
					unsigned char bgra[4];
					BitmapHandlerClass::Read_B8G8R8A8(bgra, src + sy * src_pitch + sx * src_bpp, src_format, nullptr, 0);
					sum[0] += bgra[0];
					sum[1] += bgra[1];
					sum[2] += bgra[2];
					sum[3] += bgra[3];
					++count;
				}
			}

			unsigned char bgra[4];
			for (int i = 0; i < 4; ++i)
			{
				bgra[i] = (unsigned char)((sum[i] + count / 2) / count);
			}
			// BitmapHandlerClass writes the blue channel into A8 surfaces.
			if (dst_format == WW3D_FORMAT_A8)
			{
				dst_row[dstx + x] = bgra[3];
				continue;
			}
			// BitmapHandlerClass makes any non zero alpha opaque in A1R5G5B5. A filtered texel rounds
			// its averaged alpha at one half instead, as D3DX box filtering does.
			if (dst_format == WW3D_FORMAT_A1R5G5B5 && count > 1)
			{
				bgra[3] = (bgra[3] >= 128) ? 255 : 0;
			}
			Round_To_Format_Precision(bgra, dst_format);
			BitmapHandlerClass::Write_B8G8R8A8(dst_row + (dstx + x) * dst_bpp, dst_format, bgra);
		}
	}
}

/*************************************************************************
**                             SurfaceClass
*************************************************************************/
SurfaceClass::SurfaceClass(unsigned width, unsigned height, WW3DFormat format):
	DX12Texture(nullptr),
	DX12Level(0),
	SurfaceFormat(format)
{
	WWASSERT(width);
	WWASSERT(height);
	DX12Texture = DX12TextureClass::Create(width, height, format, 1, true, true);
}

SurfaceClass::SurfaceClass(const char *filename):
	DX12Texture(nullptr),
	DX12Level(0)
{
	StringClass filename_string(filename,true);
	DX12Texture = TextureLoader::Load_DX12_Surface_Immediate(filename_string, WW3D_FORMAT_UNKNOWN, true);
	SurfaceDescription desc;
	Get_Description(desc);
	SurfaceFormat=desc.Format;
}

SurfaceClass::SurfaceClass(DX12TextureClass *texture, unsigned level) :
	DX12Texture (nullptr),
	DX12Level (level)
{
	WWASSERT(texture != nullptr);
	WWASSERT(level < texture->Get_Level_Count());
	REF_PTR_SET(DX12Texture, texture);
	SurfaceFormat=texture->Get_Format();
}

SurfaceClass::~SurfaceClass()
{
	REF_PTR_RELEASE(DX12Texture);
}

void *SurfaceClass::Lock_Rect(int *pitch, const tagRECT *rect, bool read_only)
{
	if (DX12Texture == nullptr)
	{
		*pitch = 0;
		return nullptr;
	}
	return DX12Texture->Lock(DX12Level, pitch, rect, read_only);
}

void SurfaceClass::Unlock_Rect()
{
	if (DX12Texture != nullptr)
	{
		DX12Texture->Unlock(DX12Level);
	}
}

DX12TextureClass *SurfaceClass::Create_DX12_Texture(unsigned levels)
{
	SurfaceDescription sd;
	Get_Description(sd);
	if (DX12Texture == nullptr || sd.Width == 0 || sd.Height == 0)
	{
		return nullptr;
	}

	// Mipmaps cannot be generated on the CPU for compressed surfaces.
	if (Is_DXT_Format(sd.Format) || !Is_Convertible_Format(sd.Format))
	{
		if (levels != 1)
		{
			static bool logged = false;
			if (!logged)
			{
				logged = true;
				WWDEBUG_SAY(("SurfaceClass: no mipmaps for textures made from format %d surfaces in Direct3D 12 mode", (int)sd.Format));
			}
		}
		levels = 1;
	}

	DX12TextureClass *texture = DX12TextureClass::Create(sd.Width, sd.Height, sd.Format, levels, true);
	if (texture == nullptr)
	{
		return nullptr;
	}

	int src_pitch = 0;
	const unsigned char *src = static_cast<const unsigned char *>(Lock_Rect(&src_pitch, nullptr, true));
	int dst_pitch = 0;
	unsigned char *dst = static_cast<unsigned char *>(texture->Lock(0, &dst_pitch));
	Copy_Texels(dst, dst_pitch, sd.Format, 0, 0, sd.Width, sd.Height, src, src_pitch, sd.Format, 0, 0, sd.Width, sd.Height);
	Unlock_Rect();

	// Box filter each level from the previous one, which stays locked until it has been used.
	for (unsigned level = 1; level < texture->Get_Level_Count(); ++level)
	{
		int mip_pitch = 0;
		unsigned char *mip = static_cast<unsigned char *>(texture->Lock(level, &mip_pitch));
		Copy_Texels
		(
			mip, mip_pitch, sd.Format, 0, 0, texture->Get_Width(level), texture->Get_Height(level),
			dst, dst_pitch, sd.Format, 0, 0, texture->Get_Width(level - 1), texture->Get_Height(level - 1)
		);
		texture->Unlock(level - 1);
		dst = mip;
		dst_pitch = mip_pitch;
	}
	texture->Unlock(texture->Get_Level_Count() - 1);

	return texture;
}

void SurfaceClass::Generate_DX12_Mipmaps(DX12TextureClass *texture, unsigned first_level)
{
	if (texture == nullptr || first_level == 0 || first_level >= texture->Get_Level_Count())
	{
		return;
	}

	const WW3DFormat format = texture->Get_Format();
	if (Is_DXT_Format(format) || !Is_Convertible_Format(format))
	{
		WWDEBUG_SAY(("SurfaceClass: cannot generate mipmaps for format %d in Direct3D 12 mode", (int)format));
		return;
	}

	// The source level is read back with a new lock, so its texels must still be on the CPU. A
	// texture without a CPU copy dropped them at its last unlock, and filtering the zero filled
	// level would turn every generated mip black.
	if (!texture->Keeps_CPU_Copy())
	{
		WWDEBUG_SAY(("SurfaceClass: cannot generate mipmaps for a texture without a CPU copy in Direct3D 12 mode"));
		return;
	}

	// The source level is only read, so unlocking it does not upload it again. Each written level
	// stays locked until the next one has been filtered from it; its unlock then uploads it once.
	int src_pitch = 0;
	unsigned char *src = static_cast<unsigned char *>(texture->Lock(first_level - 1, &src_pitch, nullptr, true));
	for (unsigned level = first_level; level < texture->Get_Level_Count(); ++level)
	{
		int mip_pitch = 0;
		unsigned char *mip = static_cast<unsigned char *>(texture->Lock(level, &mip_pitch));
		Copy_Texels
		(
			mip, mip_pitch, format, 0, 0, texture->Get_Width(level), texture->Get_Height(level),
			src, src_pitch, format, 0, 0, texture->Get_Width(level - 1), texture->Get_Height(level - 1)
		);
		texture->Unlock(level - 1);
		src = mip;
		src_pitch = mip_pitch;
	}
	texture->Unlock(texture->Get_Level_Count() - 1);
}

void SurfaceClass::Copy_DX12(const RECT &dest, const SurfaceClass *source, const RECT &src)
{
	if (dest.right<=dest.left || dest.bottom<=dest.top || src.right<=src.left || src.bottom<=src.top)
	{
		return;
	}

	SurfaceClass *other=const_cast<SurfaceClass*>(source);
	SurfaceDescription sd,osd;
	Get_Description(sd);
	other->Get_Description(osd);

	int dst_pitch=0;
	unsigned char *dst=static_cast<unsigned char *>(Lock_Rect(&dst_pitch,nullptr,false));
	if (dst == nullptr)
	{
		return;
	}

	// A texture level can be locked only once, so a copy within the same level reads from a snapshot.
	const bool same_level=(other->DX12Texture == DX12Texture && other->DX12Level == DX12Level);
	std::vector<unsigned char> snapshot;
	const unsigned char *src_bits=nullptr;
	int src_pitch=0;
	if (same_level)
	{
		snapshot.assign(dst,dst+DX12Texture->Get_Level_Size(DX12Level));
		src_bits=snapshot.data();
		src_pitch=dst_pitch;
	}
	else
	{
		src_bits=static_cast<const unsigned char *>(other->Lock_Rect(&src_pitch,nullptr,true));
	}

	Copy_Texels
	(
		dst,dst_pitch,sd.Format,dest.left,dest.top,dest.right-dest.left,dest.bottom-dest.top,
		src_bits,src_pitch,osd.Format,src.left,src.top,src.right-src.left,src.bottom-src.top
	);

	if (!same_level && src_bits != nullptr)
	{
		other->Unlock_Rect();
	}
	Unlock_Rect();
}

void SurfaceClass::Get_Description(SurfaceDescription &surface_desc)
{
	if (DX12Texture == nullptr)
	{
		surface_desc.Format = WW3D_FORMAT_UNKNOWN;
		surface_desc.Width = 0;
		surface_desc.Height = 0;
		return;
	}
	surface_desc.Format = DX12Texture->Get_Format();
	surface_desc.Width = DX12Texture->Get_Width(DX12Level);
	surface_desc.Height = DX12Texture->Get_Height(DX12Level);
}

unsigned int SurfaceClass::Get_Bytes_Per_Pixel()
{
	SurfaceDescription surfaceDesc;
	Get_Description(surfaceDesc);
	return ::Get_Bytes_Per_Pixel(surfaceDesc.Format);
}

SurfaceClass::LockedSurfacePtr SurfaceClass::Lock(int *pitch)
{
	return static_cast<LockedSurfacePtr>(Lock_Rect(pitch, nullptr, false));
}

SurfaceClass::LockedSurfacePtr SurfaceClass::Lock(int *pitch, const Vector2i &min, const Vector2i &max)
{
	RECT rect;
	rect.left = min.I;
	rect.top = min.J;
	rect.right = max.I;
	rect.bottom = max.J;
	return static_cast<LockedSurfacePtr>(Lock_Rect(pitch, &rect, false));
}

void SurfaceClass::Unlock()
{
	Unlock_Rect();
}

/***********************************************************************************************
 * SurfaceClass::Clear -- Clears a surface to 0                                                *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/13/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::Clear()
{
	SurfaceDescription sd;
	Get_Description(sd);

	// size of each pixel in bytes
	unsigned int size=::Get_Bytes_Per_Pixel(sd.Format);

	int pitch;
	unsigned char *mem=(unsigned char *) Lock_Rect(&pitch,nullptr,false);
	if (mem == nullptr) return;
	unsigned int i;

	for (i=0; i<sd.Height; i++)
	{
		memset(mem,0,size*sd.Width);
		mem+=pitch;
	}

	Unlock_Rect();
}


/***********************************************************************************************
 * SurfaceClass::Copy -- Copies from a byte array to the surface                               *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/15/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::Copy(const unsigned char *other)
{
	SurfaceDescription sd;
	Get_Description(sd);

	// size of each pixel in bytes
	unsigned int size=::Get_Bytes_Per_Pixel(sd.Format);

	int pitch;
	unsigned char *mem=(unsigned char *) Lock_Rect(&pitch,nullptr,false);
	if (mem == nullptr) return;
	unsigned int i;

	for (i=0; i<sd.Height; i++)
	{
		memcpy(mem,&other[i*sd.Width*size],size*sd.Width);
		mem+=pitch;
	}

	Unlock_Rect();
}


/***********************************************************************************************
 * SurfaceClass::Copy -- Copies a block of system ram to the surface                           *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/2/2001   hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::Copy(const Vector2i &min, const Vector2i &max, const unsigned char *other)
{
	SurfaceDescription sd;
	Get_Description(sd);

	// size of each pixel in bytes
	unsigned int size=::Get_Bytes_Per_Pixel(sd.Format);

	RECT rect;
	rect.left=min.I;
	rect.right=max.I;
	rect.top=min.J;
	rect.bottom=max.J;
	int pitch;
	unsigned char *mem=(unsigned char *) Lock_Rect(&pitch,&rect,false);
	if (mem == nullptr) return;
	int i;
	int dx=max.I-min.I;

	for (i=min.J; i<max.J; i++)
	{
		memcpy(mem,&other[(i*sd.Width+min.I)*size],size*dx);
		mem+=pitch;
	}

	Unlock_Rect();
}


/***********************************************************************************************
 * SurfaceClass::CreateCopy -- Creates a byte array copy of the surface                        *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/16/2001  hy : Created.                                                                  *
 *=============================================================================================*/
unsigned char *SurfaceClass::CreateCopy(int *width,int *height,int*size,bool flip)
{
	SurfaceDescription sd;
	Get_Description(sd);

	// size of each pixel in bytes
	unsigned int mysize=::Get_Bytes_Per_Pixel(sd.Format);

	*width=sd.Width;
	*height=sd.Height;
	*size=mysize;

	unsigned char *other=W3DNEWARRAY unsigned char [sd.Height*sd.Width*mysize];

	int pitch;
	unsigned char *mem=(unsigned char *) Lock_Rect(&pitch,nullptr,true);
	if (mem == nullptr)
	{
		memset(other,0,sd.Height*sd.Width*mysize);
		return other;
	}
	unsigned int i;

	for (i=0; i<sd.Height; i++)
	{
		if (flip)
		{
			memcpy(&other[(sd.Height-i-1)*sd.Width*mysize],mem,mysize*sd.Width);
		} else
		{
			memcpy(&other[i*sd.Width*mysize],mem,mysize*sd.Width);
		}
		mem+=pitch;
	}

	Unlock_Rect();

	return other;
}


/***********************************************************************************************
 * SurfaceClass::Copy -- Copies a region from one surface to another                           *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/13/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::Copy(
	unsigned int dstx, unsigned int dsty,
	unsigned int srcx, unsigned int srcy,
	unsigned int width, unsigned int height,
	const SurfaceClass *other)
{
	WWASSERT(other);
	WWASSERT(width);
	WWASSERT(height);

	SurfaceDescription sd,osd;
	Get_Description(sd);
	const_cast <SurfaceClass*>(other)->Get_Description(osd);

	RECT src;
	src.left=srcx;
	src.right=srcx+width;
	src.top=srcy;
	src.bottom=srcy+height;

	if (src.right>int(osd.Width)) src.right=int(osd.Width);
	if (src.bottom>int(osd.Height)) src.bottom=int(osd.Height);

	RECT dest;
	dest.left=dstx;
	dest.right=dstx+width;
	dest.top=dsty;
	dest.bottom=dsty+height;

	if (dest.right>int(sd.Width)) dest.right=int(sd.Width);
	if (dest.bottom>int(sd.Height)) dest.bottom=int(sd.Height);

	// Same size and format copies the source rect as is (CopyRects), anything else fits it
	// into the clipped destination rect (D3DXLoadSurfaceFromSurface).
	if (sd.Format==osd.Format && sd.Width==osd.Width && sd.Height==osd.Height)
	{
		dest.right=MIN(dest.left+(src.right-src.left),int(sd.Width));
		dest.bottom=MIN(dest.top+(src.bottom-src.top),int(sd.Height));
		src.right=src.left+(dest.right-dest.left);
		src.bottom=src.top+(dest.bottom-dest.top);
	}
	Copy_DX12(dest,other,src);
}

/***********************************************************************************************
 * SurfaceClass::Copy -- Copies a region from one surface to another                           *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/13/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::Stretch_Copy(
	unsigned int dstx, unsigned int dsty, unsigned int dstwidth, unsigned int dstheight,
	unsigned int srcx, unsigned int srcy, unsigned int srcwidth, unsigned int srcheight,
	const SurfaceClass *other)
{
	WWASSERT(other);

	SurfaceDescription sd,osd;
	Get_Description(sd);
	const_cast <SurfaceClass*>(other)->Get_Description(osd);

	RECT src;
	src.left=srcx;
	src.right=srcx+srcwidth;
	src.top=srcy;
	src.bottom=srcy+srcheight;

	RECT dest;
	dest.left=dstx;
	dest.right=dstx+dstwidth;
	dest.top=dsty;
	dest.bottom=dsty+dstheight;

	if (src.right>int(osd.Width)) src.right=int(osd.Width);
	if (src.bottom>int(osd.Height)) src.bottom=int(osd.Height);
	if (dest.right>int(sd.Width)) dest.right=int(sd.Width);
	if (dest.bottom>int(sd.Height)) dest.bottom=int(sd.Height);
	Copy_DX12(dest,other,src);
}

/***********************************************************************************************
 * SurfaceClass::FindBB -- Finds the bounding box of non zero pixels in the region             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/13/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::FindBB(Vector2i *min,Vector2i*max)
{
	SurfaceDescription sd;
	Get_Description(sd);

	WWASSERT(Has_Alpha(sd.Format));

	int alphabits=Alpha_Bits(sd.Format);
	int mask=0;
	switch (alphabits)
	{
	case 1: mask=1;
		break;
	case 4: mask=0xf;
		break;
	case 8: mask=0xff;
		break;
	}

	D3DLOCKED_RECT lock_rect;
	::ZeroMemory(&lock_rect, sizeof(D3DLOCKED_RECT));
	RECT rect;
	::ZeroMemory(&rect, sizeof(RECT));

	rect.bottom=max->J;
	rect.top=min->J;
	rect.left=min->I;
	rect.right=max->I;

	lock_rect.pBits=Lock_Rect(&lock_rect.Pitch,&rect,true);
	if (lock_rect.pBits == nullptr) return;

	int x,y;
	unsigned int size=::Get_Bytes_Per_Pixel(sd.Format);
	Vector2i realmin=*max;
	Vector2i realmax=*min;

	// the assumption here is that whenever a pixel has alpha it's in the MSB
	for (y = min->J; y < max->J; y++) {
		for (x = min->I; x < max->I; x++) {

			// HY - this is not endian safe
			unsigned char *alpha=(unsigned char*) ((uintptr_t)lock_rect.pBits+(y-min->J)*lock_rect.Pitch+(x-min->I)*size);
			unsigned char myalpha=alpha[size-1];
			myalpha=(myalpha>>(8-alphabits)) & mask;
			if (myalpha) {
				realmin.I = MIN(realmin.I, x);
				realmax.I = MAX(realmax.I, x);
				realmin.J = MIN(realmin.J, y);
				realmax.J = MAX(realmax.J, y);
			}
		}
	}

	Unlock_Rect();

	*max=realmax;
	*min=realmin;
}


/***********************************************************************************************
 * SurfaceClass::Is_Transparent_Column -- Tests to see if the column is transparent or not     *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/13/2001  hy : Created.                                                                  *
 *=============================================================================================*/
bool SurfaceClass::Is_Transparent_Column(unsigned int column)
{
	SurfaceDescription sd;
	Get_Description(sd);

	WWASSERT(column<sd.Width);
	WWASSERT(Has_Alpha(sd.Format));

	int alphabits=Alpha_Bits(sd.Format);
	int mask=0;
	switch (alphabits)
	{
	case 1: mask=1;
		break;
	case 4: mask=0xf;
		break;
	case 8: mask=0xff;
		break;
	}

	unsigned int size=::Get_Bytes_Per_Pixel(sd.Format);

	D3DLOCKED_RECT lock_rect;
	::ZeroMemory(&lock_rect, sizeof(D3DLOCKED_RECT));
	RECT rect;
	::ZeroMemory(&rect, sizeof(RECT));

	rect.bottom=sd.Height;
	rect.top=0;
	rect.left=column;
	rect.right=column+1;

	lock_rect.pBits=Lock_Rect(&lock_rect.Pitch,&rect,true);
	if (lock_rect.pBits == nullptr) return true;

	int y;

	// the assumption here is that whenever a pixel has alpha it's in the MSB
	for (y = 0; y < (int) sd.Height; y++)
	{
		// HY - this is not endian safe
		unsigned char *alpha=(unsigned char*) ((uintptr_t)lock_rect.pBits+y*lock_rect.Pitch);
		unsigned char myalpha=alpha[size-1];
		myalpha=(myalpha>>(8-alphabits)) & mask;
		if (myalpha) {
			Unlock_Rect();
			return false;
		}
	}

	Unlock_Rect();
	return true;
}

/***********************************************************************************************
 * SurfaceClass::Get_Pixel -- Returns the pixel's RGB valus to the caller                      *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/13/2001  hy : Created.                                                                  *
 *   1/10/2025  TheSuperHackers : Added bits and pitch to argument list for better performance *
 *=============================================================================================*/
void SurfaceClass::Get_Pixel(Vector3 &rgb, int x, int y, LockedSurfacePtr pBits, int pitch)
{
	SurfaceDescription sd;
	Get_Description(sd);

	unsigned int bytesPerPixel = ::Get_Bytes_Per_Pixel(sd.Format);
	unsigned char* dst = static_cast<unsigned char *>(pBits) + y * pitch + x * bytesPerPixel;
	Convert_Pixel(rgb,sd,dst);
}

/***********************************************************************************************
 * SurfaceClass::DrawPixel -- draws a pixel                                                    *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   1/10/2025  TheSuperHackers : Added bits and pitch to argument list for better performance *
 *=============================================================================================*/
void SurfaceClass::Draw_Pixel(const unsigned int x, const unsigned int y, unsigned int color,
	unsigned int bytesPerPixel, LockedSurfacePtr pBits, int pitch)
{
	unsigned char* dst = static_cast<unsigned char*>(pBits) + y * pitch + x * bytesPerPixel;
	memcpy(dst, &color, bytesPerPixel);
}



/***********************************************************************************************
 * SurfaceClass::DrawHLine -- draws a horizontal line                                          *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/9/2001   hy : Created.                                                                  *
 *   1/10/2025  TheSuperHackers : Added bits and pitch to argument list for better performance *
 *=============================================================================================*/
void SurfaceClass::Draw_H_Line(const unsigned int y, const unsigned int x1, const unsigned int x2,
	unsigned int color, unsigned int bytesPerPixel, LockedSurfacePtr pBits, int pitch)
{
	unsigned char* row = static_cast<unsigned char*>(pBits) + y * pitch;

	for (unsigned int x = x1; x <= x2; ++x)
	{
		unsigned char* dst = row + x * bytesPerPixel;
		memcpy(dst, &color, bytesPerPixel);
	}
}


/***********************************************************************************************
 * SurfaceClass::Is_Monochrome -- Checks if surface is monochrome or not                       *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/5/2001   hy : Created.                                                                  *
 *=============================================================================================*/
bool SurfaceClass::Is_Monochrome()
{
	unsigned int x,y;
	SurfaceDescription sd;
	Get_Description(sd);
	bool is_compressed = false;

	switch (sd.Format)
	{
		// these formats are always monochrome
		case WW3D_FORMAT_A8L8:
		case WW3D_FORMAT_A8:
		case WW3D_FORMAT_L8:
		case WW3D_FORMAT_A4L4:
			return true;
		break;
		// these formats cannot be determined to be monochrome or not
		case WW3D_FORMAT_UNKNOWN:
		case WW3D_FORMAT_A8P8:
		case WW3D_FORMAT_P8:
		case WW3D_FORMAT_U8V8:		// Bumpmap
		case WW3D_FORMAT_L6V5U5:	// Bumpmap
		case WW3D_FORMAT_X8L8V8U8:	// Bumpmap
			return false;
		break;
		// these formats need decompression first
		case WW3D_FORMAT_DXT1:
		case WW3D_FORMAT_DXT2:
		case WW3D_FORMAT_DXT3:
		case WW3D_FORMAT_DXT4:
		case WW3D_FORMAT_DXT5:
			is_compressed = true;
		break;
	}

	// if it's in some compressed texture format, be sure to decompress first
	if (is_compressed) {
		WW3DFormat new_format = Get_Valid_Texture_Format(sd.Format, false);
		SurfaceClass *new_surf = NEW_REF( SurfaceClass, (sd.Width, sd.Height, new_format) );
		new_surf->Copy(0, 0, 0, 0, sd.Width, sd.Height, this);
		bool result = new_surf->Is_Monochrome();
		REF_PTR_RELEASE(new_surf);
		return result;
	}

	int pitch,size;

	size=::Get_Bytes_Per_Pixel(sd.Format);
	unsigned char *bits=(unsigned char*) Lock(&pitch);
	if (bits == nullptr) return false;

	Vector3 rgb;
	bool mono=true;

	for (y=0; y<sd.Height; y++)
	{
		for (x=0; x<sd.Width; x++)
		{
			Convert_Pixel(rgb,sd,&bits[x*size]);
			mono&=(rgb.X==rgb.Y);
			mono&=(rgb.X==rgb.Z);
			mono&=(rgb.Z==rgb.Y);
			if (!mono)
			{
				Unlock();
				return false;
			}
		}
		bits+=pitch;
	}

	Unlock();

	return true;
}

/***********************************************************************************************
 * SurfaceClass::Hue_Shift -- changes the hue of the surface                                   *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/3/2001   hy : Created.                                                                  *
 *=============================================================================================*/
void SurfaceClass::Hue_Shift(const Vector3 &hsv_shift)
{
	unsigned int x,y;
	SurfaceDescription sd;
	Get_Description(sd);
	int pitch,size;

	size=::Get_Bytes_Per_Pixel(sd.Format);
	unsigned char *bits=(unsigned char*) Lock(&pitch);
	if (bits == nullptr) return;

	Vector3 rgb;

	for (y=0; y<sd.Height; y++)
	{
		for (x=0; x<sd.Width; x++)
		{
			Convert_Pixel(rgb,sd,&bits[x*size]);
			Recolor(rgb,hsv_shift);
			rgb.X=Bound(rgb.X,0.0f,1.0f);
			rgb.Y=Bound(rgb.Y,0.0f,1.0f);
			rgb.Z=Bound(rgb.Z,0.0f,1.0f);
			Convert_Pixel(&bits[x*size],sd,rgb);
		}
		bits+=pitch;
	}

	Unlock();
}
