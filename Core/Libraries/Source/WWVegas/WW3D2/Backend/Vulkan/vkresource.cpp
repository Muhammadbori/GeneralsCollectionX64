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

// TheSuperHackers @feature O138 DX12TextureClass and DX12BufferClass for the Vulkan renderer (RTS_RENDERER=vulkan),
// in place of dx12resource.cpp. The CPU side (levels, locks, dirty levels, the conversion to the GPU format) is the
// one of dx12resource.cpp; the GPU side is a VkImage or VkBuffer from vkdevice.cpp. See vkinternal.h.

#include "WW3D2/Backend/DX12/dx12resource.h"
#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/Backend/Vulkan/vkinternal.h"
#include "WWDebug/wwdebug.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace
{

// Lock/Unlock may run on the loader thread. The header has no room for a lock per texture, so
// textures share a small set of striped locks keyed on their address. Each lock is held only
// inside Lock() and Unlock(), never between them.
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

VkFormat Get_GPU_Format(WW3DFormat format)
{
	switch (format) {
	case WW3D_FORMAT_DXT1:
		return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
		return VK_FORMAT_BC2_UNORM_BLOCK;
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		return VK_FORMAT_BC3_UNORM_BLOCK;
	default:
		return VK_FORMAT_B8G8R8A8_UNORM;
	}
}

unsigned Expand2(unsigned v) { return v * 85; }
unsigned Expand3(unsigned v) { return (v << 5) | (v << 2) | (v >> 1); }
unsigned Expand4(unsigned v) { return v * 17; }
unsigned Expand5(unsigned v) { return (v << 3) | (v >> 2); }
unsigned Expand6(unsigned v) { return (v << 2) | (v >> 4); }

unsigned Make_BGRA(unsigned a, unsigned r, unsigned g, unsigned b)
{
	return (a << 24) | (r << 16) | (g << 8) | b;
}

// Converts one row of count texels to B8G8R8A8 (little endian 0xAARRGGBB).
void Convert_Row(unsigned * dst, const unsigned char * src, unsigned count, WW3DFormat format)
{
	switch (format) {
	case WW3D_FORMAT_A8R8G8B8:
		memcpy(dst, src, count * 4);
		break;
	case WW3D_FORMAT_X8R8G8B8:
		for (unsigned i = 0; i < count; ++i, src += 4) {
			dst[i] = Make_BGRA(255, src[2], src[1], src[0]);
		}
		break;
	case WW3D_FORMAT_R8G8B8:
		for (unsigned i = 0; i < count; ++i, src += 3) {
			dst[i] = Make_BGRA(255, src[2], src[1], src[0]);
		}
		break;
	case WW3D_FORMAT_R5G6B5:
		for (unsigned i = 0; i < count; ++i, src += 2) {
			const unsigned v = src[0] | (src[1] << 8);
			dst[i] = Make_BGRA(255, Expand5((v >> 11) & 0x1F), Expand6((v >> 5) & 0x3F), Expand5(v & 0x1F));
		}
		break;
	case WW3D_FORMAT_X1R5G5B5:
	case WW3D_FORMAT_A1R5G5B5:
		for (unsigned i = 0; i < count; ++i, src += 2) {
			const unsigned v = src[0] | (src[1] << 8);
			const unsigned a = (format == WW3D_FORMAT_X1R5G5B5 || (v & 0x8000)) ? 255 : 0;
			dst[i] = Make_BGRA(a, Expand5((v >> 10) & 0x1F), Expand5((v >> 5) & 0x1F), Expand5(v & 0x1F));
		}
		break;
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_X4R4G4B4:
		for (unsigned i = 0; i < count; ++i, src += 2) {
			const unsigned v = src[0] | (src[1] << 8);
			const unsigned a = (format == WW3D_FORMAT_X4R4G4B4) ? 255 : Expand4((v >> 12) & 0xF);
			dst[i] = Make_BGRA(a, Expand4((v >> 8) & 0xF), Expand4((v >> 4) & 0xF), Expand4(v & 0xF));
		}
		break;
	case WW3D_FORMAT_R3G3B2:
		for (unsigned i = 0; i < count; ++i, ++src) {
			const unsigned v = src[0];
			dst[i] = Make_BGRA(255, Expand3((v >> 5) & 0x7), Expand3((v >> 2) & 0x7), Expand2(v & 0x3));
		}
		break;
	case WW3D_FORMAT_A8R3G3B2:
		for (unsigned i = 0; i < count; ++i, src += 2) {
			const unsigned v = src[0];
			dst[i] = Make_BGRA(src[1], Expand3((v >> 5) & 0x7), Expand3((v >> 2) & 0x7), Expand2(v & 0x3));
		}
		break;
	case WW3D_FORMAT_A8:
		for (unsigned i = 0; i < count; ++i, ++src) {
			dst[i] = Make_BGRA(src[0], 0, 0, 0);
		}
		break;
	case WW3D_FORMAT_L8:
		for (unsigned i = 0; i < count; ++i, ++src) {
			dst[i] = Make_BGRA(255, src[0], src[0], src[0]);
		}
		break;
	case WW3D_FORMAT_A8L8:
		for (unsigned i = 0; i < count; ++i, src += 2) {
			dst[i] = Make_BGRA(src[1], src[0], src[0], src[0]);
		}
		break;
	case WW3D_FORMAT_A4L4:
		for (unsigned i = 0; i < count; ++i, ++src) {
			const unsigned l = Expand4(src[0] & 0xF);
			dst[i] = Make_BGRA(Expand4(src[0] >> 4), l, l, l);
		}
		break;
	default:
		// Palettized and bump map formats are not supported.
		memset(dst, 0, count * 4);
		break;
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

// ----------------------------------------------------------------------------
// DX12TextureClass
// ----------------------------------------------------------------------------

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
	NeedsInitialClear(false),
	VKImage(nullptr)
{
}

DX12TextureClass::~DX12TextureClass()
{
	if (VKImage) {
		VKDevice_Release_Image(VKImage);
		VKImage = nullptr;
	}
}

bool DX12TextureClass::Init(unsigned width, unsigned height, WW3DFormat format, unsigned levels, bool keep_cpu_copy, bool cpu_only)
{
	if (width == 0 || height == 0 || Get_Element_Size(format) == 0) {
		WWDEBUG_SAY(("Vulkan: unsupported texture %ux%u format %d", width, height, (int)format));
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
	// A cpu_only texture has nowhere else to keep its texels.
	KeepCPUCopy = keep_cpu_copy || cpu_only;
	LevelData.resize(LevelCount);
	LevelLockedReadOnly.assign(LevelCount, 0);
	LevelLocked.assign(LevelCount, 0);
	LevelDirty.assign(LevelCount, 0);

	if (CPUOnly) {
		return true;
	}
	if (VKDevice_Get() == VK_NULL_HANDLE) {
		return false;
	}
	DX12StatsScope stats_scope(DX12Device_Counters.TextureCreateTicks);

	// As with Direct3D 12, the top level of a block compressed image is rounded to a multiple of 4; it has the
	// same number of blocks per level as the requested size.
	unsigned image_width = width;
	unsigned image_height = height;
	if (Is_Block_Format(format)) {
		image_width = (width + 3) & ~3u;
		image_height = (height + 3) & ~3u;
	}
	VKImage = VKDevice_Create_Image(image_width, image_height, LevelCount, Get_GPU_Format(format),
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, false);
	if (VKImage == nullptr) {
		WWDEBUG_SAY(("Vulkan: failed to create texture %ux%u format %d levels %u", Width, Height, (int)Format, LevelCount));
		return false;
	}
	VKDevice_Queue_Initial_Layout(this);
	return true;
}

// Main thread only.
bool DX12TextureClass::Init_Target(unsigned width, unsigned height, WW3DFormat format, UsageType usage)
{
	const bool depth = (usage == USAGE_DEPTH_TARGET);
	if (width == 0 || height == 0 || width > 16384 || height > 16384) {
		WWDEBUG_SAY(("Vulkan: unsupported %s target size %ux%u", depth ? "depth" : "render", width, height));
		return false;
	}
	if (!depth && (Is_Block_Format(format) || Get_Element_Size(format) == 0)) {
		WWDEBUG_SAY(("Vulkan: unsupported render target format %d", (int)format));
		return false;
	}
	if (VKDevice_Get() == VK_NULL_HANDLE) {
		return false;
	}
	DX12StatsScope stats_scope(DX12Device_Counters.TextureCreateTicks);

	Width = width;
	Height = height;
	LevelCount = 1;
	Format = depth ? WW3D_FORMAT_UNKNOWN : format;
	Usage = usage;
	CPUOnly = false;
	KeepCPUCopy = false;
	LevelData.resize(LevelCount);
	LevelLockedReadOnly.assign(LevelCount, 0);
	LevelLocked.assign(LevelCount, 0);
	LevelDirty.assign(LevelCount, 0);

	if (depth) {
		// VK_FORMAT_UNDEFINED selects the device's depth stencil format.
		VKImage = VKDevice_Create_Image(width, height, 1, VK_FORMAT_UNDEFINED,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, false);
	} else {
		// Direct3D 8 reads alpha as 1 from formats without alpha.
		VKImage = VKDevice_Create_Image(width, height, 1, VK_FORMAT_B8G8R8A8_UNORM,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, !::Has_Alpha(format));
	}
	if (VKImage == nullptr) {
		WWDEBUG_SAY(("Vulkan: failed to create %s target %ux%u", depth ? "depth" : "render", width, height));
		return false;
	}
	// Cleared (black with zero alpha, depth 1 and stencil 0) by the device when it is first used.
	NeedsInitialClear = true;
	return true;
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
	WWASSERT(level < LevelCount);
	if (level >= LevelCount || Usage == USAGE_DEPTH_TARGET) {
		return nullptr;
	}

	std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));

	std::vector<unsigned char> & data = LevelData[level];
	if (data.empty()) {
		data.assign(Get_Level_Size(level), 0);
	}
	// A second lock of a locked level (Direct3D 8 allows none) keeps the first one's mode.
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
	WWASSERT(level < LevelCount);
	if (level >= LevelCount || Usage == USAGE_DEPTH_TARGET) {
		return;
	}

	bool queue = false;
	{
		std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));

		if (!LevelLockedReadOnly[level] && !CPUOnly && !LevelData[level].empty()) {
			if (DX12Device_Is_Main_Thread()) {
				// Uploaded with the other dirty levels when the device next records uploads.
				LevelDirty[level] = 1;
				if (!QueuedForUpload) {
					QueuedForUpload = true;
					queue = true;
				}
			} else {
				Upload_Level(level);
			}
		}
		LevelLockedReadOnly[level] = 0;
		LevelLocked[level] = 0;

		// A dirty level keeps its CPU copy until it is uploaded.
		if (!KeepCPUCopy && !LevelDirty[level]) {
			std::vector<unsigned char>().swap(LevelData[level]);
		}
	}
	if (queue) {
		DX12Device::Queue_Dirty_Texture(this);
	}
}

