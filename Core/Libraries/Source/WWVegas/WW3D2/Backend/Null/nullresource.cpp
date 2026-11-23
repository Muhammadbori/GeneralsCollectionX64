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

// TheSuperHackers @feature O158 DX12TextureClass and DX12BufferClass for the null renderer (RTS_RENDERER=null, the
// headless builds of macOS and Linux), in place of dx12resource.cpp. There is no GPU: as with Direct3D 12 when no
// device was made (the -headless runs of Windows), only CPU-only textures can be made; buffers keep their data in
// CPU memory.

#include "WW3D2/Backend/DX12/dx12resource.h"
#include "WW3D2/Backend/DX12/dx12device.h"
#include "WWDebug/wwdebug.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace
{

enum { TEXTURE_LOCK_COUNT = 32 };
std::mutex TextureLocks[TEXTURE_LOCK_COUNT];

std::mutex & Get_Texture_Lock(const void * texture)
{
	return TextureLocks[(reinterpret_cast<uintptr_t>(texture) >> 4) % TEXTURE_LOCK_COUNT];
}

bool Is_Block_Format(WW3DFormat format)
{
	switch (format) {
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

// Bytes per pixel, or bytes per 4x4 block for DXT formats. Zero for formats that cannot be stored.
unsigned Get_Element_Size(WW3DFormat format)
{
	switch (format) {
	case WW3D_FORMAT_DXT1:
		return 8;
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		return 16;
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_X8L8V8U8:
		return 4;
	case WW3D_FORMAT_R8G8B8:
		return 3;
	case WW3D_FORMAT_R5G6B5:
	case WW3D_FORMAT_X1R5G5B5:
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_A8R3G3B2:
	case WW3D_FORMAT_X4R4G4B4:
	case WW3D_FORMAT_A8P8:
	case WW3D_FORMAT_A8L8:
	case WW3D_FORMAT_U8V8:
	case WW3D_FORMAT_L6V5U5:
		return 2;
	case WW3D_FORMAT_R3G3B2:
	case WW3D_FORMAT_A8:
	case WW3D_FORMAT_P8:
	case WW3D_FORMAT_L8:
	case WW3D_FORMAT_A4L4:
		return 1;
	default:
		return 0;
	}
}

unsigned Get_Full_Level_Count(unsigned width, unsigned height)
{
	unsigned size = (width > height) ? width : height;
	unsigned levels = 1;
	while (size > 1) {
		size >>= 1;
		++levels;
	}
	return levels;
}

} // namespace

DX12TextureClass * DX12TextureClass::Create(unsigned width, unsigned height, WW3DFormat format, unsigned levels, bool keep_cpu_copy, bool cpu_only)
{
	DX12TextureClass * texture = new DX12TextureClass;
	if (!texture->Init(width, height, format, levels, keep_cpu_copy, cpu_only)) {
		texture->Release_Ref();
		return nullptr;
	}
	return texture;
}

DX12TextureClass * DX12TextureClass::Create_Render_Target(unsigned width, unsigned height, WW3DFormat format)
{
	DX12TextureClass * texture = new DX12TextureClass;
	if (!texture->Init_Target(width, height, format, USAGE_RENDER_TARGET)) {
		texture->Release_Ref();
		return nullptr;
	}
	return texture;
}

DX12TextureClass * DX12TextureClass::Create_Depth_Target(unsigned width, unsigned height)
{
	DX12TextureClass * texture = new DX12TextureClass;
	if (!texture->Init_Target(width, height, WW3D_FORMAT_UNKNOWN, USAGE_DEPTH_TARGET)) {
		texture->Release_Ref();
		return nullptr;
	}
	return texture;
}

DX12TextureClass::DX12TextureClass() :
	Width(0),
	Height(0),
	LevelCount(0),
	MinLOD(0),
	Format(WW3D_FORMAT_UNKNOWN),
	KeepCPUCopy(false),
	CPUOnly(false),
	QueuedForUpload(false),
	Usage(USAGE_TEXTURE),
	Resource(nullptr),
	SRVSlot(0),
	RTVSlot(0),
	DSVSlot(0),
	State(0),
	DXGIFormat(0),
	SRVComponentMapping(0),
	NeedsInitialClear(false)
{
}

DX12TextureClass::~DX12TextureClass()
{
}

bool DX12TextureClass::Init(unsigned width, unsigned height, WW3DFormat format, unsigned levels, bool keep_cpu_copy, bool cpu_only)
{
	if (width == 0 || height == 0 || Get_Element_Size(format) == 0) {
		return false;
	}
	const unsigned full_levels = Get_Full_Level_Count(width, height);
	if (levels == 0 || levels > full_levels) {
		levels = full_levels;
	}
	Width = width;
	Height = height;
	LevelCount = levels;
	Format = format;
	CPUOnly = cpu_only;
	KeepCPUCopy = keep_cpu_copy || cpu_only;
	LevelData.resize(LevelCount);
	LevelLockedReadOnly.assign(LevelCount, 0);
	LevelLocked.assign(LevelCount, 0);
	LevelDirty.assign(LevelCount, 0);
	// Without a device only CPU-only textures exist (as in dx12resource.cpp).
	return CPUOnly;
}

bool DX12TextureClass::Init_Target(unsigned, unsigned, WW3DFormat, UsageType)
{
	// Render and depth targets need a device.
	return false;
}

unsigned DX12TextureClass::Get_Width(unsigned level) const
{
	const unsigned w = Width >> level;
	return w ? w : 1;
}

unsigned DX12TextureClass::Get_Height(unsigned level) const
{
	const unsigned h = Height >> level;
	return h ? h : 1;
}

unsigned DX12TextureClass::Get_Pitch(unsigned level) const
{
	const unsigned w = Get_Width(level);
	if (Is_Block_Format(Format)) {
		return ((w + 3) / 4) * Get_Element_Size(Format);
	}
	return w * Get_Element_Size(Format);
}

unsigned DX12TextureClass::Get_Level_Size(unsigned level) const
{
	const unsigned h = Get_Height(level);
	const unsigned rows = Is_Block_Format(Format) ? (h + 3) / 4 : h;
	return Get_Pitch(level) * rows;
}

void * DX12TextureClass::Lock(unsigned level, int * pitch, const tagRECT * rect, bool read_only)
{
	if (level >= LevelCount || Usage == USAGE_DEPTH_TARGET) {
		return nullptr;
	}
	std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));
	std::vector<unsigned char> & data = LevelData[level];
	if (data.empty()) {
		data.assign(Get_Level_Size(level), 0);
	}
	if (!LevelLocked[level]) {
		LevelLockedReadOnly[level] = read_only ? 1 : 0;
	}
	LevelLocked[level] = 1;
	const unsigned level_pitch = Get_Pitch(level);
	if (pitch) {
		*pitch = (int)level_pitch;
	}
	unsigned char * ptr = data.data();
	if (rect) {
		const RECT * r = reinterpret_cast<const RECT *>(rect);
		const unsigned block = Is_Block_Format(Format) ? 4 : 1;
		ptr += (r->top / block) * level_pitch + (r->left / block) * Get_Element_Size(Format);
	}
	return ptr;
}

