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

// Direct3D 12 device, swap chain, frame synchronization and fixed-function emulation.
//
// Frames: two frame slots, each with a command allocator, a fence value and a linear upload heap
// for per-frame data (constants and dynamic vertex/index buffers). A slot is reused once the GPU
// has finished the frame that last used it. Resources that the GPU may still read are released
// through Release_When_Idle, which waits for the fence of the frame being recorded.
//
// Draws: DX8Wrapper describes each draw with DX12DrawDesc. The vertex format picks a vertex
// shader variant and an input layout, the render states pick a pipeline state, and the texture
// stage states go to the pixel shader as constants.

#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/Backend/DX12/dx12resource.h"
#include "WW3D2/Backend/DX12/dx12shaders.h"
#include "WWDebug/wwdebug.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <stdio.h>
#include <string.h>

using Microsoft::WRL::ComPtr;

bool DX12Device::Requested = false;
ID3D12Device * DX12Device::Device = nullptr;
unsigned DX12Device::Width = 0;
unsigned DX12Device::Height = 0;
unsigned long long DX12Device::FrameNumber = 0;

namespace
{

enum
{
	FRAME_COUNT = 2,
	BACK_BUFFER_COUNT = 3,
	SRV_HEAP_SIZE = 16384,
	SAMPLER_HEAP_SIZE = 2048,
	FRAME_MEMORY_SIZE = 32 * 1024 * 1024,
	FRAME_MEMORY_EXTRA_PAGE_SIZE = 4 * 1024 * 1024,
	FRAME_MEMORY_MAX_SIZE = 256 * 1024 * 1024,
	MAX_PENDING_UPLOAD_BYTES = 64 * 1024 * 1024,
	CONSTANT_ALIGNMENT = 256,
	MAX_INPUT_ELEMENTS = 16,
};

// Direct3D 8 FVF bits. Spelled out here so this file does not depend on d3d8.h.
enum
{
	FVF_POSITION_MASK = 0x00e,
	FVF_XYZ = 0x002,
	FVF_XYZRHW = 0x004,
	FVF_XYZB1 = 0x006,
	FVF_NORMAL = 0x010,
	FVF_PSIZE = 0x020,
	FVF_DIFFUSE = 0x040,
	FVF_SPECULAR = 0x080,
	FVF_TEXCOUNT_MASK = 0xf00,
	FVF_TEXCOUNT_SHIFT = 8,
	FVF_MAX_TEXCOORDS = 8,
};

// D3DPRIMITIVETYPE
enum
{
	PRIM_POINTLIST = 1,
	PRIM_LINELIST = 2,
	PRIM_LINESTRIP = 3,
	PRIM_TRIANGLELIST = 4,
	PRIM_TRIANGLESTRIP = 5,
	PRIM_TRIANGLEFAN = 6,
};

// D3DBLEND values that have no Direct3D 12 equivalent.
enum
{
	BLEND_BOTHSRCALPHA = 12,
	BLEND_BOTHINVSRCALPHA = 13,
};

enum
{
	TOP_DISABLE = 1,
	CMP_ALWAYS = 8,
};

// DX12DrawDesc::DepthBias is D3DRS_ZBIAS (0..16, larger is closer). One step moves depth by about
// 0.000005 (the usual Direct3D 8 to 9 conversion), which is -84 units of a 24-bit depth buffer.
const int ZBIAS_TO_DEPTH_BIAS = -84;

// Must match VSConstants and PSConstants in dx12shaders.h.
struct DX12VSConstants
{
	float WorldViewProjection[16];
	float ScreenToClip[4];
	float DepthToClip[4];
	float PixelOffset[4];
	unsigned TexCoordIndex[4];
	float MaterialDiffuse[4];
};

struct DX12PSConstants
{
	unsigned ColorStage[DX12_MAX_STAGES][4];	// op, arg1, arg2, arg0
	unsigned AlphaStage[DX12_MAX_STAGES][4];
	unsigned Flags[4];							// alpha func (0 = off), alpha ref, specular enable
	float TextureFactor[4];
};

static_assert(sizeof(DX12VSConstants) == 144, "DX12VSConstants must match the HLSL constant buffer");
static_assert(sizeof(DX12PSConstants) == 96, "DX12PSConstants must match the HLSL constant buffer");

struct FramePage
{
	FramePage() : CPU(nullptr), GPU(0), Size(0), Offset(0) {}

	ComPtr<ID3D12Resource> Resource;
	unsigned char * CPU;
	UINT64 GPU;
	unsigned Size;
	unsigned Offset;
};

struct FrameSlot
{
	FrameSlot() : FenceValue(0) {}

	ComPtr<ID3D12CommandAllocator> Allocator;
	UINT64 FenceValue;
	FramePage Memory;
	std::vector<FramePage> Extra;
};

struct PendingUpload
{
	DX12TextureClass * Texture;
	unsigned Level;
	ID3D12Resource * Upload;
	UINT64 Bytes;
};

struct DeferredRelease
{
	UINT64 FenceValue;
	ID3D12Resource * Resource;
};

struct DeferredSlot
{
	UINT64 FenceValue;
	unsigned Slot;
};

// All members are 32-bit so the key has no padding and can be compared with memcmp.
struct PSOKey
{
	unsigned FVF;
	unsigned TopologyType;
	unsigned AlphaBlendEnable;
	unsigned SrcBlend;
	unsigned DestBlend;
	unsigned BlendOp;
	unsigned ColorWriteMask;
	unsigned ZEnable;
	unsigned ZWriteEnable;
	unsigned ZFunc;
	unsigned CullMode;
	unsigned FillMode;
	int DepthBias;
};

struct PSOKeyLess
{
	bool operator()(const PSOKey & a, const PSOKey & b) const { return memcmp(&a, &b, sizeof(PSOKey)) < 0; }
};

struct FVFLayout
{
	D3D12_INPUT_ELEMENT_DESC Elements[MAX_INPUT_ELEMENTS];
	unsigned ElementCount;
	unsigned Size;
	unsigned VSKey;
};

ComPtr<IDXGIFactory4> Factory;
ComPtr<IDXGIAdapter1> Adapter;
DXGI_ADAPTER_DESC1 AdapterDesc;
char AdapterName[128];
std::vector<DX12Device::DisplayMode> DisplayModes;
DWORD MainThreadID = 0;
HWND Window = nullptr;

ComPtr<ID3D12Device> D3D;
ComPtr<ID3D12CommandQueue> Queue;
ComPtr<ID3D12Fence> Fence;
HANDLE FenceEvent = nullptr;
UINT64 NextFenceValue = 1;

ComPtr<IDXGISwapChain3> SwapChain;
ComPtr<ID3D12Resource> BackBuffers[BACK_BUFFER_COUNT];
UINT BackBufferIndex = 0;
ComPtr<ID3D12Resource> DepthBuffer;
ComPtr<ID3D12DescriptorHeap> RTVHeap;
ComPtr<ID3D12DescriptorHeap> DSVHeap;
UINT RTVDescriptorSize = 0;

FrameSlot Frames[FRAME_COUNT];
unsigned FrameIndex = 0;
bool FrameSlotPrepared = false;
bool FrameHasRenderTarget = false;
bool Recording = false;
ComPtr<ID3D12GraphicsCommandList> CommandList;

// Used for uploads outside of frames (white texture, uploads piling up during loading).
ComPtr<ID3D12CommandAllocator> UploadAllocator;
ComPtr<ID3D12GraphicsCommandList> UploadList;

ComPtr<ID3D12DescriptorHeap> SRVHeap;
UINT SRVDescriptorSize = 0;
D3D12_CPU_DESCRIPTOR_HANDLE SRVHeapCPU;
D3D12_GPU_DESCRIPTOR_HANDLE SRVHeapGPU;
std::vector<ID3D12Resource *> SRVSlotResources;	// main thread only, which resource each slot describes
unsigned NextSRVSlot = 1;
std::vector<unsigned> FreeSRVSlots;
std::vector<DeferredSlot> DeferredSRVFrees;

ComPtr<ID3D12DescriptorHeap> SamplerHeap;
UINT SamplerDescriptorSize = 0;
D3D12_CPU_DESCRIPTOR_HANDLE SamplerHeapCPU;
D3D12_GPU_DESCRIPTOR_HANDLE SamplerHeapGPU;
std::map<unsigned, unsigned> SamplerCache;

// Guards the fence counter, deferred releases, SRV slot lists and pending uploads.
std::mutex ResourceMutex;
std::vector<DeferredRelease> DeferredReleases;
std::vector<PendingUpload> PendingUploads;
UINT64 PendingUploadBytes = 0;
std::atomic<bool> HasPendingUploads(false);

ComPtr<ID3D12RootSignature> RootSignature;
ComPtr<ID3DBlob> PixelShader;
std::map<unsigned, ComPtr<ID3DBlob>> VertexShaders;
std::map<PSOKey, ComPtr<ID3D12PipelineState>, PSOKeyLess> PipelineStates;

ComPtr<ID3D12Resource> WhiteTexture;

D3D12_VIEWPORT CurrentViewport;
D3D12_RECT CurrentScissor;

// Per-frame command list state, reset by Begin_Frame.
ID3D12PipelineState * BoundPipelineState = nullptr;
D3D_PRIMITIVE_TOPOLOGY BoundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
DX12VSConstants LastVSConstants;
UINT64 LastVSConstantsGPU = 0;
DX12PSConstants LastPSConstants;
UINT64 LastPSConstantsGPU = 0;

bool LoggedTriangleFan = false;
bool LoggedIndexOverflow = false;
bool LoggedVertexOverflow = false;
bool LoggedSamplerHeapFull = false;
unsigned LastBadFVF = 0;

// d3d12.dll, dxgi.dll and d3dcompiler_47.dll are loaded at run time so that the game still starts
// on systems without them when Direct3D 12 is not requested.
typedef HRESULT (WINAPI * CreateDXGIFactory2Func)(UINT flags, REFIID riid, void ** factory);

PFN_D3D12_CREATE_DEVICE D3D12CreateDeviceFunc = nullptr;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE D3D12SerializeRootSignatureFunc = nullptr;
PFN_D3D12_GET_DEBUG_INTERFACE D3D12GetDebugInterfaceFunc = nullptr;
CreateDXGIFactory2Func CreateDXGIFactory2Func_ = nullptr;
pD3DCompile D3DCompileFunc = nullptr;

bool Load_Libraries()
{
	HMODULE d3d12 = LoadLibraryA("d3d12.dll");
	HMODULE dxgi = LoadLibraryA("dxgi.dll");
	HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
	if (d3d12 == nullptr || dxgi == nullptr || compiler == nullptr)
	{
		WWDEBUG_SAY(("DX12: missing d3d12.dll, dxgi.dll or d3dcompiler_47.dll"));
		return false;
	}

	D3D12CreateDeviceFunc = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
	D3D12SerializeRootSignatureFunc = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
	D3D12GetDebugInterfaceFunc = (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(d3d12, "D3D12GetDebugInterface");
	CreateDXGIFactory2Func_ = (CreateDXGIFactory2Func)GetProcAddress(dxgi, "CreateDXGIFactory2");
	D3DCompileFunc = (pD3DCompile)GetProcAddress(compiler, "D3DCompile");
	if (D3D12CreateDeviceFunc == nullptr || D3D12SerializeRootSignatureFunc == nullptr
		|| CreateDXGIFactory2Func_ == nullptr || D3DCompileFunc == nullptr)
	{
		WWDEBUG_SAY(("DX12: d3d12.dll, dxgi.dll or d3dcompiler_47.dll lacks a required entry point"));
		return false;
	}
	return true;
}

bool Is_Main_Thread()
{
	return GetCurrentThreadId() == MainThreadID;
}

void Log_Failure(const char * what, HRESULT hr)
{
	WWDEBUG_SAY(("DX12: %s failed (0x%08x)", what, (unsigned)hr));
	if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && D3D)
	{
		WWDEBUG_SAY(("DX12: device removed, reason 0x%08x", (unsigned)D3D->GetDeviceRemovedReason()));
	}
}

D3D12_RESOURCE_BARRIER Make_Transition(ID3D12Resource * resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

void Transition(ID3D12GraphicsCommandList * list, ID3D12Resource * resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier = Make_Transition(resource, before, after);
	list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> Create_Upload_Buffer(UINT64 size)
{
	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> resource;
	HRESULT hr = D3D->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommittedResource (upload buffer)", hr);
		return nullptr;
	}
	return resource;
}

bool Create_Frame_Page(unsigned size, FramePage & page)
{
	page = FramePage();
	page.Resource = Create_Upload_Buffer(size);
	if (!page.Resource)
	{
		return false;
	}

	D3D12_RANGE no_read = { 0, 0 };
	void * cpu = nullptr;
	HRESULT hr = page.Resource->Map(0, &no_read, &cpu);
	if (FAILED(hr))
	{
		Log_Failure("Map (frame memory)", hr);
		page = FramePage();
		return false;
	}

	page.CPU = static_cast<unsigned char *>(cpu);
	page.GPU = page.Resource->GetGPUVirtualAddress();
	page.Size = size;
	page.Offset = 0;
	return true;
}

ComPtr<ID3D12DescriptorHeap> Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned count, bool shader_visible)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Type = type;
	desc.NumDescriptors = count;
	desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	ComPtr<ID3D12DescriptorHeap> heap;
	HRESULT hr = D3D->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
	if (FAILED(hr))
	{
		Log_Failure("CreateDescriptorHeap", hr);
		return nullptr;
	}
	return heap;
}