bool DX12TextureClass::Upload_Dirty_Levels()
{
	std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));

	bool still_dirty = false;
	for (unsigned level = 0; level < LevelCount; ++level) {
		if (!LevelDirty[level]) {
			continue;
		}
		if (LevelLocked[level]) {
			// Uploaded after its Unlock instead.
			still_dirty = true;
			continue;
		}
		if (!LevelData[level].empty()) {
			Upload_Level(level);
		}
		LevelDirty[level] = 0;
		if (!KeepCPUCopy) {
			std::vector<unsigned char>().swap(LevelData[level]);
		}
	}
	QueuedForUpload = still_dirty;
	return still_dirty;
}

// Converts the CPU copy of a level into the image's layout and queues the copy. Called with the texture
// lock held.
void DX12TextureClass::Upload_Level(unsigned level)
{
	if (VKImage == nullptr || level >= VKImage->Levels) {
		return;
	}
	DX12StatsScope stats_scope(DX12Device_Counters.TextureUploadTicks);

	unsigned level_width = VKImage->Width >> level;
	unsigned level_height = VKImage->Height >> level;
	if (level_width == 0)
		level_width = 1;
	if (level_height == 0)
		level_height = 1;

	const std::vector<unsigned char> & data = LevelData[level];
	const unsigned src_pitch = Get_Pitch(level);
	const unsigned src_rows = Get_Level_Size(level) / src_pitch;

	std::vector<unsigned char> upload;
	if (Is_Block_Format(Format)) {
		const unsigned blocks_x = (level_width + 3) / 4;
		const unsigned blocks_y = (level_height + 3) / 4;
		const unsigned dst_pitch = blocks_x * Get_Element_Size(Format);
		// Rows and blocks the CPU copy does not cover (only possible with the rounded size) stay zero.
		upload.assign((size_t)dst_pitch * blocks_y, 0);
		const unsigned rows = (src_rows < blocks_y) ? src_rows : blocks_y;
		const unsigned copy = (src_pitch < dst_pitch) ? src_pitch : dst_pitch;
		for (unsigned y = 0; y < rows; ++y) {
			memcpy(upload.data() + (size_t)y * dst_pitch, data.data() + (size_t)y * src_pitch, copy);
		}
	} else {
		const unsigned dst_pitch = level_width * 4;
		upload.assign((size_t)dst_pitch * level_height, 0);
		const unsigned rows = (src_rows < level_height) ? src_rows : level_height;
		const unsigned width = (Get_Width(level) < level_width) ? Get_Width(level) : level_width;
		for (unsigned y = 0; y < rows; ++y) {
			Convert_Row(reinterpret_cast<unsigned *>(upload.data() + (size_t)y * dst_pitch), data.data() + (size_t)y * src_pitch, width, Format);
		}
	}
	VKDevice_Queue_Texture_Upload(this, level, upload);
}

