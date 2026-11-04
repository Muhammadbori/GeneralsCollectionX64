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

#include "WW3D2/Backend/DX12/dx12resource.h"
#include "WW3D2/Backend/DX12/dx12device.h"
#include "WWDebug/wwdebug.h"

#include <d3d12.h>
#include <cstdint>
#include <cstring>
#include <mutex>

// Implemented in dx12device.cpp: writes a 2D texture SRV into the given slot of the shader visible
// CBV/SRV heap.
extern void DX12Device_Write_SRV(unsigned slot, ID3D12Resource * resource, unsigned dxgi_format, unsigned mip_levels);

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

DXGI_FORMAT Get_GPU_Format(WW3DFormat format)
{
	switch (format) {
	case WW3D_FORMAT_DXT1:
		return DXGI_FORMAT_BC1_UNORM;
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
		return DXGI_FORMAT_BC2_UNORM;
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		return DXGI_FORMAT_BC3_UNORM;
	default:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
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

// Direct3D 12 requires the top level of a block compressed texture to be a multiple of 4. The
// rounded resource has the same number of blocks per level as the requested size.
void Build_Texture_Desc(D3D12_RESOURCE_DESC & desc, unsigned width, unsigned height, unsigned levels, WW3DFormat format)
{
	if (Is_Block_Format(format)) {
		width = (width + 3) & ~3u;
		height = (height + 3) & ~3u;
	}

	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Alignment = 0;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = (UINT16)levels;
	desc.Format = Get_GPU_Format(format);
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
}

void Build_Buffer_Desc(D3D12_RESOURCE_DESC & desc, unsigned size)
{
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
}

ID3D12Resource * Create_Upload_Buffer(ID3D12Device * device, unsigned long long size)
{
	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc;
	Build_Buffer_Desc(desc, (unsigned)size);

	ID3D12Resource * resource = nullptr;
	HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void **)&resource);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX12: failed to create upload buffer of %u bytes (0x%08X)", (unsigned)size, (unsigned)hr));
		return nullptr;
	}
	return resource;
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

DX12TextureClass::DX12TextureClass() :
	Width(0),
	Height(0),
	LevelCount(0),
	Format(WW3D_FORMAT_UNKNOWN),
	KeepCPUCopy(false),
	CPUOnly(false),
	Resource(nullptr),
	SRVSlot(0),
	State(D3D12_RESOURCE_STATE_COMMON),
	DXGIFormat(DXGI_FORMAT_UNKNOWN)
{
}

DX12TextureClass::~DX12TextureClass()
{
	if (Resource) {
		if (DX12Device::Is_Active()) {
			DX12Device::Release_When_Idle(Resource);
		} else {
			Resource->Release();
		}
		Resource = nullptr;
	}
	if (SRVSlot != 0) {
		if (DX12Device::Is_Active()) {
			DX12Device::Free_SRV_Slot(SRVSlot);
		}
		SRVSlot = 0;
	}
}

bool DX12TextureClass::Init(unsigned width, unsigned height, WW3DFormat format, unsigned levels, bool keep_cpu_copy, bool cpu_only)
{
	if (width == 0 || height == 0 || Get_Element_Size(format) == 0) {
		WWDEBUG_SAY(("DX12: unsupported texture %ux%u format %d", width, height, (int)format));
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
	DXGIFormat = Get_GPU_Format(format);
	LevelData.resize(LevelCount);
	LevelLockedReadOnly.assign(LevelCount, 0);

	if (CPUOnly) {
		return true;
	}

	ID3D12Device * device = DX12Device::Get_D3D12_Device();
	if (device == nullptr) {
		return false;
	}

	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc;
	Build_Texture_Desc(desc, Width, Height, LevelCount, Format);

	HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void **)&Resource);
	if (FAILED(hr)) {
		WWDEBUG_SAY(("DX12: failed to create texture %ux%u format %d levels %u (0x%08X)", Width, Height, (int)Format, LevelCount, (unsigned)hr));
		Resource = nullptr;
		return false;
	}
	State = D3D12_RESOURCE_STATE_COPY_DEST;

	// Slot 0 holds the white texture, so 0 means no slot was available.
	SRVSlot = DX12Device::Allocate_SRV_Slot();
	if (SRVSlot == 0) {
		WWDEBUG_SAY(("DX12: out of SRV slots"));
		return false;
	}
	DX12Device_Write_SRV(SRVSlot, Resource, DXGIFormat, LevelCount);
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
	if (level >= LevelCount) {
		return nullptr;
	}

	std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));

	std::vector<unsigned char> & data = LevelData[level];
	if (data.empty()) {
		data.assign(Get_Level_Size(level), 0);
	}
	LevelLockedReadOnly[level] = read_only ? 1 : 0;

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
	if (level >= LevelCount) {
		return;
	}

	std::lock_guard<std::mutex> guard(Get_Texture_Lock(this));

	if (!LevelLockedReadOnly[level] && !CPUOnly && !LevelData[level].empty()) {
		Upload_Level(level);
	}
	LevelLockedReadOnly[level] = 0;

	if (!KeepCPUCopy) {
		std::vector<unsigned char>().swap(LevelData[level]);
	}
}