UINT64 Signal_Queue()
{
	UINT64 value;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		value = NextFenceValue++;
	}
	HRESULT hr = Queue->Signal(Fence.Get(), value);
	if (FAILED(hr))
	{
		Log_Failure("Signal", hr);
	}
	return value;
}

void Wait_For_Fence(UINT64 value)
{
	if (value == 0 || !Fence || Fence->GetCompletedValue() >= value)
	{
		return;
	}

	// The event may still be signaled from an earlier wait that timed out, so the completed value,
	// not the event, decides when the wait is over. Wait in steps so that a removed device cannot
	// hang the game.
	while (Fence->GetCompletedValue() < value)
	{
		if (FAILED(Fence->SetEventOnCompletion(value, FenceEvent)))
		{
			return;
		}
		if (WaitForSingleObject(FenceEvent, 2000) != WAIT_TIMEOUT)
		{
			continue;
		}
		HRESULT reason = D3D->GetDeviceRemovedReason();
		if (reason != S_OK)
		{
			Log_Failure("Wait_For_Fence", reason);
			return;
		}
		WWDEBUG_SAY(("DX12: still waiting for the GPU (fence %u)", (unsigned)value));
	}
}

// Main thread only.
void Process_Deferred()
{
	if (!Fence)
	{
		return;
	}

	UINT64 completed = Fence->GetCompletedValue();
	std::vector<ID3D12Resource *> release;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);

		size_t keep = 0;
		for (size_t i = 0; i < DeferredReleases.size(); ++i)
		{
			if (DeferredReleases[i].FenceValue <= completed)
				release.push_back(DeferredReleases[i].Resource);
			else
				DeferredReleases[keep++] = DeferredReleases[i];
		}
		DeferredReleases.resize(keep);

		keep = 0;
		for (size_t i = 0; i < DeferredSRVFrees.size(); ++i)
		{
			if (DeferredSRVFrees[i].FenceValue <= completed)
			{
				unsigned slot = DeferredSRVFrees[i].Slot;
				SRVSlotResources[slot] = nullptr;
				FreeSRVSlots.push_back(slot);
			}
			else
			{
				DeferredSRVFrees[keep++] = DeferredSRVFrees[i];
			}
		}
		DeferredSRVFrees.resize(keep);
	}

	for (size_t i = 0; i < release.size(); ++i)
	{
		release[i]->Release();
	}
}

// Waits until the GPU is done with the current frame slot and resets its memory. A slot whose
// memory overflowed into extra pages gets one bigger page instead.
void Prepare_Frame_Slot()
{
	if (FrameSlotPrepared)
	{
		return;
	}

	FrameSlot & frame = Frames[FrameIndex];
	Wait_For_Fence(frame.FenceValue);

	if (!frame.Extra.empty() || !frame.Memory.Resource)
	{
		UINT64 total = frame.Memory.Size;
		for (size_t i = 0; i < frame.Extra.size(); ++i)
		{
			total += frame.Extra[i].Size;
		}
		frame.Extra.clear();

		if (total < FRAME_MEMORY_SIZE)
			total = FRAME_MEMORY_SIZE;
		if (total > FRAME_MEMORY_MAX_SIZE)
			total = FRAME_MEMORY_MAX_SIZE;

		if (total != frame.Memory.Size || !frame.Memory.Resource)
		{
			FramePage page;
			if (Create_Frame_Page((unsigned)total, page))
			{
				frame.Memory = page;
			}
		}
	}

	frame.Memory.Offset = 0;
	FrameSlotPrepared = true;
}

// Records the queued texture uploads into list. Main thread only.
void Record_Uploads(ID3D12GraphicsCommandList * list)
{
	if (!HasPendingUploads.load())
	{
		return;
	}

	std::vector<PendingUpload> uploads;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		uploads.swap(PendingUploads);
		PendingUploadBytes = 0;
		HasPendingUploads = false;
	}

	std::vector<D3D12_RESOURCE_BARRIER> barriers;
	for (size_t i = 0; i < uploads.size(); ++i)
	{
		DX12TextureClass * texture = uploads[i].Texture;
		ID3D12Resource * resource = texture->Peek_Resource();
		if (resource && texture->Get_State() != (unsigned)D3D12_RESOURCE_STATE_COPY_DEST)
		{
			barriers.push_back(Make_Transition(resource, (D3D12_RESOURCE_STATES)texture->Get_State(), D3D12_RESOURCE_STATE_COPY_DEST));
			texture->Set_State(D3D12_RESOURCE_STATE_COPY_DEST);
		}
	}
	if (!barriers.empty())
	{
		list->ResourceBarrier((UINT)barriers.size(), &barriers[0]);
	}

	for (size_t i = 0; i < uploads.size(); ++i)
	{
		DX12TextureClass * texture = uploads[i].Texture;
		ID3D12Resource * resource = texture->Peek_Resource();
		if (resource)
		{
			D3D12_RESOURCE_DESC desc = resource->GetDesc();
			if (uploads[i].Level < desc.MipLevels)
			{
				D3D12_TEXTURE_COPY_LOCATION dst;
				memset(&dst, 0, sizeof(dst));
				dst.pResource = resource;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = uploads[i].Level;

				D3D12_TEXTURE_COPY_LOCATION src;
				memset(&src, 0, sizeof(src));
				src.pResource = uploads[i].Upload;
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				D3D->GetCopyableFootprints(&desc, uploads[i].Level, 1, 0, &src.PlacedFootprint, nullptr, nullptr, nullptr);

				list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}
		}

		DX12Device::Release_When_Idle(uploads[i].Upload);
		texture->Release_Ref();
	}
}

