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
// Draws: DX8Wrapper describes each draw with DX12DrawDesc. The fixed-function state picks a
// generated vertex and pixel shader (dx12ffp.h), the vertex format picks an input layout, and
// both shaders and the render states pick a pipeline state. Game shader handles replace the
// generated vertex shader (with an input layout from the Direct3D 8 vertex declaration) or pixel
// shader (compiled per wrapper variant: projection, alpha test, fog) with HLSL ports of the game's
// shaders (dx12gameshaders.h).
//
// Root signature: b0 fixed-function VS constants, b1 fixed-function PS constants, b2 game PS
// constants, b3 game VS constants (root CBVs), then one descriptor table per stage for t0..t7 and
// for s0..s7.
//
// -dx12debug: the debug layer is enabled (GPU-based validation stays off) and its messages are
// written to dx12debug.log every frame, together with shader compile errors and failed calls.
//
// Render to texture: Set_Render_Target selects a render target texture (or the back buffer) and a
// depth target (or the default depth buffer, or none). The selection lives outside the command
// list, so every new command list (Begin_Frame, Flush_Frame_And_Wait) binds it again. Pipeline
// states are keyed on the bound target formats. Render and depth target textures track their own
// resource state; Bind_Targets, Bind_Texture, Record_Uploads and Copy_Target_To_Texture move them
// between states as they are used.

#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/Backend/DX12/dx12ffp.h"
#include "WW3D2/Backend/DX12/dx12gameshaders.h"
#include "WW3D2/Backend/DX12/dx12resource.h"
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
#include <string>
#include <unordered_map>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using Microsoft::WRL::ComPtr;

void DX12Device_Write_SRV(unsigned slot, ID3D12Resource * resource, unsigned dxgi_format, unsigned mip_levels, unsigned component_mapping);

bool DX12Device_DebugLayer = false;

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
	// RTV slots 0 .. BACK_BUFFER_COUNT - 1 are the back buffers, DSV slot 0 the default depth buffer.
	RTV_HEAP_SIZE = BACK_BUFFER_COUNT + 256,
	DSV_HEAP_SIZE = 64,
	SRV_HEAP_SIZE = 16384,
	SAMPLER_HEAP_SIZE = 2048,
	FRAME_MEMORY_SIZE = 32 * 1024 * 1024,
	FRAME_MEMORY_EXTRA_PAGE_SIZE = 4 * 1024 * 1024,
	FRAME_MEMORY_MAX_SIZE = 256 * 1024 * 1024,
	MAX_PENDING_UPLOAD_BYTES = 64 * 1024 * 1024,
	CONSTANT_ALIGNMENT = 256,
	MAX_INPUT_ELEMENTS = 16,

	// Root signature parameters.
	ROOT_VS_CONSTANTS = 0,
	ROOT_PS_CONSTANTS = 1,
	ROOT_GAME_PS_CONSTANTS = 2,
	ROOT_GAME_VS_CONSTANTS = 3,
	ROOT_TEXTURES = 4,
	ROOT_SAMPLERS = ROOT_TEXTURES + DX12_MAX_STAGES,
	ROOT_PARAMETER_COUNT = ROOT_SAMPLERS + DX12_MAX_STAGES,
};

static_assert(DX12_GAME_MAX_INPUTS <= MAX_INPUT_ELEMENTS, "game vertex shader inputs must fit an input layout");

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

// D3DSTENCILOP_KEEP .. D3DSTENCILOP_DECR and D3DCMP_NEVER .. D3DCMP_ALWAYS have the same values as
// D3D12_STENCIL_OP and D3D12_COMPARISON_FUNC.
enum
{
	STENCILOP_KEEP = 1,
	STENCILOP_DECR = 8,
	CMP_NEVER = 1,
	CMP_ALWAYS = 8,
};

// BoundStencilRef before the first OMSetStencilRef of a command list. References are 8-bit values.
const UINT NO_STENCIL_REF = 0xffffffff;

// DX12DrawDesc::DepthBias is D3DRS_ZBIAS (0..16, larger is closer). One step moves depth by about
// 0.000005 (the usual Direct3D 8 to 9 conversion), which is -84 units of a 24-bit depth buffer.
const int ZBIAS_TO_DEPTH_BIAS = -84;

// dx12debug.log keeps the full text of this many messages per message ID; later ones are counted.
const unsigned DEBUG_LOG_MAX_MESSAGES_PER_ID = 1000;

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
	unsigned VertexShader;	// ShaderEntry::Id
	unsigned PixelShader;
	unsigned FVF;
	unsigned InputLayout;	// game vertex shader input layout id, 0 = the FVF layout
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
	// Stencil, all zero when D3DRS_STENCILENABLE is off. The reference value is not part of the
	// pipeline state; Draw sets it with OMSetStencilRef.
	unsigned StencilEnable;
	unsigned StencilFail;	// D3DSTENCILOP, numbered like D3D12_STENCIL_OP
	unsigned StencilZFail;
	unsigned StencilPass;
	unsigned StencilFunc;	// D3DCMPFUNC, numbered like D3D12_COMPARISON_FUNC
	unsigned StencilReadMask;	// truncated to the 8 bits of the stencil plane
	unsigned StencilWriteMask;
	// DXGI_FORMAT of the bound render target and depth stencil view. Without a depth stencil view
	// (DXGI_FORMAT_UNKNOWN) the depth, stencil and depth bias fields are all zero.
	unsigned RTVFormat;
	unsigned DSVFormat;
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
};

// A generated shader. A failed compile is cached with null Code, so it is reported only once.
struct ShaderEntry
{
	ShaderEntry() : Id(0) {}

	ComPtr<ID3DBlob> Code;
	unsigned Id;
};

template <class Key>
struct FFPKeyHash
{
	size_t operator()(const Key & key) const { return DX12FFP_Hash(&key, sizeof(Key)); }
};

template <class Key>
struct FFPKeyEqual
{
	bool operator()(const Key & a, const Key & b) const { return memcmp(&a, &b, sizeof(Key)) == 0; }
};

typedef std::unordered_map<DX12VSKey, ShaderEntry, FFPKeyHash<DX12VSKey>, FFPKeyEqual<DX12VSKey>> VertexShaderMap;
typedef std::unordered_map<DX12PSKey, ShaderEntry, FFPKeyHash<DX12PSKey>, FFPKeyEqual<DX12PSKey>> PixelShaderMap;

struct GamePSVariantLess
{
	bool operator()(const DX12GamePSVariant & a, const DX12GamePSVariant & b) const { return memcmp(&a, &b, sizeof(a)) < 0; }
};

// Compiled code of one registered game shader: the vertex shader, or each pixel shader variant.
struct GameShaderCache
{
	GameShaderCache() : VertexCompiled(false), Draws(0) {}

	ShaderEntry Vertex;
	bool VertexCompiled;
	unsigned Draws; // draws since the last heartbeat, for -dx12debug
	std::map<DX12GamePSVariant, ShaderEntry, GamePSVariantLess> Pixel;
};