// Converts the CPU copy of a level into the GPU layout and queues the copy. Called with the
// texture lock held.
void DX12TextureClass::Upload_Level(unsigned level)
{
	ID3D12Device * device = DX12Device::Get_D3D12_Device();
	if (device == nullptr || Resource == nullptr) {
		return;
	}

	D3D12_RESOURCE_DESC desc;
	Build_Texture_Desc(desc, Width, Height, LevelCount, Format);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT num_rows = 0;
	UINT64 row_size = 0;
	UINT64 total_bytes = 0;
	device->GetCopyableFootprints(&desc, level, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);
	if (total_bytes == 0 || total_bytes == ~0ull) {
		return;
	}

	ID3D12Resource * upload = Create_Upload_Buffer(device, total_bytes);
	if (upload == nullptr) {
		return;
	}

	unsigned char * mapped = nullptr;
	D3D12_RANGE read_range = { 0, 0 };
	if (FAILED(upload->Map(0, &read_range, (void **)&mapped)) || mapped == nullptr) {
		WWDEBUG_SAY(("DX12: failed to map texture upload buffer"));
		upload->Release();
		return;
	}

	const std::vector<unsigned char> & data = LevelData[level];
	const unsigned src_pitch = Get_Pitch(level);
	const unsigned src_rows = Get_Level_Size(level) / src_pitch;
	const unsigned rows = (src_rows < num_rows) ? src_rows : (unsigned)num_rows;
	unsigned char * dst = mapped + footprint.Offset;
	const unsigned dst_pitch = footprint.Footprint.RowPitch;

	// Rows the CPU copy does not cover (only possible with the rounded DXT size) stay zero.
	if (rows < num_rows || (Is_Block_Format(Format) && src_pitch < row_size)) {
		memset(dst, 0, (size_t)dst_pitch * num_rows);
	}

	if (Is_Block_Format(Format)) {
		const unsigned copy = (src_pitch < row_size) ? src_pitch : (unsigned)row_size;
		for (unsigned y = 0; y < rows; ++y) {
			memcpy(dst + (size_t)y * dst_pitch, data.data() + (size_t)y * src_pitch, copy);
		}
	} else {
		const unsigned width = Get_Width(level);
		for (unsigned y = 0; y < rows; ++y) {
			Convert_Row(reinterpret_cast<unsigned *>(dst + (size_t)y * dst_pitch), data.data() + (size_t)y * src_pitch, width, Format);
		}
	}

	D3D12_RANGE written = { 0, (SIZE_T)total_bytes };
	upload->Unmap(0, &written);

	// Takes ownership of the upload buffer.
	DX12Device::Queue_Texture_Upload(this, level, upload);
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
	LockSize(0)
{
	const unsigned alloc_size = (Size + 3) & ~3u;

	if (Dynamic) {
		Shadow = new unsigned char[alloc_size ? alloc_size : 4];
		memset(Shadow, 0, alloc_size ? alloc_size : 4);
		return;
	}

	ID3D12Device * device = DX12Device::Get_D3D12_Device();
	if (device) {
		Resource = Create_Upload_Buffer(device, alloc_size ? alloc_size : 4);
	}
	if (Resource) {
		D3D12_RANGE read_range = { 0, 0 };
		if (FAILED(Resource->Map(0, &read_range, (void **)&CPUAddress)) || CPUAddress == nullptr) {
			WWDEBUG_SAY(("DX12: failed to map static buffer of %u bytes", Size));
			Resource->Release();
			Resource = nullptr;
			CPUAddress = nullptr;
		} else {
			GPUAddress = Resource->GetGPUVirtualAddress();
		}
	}

	// Keep locks working without a GPU buffer; draws see a zero address.
	if (Resource == nullptr) {
		CPUAddress = new unsigned char[alloc_size ? alloc_size : 4];
	}
}

DX12BufferClass::~DX12BufferClass()
{
	if (Resource) {
		// Upload heap resources may stay mapped until they are released.
		if (DX12Device::Is_Active()) {
			DX12Device::Release_When_Idle(Resource);
		} else {
			Resource->Release();
		}
		Resource = nullptr;
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
	if (Dynamic) {
		if (CPUAddress && LockSize) {
			memcpy(CPUAddress + LockOffset, Shadow + LockOffset, LockSize);
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
		}
	} else {
		WWDEBUG_SAY(("DX12: failed to allocate %u bytes of frame memory", Size));
		CPUAddress = nullptr;
		GPUAddress = 0;
	}
	FrameNumber = DX12Device::Get_Frame_Number();
}