// Records and executes the pending uploads in a standalone command list and waits for them.
// Main thread only, and only outside a frame so the copies stay in submission order.
void Execute_Uploads_Now()
{
	if (Recording || !HasPendingUploads.load())
	{
		return;
	}

	if (FAILED(UploadAllocator->Reset()) || FAILED(UploadList->Reset(UploadAllocator.Get(), nullptr)))
	{
		WWDEBUG_SAY(("DX12: could not reset the upload command list"));
		return;
	}

	Record_Uploads(UploadList.Get());

	HRESULT hr = UploadList->Close();
	if (SUCCEEDED(hr))
	{
		ID3D12CommandList * lists[] = { UploadList.Get() };
		Queue->ExecuteCommandLists(1, lists);
	}
	else
	{
		Log_Failure("Close (upload list)", hr);
	}

	Wait_For_Fence(Signal_Queue());
	Process_Deferred();
}

void Ensure_Recording()
{
	if (!Recording)
	{
		DX12Device::Begin_Frame();
	}
}

void Apply_Viewport()
{
	if (!Recording)
	{
		return;
	}

	D3D12_VIEWPORT viewport = CurrentViewport;
	if (viewport.Width < 1.0f)
		viewport.Width = 1.0f;
	if (viewport.Height < 1.0f)
		viewport.Height = 1.0f;

	CommandList->RSSetViewports(1, &viewport);
	CommandList->RSSetScissorRects(1, &CurrentScissor);
}

void Set_Full_Viewport()
{
	unsigned width = DX12Device::Get_Width();
	unsigned height = DX12Device::Get_Height();

	CurrentViewport.TopLeftX = 0.0f;
	CurrentViewport.TopLeftY = 0.0f;
	CurrentViewport.Width = (float)width;
	CurrentViewport.Height = (float)height;
	CurrentViewport.MinDepth = 0.0f;
	CurrentViewport.MaxDepth = 1.0f;

	CurrentScissor.left = 0;
	CurrentScissor.top = 0;
	CurrentScissor.right = (LONG)width;
	CurrentScissor.bottom = (LONG)height;

	Apply_Viewport();
}

ComPtr<ID3DBlob> Compile_Shader(const char * source, size_t length, const char * entry, const char * target, const D3D_SHADER_MACRO * macros)
{
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> errors;
	HRESULT hr = D3DCompileFunc(source, length, "dx12shaders.h", macros, nullptr, entry, target, flags, 0, &code, &errors);
	if (FAILED(hr))
	{
		WWDEBUG_SAY(("DX12: compiling %s (%s) failed (0x%08x): %s", entry, target, (unsigned)hr,
			errors ? (const char *)errors->GetBufferPointer() : "no compiler output"));
		return nullptr;
	}
	return code;
}

ID3DBlob * Get_Vertex_Shader(unsigned key)
{
	std::map<unsigned, ComPtr<ID3DBlob>>::iterator it = VertexShaders.find(key);
	if (it != VertexShaders.end())
	{
		return it->second.Get();
	}

	char rhw[4], normal[4], diffuse[4], specular[4], texcount[4];
	snprintf(rhw, sizeof(rhw), "%u", key & 1);
	snprintf(normal, sizeof(normal), "%u", (key >> 1) & 1);
	snprintf(diffuse, sizeof(diffuse), "%u", (key >> 2) & 1);
	snprintf(specular, sizeof(specular), "%u", (key >> 3) & 1);
	snprintf(texcount, sizeof(texcount), "%u", (key >> 4) & 15);

	const D3D_SHADER_MACRO macros[] =
	{
		{ "RHW", rhw },
		{ "HAS_NORMAL", normal },
		{ "HAS_DIFFUSE", diffuse },
		{ "HAS_SPECULAR", specular },
		{ "TEXCOUNT", texcount },
		{ nullptr, nullptr },
	};

	// A failed compile is cached too, so it is reported only once.
	ComPtr<ID3DBlob> blob = Compile_Shader(DX12VertexShaderSource, sizeof(DX12VertexShaderSource) - 1, "VSMain", "vs_5_0", macros);
	VertexShaders[key] = blob;
	return blob.Get();
}

// Builds the input layout for a Direct3D 8 FVF. Fails for formats without a position or with
// more texture coordinate sets than Direct3D 8 allows.
bool Decode_FVF(unsigned fvf, FVFLayout & layout)
{
	static const DXGI_FORMAT texcoord_formats[4] =
	{
		DXGI_FORMAT_R32G32_FLOAT,			// D3DFVF_TEXTUREFORMAT2
		DXGI_FORMAT_R32G32B32_FLOAT,		// D3DFVF_TEXTUREFORMAT3
		DXGI_FORMAT_R32G32B32A32_FLOAT,		// D3DFVF_TEXTUREFORMAT4
		DXGI_FORMAT_R32_FLOAT,				// D3DFVF_TEXTUREFORMAT1
	};
	static const unsigned texcoord_sizes[4] = { 8, 12, 16, 4 };

	memset(&layout, 0, sizeof(layout));

	unsigned offset = 0;
	unsigned count = 0;
	bool rhw = false;

	D3D12_INPUT_ELEMENT_DESC element;
	memset(&element, 0, sizeof(element));
	element.InputSlot = 0;
	element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
	element.InstanceDataStepRate = 0;

	unsigned position = fvf & FVF_POSITION_MASK;
	element.SemanticName = "POSITION";
	element.SemanticIndex = 0;
	element.AlignedByteOffset = 0;
	if (position == FVF_XYZ)
	{
		element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
		offset = 12;
	}
	else if (position == FVF_XYZRHW)
	{
		element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		offset = 16;
		rhw = true;
	}
	else if (position >= FVF_XYZB1)
	{
		// Blend weights are skipped; there is no vertex blending in milestone 1.
		element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
		offset = 12 + 4 * (((position - FVF_XYZB1) >> 1) + 1);
	}
	else
	{
		return false;
	}
	layout.Elements[count++] = element;

	bool normal = (fvf & FVF_NORMAL) != 0;
	if (normal)
	{
		element.SemanticName = "NORMAL";
		element.SemanticIndex = 0;
		element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
		element.AlignedByteOffset = offset;
		layout.Elements[count++] = element;
		offset += 12;
	}

	if (fvf & FVF_PSIZE)
	{
		offset += 4;
	}

	// Direct3D 8 colors are BGRA bytes; the vertex shader swizzles them back.
	bool diffuse = (fvf & FVF_DIFFUSE) != 0;
	if (diffuse)
	{
		element.SemanticName = "COLOR";
		element.SemanticIndex = 0;
		element.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		element.AlignedByteOffset = offset;
		layout.Elements[count++] = element;
		offset += 4;
	}

	bool specular = (fvf & FVF_SPECULAR) != 0;
	if (specular)
	{
		element.SemanticName = "COLOR";
		element.SemanticIndex = 1;
		element.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		element.AlignedByteOffset = offset;
		layout.Elements[count++] = element;
		offset += 4;
	}

	unsigned texcount = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	if (texcount > FVF_MAX_TEXCOORDS)
	{
		return false;
	}
	for (unsigned i = 0; i < texcount; ++i)
	{
		unsigned size_bits = (fvf >> (16 + i * 2)) & 3;
		element.SemanticName = "TEXCOORD";
		element.SemanticIndex = i;
		element.Format = texcoord_formats[size_bits];
		element.AlignedByteOffset = offset;
		layout.Elements[count++] = element;
		offset += texcoord_sizes[size_bits];
	}

	layout.ElementCount = count;
	layout.Size = offset;
	layout.VSKey = (rhw ? 1u : 0u) | (normal ? 2u : 0u) | (diffuse ? 4u : 0u) | (specular ? 8u : 0u) | (texcount << 4);
	return true;
}

D3D12_BLEND Map_Blend(unsigned blend, bool alpha, D3D12_BLEND fallback)
{
	if (blend < 1 || blend > 11)
	{
		return fallback;
	}

	// Direct3D 12 does not accept color factors for the alpha channel.
	if (alpha)
	{
		switch (blend)
		{
		case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
		case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
		case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
		case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
		default: break;
		}
	}
	return (D3D12_BLEND)blend;
}