// ----------------------------------------------------------------------------
// DX12BufferClass
// ----------------------------------------------------------------------------

DX12BufferClass::DX12BufferClass(unsigned size, bool dynamic) :
	Size(size),
	Dynamic(dynamic),
	Resource(nullptr),
	CPUAddress(nullptr),
	GPUAddress(0),
	FrameNumber(~0ull),
	Shadow(nullptr),
	LockOffset(0),
	LockSize(0),
	VKBuffer(nullptr)
{
	const unsigned alloc_size = (Size + 3) & ~3u;

	if (Dynamic) {
		Shadow = new unsigned char[alloc_size ? alloc_size : 4];
		memset(Shadow, 0, alloc_size ? alloc_size : 4);
		return;
	}

	void * cpu = nullptr;
	unsigned long long address = 0;
	if (VKDevice_Get() != VK_NULL_HANDLE) {
		VKBuffer = VKDevice_Create_Host_Buffer(alloc_size ? alloc_size : 4, &cpu, &address);
	}
	if (VKBuffer) {
		CPUAddress = static_cast<unsigned char *>(cpu);
		GPUAddress = address;
	} else {
		// Keep locks working without a GPU buffer; draws see a zero address.
		CPUAddress = new unsigned char[alloc_size ? alloc_size : 4];
	}
}

