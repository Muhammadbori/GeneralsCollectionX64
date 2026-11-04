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

// Direct3D 12 textures and buffers used by TextureClass, SurfaceClass, DX8VertexBufferClass and
// DX8IndexBufferClass in Direct3D 12 mode.

#pragma once

#include "WWLib/always.h"
#include "WWLib/refcount.h"
#include "WW3D2/ww3dformat.h"

#include <vector>

struct ID3D12Resource;
struct tagRECT;

// A 2D texture. The game reads and writes texels through Lock/Unlock in the texture's own
// WW3DFormat, exactly as it did with Direct3D 8 surfaces. Each level has a CPU copy while it is
// locked; Unlock converts the level to the GPU format (BC1-3 for DXT, B8G8R8A8 for everything
// else) and queues the upload. The CPU copy is kept afterwards only for textures created with
// keep_cpu_copy (procedural textures that are updated in parts, such as font pages), so a later
// partial lock still sees the rest of the level.
//
// A cpu_only texture has no GPU resource. It backs standalone SurfaceClass objects.
//
// Lock/Unlock may be called from the texture loader thread. Everything else that touches the GPU
// resource runs on the main thread.
class DX12TextureClass : public RefCountClass
{
public:
	// levels: 0 means the full mip chain down to 1x1.
	static DX12TextureClass * Create(unsigned width, unsigned height, WW3DFormat format, unsigned levels, bool keep_cpu_copy, bool cpu_only = false);

	unsigned Get_Width(unsigned level = 0) const;
	unsigned Get_Height(unsigned level = 0) const;
	unsigned Get_Level_Count() const { return LevelCount; }
	WW3DFormat Get_Format() const { return Format; }
	bool Is_CPU_Only() const { return CPUOnly; }
	// True when every level keeps its CPU copy after Unlock, so a later Lock still sees its texels.
	bool Keeps_CPU_Copy() const { return KeepCPUCopy; }

	// CPU row pitch of a level in bytes. For DXT formats this is the pitch of one row of 4x4 blocks.
	unsigned Get_Pitch(unsigned level) const;
	// CPU size of a level in bytes.
	unsigned Get_Level_Size(unsigned level) const;

	// Returns a pointer to the texel (or 4x4 block) at the top left of rect, or to the level start
	// when rect is null. A level that has no CPU copy yet is zero filled. Unlock uploads the level
	// unless it was locked read only.
	void * Lock(unsigned level, int * pitch, const tagRECT * rect = nullptr, bool read_only = false);
	void Unlock(unsigned level);

	// Main thread only, used by DX12Device while recording.
	ID3D12Resource * Peek_Resource() const { return Resource; }
	unsigned Get_SRV_Slot() const { return SRVSlot; }
	unsigned Get_State() const { return State; }			// D3D12_RESOURCE_STATES
	void Set_State(unsigned state) { State = state; }
	unsigned Get_DXGI_Format() const { return DXGIFormat; }	// DXGI_FORMAT of the GPU resource

protected:
	DX12TextureClass();
	virtual ~DX12TextureClass() override;

	bool Init(unsigned width, unsigned height, WW3DFormat format, unsigned levels, bool keep_cpu_copy, bool cpu_only);
	void Upload_Level(unsigned level);

	unsigned Width;
	unsigned Height;
	unsigned LevelCount;
	WW3DFormat Format;
	bool KeepCPUCopy;
	bool CPUOnly;
	std::vector<std::vector<unsigned char>> LevelData;
	std::vector<unsigned char> LevelLockedReadOnly;

	ID3D12Resource * Resource;
	unsigned SRVSlot;
	unsigned State;
	unsigned DXGIFormat;
};

// A vertex or index buffer.
//
// A static buffer lives in an upload heap and stays mapped, so Lock returns a pointer into GPU
// visible memory directly.
//
// A dynamic buffer is backed by per-frame memory from DX12Device::Allocate_Frame_Memory. Lock
// returns a pointer into a CPU shadow copy and Unlock copies the locked range into frame memory. A
// discard lock moves the buffer to fresh frame memory first (as with D3DLOCK_DISCARD); a non-discard
// lock writes into the current memory (D3DLOCK_NOOVERWRITE). Frame memory is only valid for the
// frame it was allocated in, so a buffer that is locked or drawn in a later frame is moved to fresh
// memory and refilled from the shadow copy; Get_GPU_Address never returns an earlier frame's memory.
//
// Main thread only.
class DX12BufferClass : public RefCountClass
{
public:
	DX12BufferClass(unsigned size, bool dynamic);

	unsigned Get_Size() const { return Size; }
	bool Is_Dynamic() const { return Dynamic; }

	void * Lock(unsigned offset, unsigned size, bool discard);
	void Unlock();

	// GPU address of the start of the buffer's current memory.
	unsigned long long Get_GPU_Address() const;

	// CPU visible contents of the whole buffer: the mapped upload memory of a static buffer (write
	// combined, so read sparingly) or the shadow copy of a dynamic one.
	const unsigned char * Peek_CPU_Data() const { return Dynamic ? Shadow : CPUAddress; }

protected:
	virtual ~DX12BufferClass() override;

	void Move_To_Frame_Memory(bool copy_shadow) const;

	unsigned Size;
	bool Dynamic;
	ID3D12Resource * Resource;
	mutable unsigned char * CPUAddress;
	mutable unsigned long long GPUAddress;
	mutable unsigned long long FrameNumber;
	unsigned char * Shadow;		// dynamic buffers only
	unsigned LockOffset;
	unsigned LockSize;
};