ID3D12PipelineState * Get_Pipeline_State(const PSOKey & key, const FVFLayout & layout)
{
	std::map<PSOKey, ComPtr<ID3D12PipelineState>, PSOKeyLess>::iterator it = PipelineStates.find(key);
	if (it != PipelineStates.end())
	{
		return it->second.Get();
	}

	ComPtr<ID3D12PipelineState> pso;
	ID3DBlob * vertex_shader = Get_Vertex_Shader(layout.VSKey);
	if (vertex_shader && PixelShader)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.pRootSignature = RootSignature.Get();
		desc.VS.pShaderBytecode = vertex_shader->GetBufferPointer();
		desc.VS.BytecodeLength = vertex_shader->GetBufferSize();
		desc.PS.pShaderBytecode = PixelShader->GetBufferPointer();
		desc.PS.BytecodeLength = PixelShader->GetBufferSize();

		D3D12_BLEND src_color, dest_color, src_alpha, dest_alpha;
		if (key.SrcBlend == BLEND_BOTHSRCALPHA)
		{
			src_color = src_alpha = D3D12_BLEND_SRC_ALPHA;
			dest_color = dest_alpha = D3D12_BLEND_INV_SRC_ALPHA;
		}
		else if (key.SrcBlend == BLEND_BOTHINVSRCALPHA)
		{
			src_color = src_alpha = D3D12_BLEND_INV_SRC_ALPHA;
			dest_color = dest_alpha = D3D12_BLEND_SRC_ALPHA;
		}
		else
		{
			src_color = Map_Blend(key.SrcBlend, false, D3D12_BLEND_ONE);
			src_alpha = Map_Blend(key.SrcBlend, true, D3D12_BLEND_ONE);
			dest_color = Map_Blend(key.DestBlend, false, D3D12_BLEND_ZERO);
			dest_alpha = Map_Blend(key.DestBlend, true, D3D12_BLEND_ZERO);
		}
		D3D12_BLEND_OP blend_op = (key.BlendOp >= 1 && key.BlendOp <= 5) ? (D3D12_BLEND_OP)key.BlendOp : D3D12_BLEND_OP_ADD;

		D3D12_RENDER_TARGET_BLEND_DESC & blend = desc.BlendState.RenderTarget[0];
		blend.BlendEnable = key.AlphaBlendEnable ? TRUE : FALSE;
		blend.LogicOpEnable = FALSE;
		blend.SrcBlend = src_color;
		blend.DestBlend = dest_color;
		blend.BlendOp = blend_op;
		blend.SrcBlendAlpha = src_alpha;
		blend.DestBlendAlpha = dest_alpha;
		blend.BlendOpAlpha = blend_op;
		blend.LogicOp = D3D12_LOGIC_OP_NOOP;
		blend.RenderTargetWriteMask = (UINT8)(key.ColorWriteMask & D3D12_COLOR_WRITE_ENABLE_ALL);
		desc.SampleMask = 0xffffffff;

		D3D12_RASTERIZER_DESC & raster = desc.RasterizerState;
		raster.FillMode = (key.FillMode == 2) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
		if (key.CullMode == 2)
			raster.CullMode = D3D12_CULL_MODE_FRONT;
		else if (key.CullMode == 3)
			raster.CullMode = D3D12_CULL_MODE_BACK;
		else
			raster.CullMode = D3D12_CULL_MODE_NONE;
		raster.FrontCounterClockwise = FALSE;
		raster.DepthBias = key.DepthBias * ZBIAS_TO_DEPTH_BIAS;
		raster.DepthBiasClamp = 0.0f;
		raster.SlopeScaledDepthBias = 0.0f;
		raster.DepthClipEnable = TRUE;
		raster.MultisampleEnable = FALSE;
		raster.AntialiasedLineEnable = FALSE;
		raster.ForcedSampleCount = 0;
		raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		D3D12_DEPTH_STENCIL_DESC & depth = desc.DepthStencilState;
		depth.DepthEnable = key.ZEnable ? TRUE : FALSE;
		depth.DepthWriteMask = key.ZWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		depth.DepthFunc = (key.ZFunc >= 1 && key.ZFunc <= 8) ? (D3D12_COMPARISON_FUNC)key.ZFunc : D3D12_COMPARISON_FUNC_LESS_EQUAL;
		depth.StencilEnable = FALSE;
		depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		D3D12_DEPTH_STENCILOP_DESC stencil_op = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
		depth.FrontFace = stencil_op;
		depth.BackFace = stencil_op;

		desc.InputLayout.pInputElementDescs = layout.Elements;
		desc.InputLayout.NumElements = layout.ElementCount;
		desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		desc.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)key.TopologyType;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;

		HRESULT hr = D3D->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			WWDEBUG_SAY(("DX12: CreateGraphicsPipelineState failed (0x%08x) for FVF 0x%x", (unsigned)hr, key.FVF));
			pso.Reset();
		}
	}

	// A failed pipeline state is cached too, so it is reported only once.
	PipelineStates[key] = pso;
	return pso.Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE SRV_GPU_Handle(unsigned slot)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = SRVHeapGPU;
	handle.ptr += (UINT64)slot * SRVDescriptorSize;
	return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE SRV_CPU_Handle(unsigned slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = SRVHeapCPU;
	handle.ptr += (SIZE_T)slot * SRVDescriptorSize;
	return handle;
}