DX12BufferClass::~DX12BufferClass()
{
	if (VKBuffer) {
		VKDevice_Release_Buffer(VKBuffer);
		VKBuffer = nullptr;
	} else if (!Dynamic) {
		delete [] CPUAddress;
	}
	CPUAddress = nullptr;
	delete [] Shadow;
	Shadow = nullptr;
}

void * DX12BufferClass::Lock(unsigned offset, unsigned size, bool discard)
{
	WWASSERT(offset + size <= Size);
	DX12StatsScope stats_scope(DX12Device_Counters.BufferTicks);

	if (Dynamic) {
		if (discard) {
			Move_To_Frame_Memory(false);
		} else if (CPUAddress == nullptr || FrameNumber != DX12Device::Get_Frame_Number()) {
			// The old frame memory may already be reused; keep the rest of the buffer intact.
			Move_To_Frame_Memory(true);
		}
		LockOffset = offset;
		LockSize = size;
		return Shadow + offset;
	}

	if (CPUAddress == nullptr) {
		return nullptr;
	}
	return CPUAddress + offset;
}

void DX12BufferClass::Unlock()
{
	DX12StatsScope stats_scope(DX12Device_Counters.BufferTicks);
	if (Dynamic) {
		if (CPUAddress && LockSize) {
			memcpy(CPUAddress + LockOffset, Shadow + LockOffset, LockSize);
			++DX12Device_Counters.BufferLockCopies;
			DX12Device_Counters.BufferLockCopyBytes += LockSize;
		}
		LockOffset = 0;
		LockSize = 0;
	}
}

unsigned long long DX12BufferClass::Get_GPU_Address() const
{
	if (Dynamic && (CPUAddress == nullptr || FrameNumber != DX12Device::Get_Frame_Number())) {
		// Drawn in a later frame without being locked again: the memory it pointed at belongs to
		// an earlier frame and may already be reused or released.
		DX12StatsScope stats_scope(DX12Device_Counters.BufferTicks);
		Move_To_Frame_Memory(true);
	}
	return GPUAddress;
}

void DX12BufferClass::Move_To_Frame_Memory(bool copy_shadow) const
{
	void * cpu = nullptr;
	unsigned long long gpu = 0;
	const unsigned alloc_size = (Size + 3) & ~3u;
	if (DX12Device::Allocate_Frame_Memory(alloc_size ? alloc_size : 4, &cpu, &gpu)) {
		CPUAddress = static_cast<unsigned char *>(cpu);
		GPUAddress = gpu;
		if (copy_shadow && Shadow) {
			memcpy(CPUAddress, Shadow, alloc_size ? alloc_size : 4);
			++DX12Device_Counters.BufferFullCopies;
			DX12Device_Counters.BufferFullCopyBytes += alloc_size ? alloc_size : 4;
		}
	} else {
		WWDEBUG_SAY(("Vulkan: failed to allocate %u bytes of frame memory", Size));
		CPUAddress = nullptr;
		GPUAddress = 0;
	}
	FrameNumber = DX12Device::Get_Frame_Number();
}