// A game vertex shader input layout; its id is the index plus 1.
struct GameLayout
{
	DX12GameInputLayout Source;
	FVFLayout Layout;
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
UINT DSVDescriptorSize = 0;
// Render target and depth stencil view slots of render and depth target textures. Guarded by
// ResourceMutex.
unsigned NextRTVSlot = BACK_BUFFER_COUNT;
std::vector<unsigned> FreeRTVSlots;
unsigned NextDSVSlot = 1;
std::vector<unsigned> FreeDSVSlots;
UINT LastPresentedIndex = 0;
bool HasPresented = false;

// Render to texture, main thread only. The selected targets are owned references; a null color
// target is the back buffer. TargetHasDepth says whether the targets bound by Bind_Targets include
// a depth stencil view.
DX12TextureClass * TargetColor = nullptr;
DX12TextureClass * TargetDepth = nullptr;
bool TargetDefaultDepth = true;
bool TargetHasDepth = false;

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
VertexShaderMap VertexShaders;
PixelShaderMap PixelShaders;
unsigned NextShaderId = 1;
std::map<PSOKey, ComPtr<ID3D12PipelineState>, PSOKeyLess> PipelineStates;

// The generated shaders of the last draw. Map elements do not move, so the pointers stay valid
// until the maps are cleared.
DX12VSKey LastVSKey;
DX12PSKey LastPSKey;
const ShaderEntry * LastVertexShader = nullptr;
const ShaderEntry * LastPixelShader = nullptr;

// Game shaders, by registry number, and their input layouts.
std::vector<GameShaderCache> GameShaderCaches;
std::vector<GameLayout> GameLayouts;
bool ColorVertexFormatSupported = true;		// DXGI_FORMAT_B8G8R8A8_UNORM in vertex buffers, for D3DVSDT_D3DCOLOR

// -dx12debug
FILE * DebugLog = nullptr;
bool DebugLogStarted = false;
std::mutex DebugLogMutex;
ComPtr<ID3D12InfoQueue> InfoQueue;
std::map<unsigned, unsigned> DebugMessageCounts;
UINT64 DebugMessagesDiscarded = 0;
// Render-to-texture activity since the last heartbeat, for the log.
unsigned DebugTargetSelects = 0;
unsigned DebugTargetCopies = 0;

ComPtr<ID3D12Resource> WhiteTexture;

D3D12_VIEWPORT CurrentViewport;
D3D12_RECT CurrentScissor;

// Per-frame command list state, reset by Begin_Frame.
ID3D12PipelineState * BoundPipelineState = nullptr;
D3D_PRIMITIVE_TOPOLOGY BoundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
UINT BoundStencilRef = NO_STENCIL_REF;
DX12VSConstants LastVSConstants;
UINT64 LastVSConstantsGPU = 0;
DX12PSConstants LastPSConstants;
UINT64 LastPSConstantsGPU = 0;
DX12GameVSConstants LastGameVSConstants;
UINT64 LastGameVSConstantsGPU = 0;
DX12GamePSConstants LastGamePSConstants;
UINT64 LastGamePSConstantsGPU = 0;

// Bound for stages whose sampler no shader reads. Valid while the sampler heap exists.
D3D12_GPU_DESCRIPTOR_HANDLE DefaultSampler;

bool LoggedTriangleFan = false;
bool LoggedIndexOverflow = false;
bool LoggedVertexOverflow = false;
bool LoggedSamplerHeapFull = false;
bool LoggedGameLayoutStride = false;
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

// Appends a line to dx12debug.log when -dx12debug opened it.
void Debug_Log(const char * format, ...)
{
	std::lock_guard<std::mutex> lock(DebugLogMutex);
	if (!DebugLog)
	{
		return;
	}
	va_list args;
	va_start(args, format);
	vfprintf(DebugLog, format, args);
	va_end(args);
	fputc('\n', DebugLog);
}

// The first open in a process starts a new log, later ones (after a device shutdown) append.
void Open_Debug_Log()
{
	std::lock_guard<std::mutex> lock(DebugLogMutex);
	if (DebugLog)
	{
		return;
	}
	DebugLog = fopen("dx12debug.log", DebugLogStarted ? "a" : "w");
	if (!DebugLog)
	{
		WWDEBUG_SAY(("DX12: could not open dx12debug.log"));
		return;
	}
	DebugLogStarted = true;
	DebugMessageCounts.clear();
	DebugMessagesDiscarded = 0;
}

const char * Severity_Name(D3D12_MESSAGE_SEVERITY severity)
{
	switch (severity)
	{
	case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
	case D3D12_MESSAGE_SEVERITY_ERROR: return "ERROR";
	case D3D12_MESSAGE_SEVERITY_WARNING: return "WARNING";
	case D3D12_MESSAGE_SEVERITY_INFO: return "INFO";
	case D3D12_MESSAGE_SEVERITY_MESSAGE: return "MESSAGE";
	default: return "UNKNOWN";
	}
}

// Moves the debug layer messages to dx12debug.log and flushes it. Main thread only.
void Flush_Debug_Messages()
{
	if (InfoQueue)
	{
		// Re-read the count so messages stored by other threads meanwhile are not cleared unlogged.
		std::vector<char> buffer;
		for (UINT64 i = 0; i < InfoQueue->GetNumStoredMessages(); ++i)
		{
			SIZE_T length = 0;
			if (FAILED(InfoQueue->GetMessage(i, nullptr, &length)) || length < sizeof(D3D12_MESSAGE))
			{
				continue;
			}
			buffer.resize(length);
			D3D12_MESSAGE * message = reinterpret_cast<D3D12_MESSAGE *>(&buffer[0]);
			if (FAILED(InfoQueue->GetMessage(i, message, &length)))
			{
				continue;
			}

			unsigned & seen = DebugMessageCounts[(unsigned)message->ID];
			++seen;
			if (seen <= DEBUG_LOG_MAX_MESSAGES_PER_ID)
			{
				Debug_Log("frame %llu: %s #%d: %s", DX12Device::Get_Frame_Number(), Severity_Name(message->Severity),
					(int)message->ID, message->pDescription ? message->pDescription : "");
			}
			if (seen == DEBUG_LOG_MAX_MESSAGES_PER_ID)
			{
				Debug_Log("frame %llu: message #%d logged %u times, further ones are only counted",
					DX12Device::Get_Frame_Number(), (int)message->ID, seen);
			}
		}

		UINT64 discarded = InfoQueue->GetNumMessagesDiscardedByMessageCountLimit();
		if (discarded > DebugMessagesDiscarded)
		{
			Debug_Log("frame %llu: %llu messages were discarded by the message count limit", DX12Device::Get_Frame_Number(), discarded - DebugMessagesDiscarded);
		}
		InfoQueue->ClearStoredMessages();
		DebugMessagesDiscarded = InfoQueue->GetNumMessagesDiscardedByMessageCountLimit();
	}

	std::lock_guard<std::mutex> lock(DebugLogMutex);
	if (DebugLog)
	{
		fflush(DebugLog);
	}
}

void Close_Debug_Log()
{
	for (std::map<unsigned, unsigned>::const_iterator it = DebugMessageCounts.begin(); it != DebugMessageCounts.end(); ++it)
	{
		if (it->second > DEBUG_LOG_MAX_MESSAGES_PER_ID)
		{
			Debug_Log("message #%d occurred %u times in total", (int)it->first, it->second);
		}
	}
	Debug_Log("shutdown");

	std::lock_guard<std::mutex> lock(DebugLogMutex);
	if (DebugLog)
	{
		fclose(DebugLog);
		DebugLog = nullptr;
	}
}

void Log_Failure(const char * what, HRESULT hr)
{
	WWDEBUG_SAY(("DX12: %s failed (0x%08x)", what, (unsigned)hr));
	Debug_Log("frame %llu: %s failed (0x%08x)", DX12Device::Get_Frame_Number(), what, (unsigned)hr);
	if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && D3D)
	{
		WWDEBUG_SAY(("DX12: device removed, reason 0x%08x", (unsigned)D3D->GetDeviceRemovedReason()));
		Debug_Log("device removed, reason 0x%08x", (unsigned)D3D->GetDeviceRemovedReason());
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
		// Uploads replace whole levels, which initializes a render target as a clear would.
		texture->Set_Initialized();
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

unsigned Target_Width()
{
	return TargetColor ? TargetColor->Get_Width() : DX12Device::Get_Width();
}

unsigned Target_Height()
{
	return TargetColor ? TargetColor->Get_Height() : DX12Device::Get_Height();
}

D3D12_CPU_DESCRIPTOR_HANDLE RTV_Handle(unsigned slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)slot * RTVDescriptorSize;
	return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DSV_Handle(unsigned slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = DSVHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)slot * DSVDescriptorSize;
	return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE Current_RTV()
{
	return TargetColor ? RTV_Handle(TargetColor->Get_RTV_Slot()) : RTV_Handle(BackBufferIndex);
}

// The depth stencil view for the current color target: the selected depth target, or the default
// depth buffer when use_default_depth was set. Like Direct3D 8, a depth buffer smaller than the
// color target cannot be used, and there is none. depth_texture is set to the depth target texture,
// or null for the default depth buffer and for none.
bool Current_DSV(D3D12_CPU_DESCRIPTOR_HANDLE & dsv, DX12TextureClass ** depth_texture)
{
	const unsigned width = Target_Width();
	const unsigned height = Target_Height();
	*depth_texture = nullptr;
	if (TargetDepth)
	{
		if (TargetDepth->Get_Width() < width || TargetDepth->Get_Height() < height)
		{
			return false;
		}
		dsv = DSV_Handle(TargetDepth->Get_DSV_Slot());
		*depth_texture = TargetDepth;
		return true;
	}
	if (!TargetDefaultDepth || !DepthBuffer || DX12Device::Get_Width() < width || DX12Device::Get_Height() < height)
	{
		return false;
	}
	dsv = DSV_Handle(0);
	return true;
}

void Ensure_State(DX12TextureClass * texture, D3D12_RESOURCE_STATES state)
{
	if (texture->Get_State() != (unsigned)state)
	{
		Transition(CommandList.Get(), texture->Peek_Resource(), (D3D12_RESOURCE_STATES)texture->Get_State(), state);
		texture->Set_State(state);
	}
}

// Clears a render or depth target that has not been drawn into yet, so that it never shows
// uninitialized memory. Recording only.
void Initialize_Target(DX12TextureClass * texture)
{
	if (!texture->Needs_Initial_Clear() || !texture->Peek_Resource())
	{
		return;
	}
	if (texture->Is_Render_Target())
	{
		Ensure_State(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		CommandList->ClearRenderTargetView(RTV_Handle(texture->Get_RTV_Slot()), black, 0, nullptr);
	}
	else if (texture->Is_Depth_Target())
	{
		Ensure_State(texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		CommandList->ClearDepthStencilView(DSV_Handle(texture->Get_DSV_Slot()), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	}
	texture->Set_Initialized();
}

// Binds the selected targets to the command list being recorded.
void Bind_Targets()
{
	TargetHasDepth = false;
	if (!Recording || !FrameHasRenderTarget)
	{
		return;
	}

	if (TargetColor)
	{
		Initialize_Target(TargetColor);
		Ensure_State(TargetColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = Current_RTV();

	D3D12_CPU_DESCRIPTOR_HANDLE dsv;
	DX12TextureClass * depth_texture = nullptr;
	TargetHasDepth = Current_DSV(dsv, &depth_texture);
	if (depth_texture)
	{
		Initialize_Target(depth_texture);
		Ensure_State(depth_texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	CommandList->OMSetRenderTargets(1, &rtv, FALSE, TargetHasDepth ? &dsv : nullptr);
}

// Moves the bound target textures back to their target states, in case an upload changed them.
void Ensure_Target_States()
{
	if (TargetColor)
	{
		Ensure_State(TargetColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	if (TargetDepth && TargetHasDepth)
	{
		Ensure_State(TargetDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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

// The whole current color target.
void Set_Full_Viewport()
{
	unsigned width = Target_Width();
	unsigned height = Target_Height();

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

// Sets the state every new command list starts with: descriptor heaps, root signature, targets and
// viewport. Recording only.
void Restore_Command_List_State()
{
	BoundPipelineState = nullptr;
	BoundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	BoundStencilRef = NO_STENCIL_REF;
	LastVSConstantsGPU = 0;
	LastPSConstantsGPU = 0;
	LastGameVSConstantsGPU = 0;
	LastGamePSConstantsGPU = 0;

	ID3D12DescriptorHeap * heaps[] = { SRVHeap.Get(), SamplerHeap.Get() };
	CommandList->SetDescriptorHeaps(2, heaps);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());

	Bind_Targets();
	if (CurrentViewport.Width < 1.0f || CurrentViewport.Height < 1.0f)
	{
		Set_Full_Viewport();
	}
	else
	{
		Apply_Viewport();
	}
}

// Executes what the frame recorded so far, waits for the GPU and continues the frame in a new
// command list. The back buffer stays in the render target state. Recording only.
bool Flush_Frame_And_Wait()
{
	Record_Uploads(CommandList.Get());
	HRESULT hr = CommandList->Close();
	if (SUCCEEDED(hr))
	{
		ID3D12CommandList * lists[] = { CommandList.Get() };
		Queue->ExecuteCommandLists(1, lists);
	}
	else
	{
		Log_Failure("CommandList::Close (flush)", hr);
	}
	Wait_For_Fence(Signal_Queue());
	Process_Deferred();

	// The GPU is idle, so the frame's allocator can be reset; the frame memory stays in use.
	FrameSlot & frame = Frames[FrameIndex];
	HRESULT reset = frame.Allocator->Reset();
	if (SUCCEEDED(reset))
	{
		reset = CommandList->Reset(frame.Allocator.Get(), nullptr);
	}
	if (FAILED(reset))
	{
		Log_Failure("CommandList::Reset (flush)", reset);
		Recording = false;
		FrameHasRenderTarget = false;
		return false;
	}
	Restore_Command_List_State();
	return SUCCEEDED(hr);
}

void Release_Targets()
{
	if (TargetColor)
	{
		TargetColor->Release_Ref();
		TargetColor = nullptr;
	}
	if (TargetDepth)
	{
		TargetDepth->Release_Ref();
		TargetDepth = nullptr;
	}
	TargetDefaultDepth = true;
	TargetHasDepth = false;
}

// Writes source to dx12debug.log with line numbers, matching the compiler's error positions.
void Debug_Log_Source(const std::string & source)
{
	unsigned line = 1;
	size_t start = 0;
	while (start < source.size())
	{
		size_t end = source.find('\n', start);
		if (end == std::string::npos)
			end = source.size();
		Debug_Log("%4u: %s", line++, source.substr(start, end - start).c_str());
		start = end + 1;
	}
}

ComPtr<ID3DBlob> Compile_Shader(const std::string & source, const char * name, const char * entry, const char * target)
{
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> errors;
	HRESULT hr = D3DCompileFunc(source.c_str(), source.size(), name, nullptr, nullptr, entry, target, flags, 0, &code, &errors);
	if (FAILED(hr))
	{
		const char * output = errors ? (const char *)errors->GetBufferPointer() : "no compiler output";
		WWDEBUG_SAY(("DX12: compiling %s (%s) failed (0x%08x): %s", name, target, (unsigned)hr, output));
		Debug_Log("frame %llu: compiling %s (%s) failed (0x%08x):\n%s", DX12Device::Get_Frame_Number(), name, target, (unsigned)hr, output);
		Debug_Log_Source(source);
		return nullptr;
	}
	return code;
}

const ShaderEntry & Get_Vertex_Shader(const DX12VSKey & key)
{
	VertexShaderMap::iterator it = VertexShaders.find(key);
	if (it != VertexShaders.end())
	{
		return it->second;
	}

	std::string source;
	DX12FFP_Generate_Vertex_Shader(key, source);

	char name[64];
	snprintf(name, sizeof(name), "ffp_vs_%08x", DX12FFP_Hash(&key, sizeof(key)));

	ShaderEntry entry;
	entry.Id = NextShaderId++;
	entry.Code = Compile_Shader(source, name, "VSMain", "vs_5_0");
	if (entry.Code)
	{
		Debug_Log("frame %llu: compiled %s (FVF 0x%x)", DX12Device::Get_Frame_Number(), name, key.FVF);
	}
	return VertexShaders.insert(VertexShaderMap::value_type(key, entry)).first->second;
}

const ShaderEntry & Get_Pixel_Shader(const DX12PSKey & key)
{
	PixelShaderMap::iterator it = PixelShaders.find(key);
	if (it != PixelShaders.end())
	{
		return it->second;
	}

	std::string source;
	DX12FFP_Generate_Pixel_Shader(key, source);

	char name[64];
	snprintf(name, sizeof(name), "ffp_ps_%08x", DX12FFP_Hash(&key, sizeof(key)));

	ShaderEntry entry;
	entry.Id = NextShaderId++;
	entry.Code = Compile_Shader(source, name, "PSMain", "ps_5_0");
	if (entry.Code)
	{
		Debug_Log("frame %llu: compiled %s", DX12Device::Get_Frame_Number(), name);
	}
	return PixelShaders.insert(PixelShaderMap::value_type(key, entry)).first->second;
}

// The compiled code of a registered game shader; variant is for pixel shaders. A failed compile is
// cached with null Code.
const ShaderEntry & Get_Game_Shader(int index, const DX12GamePSVariant * variant)
{
	static const ShaderEntry none;
	if (index < 0 || index >= (int)GameShaderCaches.size())
	{
		return none;
	}

	GameShaderCache & cache = GameShaderCaches[index];
	const bool vertex_shader = DX12GameShaders_Is_Vertex_Shader(index);
	DX12GamePSVariant key;
	memset(&key, 0, sizeof(key));
	ShaderEntry * entry;
	if (vertex_shader)
	{
		if (cache.VertexCompiled)
		{
			return cache.Vertex;
		}
		cache.VertexCompiled = true;
		entry = &cache.Vertex;
	}
	else
	{
		if (variant)
		{
			key = *variant;
		}
		std::map<DX12GamePSVariant, ShaderEntry, GamePSVariantLess>::iterator it = cache.Pixel.find(key);
		if (it != cache.Pixel.end())
		{
			return it->second;
		}
		entry = &cache.Pixel[key];
	}

	std::string source;
	const char * entry_point = "";
	const char * target = "";
	DX12GameShaders_Generate(index, vertex_shader ? nullptr : &key, source, &entry_point, &target);

	char name[128];
	if (vertex_shader)
		snprintf(name, sizeof(name), "game_%s", DX12GameShaders_Get_Name(index));
	else
		snprintf(name, sizeof(name), "game_%s_%08x", DX12GameShaders_Get_Name(index), DX12FFP_Hash(&key, sizeof(key)));

	entry->Id = NextShaderId++;
	entry->Code = Compile_Shader(source, name, entry_point, target);
	if (entry->Code)
	{
		Debug_Log("frame %llu: compiled %s", DX12Device::Get_Frame_Number(), name);
	}
	return *entry;
}

// Returns the id of the input layout for a game vertex shader declaration, adding it when new.
unsigned Intern_Game_Layout(const DX12GameInputLayout & source)
{
	for (size_t i = 0; i < GameLayouts.size(); ++i)
	{
		if (memcmp(&GameLayouts[i].Source, &source, sizeof(source)) == 0)
		{
			return (unsigned)i + 1;
		}
	}

	GameLayout layout;
	memset(&layout, 0, sizeof(layout));
	layout.Source = source;
	for (unsigned i = 0; i < source.ElementCount; ++i)
	{
		D3D12_INPUT_ELEMENT_DESC & element = layout.Layout.Elements[i];
		element.SemanticName = "V";
		element.SemanticIndex = source.Elements[i].Register;
		element.Format = (DXGI_FORMAT)source.Elements[i].Format;
		element.InputSlot = 0;
		element.AlignedByteOffset = source.Elements[i].Offset;
		element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		element.InstanceDataStepRate = 0;
	}
	layout.Layout.ElementCount = source.ElementCount;
	layout.Layout.Size = source.Size;
	GameLayouts.push_back(layout);
	return (unsigned)GameLayouts.size();
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

ID3D12PipelineState * Get_Pipeline_State(const PSOKey & key, const FVFLayout & layout, ID3DBlob * vertex_shader, ID3DBlob * pixel_shader)
{
	std::map<PSOKey, ComPtr<ID3D12PipelineState>, PSOKeyLess>::iterator it = PipelineStates.find(key);
	if (it != PipelineStates.end())
	{
		return it->second.Get();
	}

	ComPtr<ID3D12PipelineState> pso;
	if (vertex_shader && pixel_shader)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.pRootSignature = RootSignature.Get();
		desc.VS.pShaderBytecode = vertex_shader->GetBufferPointer();
		desc.VS.BytecodeLength = vertex_shader->GetBufferSize();
		desc.PS.pShaderBytecode = pixel_shader->GetBufferPointer();
		desc.PS.BytecodeLength = pixel_shader->GetBufferSize();

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
		D3D12_DEPTH_STENCILOP_DESC stencil_op = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
		if (key.StencilEnable)
		{
			// Direct3D 8 has no two-sided stencil, so both faces use the same operations.
			depth.StencilEnable = TRUE;
			depth.StencilReadMask = (UINT8)key.StencilReadMask;
			depth.StencilWriteMask = (UINT8)key.StencilWriteMask;
			stencil_op.StencilFailOp = (D3D12_STENCIL_OP)key.StencilFail;
			stencil_op.StencilDepthFailOp = (D3D12_STENCIL_OP)key.StencilZFail;
			stencil_op.StencilPassOp = (D3D12_STENCIL_OP)key.StencilPass;
			stencil_op.StencilFunc = (D3D12_COMPARISON_FUNC)key.StencilFunc;
		}
		else
		{
			depth.StencilEnable = FALSE;
			depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
			depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		}
		depth.FrontFace = stencil_op;
		depth.BackFace = stencil_op;

		desc.InputLayout.pInputElementDescs = layout.Elements;
		desc.InputLayout.NumElements = layout.ElementCount;
		desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		desc.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)key.TopologyType;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = (DXGI_FORMAT)key.RTVFormat;
		desc.DSVFormat = (DXGI_FORMAT)key.DSVFormat;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;

		HRESULT hr = D3D->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			WWDEBUG_SAY(("DX12: CreateGraphicsPipelineState failed (0x%08x) for FVF 0x%x", (unsigned)hr, key.FVF));
			Debug_Log("frame %llu: CreateGraphicsPipelineState failed (0x%08x) for FVF 0x%x, input layout %u, shaders %u/%u",
				DX12Device::Get_Frame_Number(), (unsigned)hr, key.FVF, key.InputLayout, key.VertexShader, key.PixelShader);
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
// adding a transition to PIXEL_SHADER_RESOURCE when needed. Null textures bind the white texture,
// and so do the textures bound as the current targets, which cannot be read while they are written.
D3D12_GPU_DESCRIPTOR_HANDLE Bind_Texture(DX12TextureClass * texture, D3D12_RESOURCE_BARRIER * barriers, unsigned & barrier_count)
{
	if (texture && !texture->Is_CPU_Only() && texture != TargetColor && !(texture == TargetDepth && TargetHasDepth))
	{
		ID3D12Resource * resource = texture->Peek_Resource();
		unsigned slot = texture->Get_SRV_Slot();
		if (resource && slot > 0 && slot < SRV_HEAP_SIZE)
		{
			if (SRVSlotResources[slot] != resource)
			{
				DX12Device_Write_SRV(slot, resource, texture->Get_DXGI_Format(), texture->Get_Level_Count(), texture->Get_SRV_Component_Mapping());
				SRVSlotResources[slot] = resource;
			}
			// Recorded before the batched transitions, while the target is still in its initial state.
			Initialize_Target(texture);
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
	// One single descriptor range per stage texture and sampler, since textures keep their own slots.
	D3D12_DESCRIPTOR_RANGE ranges[2 * DX12_MAX_STAGES];
	memset(ranges, 0, sizeof(ranges));
	for (int i = 0; i < 2 * DX12_MAX_STAGES; ++i)
	{
		ranges[i].RangeType = (i < DX12_MAX_STAGES) ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[i].NumDescriptors = 1;
		ranges[i].BaseShaderRegister = i % DX12_MAX_STAGES;
		ranges[i].RegisterSpace = 0;
		ranges[i].OffsetInDescriptorsFromTableStart = 0;
	}

	D3D12_ROOT_PARAMETER params[ROOT_PARAMETER_COUNT];
	memset(params, 0, sizeof(params));
	params[ROOT_VS_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[ROOT_VS_CONSTANTS].Descriptor.ShaderRegister = 0;
	params[ROOT_VS_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[ROOT_PS_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[ROOT_PS_CONSTANTS].Descriptor.ShaderRegister = 1;
	params[ROOT_PS_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[ROOT_GAME_PS_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[ROOT_GAME_PS_CONSTANTS].Descriptor.ShaderRegister = 2;
	params[ROOT_GAME_PS_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[ROOT_GAME_VS_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[ROOT_GAME_VS_CONSTANTS].Descriptor.ShaderRegister = 3;
	params[ROOT_GAME_VS_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	for (int i = 0; i < 2 * DX12_MAX_STAGES; ++i)
	{
		params[ROOT_TEXTURES + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[ROOT_TEXTURES + i].DescriptorTable.NumDescriptorRanges = 1;
		params[ROOT_TEXTURES + i].DescriptorTable.pDescriptorRanges = &ranges[i];
		params[ROOT_TEXTURES + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	D3D12_ROOT_SIGNATURE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.NumParameters = ROOT_PARAMETER_COUNT;
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

	// The debug layer has to be enabled before the device is created. It needs the Graphics Tools
	// optional Windows feature.
	bool debug_layer = DX12Device_DebugLayer;
#ifdef _DEBUG
	debug_layer = true;
#endif
	UINT factory_flags = 0;
	if (debug_layer)
	{
		ComPtr<ID3D12Debug> debug;
		if (D3D12GetDebugInterfaceFunc && SUCCEEDED(D3D12GetDebugInterfaceFunc(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			ComPtr<ID3D12Debug1> debug1;
			if (SUCCEEDED(debug.As(&debug1)))
			{
				debug1->SetEnableGPUBasedValidation(FALSE);
			}
			factory_flags = DXGI_CREATE_FACTORY_DEBUG;
			Debug_Log("debug layer enabled, GPU-based validation off");
		}
		else
		{
			Debug_Log("D3D12GetDebugInterface failed; the debug layer needs the Graphics Tools optional feature");
		}
	}

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

	Debug_Log("adapter %s (vendor 0x%04x, device 0x%04x)", AdapterName, AdapterDesc.VendorId, AdapterDesc.DeviceId);

	hr = D3D12CreateDeviceFunc(Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&D3D));
	if (FAILED(hr))
	{
		Log_Failure("D3D12CreateDevice", hr);
		return false;
	}

	// Messages are collected for dx12debug.log; nothing breaks into the debugger.
	if (DX12Device_DebugLayer && SUCCEEDED(D3D.As(&InfoQueue)))
	{
		InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
		InfoQueue->SetMessageCountLimit(65536);
		// Render targets are cleared to whatever color the game asks for, so they have no optimized
		// clear value; the performance warning about that would fire on every clear.
		D3D12_MESSAGE_ID denied[] = {
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
		};
		D3D12_INFO_QUEUE_FILTER filter;
		memset(&filter, 0, sizeof(filter));
		filter.DenyList.NumIDs = (UINT)(sizeof(denied) / sizeof(denied[0]));
		filter.DenyList.pIDList = denied;
		InfoQueue->AddStorageFilterEntries(&filter);
		DebugMessagesDiscarded = InfoQueue->GetNumMessagesDiscardedByMessageCountLimit();
	}
	else if (DX12Device_DebugLayer)
	{
		Debug_Log("ID3D12InfoQueue is not available, debug layer messages are not logged");
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

	RTVHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP_SIZE, false);
	DSVHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP_SIZE, false);
	SRVHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP_SIZE, true);
	SamplerHeap = Create_Descriptor_Heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE, true);
	if (!RTVHeap || !DSVHeap || !SRVHeap || !SamplerHeap)
	{
		return false;
	}
	RTVDescriptorSize = D3D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	DSVDescriptorSize = D3D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
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
		NextRTVSlot = BACK_BUFFER_COUNT;
		FreeRTVSlots.clear();
		NextDSVSlot = 1;
		FreeDSVSlots.clear();
	}

	if (!Create_Root_Signature())
	{
		return false;
	}

	if (!Create_White_Texture())
	{
		return false;
	}

	DefaultSampler = Get_Sampler(2, 2, 0, 1, 1, 1);

	// Direct3D 11 class hardware reads B8G8R8A8 vertex colors, but check rather than fail later.
	D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support;
	memset(&format_support, 0, sizeof(format_support));
	format_support.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	ColorVertexFormatSupported = SUCCEEDED(D3D->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format_support, sizeof(format_support)))
		&& (format_support.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) != 0;
	if (!ColorVertexFormatSupported)
	{
		WWDEBUG_SAY(("DX12: B8G8R8A8 vertex colors are not supported; game vertex shaders with D3DCOLOR inputs are unavailable"));
		Debug_Log("B8G8R8A8_UNORM is not a vertex format here; game vertex shaders with D3DVSDT_D3DCOLOR inputs fail");
	}

	memset(&CurrentViewport, 0, sizeof(CurrentViewport));
	memset(&CurrentScissor, 0, sizeof(CurrentScissor));
	HasPresented = false;
	return true;
}

// Releases every object owned by this file. The GPU must be idle.
void Release_All()
{
	// With the device pointer already cleared, the target textures release their resources at once.
	Release_Targets();

	std::vector<PendingUpload> uploads;
	std::vector<DeferredRelease> releases;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		uploads.swap(PendingUploads);
		releases.swap(DeferredReleases);
		DeferredSRVFrees.clear();
		FreeSRVSlots.clear();
		NextSRVSlot = 1;
		FreeRTVSlots.clear();
		NextRTVSlot = BACK_BUFFER_COUNT;
		FreeDSVSlots.clear();
		NextDSVSlot = 1;
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
	PixelShaders.clear();
	NextShaderId = 1;
	LastVertexShader = nullptr;
	LastPixelShader = nullptr;
	GameShaderCaches.clear();
	GameLayouts.clear();
	DX12GameShaders_Clear();
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
	HasPresented = false;
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

	Flush_Debug_Messages();
	InfoQueue.Reset();
	D3D.Reset();
	Adapter.Reset();
	Factory.Reset();
	DisplayModes.clear();

	BoundPipelineState = nullptr;
	BoundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	BoundStencilRef = NO_STENCIL_REF;
	LastVSConstantsGPU = 0;
	LastPSConstantsGPU = 0;
	LastGameVSConstantsGPU = 0;
	LastGamePSConstantsGPU = 0;
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

	D3D->CreateDepthStencilView(DepthBuffer.Get(), nullptr, DSV_Handle(0));
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
void DX12Device_Write_SRV(unsigned slot, ID3D12Resource * resource, unsigned dxgi_format, unsigned mip_levels, unsigned component_mapping)
{
	if (!D3D || !SRVHeap || !resource || slot == 0 || slot >= SRV_HEAP_SIZE)
	{
		return;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = (DXGI_FORMAT)dxgi_format;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = component_mapping;
	desc.Texture2D.MostDetailedMip = 0;
	desc.Texture2D.MipLevels = mip_levels ? mip_levels : (UINT)-1;
	desc.Texture2D.PlaneSlice = 0;
	desc.Texture2D.ResourceMinLODClamp = 0.0f;
	D3D->CreateShaderResourceView(resource, &desc, SRV_CPU_Handle(slot));
}

// Used by DX12TextureClass for render targets, which are always B8G8R8A8. Main thread only.
void DX12Device_Write_RTV(unsigned slot, ID3D12Resource * resource)
{
	if (!D3D || !RTVHeap || !resource || slot < BACK_BUFFER_COUNT || slot >= RTV_HEAP_SIZE)
	{
		return;
	}
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MipSlice = 0;
	desc.Texture2D.PlaneSlice = 0;
	D3D->CreateRenderTargetView(resource, &desc, RTV_Handle(slot));
}

// Used by DX12TextureClass for depth targets, which are R24G8_TYPELESS. Main thread only.
void DX12Device_Write_DSV(unsigned slot, ID3D12Resource * resource)
{
	if (!D3D || !DSVHeap || !resource || slot == 0 || slot >= DSV_HEAP_SIZE)
	{
		return;
	}
	D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	desc.Flags = D3D12_DSV_FLAG_NONE;
	desc.Texture2D.MipSlice = 0;
	D3D->CreateDepthStencilView(resource, &desc, DSV_Handle(slot));
}

// -dx12debug: runs a small game shader pair through the registry (registration, creation with a
// vertex declaration, a pixel shader variant) and creates pipeline states for every pairing with the
// generated shaders, which checks that their signatures match.
static void Run_Game_Shader_Self_Test()
{
	static const char vs_source[] =
		"struct GameVSInput\n"
		"{\n"
		"\tfloat3 Position : V0;\n"
		"\tfloat4 Diffuse : V1;\n"
		"\tfloat2 Tex : V2;\n"
		"};\n"
		"\n"
		"GameVSOutput SelfTestVS(GameVSInput input)\n"
		"{\n"
		"\tGameVSOutput output = (GameVSOutput)0;\n"
		"\tfloat4 position = float4(input.Position, 1.0);\n"
		"\toutput.Position = float4(dot(position, c[4]), dot(position, c[5]), dot(position, c[6]), dot(position, c[7]));\n"
		"\toutput.Diffuse = input.Diffuse;\n"
		"\toutput.Tex0 = float4(input.Tex, 0.0, 1.0);\n"
		"\toutput.Fog = 1.0;\n"
		"\treturn output;\n"
		"}\n";
	static const char ps_source[] =
		"float4 SelfTestPS(GamePSInput input)\n"
		"{\n"
		"\tfloat4 t0 = Texture0.Sample(Sampler0, input.Tex0.xy);\n"
		"\tfloat4 r0 = input.Diffuse * t0;\n"
		"\tr0.rgb = saturate(r0.rgb * c[0].rgb * 2.0);\n"
		"\treturn r0;\n"
		"}\n";
	// D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
	// D3DVSD_REG(2, D3DVSDT_FLOAT2), D3DVSD_END()
	static const unsigned declaration[] = { 0x20000000, 0x40020000, 0x40040001, 0x40010002, 0xffffffff };
	const unsigned fvf = FVF_XYZ | FVF_DIFFUSE | (1 << FVF_TEXCOUNT_SHIFT);

	DX12Device::Register_Game_Shader("dx12_selftest_vs", true, vs_source, "SelfTestVS");
	DX12Device::Register_Game_Shader("dx12_selftest_ps", false, ps_source, "SelfTestPS");

	const unsigned vs = DX12Device::Create_Game_Shader("dx12_selftest_vs", true, declaration);
	const unsigned ps = DX12Device::Create_Game_Shader("DX12_SelfTest_PS", false, nullptr);	// names compare without case
	const unsigned missing = DX12Device::Create_Game_Shader("dx12_selftest_missing_ps", false, nullptr);
	bool ok = vs != 0 && (vs & 1) != 0 && ps != 0 && (ps & 1) == 0 && missing == 0
		&& DX12Device::Is_Game_Shader(vs, true) && DX12Device::Is_Game_Shader(ps, false) && !DX12Device::Is_Game_Shader(vs, false);
	const char * failed = ok ? nullptr : "registration or creation";

	int vs_index = -1;
	int ps_index = -1;
	unsigned layout_id = 0;
	if (ok)
	{
		DX12GameShaders_Resolve_Handle(vs, true, &vs_index, &layout_id);
		DX12GameShaders_Resolve_Handle(ps, false, &ps_index, nullptr);

		DX12GamePSVariant variant;
		memset(&variant, 0, sizeof(variant));
		variant.Projected[1] = 3;
		variant.AlphaFunc = 7;		// D3DCMP_GREATEREQUAL
		variant.Fog = 1;
		const ShaderEntry & game_vs = Get_Game_Shader(vs_index, nullptr);
		const ShaderEntry & game_ps = Get_Game_Shader(ps_index, &variant);

		// Generated shaders for one modulated texture with fog.
		DX12VSKey vs_key;
		memset(&vs_key, 0, sizeof(vs_key));
		vs_key.FVF = fvf;
		for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
		{
			vs_key.TexCoordSet[stage] = 0xff;
		}
		vs_key.TexCoordSet[0] = 0;
		vs_key.Fog = FFP_VS_FOG_LINEAR;
		DX12PSKey ps_key;
		memset(&ps_key, 0, sizeof(ps_key));
		ps_key.Stages[0].ColorOp = 4;		// D3DTOP_MODULATE
		ps_key.Stages[0].ColorArg1 = 2;		// D3DTA_TEXTURE
		ps_key.Stages[0].ColorArg2 = 0;		// D3DTA_DIFFUSE
		ps_key.Stages[0].AlphaOp = 2;		// D3DTOP_SELECTARG1
		ps_key.Stages[0].AlphaArg1 = 2;
		ps_key.Stages[0].Texture = FFP_TEXTURE_SAMPLED;
		ps_key.Fog = 1;
		const ShaderEntry & ffp_vs = Get_Vertex_Shader(vs_key);
		const ShaderEntry & ffp_ps = Get_Pixel_Shader(ps_key);
		FVFLayout ffp_layout;
		ok = game_vs.Code && game_ps.Code && ffp_vs.Code && ffp_ps.Code && Decode_FVF(fvf, ffp_layout)
			&& layout_id >= 1 && layout_id <= GameLayouts.size();
		failed = ok ? nullptr : "compile";

		if (ok)
		{
			PSOKey key;
			memset(&key, 0, sizeof(key));
			key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			key.ColorWriteMask = 0xf;
			key.ZEnable = 1;
			key.ZWriteEnable = 1;
			key.ZFunc = 4;				// D3DCMP_LESSEQUAL
			key.CullMode = 1;			// D3DCULL_NONE
			key.FillMode = 3;			// D3DFILL_SOLID
			key.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			key.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

			const FVFLayout & game_layout = GameLayouts[layout_id - 1].Layout;
			key.VertexShader = game_vs.Id;
			key.PixelShader = game_ps.Id;
			key.InputLayout = layout_id;
			ok = Get_Pipeline_State(key, game_layout, game_vs.Code.Get(), game_ps.Code.Get()) != nullptr;
			key.PixelShader = ffp_ps.Id;
			ok = ok && Get_Pipeline_State(key, game_layout, game_vs.Code.Get(), ffp_ps.Code.Get()) != nullptr;
			key.VertexShader = ffp_vs.Id;
			key.PixelShader = game_ps.Id;
			key.InputLayout = 0;
			key.FVF = fvf;
			ok = ok && Get_Pipeline_State(key, ffp_layout, ffp_vs.Code.Get(), game_ps.Code.Get()) != nullptr;
			failed = ok ? nullptr : "pipeline state";
		}
	}

	DX12Device::Delete_Game_Shader(vs);
	DX12Device::Delete_Game_Shader(ps);
	if (ok && (DX12Device::Is_Game_Shader(vs, true) || DX12Device::Is_Game_Shader(ps, false)))
	{
		ok = false;
		failed = "delete";
	}

	if (ok)
	{
		Debug_Log("game shader self test passed");
	}
	else
	{
		WWDEBUG_SAY(("DX12: game shader self test failed (%s)", failed));
		Debug_Log("game shader self test FAILED (%s)", failed);
	}
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
	if (DX12Device_DebugLayer)
	{
		Open_Debug_Log();
		Debug_Log("initializing Direct3D 12");
	}
	if (!Init_Device((HWND)hwnd))
	{
		WWDEBUG_SAY(("DX12: initialization failed"));
		Debug_Log("initialization failed");
		if (Queue && Fence && FenceEvent)
		{
			Wait_For_Fence(Signal_Queue());
		}
		Release_All();
		Close_Debug_Log();
		return false;
	}
	Flush_Debug_Messages();

	Device = D3D.Get();
	Width = 0;
	Height = 0;

	// The registry is filled once per device, before the game creates any shader.
	DX12_Register_Game_Shaders();
	if (DX12Device_DebugLayer)
	{
#if DX12_GAME_SHADERS_ENABLED
		Run_Game_Shader_Self_Test();
#endif
		const int count = DX12GameShaders_Get_Count();
		Debug_Log("%d game shaders registered%s", count, DX12_GAME_SHADERS_ENABLED ? "" : " (game shaders are disabled)");
		for (int i = 0; i < count; ++i)
		{
			Debug_Log("  %s (%s)", DX12GameShaders_Get_Name(i), DX12GameShaders_Is_Vertex_Shader(i) ? "vertex" : "pixel");
		}
		Flush_Debug_Messages();
	}
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
		Submit_Frame(false);
	}
	Execute_Uploads_Now();
	Wait_For_Fence(Signal_Queue());

	// Resources released from here on are released immediately.
	Device = nullptr;
	Release_All();
	Close_Debug_Log();
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
		Submit_Frame(false);
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
		// Sequential, not discard: the presented back buffer keeps its contents, which Read_Back_Buffer
		// reads between frames (screenshots).
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
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
	HasPresented = false;
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

	if (SwapChain)
	{
		BackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
	}
	FrameHasRenderTarget = Has_Render_Target();
	if (FrameHasRenderTarget)
	{
		Transition(CommandList.Get(), BackBuffers[BackBufferIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// Binds the selected targets and keeps the viewport, which Direct3D 8 also keeps across frames.
	Restore_Command_List_State();
	Record_Uploads(CommandList.Get());
}

void DX12Device::End_Frame(bool present)
{
	if (!Device)
	{
		return;
	}

	if (Recording && !present)
	{
		// Direct3D 8 EndScene without Present costs nothing; ending the command list here would take
		// a frame slot per render to texture pass and make the next frame wait for the GPU.
		return;
	}
	Submit_Frame(present);
}

void DX12Device::Submit_Frame(bool present)
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
		if (SUCCEEDED(hr))
		{
			LastPresentedIndex = BackBufferIndex;
			HasPresented = true;
		}
	}

	Frames[FrameIndex].FenceValue = Signal_Queue();
	FrameIndex = (FrameIndex + 1) % FRAME_COUNT;
	FrameSlotPrepared = false;

	Process_Deferred();
	if (present && (FrameNumber % 300) == 0)
	{
		// Heartbeat, so the log shows whether frames keep coming.
		Debug_Log("frame %llu: presented (render target selections %u, target copies %u)", FrameNumber,
			DebugTargetSelects, DebugTargetCopies);
		DebugTargetSelects = 0;
		DebugTargetCopies = 0;

		// Which game shaders drew since the last heartbeat.
		std::string used;
		for (size_t i = 0; i < GameShaderCaches.size(); ++i)
		{
			if (GameShaderCaches[i].Draws != 0)
			{
				char item[96];
				snprintf(item, sizeof(item), " %s=%u", DX12GameShaders_Get_Name((int)i), GameShaderCaches[i].Draws);
				used += item;
				GameShaderCaches[i].Draws = 0;
			}
		}
		if (!used.empty())
		{
			Debug_Log("frame %llu: game shader draws:%s", FrameNumber, used.c_str());
		}
	}
	Flush_Debug_Messages();
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
	Ensure_Target_States();

	// Like Direct3D 8, clear only the current viewport of the current target.
	D3D12_RECT rect = CurrentScissor;
	if (rect.left < 0)
		rect.left = 0;
	if (rect.top < 0)
		rect.top = 0;
	if (rect.right > (LONG)Target_Width())
		rect.right = (LONG)Target_Width();
	if (rect.bottom > (LONG)Target_Height())
		rect.bottom = (LONG)Target_Height();
	if (rect.right <= rect.left || rect.bottom <= rect.top)
	{
		return;
	}

	if (clear_color)
	{
		float color[4];
		Color_To_Float4(argb, color);
		CommandList->ClearRenderTargetView(Current_RTV(), color, 1, &rect);
	}

	// A target without a depth buffer has nothing to clear, as in Direct3D 8.
	D3D12_CPU_DESCRIPTOR_HANDLE dsv;
	DX12TextureClass * depth_texture = nullptr;
	if (clear_z_stencil && TargetHasDepth && Current_DSV(dsv, &depth_texture))
	{
		if (z < 0.0f)
			z = 0.0f;
		if (z > 1.0f)
			z = 1.0f;
		CommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, z, (UINT8)(stencil & 0xff), 1, &rect);
	}
}

void DX12Device::Set_Viewport(unsigned x, unsigned y, unsigned width, unsigned height, float min_z, float max_z)
{
	if (!Device)
	{
		return;
	}

	// Begin_Frame keeps the viewport, so this could also wait for the frame; it starts the frame
	// as it always did.
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

	// Game shaders, from the handles of the Direct3D 8 state mirror. A game vertex shader reads the
	// vertex through the input layout of its Direct3D 8 vertex declaration instead of the FVF.
	int game_vs = -1;
	int game_ps = -1;
	unsigned game_layout_id = 0;
	if (desc.VertexShader != 0 && !DX12GameShaders_Resolve_Handle(desc.VertexShader, true, &game_vs, &game_layout_id))
	{
		return;
	}
	if (desc.PixelShader != 0 && !DX12GameShaders_Resolve_Handle(desc.PixelShader, false, &game_ps, nullptr))
	{
		return;
	}

	FVFLayout fvf_layout;
	const FVFLayout * layout = &fvf_layout;
	if (game_vs >= 0)
	{
		if (game_layout_id == 0 || game_layout_id > GameLayouts.size())
		{
			return;
		}
		layout = &GameLayouts[game_layout_id - 1].Layout;
		if (layout->Size > desc.VertexStride)
		{
			if (!LoggedGameLayoutStride)
			{
				WWDEBUG_SAY(("DX12: game vertex shader declaration of %u bytes is larger than the stride %u, draw skipped", layout->Size, desc.VertexStride));
				LoggedGameLayoutStride = true;
			}
			return;
		}
	}
	else if (!Decode_FVF(desc.FVF, fvf_layout) || fvf_layout.Size > desc.VertexStride)
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
	Ensure_Target_States();

	// Fixed-function shaders, derived from the Direct3D 8 state mirror.
	const unsigned * render_states = desc.RenderStates;
	const unsigned * texture_stage_states = desc.TextureStageStates;
	unsigned fallback_render_states[FFP_RS_COUNT];
	unsigned fallback_texture_stage_states[(unsigned)FFP_TSS_COUNT * DX12_MAX_STAGES];
	if (!render_states || !texture_stage_states)
	{
		DX12FFP_Fill_Fallback_States(desc, fallback_render_states, fallback_texture_stage_states);
		render_states = fallback_render_states;
		texture_stage_states = fallback_texture_stage_states;
	}

	DX12FFPKey ffp;
	DX12FFP_Build_Key(desc, render_states, texture_stage_states, game_ps >= 0, ffp);
	if (game_vs >= 0)
	{
		// Like Direct3D 8, a vertex shader writes the fog factor itself (oFog), used with
		// D3DRS_FOGENABLE, and its specular color (oD1) is added with D3DRS_SPECULARENABLE.
		ffp.PS.Fog = render_states[FFP_RS_FOGENABLE] != 0 ? 1 : 0;
		ffp.PS.FogTableMode = 0;
		if (game_ps < 0)
			ffp.PS.Specular = render_states[FFP_RS_SPECULARENABLE] != 0 ? 1 : 0;
	}

	ID3DBlob * vertex_code = nullptr;
	unsigned vertex_id = 0;
	if (game_vs >= 0)
	{
		const ShaderEntry & entry = Get_Game_Shader(game_vs, nullptr);
		vertex_code = entry.Code.Get();
		vertex_id = entry.Id;
	}
	else
	{
		if (!LastVertexShader || memcmp(&ffp.VS, &LastVSKey, sizeof(LastVSKey)) != 0)
		{
			LastVertexShader = &Get_Vertex_Shader(ffp.VS);
			LastVSKey = ffp.VS;
		}
		vertex_code = LastVertexShader->Code.Get();
		vertex_id = LastVertexShader->Id;
	}

	ID3DBlob * pixel_code = nullptr;
	unsigned pixel_id = 0;
	if (game_ps >= 0)
	{
		DX12GamePSVariant variant;
		memset(&variant, 0, sizeof(variant));
		for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
		{
			variant.Projected[stage] = ffp.PS.Stages[stage].Projected;
		}
		variant.AlphaFunc = ffp.PS.AlphaFunc;
		variant.Fog = ffp.PS.Fog;
		variant.FogTableMode = ffp.PS.FogTableMode;
		const ShaderEntry & entry = Get_Game_Shader(game_ps, &variant);
		pixel_code = entry.Code.Get();
		pixel_id = entry.Id;
	}
	else
	{
		if (!LastPixelShader || memcmp(&ffp.PS, &LastPSKey, sizeof(LastPSKey)) != 0)
		{
			LastPixelShader = &Get_Pixel_Shader(ffp.PS);
			LastPSKey = ffp.PS;
		}
		pixel_code = LastPixelShader->Code.Get();
		pixel_id = LastPixelShader->Id;
	}

	if (!vertex_code || !pixel_code)
	{
		return;
	}
	if (game_vs >= 0)
	{
		++GameShaderCaches[game_vs].Draws;
	}
	if (game_ps >= 0)
	{
		++GameShaderCaches[game_ps].Draws;
	}

	// Pipeline state. States that do not matter are normalized to share pipeline states.
	PSOKey key;
	memset(&key, 0, sizeof(key));
	key.VertexShader = vertex_id;
	key.PixelShader = pixel_id;
	key.FVF = game_vs >= 0 ? 0 : desc.FVF;
	key.InputLayout = game_vs >= 0 ? game_layout_id : 0;
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
	key.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	key.DSVFormat = TargetHasDepth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
	if (!TargetHasDepth)
	{
		// Without a depth buffer Direct3D 8 skips the depth and stencil tests.
		key.ZEnable = 0;
		key.ZWriteEnable = 0;
		key.ZFunc = 0;
		key.DepthBias = 0;
	}

	// Stencil, from the render state mirror (the fallback states leave it off). Masks such as
	// 0x80808080 are truncated to the 8 bits of the D24S8 stencil plane.
	bool stencil = TargetHasDepth && render_states[FFP_RS_STENCILENABLE] != 0;
	if (stencil)
	{
		unsigned fail = render_states[FFP_RS_STENCILFAIL];
		unsigned zfail = render_states[FFP_RS_STENCILZFAIL];
		unsigned pass = render_states[FFP_RS_STENCILPASS];
		unsigned func = render_states[FFP_RS_STENCILFUNC];
		key.StencilEnable = 1;
		key.StencilFail = (fail >= STENCILOP_KEEP && fail <= STENCILOP_DECR) ? fail : STENCILOP_KEEP;
		key.StencilZFail = (zfail >= STENCILOP_KEEP && zfail <= STENCILOP_DECR) ? zfail : STENCILOP_KEEP;
		key.StencilPass = (pass >= STENCILOP_KEEP && pass <= STENCILOP_DECR) ? pass : STENCILOP_KEEP;
		key.StencilFunc = (func >= CMP_NEVER && func <= CMP_ALWAYS) ? func : CMP_ALWAYS;
		key.StencilReadMask = render_states[FFP_RS_STENCILMASK] & 0xff;
		key.StencilWriteMask = render_states[FFP_RS_STENCILWRITEMASK] & 0xff;
	}

	ID3D12PipelineState * pso = Get_Pipeline_State(key, *layout, vertex_code, pixel_code);
	if (!pso)
	{
		return;
	}

	// Textures. Stages that do not sample their texture get the white texture; a game pixel shader
	// may sample every stage.
	D3D12_RESOURCE_BARRIER barriers[DX12_MAX_STAGES];
	unsigned barrier_count = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE textures[DX12_MAX_STAGES];
	D3D12_GPU_DESCRIPTOR_HANDLE samplers[DX12_MAX_STAGES];
	for (int stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		bool sampled = game_ps >= 0 || ffp.PS.Stages[stage].Texture == FFP_TEXTURE_SAMPLED;
		textures[stage] = Bind_Texture(sampled ? desc.Textures[stage] : nullptr, barriers, barrier_count);
		samplers[stage] = sampled ? Get_Sampler(desc.MinFilter[stage], desc.MagFilter[stage], desc.MipFilter[stage],
			desc.AddressU[stage], desc.AddressV[stage], desc.MaxAnisotropy[stage]) : DefaultSampler;
	}
	if (barrier_count > 0)
	{
		CommandList->ResourceBarrier(barrier_count, barriers);
	}

	DX12FFPViewport viewport;
	viewport.X = CurrentViewport.TopLeftX;
	viewport.Y = CurrentViewport.TopLeftY;
	viewport.Width = CurrentViewport.Width;
	viewport.Height = CurrentViewport.Height;
	viewport.MinZ = CurrentViewport.MinDepth;
	viewport.MaxZ = CurrentViewport.MaxDepth;

	DX12VSConstants vs;
	DX12PSConstants ps;
	DX12FFP_Fill_Constants(desc, render_states, ffp, viewport, vs, ps);

	UINT64 vs_gpu = Upload_Constants(&vs, sizeof(vs), &LastVSConstants, LastVSConstantsGPU);
	UINT64 ps_gpu = Upload_Constants(&ps, sizeof(ps), &LastPSConstants, LastPSConstantsGPU);
	if (vs_gpu == 0 || ps_gpu == 0)
	{
		return;
	}

	// The Direct3D 8 shader constant registers. Without a game shader the root parameter gets any
	// valid address, which no shader reads.
	UINT64 game_vs_gpu = vs_gpu;
	UINT64 game_ps_gpu = ps_gpu;
	if (game_vs >= 0)
	{
		DX12GameVSConstants game_vs_constants;
		if (desc.VertexShaderConstants)
			memcpy(game_vs_constants.C, desc.VertexShaderConstants, sizeof(game_vs_constants.C));
		else
			memset(game_vs_constants.C, 0, sizeof(game_vs_constants.C));
		game_vs_gpu = Upload_Constants(&game_vs_constants, sizeof(game_vs_constants), &LastGameVSConstants, LastGameVSConstantsGPU);
	}
	if (game_ps >= 0)
	{
		// ps.1.x constant registers hold -1 .. 1.
		DX12GamePSConstants game_ps_constants;
		for (unsigned i = 0; i < DX12_MAX_PS_CONSTANTS; ++i)
		{
			for (unsigned c = 0; c < 4; ++c)
			{
				float value = desc.PixelShaderConstants ? desc.PixelShaderConstants[i * 4 + c] : 0.0f;
				game_ps_constants.C[i][c] = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
			}
		}
		for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
		{
			const unsigned * t = texture_stage_states + stage * FFP_TSS_COUNT;
			const unsigned matrix[4] = { t[FFP_TSS_BUMPENVMAT00], t[FFP_TSS_BUMPENVMAT01], t[FFP_TSS_BUMPENVMAT10], t[FFP_TSS_BUMPENVMAT11] };
			memcpy(game_ps_constants.BumpEnvMat[stage], matrix, sizeof(matrix));
			memcpy(&game_ps_constants.BumpEnvLuminance[stage][0], &t[FFP_TSS_BUMPENVLSCALE], sizeof(float));
			memcpy(&game_ps_constants.BumpEnvLuminance[stage][1], &t[FFP_TSS_BUMPENVLOFFSET], sizeof(float));
			game_ps_constants.BumpEnvLuminance[stage][2] = 0.0f;
			game_ps_constants.BumpEnvLuminance[stage][3] = 0.0f;
		}
		game_ps_gpu = Upload_Constants(&game_ps_constants, sizeof(game_ps_constants), &LastGamePSConstants, LastGamePSConstantsGPU);
	}
	if (game_vs_gpu == 0 || game_ps_gpu == 0)
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
	if (stencil)
	{
		UINT stencil_ref = render_states[FFP_RS_STENCILREF] & 0xff;
		if (stencil_ref != BoundStencilRef)
		{
			CommandList->OMSetStencilRef(stencil_ref);
			BoundStencilRef = stencil_ref;
		}
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

	CommandList->SetGraphicsRootConstantBufferView(ROOT_VS_CONSTANTS, vs_gpu);
	CommandList->SetGraphicsRootConstantBufferView(ROOT_PS_CONSTANTS, ps_gpu);
	CommandList->SetGraphicsRootConstantBufferView(ROOT_GAME_PS_CONSTANTS, game_ps_gpu);
	CommandList->SetGraphicsRootConstantBufferView(ROOT_GAME_VS_CONSTANTS, game_vs_gpu);
	for (int stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		CommandList->SetGraphicsRootDescriptorTable(ROOT_TEXTURES + stage, textures[stage]);
		CommandList->SetGraphicsRootDescriptorTable(ROOT_SAMPLERS + stage, samplers[stage]);
	}

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

unsigned DX12Device::Allocate_RTV_Slot()
{
	std::lock_guard<std::mutex> lock(ResourceMutex);
	if (!FreeRTVSlots.empty())
	{
		unsigned slot = FreeRTVSlots.back();
		FreeRTVSlots.pop_back();
		return slot;
	}
	if (NextRTVSlot < RTV_HEAP_SIZE)
	{
		return NextRTVSlot++;
	}
	WWDEBUG_SAY(("DX12: out of render target view slots"));
	return 0;
}

void DX12Device::Free_RTV_Slot(unsigned slot)
{
	if (slot < BACK_BUFFER_COUNT || slot >= RTV_HEAP_SIZE || !Device)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(ResourceMutex);
	FreeRTVSlots.push_back(slot);
}

unsigned DX12Device::Allocate_DSV_Slot()
{
	std::lock_guard<std::mutex> lock(ResourceMutex);
	if (!FreeDSVSlots.empty())
	{
		unsigned slot = FreeDSVSlots.back();
		FreeDSVSlots.pop_back();
		return slot;
	}
	if (NextDSVSlot < DSV_HEAP_SIZE)
	{
		return NextDSVSlot++;
	}
	WWDEBUG_SAY(("DX12: out of depth stencil view slots"));
	return 0;
}

void DX12Device::Free_DSV_Slot(unsigned slot)
{
	if (slot == 0 || slot >= DSV_HEAP_SIZE || !Device)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(ResourceMutex);
	FreeDSVSlots.push_back(slot);
}

void DX12Device::Set_Render_Target(DX12TextureClass * color, DX12TextureClass * depth, bool use_default_depth)
{
	if (!Device)
	{
		return;
	}
	WWASSERT(Is_Main_Thread());

	if (color && (!color->Is_Render_Target() || !color->Peek_Resource()))
	{
		WWDEBUG_SAY(("DX12: Set_Render_Target with a texture that is not a render target, using the back buffer"));
		color = nullptr;
	}
	if (depth && (!depth->Is_Depth_Target() || !depth->Peek_Resource()))
	{
		WWDEBUG_SAY(("DX12: Set_Render_Target with a texture that is not a depth target, using no depth buffer"));
		depth = nullptr;
		use_default_depth = false;
	}
	if (!color)
	{
		// The back buffer always uses the default depth buffer. Restoring the back buffer is a no-op
		// when no texture is selected, so it must never be left with another depth buffer or none.
		depth = nullptr;
		use_default_depth = true;
	}

	if (color != TargetColor)
	{
		if (color)
			color->Add_Ref();
		if (TargetColor)
			TargetColor->Release_Ref();
		TargetColor = color;
	}
	if (depth != TargetDepth)
	{
		if (depth)
			depth->Add_Ref();
		if (TargetDepth)
			TargetDepth->Release_Ref();
		TargetDepth = depth;
	}
	TargetDefaultDepth = use_default_depth;
	if (color)
	{
		++DebugTargetSelects;
	}

	// Outside a frame, Begin_Frame binds the targets and applies the viewport.
	Bind_Targets();
	Set_Full_Viewport();
}

DX12TextureClass * DX12Device::Peek_Render_Target()
{
	return TargetColor;
}

unsigned DX12Device::Get_Target_Width()
{
	return Target_Width();
}

unsigned DX12Device::Get_Target_Height()
{
	return Target_Height();
}

bool DX12Device::Copy_Target_To_Texture(DX12TextureClass * dest, const tagRECT * src_rect, int dest_x, int dest_y)
{
	if (!Device || !dest || dest->Is_CPU_Only() || dest->Is_Depth_Target() || !dest->Peek_Resource()
		|| dest->Get_DXGI_Format() != (unsigned)DXGI_FORMAT_B8G8R8A8_UNORM || dest == TargetColor)
	{
		return false;
	}
	WWASSERT(Is_Main_Thread());

	Ensure_Recording();
	if (!Recording || !FrameHasRenderTarget)
	{
		return false;
	}
	// A pending upload to dest has to land before the copy, not overwrite it later.
	Record_Uploads(CommandList.Get());
	Ensure_Target_States();

	LONG left = 0;
	LONG top = 0;
	LONG right = (LONG)Target_Width();
	LONG bottom = (LONG)Target_Height();
	if (src_rect)
	{
		const RECT * r = reinterpret_cast<const RECT *>(src_rect);
		left = std::max(left, r->left);
		top = std::max(top, r->top);
		right = std::min(right, r->right);
		bottom = std::min(bottom, r->bottom);
		// The destination position follows the clipped source corner.
		dest_x += left - r->left;
		dest_y += top - r->top;
	}
	if (dest_x < 0)
	{
		left -= dest_x;
		dest_x = 0;
	}
	if (dest_y < 0)
	{
		top -= dest_y;
		dest_y = 0;
	}
	right = std::min(right, left + (LONG)dest->Get_Width() - dest_x);
	bottom = std::min(bottom, top + (LONG)dest->Get_Height() - dest_y);
	if (right <= left || bottom <= top)
	{
		return false;
	}

	ID3D12Resource * source = TargetColor ? TargetColor->Peek_Resource() : BackBuffers[BackBufferIndex].Get();

	// A render target that was never drawn into gets defined contents outside the copied rectangle.
	Initialize_Target(dest);

	D3D12_RESOURCE_BARRIER barriers[2];
	unsigned barrier_count = 0;
	barriers[barrier_count++] = Make_Transition(source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	if (dest->Get_State() != (unsigned)D3D12_RESOURCE_STATE_COPY_DEST)
	{
		barriers[barrier_count++] = Make_Transition(dest->Peek_Resource(), (D3D12_RESOURCE_STATES)dest->Get_State(), D3D12_RESOURCE_STATE_COPY_DEST);
		dest->Set_State(D3D12_RESOURCE_STATE_COPY_DEST);
	}
	CommandList->ResourceBarrier(barrier_count, barriers);

	D3D12_TEXTURE_COPY_LOCATION dst;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = dest->Peek_Resource();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src;
	memset(&src, 0, sizeof(src));
	src.pResource = source;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	D3D12_BOX box;
	box.left = (UINT)left;
	box.top = (UINT)top;
	box.front = 0;
	box.right = (UINT)right;
	box.bottom = (UINT)bottom;
	box.back = 1;

	CommandList->CopyTextureRegion(&dst, (UINT)dest_x, (UINT)dest_y, 0, &src, &box);
	++DebugTargetCopies;
	Transition(CommandList.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
}

DX12TextureClass * DX12Device::Read_Back_Buffer()
{
	if (!Device || !SwapChain)
	{
		return nullptr;
	}
	WWASSERT(Is_Main_Thread());

	const bool in_frame = Recording;
	ID3D12Resource * source = nullptr;
	D3D12_RESOURCE_STATES source_state = D3D12_RESOURCE_STATE_PRESENT;
	if (in_frame)
	{
		if (!FrameHasRenderTarget)
		{
			return nullptr;
		}
		source = BackBuffers[BackBufferIndex].Get();
		source_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}
	else
	{
		if (!HasPresented)
		{
			return nullptr;
		}
		source = BackBuffers[LastPresentedIndex].Get();
		source_state = D3D12_RESOURCE_STATE_PRESENT;
	}
	if (!source)
	{
		return nullptr;
	}

	D3D12_RESOURCE_DESC desc = source->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT num_rows = 0;
	UINT64 row_size = 0;
	UINT64 total = 0;
	D3D->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total);
	if (total == 0 || total == ~0ull)
	{
		return nullptr;
	}

	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_READBACK;

	D3D12_RESOURCE_DESC buffer_desc;
	memset(&buffer_desc, 0, sizeof(buffer_desc));
	buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer_desc.Width = total;
	buffer_desc.Height = 1;
	buffer_desc.DepthOrArraySize = 1;
	buffer_desc.MipLevels = 1;
	buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
	buffer_desc.SampleDesc.Count = 1;
	buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> readback;
	HRESULT hr = D3D->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	if (FAILED(hr))
	{
		Log_Failure("CreateCommittedResource (readback buffer)", hr);
		return nullptr;
	}

	ID3D12GraphicsCommandList * list = CommandList.Get();
	if (!in_frame)
	{
		if (FAILED(UploadAllocator->Reset()) || FAILED(UploadList->Reset(UploadAllocator.Get(), nullptr)))
		{
			WWDEBUG_SAY(("DX12: could not reset the upload command list"));
			return nullptr;
		}
		list = UploadList.Get();
	}

	D3D12_TEXTURE_COPY_LOCATION dst;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = readback.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint;

	D3D12_TEXTURE_COPY_LOCATION src;
	memset(&src, 0, sizeof(src));
	src.pResource = source;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	Transition(list, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);

	if (in_frame)
	{
		if (!Flush_Frame_And_Wait())
		{
			return nullptr;
		}
	}
	else
	{
		hr = UploadList->Close();
		if (FAILED(hr))
		{
			Log_Failure("Close (readback)", hr);
			return nullptr;
		}
		ID3D12CommandList * lists[] = { UploadList.Get() };
		Queue->ExecuteCommandLists(1, lists);
		Wait_For_Fence(Signal_Queue());
	}

	const unsigned width = (unsigned)desc.Width;
	const unsigned height = desc.Height;
	DX12TextureClass * texture = DX12TextureClass::Create(width, height, WW3D_FORMAT_A8R8G8B8, 1, true, true);
	if (!texture)
	{
		return nullptr;
	}

	unsigned char * mapped = nullptr;
	D3D12_RANGE read_range = { 0, (SIZE_T)total };
	hr = readback->Map(0, &read_range, (void **)&mapped);
	if (FAILED(hr) || !mapped)
	{
		Log_Failure("Map (readback buffer)", hr);
		texture->Release_Ref();
		return nullptr;
	}

	int pitch = 0;
	unsigned char * dest = static_cast<unsigned char *>(texture->Lock(0, &pitch));
	if (dest)
	{
		const unsigned rows = std::min(height, (unsigned)num_rows);
		const size_t row_bytes = std::min((size_t)width * 4, (size_t)row_size);
		for (unsigned y = 0; y < rows; ++y)
		{
			memcpy(dest + (size_t)y * pitch, mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, row_bytes);
		}
		texture->Unlock(0);
	}
	D3D12_RANGE no_write = { 0, 0 };
	readback->Unmap(0, &no_write);
	return texture;
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

void DX12Device::Register_Game_Shader(const char * name, bool vertex_shader, const char * hlsl_source, const char * entry_point)
{
	const int index = DX12GameShaders_Register(name, vertex_shader, hlsl_source, entry_point);
	if (index < 0)
	{
		WWDEBUG_SAY(("DX12: game shader registration of %s rejected", name ? name : "(null)"));
		Debug_Log("game shader registration of %s rejected (empty name, source or entry point, or a reserved entry point)", name ? name : "(null)");
		return;
	}

	// A new registration, or new source for an existing one: compile it again on next use.
	if ((size_t)index >= GameShaderCaches.size())
	{
		GameShaderCaches.resize(index + 1);
	}
	else
	{
		GameShaderCaches[index] = GameShaderCache();
	}
	Debug_Log("registered game %s shader %s (entry point %s)", vertex_shader ? "vertex" : "pixel", name, entry_point);
}

bool DX12Device::Is_Game_Shader_Registered(const char * name, bool vertex_shader)
{
	return DX12GameShaders_Find(name, vertex_shader) >= 0;
}

unsigned DX12Device::Create_Game_Shader(const char * name, bool vertex_shader, const unsigned * declaration)
{
	if (!Device || !DX12_GAME_SHADERS_ENABLED || !name)
	{
		return 0;
	}
	WWASSERT(Is_Main_Thread());

	const char * kind = vertex_shader ? "vertex" : "pixel";
	const int index = DX12GameShaders_Find(name, vertex_shader);
	if (index < 0)
	{
		Debug_Log("game %s shader %s is not registered; the game falls back", kind, name);
		return 0;
	}

	unsigned layout_id = 0;
	if (vertex_shader)
	{
		DX12GameInputLayout source;
		std::string error;
		if (!DX12GameShaders_Parse_Declaration(declaration, source, error))
		{
			WWDEBUG_SAY(("DX12: game vertex shader %s: %s", name, error.c_str()));
			Debug_Log("game vertex shader %s: unsupported vertex declaration: %s", name, error.c_str());
			return 0;
		}
		for (unsigned i = 0; i < source.ElementCount; ++i)
		{
			if (source.Elements[i].Format == DXGI_FORMAT_B8G8R8A8_UNORM && !ColorVertexFormatSupported)
			{
				Debug_Log("game vertex shader %s: D3DVSDT_D3DCOLOR inputs are not supported by this adapter", name);
				return 0;
			}
		}
		layout_id = Intern_Game_Layout(source);
	}

	// Compiled now (the plain variant for a pixel shader) so a shader that does not compile fails here
	// and the game falls back, instead of drawing nothing.
	const ShaderEntry & entry = Get_Game_Shader(index, nullptr);
	if (!entry.Code)
	{
		WWDEBUG_SAY(("DX12: game %s shader %s does not compile", kind, name));
		Debug_Log("game %s shader %s does not compile; the game falls back", kind, name);
		return 0;
	}

	const unsigned handle = DX12GameShaders_Allocate_Handle(index, vertex_shader, layout_id);
	Debug_Log("created game %s shader %s, handle 0x%x", kind, name, handle);
	return handle;
}

void DX12Device::Delete_Game_Shader(unsigned handle)
{
	DX12GameShaders_Free_Handle(handle);
}

bool DX12Device::Is_Game_Shader(unsigned handle, bool vertex_shader)
{
	return DX12GameShaders_Resolve_Handle(handle, vertex_shader, nullptr, nullptr);
}