// Returns the descriptor table for a texture, writing its view into its slot on first use and
// adding a transition to PIXEL_SHADER_RESOURCE when needed. Null textures bind the white texture.
D3D12_GPU_DESCRIPTOR_HANDLE Bind_Texture(DX12TextureClass * texture, D3D12_RESOURCE_BARRIER * barriers, unsigned & barrier_count)
{
	if (texture && !texture->Is_CPU_Only())
	{
		ID3D12Resource * resource = texture->Peek_Resource();
		unsigned slot = texture->Get_SRV_Slot();
		if (resource && slot > 0 && slot < SRV_HEAP_SIZE)
		{
			if (SRVSlotResources[slot] != resource)
			{
				D3D->CreateShaderResourceView(resource, nullptr, SRV_CPU_Handle(slot));
				SRVSlotResources[slot] = resource;
			}
			if (texture->Get_State() != (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			{
				barriers[barrier_count++] = Make_Transition(resource, (D3D12_RESOURCE_STATES)texture->Get_State(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				texture->Set_State(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			return SRV_GPU_Handle(slot);
		}
	}
	return SRV_GPU_Handle(0);
}

// D3DTEXTUREFILTERTYPE: 0 none, 1 point, 2 linear, 3 anisotropic, 4/5 cubic (treated as linear).
// D3DTEXTUREADDRESS equals D3D12_TEXTURE_ADDRESS_MODE (1 wrap .. 5 mirror once).
D3D12_GPU_DESCRIPTOR_HANDLE Get_Sampler(unsigned min_filter, unsigned mag_filter, unsigned mip_filter, unsigned address_u, unsigned address_v, unsigned max_anisotropy)
{
	if (min_filter > 3)
		min_filter = 2;
	if (mag_filter > 3)
		mag_filter = 2;
	if (mip_filter > 3)
		mip_filter = 2;
	if (address_u < 1 || address_u > 5)
		address_u = 1;
	if (address_v < 1 || address_v > 5)
		address_v = 1;

	bool anisotropic = (min_filter == 3 || mag_filter == 3);
	if (!anisotropic || max_anisotropy < 1)
		max_anisotropy = 1;
	if (max_anisotropy > 16)
		max_anisotropy = 16;

	unsigned key = min_filter | (mag_filter << 2) | (mip_filter << 4) | (address_u << 6) | (address_v << 9) | ((max_anisotropy - 1) << 12);

	unsigned index = 0;
	std::map<unsigned, unsigned>::iterator it = SamplerCache.find(key);
	if (it != SamplerCache.end())
	{
		index = it->second;
	}
	else if (SamplerCache.size() >= SAMPLER_HEAP_SIZE)
	{
		if (!LoggedSamplerHeapFull)
		{
			WWDEBUG_SAY(("DX12: sampler heap is full"));
			LoggedSamplerHeapFull = true;
		}
	}
	else
	{
		index = (unsigned)SamplerCache.size();
		SamplerCache[key] = index;

		D3D12_SAMPLER_DESC desc;
		memset(&desc, 0, sizeof(desc));
		if (anisotropic)
		{
			desc.Filter = D3D12_FILTER_ANISOTROPIC;
		}
		else
		{
			D3D12_FILTER_TYPE min_type = (min_filter == 2) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
			D3D12_FILTER_TYPE mag_type = (mag_filter == 2) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
			D3D12_FILTER_TYPE mip_type = (mip_filter >= 2) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
			desc.Filter = D3D12_ENCODE_BASIC_FILTER(min_type, mag_type, mip_type, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
		}
		desc.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)address_u;
		desc.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)address_v;
		desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.MipLODBias = 0.0f;
		desc.MaxAnisotropy = max_anisotropy;
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		desc.MinLOD = 0.0f;
		desc.MaxLOD = (mip_filter == 0) ? 0.0f : D3D12_FLOAT32_MAX;

		D3D12_CPU_DESCRIPTOR_HANDLE cpu = SamplerHeapCPU;
		cpu.ptr += (SIZE_T)index * SamplerDescriptorSize;
		D3D->CreateSampler(&desc, cpu);
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpu = SamplerHeapGPU;
	gpu.ptr += (UINT64)index * SamplerDescriptorSize;
	return gpu;
}

void Color_To_Float4(unsigned argb, float * out)
{
	out[0] = ((argb >> 16) & 0xff) / 255.0f;
	out[1] = ((argb >> 8) & 0xff) / 255.0f;
	out[2] = (argb & 0xff) / 255.0f;
	out[3] = ((argb >> 24) & 0xff) / 255.0f;
}

void Set_Identity(float * m)
{
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b for row major 4x4 matrices.
void Multiply_Matrix(const float * a, const float * b, float * out)
{
	for (int row = 0; row < 4; ++row)
	{
		for (int col = 0; col < 4; ++col)
		{
			out[row * 4 + col] =
				a[row * 4 + 0] * b[0 * 4 + col] +
				a[row * 4 + 1] * b[1 * 4 + col] +
				a[row * 4 + 2] * b[2 * 4 + col] +
				a[row * 4 + 3] * b[3 * 4 + col];
		}
	}
}

// Copies constants into frame memory, reusing the previous copy when nothing changed.
UINT64 Upload_Constants(const void * data, unsigned size, void * last, UINT64 & last_gpu)
{
	if (last_gpu != 0 && memcmp(data, last, size) == 0)
	{
		return last_gpu;
	}

	void * cpu = nullptr;
	unsigned long long gpu = 0;
	if (!DX12Device::Allocate_Frame_Memory(size, &cpu, &gpu))
	{
		return 0;
	}
	memcpy(cpu, data, size);
	memcpy(last, data, size);
	last_gpu = gpu;
	return gpu;
}

bool Select_Adapter()
{
	bool found = false;
	ComPtr<IDXGIAdapter1> candidate;
	for (UINT i = 0; Factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 desc;
		if (FAILED(candidate->GetDesc1(&desc)))
		{
			continue;
		}
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			continue;
		}
		if (FAILED(D3D12CreateDeviceFunc(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
		{
			continue;
		}
		if (!found || desc.DedicatedVideoMemory > AdapterDesc.DedicatedVideoMemory)
		{
			Adapter = candidate;
			AdapterDesc = desc;
			found = true;
		}
	}

	if (found)
	{
		WideCharToMultiByte(CP_ACP, 0, AdapterDesc.Description, -1, AdapterName, sizeof(AdapterName), nullptr, nullptr);
		AdapterName[sizeof(AdapterName) - 1] = '\0';
	}
	return found;
}

bool Display_Mode_Less(const DX12Device::DisplayMode & a, const DX12Device::DisplayMode & b)
{
	if (a.Width != b.Width)
		return a.Width < b.Width;
	return a.Height < b.Height;
}

bool Display_Mode_Equal(const DX12Device::DisplayMode & a, const DX12Device::DisplayMode & b)
{
	return a.Width == b.Width && a.Height == b.Height;
}

void Enumerate_Display_Modes()
{
	DisplayModes.clear();

	ComPtr<IDXGIOutput> output;
	if (FAILED(Adapter->EnumOutputs(0, &output)))
	{
		// On hybrid laptops the display can be attached to another adapter than the one we render with.
		output.Reset();
		ComPtr<IDXGIAdapter1> other;
		for (UINT i = 0; output.Get() == nullptr && Factory->EnumAdapters1(i, &other) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			if (FAILED(other->EnumOutputs(0, &output)))
			{
				output.Reset();
			}
		}
	}

	if (output)
	{
		UINT count = 0;
		if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, nullptr)) && count > 0)
		{
			std::vector<DXGI_MODE_DESC> modes(count);
			if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, &modes[0])))
			{
				for (UINT i = 0; i < count; ++i)
				{
					DX12Device::DisplayMode mode;
					mode.Width = modes[i].Width;
					mode.Height = modes[i].Height;
					DisplayModes.push_back(mode);
				}
			}
		}
	}

	if (DisplayModes.empty())
	{
		DX12Device::DisplayMode mode;
		mode.Width = (unsigned)GetSystemMetrics(SM_CXSCREEN);
		mode.Height = (unsigned)GetSystemMetrics(SM_CYSCREEN);
		DisplayModes.push_back(mode);
	}

	std::sort(DisplayModes.begin(), DisplayModes.end(), Display_Mode_Less);
	DisplayModes.erase(std::unique(DisplayModes.begin(), DisplayModes.end(), Display_Mode_Equal), DisplayModes.end());
}

bool Create_Root_Signature()
{
	D3D12_DESCRIPTOR_RANGE ranges[4];
	memset(ranges, 0, sizeof(ranges));
	for (int i = 0; i < 4; ++i)
	{
		ranges[i].RangeType = (i < 2) ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[i].NumDescriptors = 1;
		ranges[i].BaseShaderRegister = i & 1;
		ranges[i].RegisterSpace = 0;
		ranges[i].OffsetInDescriptorsFromTableStart = 0;
	}

	// 0: b0 VS constants, 1: b1 PS constants, 2/3: t0/t1, 4/5: s0/s1.
	D3D12_ROOT_PARAMETER params[6];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 1;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	for (int i = 0; i < 4; ++i)
	{
		params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
		params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
		params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	D3D12_ROOT_SIGNATURE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.NumParameters = 6;
	desc.pParameters = params;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errors;
	HRESULT hr = D3D12SerializeRootSignatureFunc(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
	if (FAILED(hr))
	{
		WWDEBUG_SAY(("DX12: D3D12SerializeRootSignature failed (0x%08x): %s", (unsigned)hr,
			errors ? (const char *)errors->GetBufferPointer() : ""));
		return false;
	}

	hr = D3D->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&RootSignature));
	if (FAILED(hr))
	{
		Log_Failure("CreateRootSignature", hr);
		return false;
	}
	return true;
}

// The 1x1 opaque white texture in SRV slot 0, bound for stages without a texture.
bool Create_White_Texture()
{
	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = 1;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	HRESULT hr = D3D->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&WhiteTexture));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommittedResource (white texture)", hr);
		return false;
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT64 total = 0;
	D3D->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

	ComPtr<ID3D12Resource> upload = Create_Upload_Buffer(total);
	if (!upload)
	{
		return false;
	}

	void * cpu = nullptr;
	D3D12_RANGE no_read = { 0, 0 };
	hr = upload->Map(0, &no_read, &cpu);
	if (FAILED(hr))
	{
		Log_Failure("Map (white texture)", hr);
		return false;
	}
	memset(cpu, 0xff, (size_t)total);
	upload->Unmap(0, nullptr);

	if (FAILED(UploadAllocator->Reset()) || FAILED(UploadList->Reset(UploadAllocator.Get(), nullptr)))
	{
		WWDEBUG_SAY(("DX12: could not reset the upload command list"));
		return false;
	}

	D3D12_TEXTURE_COPY_LOCATION dst;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = WhiteTexture.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src;
	memset(&src, 0, sizeof(src));
	src.pResource = upload.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = footprint;

	UploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	Transition(UploadList.Get(), WhiteTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	hr = UploadList->Close();
	if (FAILED(hr))
	{
		Log_Failure("Close (white texture)", hr);
		return false;
	}
	ID3D12CommandList * lists[] = { UploadList.Get() };
	Queue->ExecuteCommandLists(1, lists);
	Wait_For_Fence(Signal_Queue());

	D3D->CreateShaderResourceView(WhiteTexture.Get(), nullptr, SRV_CPU_Handle(0));
	SRVSlotResources[0] = WhiteTexture.Get();
	return true;
}

bool Init_Device(HWND hwnd)
{
	Window = hwnd;
	MainThreadID = GetCurrentThreadId();

	if (!Load_Libraries())
	{
		return false;
	}

	UINT factory_flags = 0;
#ifdef _DEBUG
	ComPtr<ID3D12Debug> debug;
	if (D3D12GetDebugInterfaceFunc && SUCCEEDED(D3D12GetDebugInterfaceFunc(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		factory_flags = DXGI_CREATE_FACTORY_DEBUG;
	}
#endif

	HRESULT hr = CreateDXGIFactory2Func_(factory_flags, IID_PPV_ARGS(&Factory));
	if (FAILED(hr) && factory_flags != 0)
	{
		hr = CreateDXGIFactory2Func_(0, IID_PPV_ARGS(&Factory));
	}
	if (FAILED(hr))
	{
		Log_Failure("CreateDXGIFactory2", hr);
		return false;
	}

	if (!Select_Adapter())
	{
		WWDEBUG_SAY(("DX12: no hardware adapter supports Direct3D 12 at feature level 11_0"));
		return false;
	}
	WWDEBUG_SAY(("DX12: adapter %s (vendor 0x%04x, device 0x%04x, %u MB)", AdapterName,
		AdapterDesc.VendorId, AdapterDesc.DeviceId, (unsigned)(AdapterDesc.DedicatedVideoMemory / (1024 * 1024))));

	hr = D3D12CreateDeviceFunc(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&D3D));
	if (FAILED(hr))
	{
		Log_Failure("D3D12CreateDevice", hr);
		return false;
	}

	Enumerate_Display_Modes();

	D3D12_COMMAND_QUEUE_DESC queue_desc;
	memset(&queue_desc, 0, sizeof(queue_desc));
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	hr = D3D->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&Queue));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommandQueue", hr);
		return false;
	}

	hr = D3D->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
	if (FAILED(hr))
	{
		Log_Failure("CreateFence", hr);
		return false;
	}
	NextFenceValue = 1;
	FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!FenceEvent)
	{
		WWDEBUG_SAY(("DX12: CreateEvent failed"));
		return false;
	}

	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		hr = D3D->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Frames[i].Allocator));
		if (FAILED(hr))
		{
			Log_Failure("CreateCommandAllocator", hr);
			return false;
		}
		Frames[i].FenceValue = 0;
		if (!Create_Frame_Page(FRAME_MEMORY_SIZE, Frames[i].Memory))
		{
			return false;
		}
	}
	FrameIndex = 0;
	FrameSlotPrepared = false;

	hr = D3D->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Frames[0].Allocator.Get(), nullptr, IID_PPV_ARGS(&CommandList));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommandList", hr);
		return false;
	}
	CommandList->Close();
	Recording = false;

	hr = D3D->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&UploadAllocator));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommandAllocator (uploads)", hr);
		return false;
	}
	hr = D3D->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(&UploadList));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommandList (uploads)", hr);
		return false;
	}
	UploadList->Close();

	RTVHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BACK_BUFFER_COUNT, false);
	DSVHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
	SRVHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP_SIZE, true);
	SamplerHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE, true);
	if (!RTVHeap || !DSVHeap || !SRVHeap || !SamplerHeap)
	{
		return false;
	}
	RTVDescriptorSize = D3D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	SRVDescriptorSize = D3D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	SamplerDescriptorSize = D3D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	SRVHeapCPU = SRVHeap->GetCPUDescriptorHandleForHeapStart();
	SRVHeapGPU = SRVHeap->GetGPUDescriptorHandleForHeapStart();
	SamplerHeapCPU = SamplerHeap->GetCPUDescriptorHandleForHeapStart();
	SamplerHeapGPU = SamplerHeap->GetGPUDescriptorHandleForHeapStart();

	SRVSlotResources.assign(SRV_HEAP_SIZE, nullptr);
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		NextSRVSlot = 1;
		FreeSRVSlots.clear();
		DeferredSRVFrees.clear();
	}

	if (!Create_Root_Signature())
	{
		return false;
	}

	PixelShader = Compile_Shader(DX12PixelShaderSource, sizeof(DX12PixelShaderSource) - 1, "PSMain", "ps_5_0", nullptr);
	if (!PixelShader)
	{
		return false;
	}

	if (!Create_White_Texture())
	{
		return false;
	}

	memset(&CurrentViewport, 0, sizeof(CurrentViewport));
	memset(&CurrentScissor, 0, sizeof(CurrentScissor));
	return true;
}