void DX12TextureClass::Unlock(unsigned level)
{
	if (level >= LevelCount || Usage == USAGE_DEPTH_TARGET) {
		return;
	}
	std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));
	LevelLockedReadOnly[level] = 0;
	LevelLocked[level] = 0;
	if (!KeepCPUCopy) {
		std::vector<unsigned char>().swap(LevelData[level]);
	}
}

bool DX12TextureClass::Upload_Dirty_Levels()
{
	QueuedForUpload = false;
	return false;
}

void DX12TextureClass::Upload_Level(unsigned)
{
}

DX12BufferClass::DX12BufferClass(unsigned size, bool dynamic) :
	Size(size),
	Dynamic(dynamic),
	Resource(nullptr),
	CPUAddress(nullptr),
	GPUAddress(0),
	FrameNumber(~0ull),
	Shadow(nullptr),
	LockOffset(0),
	LockSize(0)
{
	const unsigned alloc_size = (Size + 3) & ~3u;
	CPUAddress = new unsigned char[alloc_size ? alloc_size : 4];
	memset(CPUAddress, 0, alloc_size ? alloc_size : 4);
	if (Dynamic) {
		Shadow = new unsigned char[alloc_size ? alloc_size : 4];
		memset(Shadow, 0, alloc_size ? alloc_size : 4);
	}
}

DX12BufferClass::~DX12BufferClass()
{
	delete [] CPUAddress;
	CPUAddress = nullptr;
	delete [] Shadow;
	Shadow = nullptr;
}

void * DX12BufferClass::Lock(unsigned offset, unsigned size, bool)
{
	if (Dynamic) {
		LockOffset = offset;
		LockSize = size;
		return Shadow + offset;
	}
	return CPUAddress + offset;
}

void DX12BufferClass::Unlock()
{
	if (Dynamic && LockSize) {
		memcpy(CPUAddress + LockOffset, Shadow + LockOffset, LockSize);
	}
	LockOffset = 0;
	LockSize = 0;
}

unsigned long long DX12BufferClass::Get_GPU_Address() const
{
	return 0;
}

void DX12BufferClass::Move_To_Frame_Memory(bool) const
{
}