// Releases every object owned by this file. The GPU must be idle.
void Release_All()
{
	std::vector<PendingUpload> uploads;
	std::vector<DeferredRelease> releases;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		uploads.swap(PendingUploads);
		releases.swap(DeferredReleases);
		DeferredSRVFrees.clear();
		FreeSRVSlots.clear();
		NextSRVSlot = 1;
		PendingUploadBytes = 0;
		HasPendingUploads = false;
	}
	for (size_t i = 0; i < uploads.size(); ++i)
	{
		if (uploads[i].Upload)
			uploads[i].Upload->Release();
		if (uploads[i].Texture)
			uploads[i].Texture->Release_Ref();
	}
	for (size_t i = 0; i < releases.size(); ++i)
	{
		releases[i].Resource->Release();
	}

	PipelineStates.clear();
	VertexShaders.clear();
	PixelShader.Reset();
	RootSignature.Reset();
	SamplerCache.clear();
	WhiteTexture.Reset();
	SRVSlotResources.clear();

	for (int i = 0; i < BACK_BUFFER_COUNT; ++i)
	{
		BackBuffers[i].Reset();
	}
	DepthBuffer.Reset();
	SwapChain.Reset();

	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		Frames[i].Allocator.Reset();
		Frames[i].FenceValue = 0;
		Frames[i].Memory = FramePage();
		Frames[i].Extra.clear();
	}
	FrameIndex = 0;
	FrameSlotPrepared = false;
	FrameHasRenderTarget = false;
	Recording = false;
	CommandList.Reset();
	UploadList.Reset();
	UploadAllocator.Reset();

	RTVHeap.Reset();
	DSVHeap.Reset();
	SRVHeap.Reset();
	SamplerHeap.Reset();

	Queue.Reset();
	Fence.Reset();
	if (FenceEvent)
	{
		CloseHandle(FenceEvent);
		FenceEvent = nullptr;
	}
	NextFenceValue = 1;

	D3D.Reset();
	Adapter.Reset();
	Factory.Reset();
	DisplayModes.clear();

	BoundPipelineState = nullptr;
	BoundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	LastVSConstantsGPU = 0;
	LastPSConstantsGPU = 0;
}

bool Create_Depth_Buffer(unsigned width, unsigned height)
{
	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear;
	memset(&clear, 0, sizeof(clear));
	clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	clear.DepthStencil.Depth = 1.0f;
	clear.DepthStencil.Stencil = 0;

	HRESULT hr = D3D->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&DepthBuffer));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommittedResource (depth buffer)", hr);
		return false;
	}

	D3D->CreateDepthStencilView(DepthBuffer.Get(), nullptr, DSVHeap->GetCPUDescriptorHandleForHeapStart());
	return true;
}

bool Has_Render_Target()
{
	return SwapChain && BackBuffers[BackBufferIndex] && DepthBuffer;
}

} // namespace

// Used by DX12TextureClass. Descriptor writes are free threaded, so this may run on the loader
// thread; the slot is not visible to the main thread until the texture is handed over. Bind_Texture
// later rewrites an equivalent default view once on first use.
void DX12Device_Write_SRV(unsigned slot, ID3D12Resource * resource, unsigned dxgi_format, unsigned mip_levels)
{
	if (!D3D || !SRVHeap || !resource || slot == 0 || slot >= SRV_HEAP_SIZE)
	{
		return;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = (DXGI_FORMAT)dxgi_format;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MostDetailedMip = 0;
	desc.Texture2D.MipLevels = mip_levels ? mip_levels : (UINT)-1;
	desc.Texture2D.PlaneSlice = 0;
	desc.Texture2D.ResourceMinLODClamp = 0.0f;
	D3D->CreateShaderResourceView(resource, &desc, SRV_CPU_Handle(slot));
}

bool DX12Device::Init(void * hwnd)
{
	if (Device)
	{
		return true;
	}
	if (!hwnd)
	{
		WWDEBUG_SAY(("DX12: Init needs a window"));
		return false;
	}

	WWDEBUG_SAY(("DX12: initializing Direct3D 12"));
	if (!Init_Device((HWND)hwnd))
	{
		WWDEBUG_SAY(("DX12: initialization failed"));
		if (Queue && Fence && FenceEvent)
		{
			Wait_For_Fence(Signal_Queue());
		}
		Release_All();
		return false;
	}

	Device = D3D.Get();
	Width = 0;
	Height = 0;
	return true;
}

void DX12Device::Shutdown()
{
	if (!D3D)
	{
		Device = nullptr;
		return;
	}

	if (Recording)
	{
		End_Frame(false);
	}
	Execute_Uploads_Now();
	Wait_For_Fence(Signal_Queue());

	// Resources released from here on are released immediately.
	Device = nullptr;
	Release_All();
	Width = 0;
	Height = 0;
}

const char * DX12Device::Get_Adapter_Name()
{
	return AdapterName;
}

unsigned DX12Device::Get_Vendor_Id()
{
	return AdapterDesc.VendorId;
}

unsigned DX12Device::Get_Device_Id()
{
	return AdapterDesc.DeviceId;
}

unsigned long long DX12Device::Get_Dedicated_Video_Memory()
{
	return AdapterDesc.DedicatedVideoMemory;
}

int DX12Device::Get_Display_Mode_Count()
{
	return (int)DisplayModes.size();
}

DX12Device::DisplayMode DX12Device::Get_Display_Mode(int index)
{
	if (index < 0 || index >= (int)DisplayModes.size())
	{
		DisplayMode none = { 0, 0 };
		return none;
	}
	return DisplayModes[index];
}

bool DX12Device::Set_Resolution(unsigned width, unsigned height)
{
	if (!Device || width == 0 || height == 0)
	{
		return false;
	}
	if (Has_Render_Target() && width == Width && height == Height)
	{
		return true;
	}

	// Every reference to the back buffers has to be gone before ResizeBuffers.
	if (Recording)
	{
		End_Frame(false);
	}
	Wait_For_Fence(Signal_Queue());
	Process_Deferred();

	for (int i = 0; i < BACK_BUFFER_COUNT; ++i)
	{
		BackBuffers[i].Reset();
	}
	DepthBuffer.Reset();

	HRESULT hr;
	if (!SwapChain)
	{
		DXGI_SWAP_CHAIN_DESC1 desc;
		memset(&desc, 0, sizeof(desc));
		desc.Width = width;
		desc.Height = height;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.Stereo = FALSE;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = BACK_BUFFER_COUNT;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		desc.Flags = 0;

		ComPtr<IDXGISwapChain1> swap_chain;
		hr = Factory->CreateSwapChainForHwnd(Queue.Get(), Window, &desc, nullptr, nullptr, &swap_chain);
		if (FAILED(hr))
		{
			Log_Failure("CreateSwapChainForHwnd", hr);
			return false;
		}
		hr = swap_chain.As(&SwapChain);
		if (FAILED(hr))
		{
			Log_Failure("QueryInterface (IDXGISwapChain3)", hr);
			return false;
		}

		// Fullscreen is a borderless window managed by the game, never DXGI exclusive mode.
		Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);
	}
	else
	{
		hr = SwapChain->ResizeBuffers(BACK_BUFFER_COUNT, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
		if (FAILED(hr))
		{
			Log_Failure("ResizeBuffers", hr);
			return false;
		}
	}

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = RTVHeap->GetCPUDescriptorHandleForHeapStart();
	for (int i = 0; i < BACK_BUFFER_COUNT; ++i)
	{
		hr = SwapChain->GetBuffer(i, IID_PPV_ARGS(&BackBuffers[i]));
		if (FAILED(hr))
		{
			Log_Failure("GetBuffer", hr);
			for (int j = 0; j < BACK_BUFFER_COUNT; ++j)
			{
				BackBuffers[j].Reset();
			}
			return false;
		}
		D3D->CreateRenderTargetView(BackBuffers[i].Get(), nullptr, rtv);
		rtv.ptr += RTVDescriptorSize;
	}

	if (!Create_Depth_Buffer(width, height))
	{
		return false;
	}

	BackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
	Width = width;
	Height = height;
	Set_Full_Viewport();

	WWDEBUG_SAY(("DX12: swap chain %ux%u", width, height));
	return true;
}

void DX12Device::Begin_Frame()
{
	if (!Device || Recording)
	{
		return;
	}

	Prepare_Frame_Slot();

	FrameSlot & frame = Frames[FrameIndex];
	HRESULT hr = frame.Allocator->Reset();
	if (FAILED(hr))
	{
		Log_Failure("CommandAllocator::Reset", hr);
		return;
	}
	hr = CommandList->Reset(frame.Allocator.Get(), nullptr);
	if (FAILED(hr))
	{
		Log_Failure("CommandList::Reset", hr);
		return;
	}
	Recording = true;

	BoundPipelineState = nullptr;
	BoundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	LastVSConstantsGPU = 0;
	LastPSConstantsGPU = 0;

	ID3D12DescriptorHeap * heaps[] = { SRVHeap.Get(), SamplerHeap.Get() };
	CommandList->SetDescriptorHeaps(2, heaps);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());

	if (SwapChain)
	{
		BackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
	}
	FrameHasRenderTarget = Has_Render_Target();
	if (FrameHasRenderTarget)
	{
		Transition(CommandList.Get(), BackBuffers[BackBufferIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

		D3D12_CPU_DESCRIPTOR_HANDLE rtv = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += (SIZE_T)BackBufferIndex * RTVDescriptorSize;
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = DSVHeap->GetCPUDescriptorHandleForHeapStart();
		CommandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	}

	Set_Full_Viewport();
	Record_Uploads(CommandList.Get());
}

void DX12Device::End_Frame(bool present)
{
	if (!Device)
	{
		return;
	}

	if (!Recording)
	{
		if (!present && !HasPendingUploads.load())
		{
			return;
		}
		Begin_Frame();
		if (!Recording)
		{
			return;
		}
	}

	Record_Uploads(CommandList.Get());

	bool has_render_target = FrameHasRenderTarget;
	if (has_render_target)
	{
		Transition(CommandList.Get(), BackBuffers[BackBufferIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	}

	HRESULT hr = CommandList->Close();
	Recording = false;
	FrameHasRenderTarget = false;
	if (SUCCEEDED(hr))
	{
		ID3D12CommandList * lists[] = { CommandList.Get() };
		Queue->ExecuteCommandLists(1, lists);
	}
	else
	{
		Log_Failure("CommandList::Close", hr);
	}

	if (present && has_render_target)
	{
		// Report each new Present error once instead of every frame.
		static HRESULT last_present_error = S_OK;
		hr = SwapChain->Present(0, 0);
		if (FAILED(hr) && hr != last_present_error)
		{
			Log_Failure("Present", hr);
		}
		last_present_error = FAILED(hr) ? hr : S_OK;
	}

	Frames[FrameIndex].FenceValue = Signal_Queue();
	FrameIndex = (FrameIndex + 1) % FRAME_COUNT;
	FrameSlotPrepared = false;

	Process_Deferred();
	++FrameNumber;
}

void DX12Device::Clear(bool clear_color, bool clear_z_stencil, unsigned argb, float z, unsigned stencil)
{
	if (!Device || (!clear_color && !clear_z_stencil))
	{
		return;
	}

	Ensure_Recording();
	if (!Recording || !FrameHasRenderTarget)
	{
		return;
	}

	// Like Direct3D 8, clear only the current viewport.
	D3D12_RECT rect = CurrentScissor;
	if (rect.right > (LONG)Width)
		rect.right = (LONG)Width;
	if (rect.bottom > (LONG)Height)
		rect.bottom = (LONG)Height;
	if (rect.right <= rect.left || rect.bottom <= rect.top)
	{
		return;
	}

	if (clear_color)
	{
		float color[4];
		Color_To_Float4(argb, color);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += (SIZE_T)BackBufferIndex * RTVDescriptorSize;
		CommandList->ClearRenderTargetView(rtv, color, 1, &rect);
	}

	if (clear_z_stencil)
	{
		if (z < 0.0f)
			z = 0.0f;
		if (z > 1.0f)
			z = 1.0f;
		CommandList->ClearDepthStencilView(DSVHeap->GetCPUDescriptorHandleForHeapStart(),
			D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, z, (UINT8)(stencil & 0xff), 1, &rect);
	}
}

void DX12Device::Set_Viewport(unsigned x, unsigned y, unsigned width, unsigned height, float min_z, float max_z)
{
	if (!Device)
	{
		return;
	}

	// Starting the frame first keeps Begin_Frame from resetting this viewport.
	Ensure_Recording();

	if (min_z < 0.0f)
		min_z = 0.0f;
	if (min_z > 1.0f)
		min_z = 1.0f;
	if (max_z < 0.0f)
		max_z = 0.0f;
	if (max_z > 1.0f)
		max_z = 1.0f;

	CurrentViewport.TopLeftX = (float)x;
	CurrentViewport.TopLeftY = (float)y;
	CurrentViewport.Width = (float)width;
	CurrentViewport.Height = (float)height;
	CurrentViewport.MinDepth = min_z;
	CurrentViewport.MaxDepth = max_z;

	CurrentScissor.left = (LONG)x;
	CurrentScissor.top = (LONG)y;
	CurrentScissor.right = (LONG)(x + width);
	CurrentScissor.bottom = (LONG)(y + height);

	Apply_Viewport();
}

void DX12Device::Draw(const DX12DrawDesc & desc)
{
	if (!Device)
	{
		return;
	}
	if (desc.VertexAddress == 0 || desc.VertexStride == 0 || desc.VertexBufferSize == 0)
	{
		return;
	}

	const bool indexed = desc.IndexAddress != 0;
	if (indexed)
	{
		if (desc.IndexCount == 0 || desc.IndexBufferSize == 0)
		{
			return;
		}
		if ((unsigned long long)desc.StartIndex + desc.IndexCount > desc.IndexBufferSize / 2
			|| desc.BaseVertex < 0 || (unsigned long long)desc.BaseVertex * desc.VertexStride >= desc.VertexBufferSize)
		{
			if (!LoggedIndexOverflow)
			{
				WWDEBUG_SAY(("DX12: indexed draw out of range (indices %u+%u of %u, base vertex %d of %u), draw skipped",
					desc.StartIndex, desc.IndexCount, desc.IndexBufferSize / 2, desc.BaseVertex, desc.VertexBufferSize / desc.VertexStride));
				LoggedIndexOverflow = true;
			}
			return;
		}
	}
	else
	{
		if (desc.VertexCount == 0)
		{
			return;
		}
		if (((unsigned long long)desc.StartVertex + desc.VertexCount) * desc.VertexStride > desc.VertexBufferSize)
		{
			if (!LoggedVertexOverflow)
			{
				WWDEBUG_SAY(("DX12: draw out of range (vertices %u+%u of %u), draw skipped",
					desc.StartVertex, desc.VertexCount, desc.VertexBufferSize / desc.VertexStride));
				LoggedVertexOverflow = true;
			}
			return;
		}
	}

	D3D_PRIMITIVE_TOPOLOGY topology;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
	switch (desc.PrimitiveType)
	{
	case PRIM_POINTLIST:
		topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		break;
	case PRIM_LINELIST:
		topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		break;
	case PRIM_LINESTRIP:
		topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		break;
	case PRIM_TRIANGLELIST:
		topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		break;
	case PRIM_TRIANGLESTRIP:
		topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		break;
	case PRIM_TRIANGLEFAN:
		if (!LoggedTriangleFan)
		{
			WWDEBUG_SAY(("DX12: triangle fans must be converted to lists before DX12Device::Draw, draw skipped"));
			LoggedTriangleFan = true;
		}
		return;
	default:
		return;
	}

	FVFLayout layout;
	if (!Decode_FVF(desc.FVF, layout) || layout.Size > desc.VertexStride)
	{
		if (LastBadFVF != desc.FVF)
		{
			WWDEBUG_SAY(("DX12: unsupported FVF 0x%x with stride %u, draw skipped", desc.FVF, desc.VertexStride));
			LastBadFVF = desc.FVF;
		}
		return;
	}

	Ensure_Recording();
	if (!Recording || !FrameHasRenderTarget)
	{
		return;
	}
	Record_Uploads(CommandList.Get());

	// Pipeline state. States that do not matter are normalized to share pipeline states.
	PSOKey key;
	memset(&key, 0, sizeof(key));
	key.FVF = desc.FVF;
	key.TopologyType = (unsigned)topology_type;
	key.AlphaBlendEnable = desc.AlphaBlendEnable ? 1 : 0;
	if (desc.AlphaBlendEnable)
	{
		key.SrcBlend = desc.SrcBlend;
		key.DestBlend = desc.DestBlend;
		key.BlendOp = desc.BlendOp;
	}
	key.ColorWriteMask = desc.ColorWriteMask & 0xf;
	key.ZEnable = desc.ZEnable ? 1 : 0;
	if (desc.ZEnable)
	{
		key.ZWriteEnable = desc.ZWriteEnable ? 1 : 0;
		key.ZFunc = desc.ZFunc;
	}
	key.CullMode = desc.CullMode;
	key.FillMode = desc.FillMode;
	key.DepthBias = desc.DepthBias;

	ID3D12PipelineState * pso = Get_Pipeline_State(key, layout);
	if (!pso)
	{
		return;
	}

	// Textures. A stage after a disabled stage is unused and gets the white texture.
	bool stage_enabled[DX12_MAX_STAGES];
	stage_enabled[0] = desc.ColorOp[0] > TOP_DISABLE;
	stage_enabled[1] = stage_enabled[0] && desc.ColorOp[1] > TOP_DISABLE;

	D3D12_RESOURCE_BARRIER barriers[DX12_MAX_STAGES];
	unsigned barrier_count = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE textures[DX12_MAX_STAGES];
	D3D12_GPU_DESCRIPTOR_HANDLE samplers[DX12_MAX_STAGES];
	for (int stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		textures[stage] = Bind_Texture(stage_enabled[stage] ? desc.Textures[stage] : nullptr, barriers, barrier_count);
		samplers[stage] = Get_Sampler(desc.MinFilter[stage], desc.MagFilter[stage], desc.MipFilter[stage],
			desc.AddressU[stage], desc.AddressV[stage], desc.MaxAnisotropy[stage]);
	}
	if (barrier_count > 0)
	{
		CommandList->ResourceBarrier(barrier_count, barriers);
	}

	// Vertex shader constants.
	DX12VSConstants vs;
	memset(&vs, 0, sizeof(vs));
	{
		float identity[16];
		Set_Identity(identity);
		const float * world = desc.World ? desc.World : identity;
		const float * view = desc.View ? desc.View : identity;
		const float * projection = desc.Projection ? desc.Projection : identity;
		float world_view[16];
		Multiply_Matrix(world, view, world_view);
		Multiply_Matrix(world_view, projection, vs.WorldViewProjection);
	}

	float vp_x = CurrentViewport.TopLeftX;
	float vp_y = CurrentViewport.TopLeftY;
	float vp_w = CurrentViewport.Width >= 1.0f ? CurrentViewport.Width : 1.0f;
	float vp_h = CurrentViewport.Height >= 1.0f ? CurrentViewport.Height : 1.0f;

	// Direct3D 8 puts pixel centers on integer coordinates, Direct3D 12 half a pixel further.
	// Pretransformed vertices: render target pixels plus half a pixel, through the viewport to NDC.
	vs.ScreenToClip[0] = 2.0f / vp_w;
	vs.ScreenToClip[1] = -2.0f / vp_h;
	vs.ScreenToClip[2] = (0.5f - vp_x) * 2.0f / vp_w - 1.0f;
	vs.ScreenToClip[3] = 1.0f - (0.5f - vp_y) * 2.0f / vp_h;

	// Pretransformed depth is final; undo the viewport depth mapping.
	float depth_range = CurrentViewport.MaxDepth - CurrentViewport.MinDepth;
	if (depth_range > 1.0e-6f || depth_range < -1.0e-6f)
	{
		vs.DepthToClip[0] = 1.0f / depth_range;
		vs.DepthToClip[1] = -CurrentViewport.MinDepth / depth_range;
	}
	else
	{
		vs.DepthToClip[0] = 1.0f;
		vs.DepthToClip[1] = 0.0f;
	}

	vs.PixelOffset[0] = 1.0f / vp_w;
	vs.PixelOffset[1] = -1.0f / vp_h;
	vs.TexCoordIndex[0] = desc.TexCoordIndex[0] & 0xffff;
	vs.TexCoordIndex[1] = desc.TexCoordIndex[1] & 0xffff;
	for (int i = 0; i < 4; ++i)
	{
		vs.MaterialDiffuse[i] = desc.MaterialDiffuse[i];
	}

	// Pixel shader constants.
	DX12PSConstants ps;
	memset(&ps, 0, sizeof(ps));
	for (int stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		ps.ColorStage[stage][0] = stage_enabled[stage] ? desc.ColorOp[stage] : TOP_DISABLE;
		ps.ColorStage[stage][1] = desc.ColorArg1[stage];
		ps.ColorStage[stage][2] = desc.ColorArg2[stage];
		ps.ColorStage[stage][3] = desc.ColorArg0[stage];
		ps.AlphaStage[stage][0] = desc.AlphaOp[stage];
		ps.AlphaStage[stage][1] = desc.AlphaArg1[stage];
		ps.AlphaStage[stage][2] = desc.AlphaArg2[stage];
		ps.AlphaStage[stage][3] = desc.AlphaArg0[stage];
	}
	ps.Flags[0] = (desc.AlphaTestEnable && desc.AlphaFunc >= 1 && desc.AlphaFunc < CMP_ALWAYS) ? desc.AlphaFunc : 0;
	ps.Flags[1] = desc.AlphaRef & 0xff;
	ps.Flags[2] = desc.SpecularEnable ? 1 : 0;
	Color_To_Float4(desc.TextureFactor, ps.TextureFactor);

	UINT64 vs_gpu = Upload_Constants(&vs, sizeof(vs), &LastVSConstants, LastVSConstantsGPU);
	UINT64 ps_gpu = Upload_Constants(&ps, sizeof(ps), &LastPSConstants, LastPSConstantsGPU);
	if (vs_gpu == 0 || ps_gpu == 0)
	{
		return;
	}

	if (pso != BoundPipelineState)
	{
		CommandList->SetPipelineState(pso);
		BoundPipelineState = pso;
	}
	if (topology != BoundTopology)
	{
		CommandList->IASetPrimitiveTopology(topology);
		BoundTopology = topology;
	}

	D3D12_VERTEX_BUFFER_VIEW vertex_view;
	vertex_view.BufferLocation = desc.VertexAddress;
	vertex_view.SizeInBytes = desc.VertexBufferSize;
	vertex_view.StrideInBytes = desc.VertexStride;
	CommandList->IASetVertexBuffers(0, 1, &vertex_view);

	if (indexed)
	{
		D3D12_INDEX_BUFFER_VIEW index_view;
		index_view.BufferLocation = desc.IndexAddress;
		index_view.SizeInBytes = desc.IndexBufferSize;
		index_view.Format = DXGI_FORMAT_R16_UINT;
		CommandList->IASetIndexBuffer(&index_view);
	}

	CommandList->SetGraphicsRootConstantBufferView(0, vs_gpu);
	CommandList->SetGraphicsRootConstantBufferView(1, ps_gpu);
	CommandList->SetGraphicsRootDescriptorTable(2, textures[0]);
	CommandList->SetGraphicsRootDescriptorTable(3, textures[1]);
	CommandList->SetGraphicsRootDescriptorTable(4, samplers[0]);
	CommandList->SetGraphicsRootDescriptorTable(5, samplers[1]);

	if (indexed)
	{
		CommandList->DrawIndexedInstanced(desc.IndexCount, 1, desc.StartIndex, desc.BaseVertex, 0);
	}
	else
	{
		CommandList->DrawInstanced(desc.VertexCount, 1, desc.StartVertex, 0);
	}
}

unsigned DX12Device::Allocate_SRV_Slot()
{
	std::lock_guard<std::mutex> lock(ResourceMutex);
	if (!FreeSRVSlots.empty())
	{
		unsigned slot = FreeSRVSlots.back();
		FreeSRVSlots.pop_back();
		return slot;
	}
	if (NextSRVSlot < SRV_HEAP_SIZE)
	{
		return NextSRVSlot++;
	}
	WWDEBUG_SAY(("DX12: out of shader resource view slots"));
	return 0;
}

void DX12Device::Free_SRV_Slot(unsigned slot)
{
	if (slot == 0 || slot >= SRV_HEAP_SIZE || !Device)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(ResourceMutex);
	DeferredSlot deferred;
	deferred.FenceValue = NextFenceValue;
	deferred.Slot = slot;
	DeferredSRVFrees.push_back(deferred);
}

void DX12Device::Release_When_Idle(ID3D12Resource * resource)
{
	if (!resource)
	{
		return;
	}
	if (!Device)
	{
		resource->Release();
		return;
	}
	std::lock_guard<std::mutex> lock(ResourceMutex);
	DeferredRelease deferred;
	deferred.FenceValue = NextFenceValue;
	deferred.Resource = resource;
	DeferredReleases.push_back(deferred);
}

void DX12Device::Queue_Texture_Upload(DX12TextureClass * texture, unsigned level, ID3D12Resource * upload)
{
	if (!upload)
	{
		return;
	}
	if (!Device || !texture)
	{
		upload->Release();
		return;
	}

	UINT64 bytes = upload->GetDesc().Width;
	bool too_many;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		texture->Add_Ref();
		PendingUpload pending;
		pending.Texture = texture;
		pending.Level = level;
		pending.Upload = upload;
		pending.Bytes = bytes;
		PendingUploads.push_back(pending);
		PendingUploadBytes += bytes;
		HasPendingUploads = true;
		too_many = PendingUploadBytes > MAX_PENDING_UPLOAD_BYTES;
	}

	// Loading screens create many textures between frames; do not let the staging memory pile up.
	if (too_many && Is_Main_Thread())
	{
		if (Recording)
			Record_Uploads(CommandList.Get());
		else
			Execute_Uploads_Now();
	}
}

bool DX12Device::Allocate_Frame_Memory(unsigned size, void ** cpu, unsigned long long * gpu)
{
	if (!Device || !cpu || !gpu)
	{
		return false;
	}
	WWASSERT(Is_Main_Thread());

	if (size > 0xffffffffu - (CONSTANT_ALIGNMENT - 1))
	{
		return false;
	}
	unsigned aligned = (size + (CONSTANT_ALIGNMENT - 1)) & ~(unsigned)(CONSTANT_ALIGNMENT - 1);
	if (aligned == 0)
	{
		aligned = CONSTANT_ALIGNMENT;
	}

	Prepare_Frame_Slot();
	FrameSlot & frame = Frames[FrameIndex];

	FramePage * page = frame.Extra.empty() ? &frame.Memory : &frame.Extra.back();
	if (!page->Resource || aligned > page->Size - page->Offset)
	{
		// Overflow goes to an extra page; the next use of this slot gets one big enough page.
		FramePage extra;
		unsigned extra_size = aligned > FRAME_MEMORY_EXTRA_PAGE_SIZE ? aligned : FRAME_MEMORY_EXTRA_PAGE_SIZE;
		if (!Create_Frame_Page(extra_size, extra))
		{
			return false;
		}
		frame.Extra.push_back(extra);
		page = &frame.Extra.back();
	}

	*cpu = page->CPU + page->Offset;
	*gpu = page->GPU + page->Offset;
	page->Offset += aligned;
	return true;
}
