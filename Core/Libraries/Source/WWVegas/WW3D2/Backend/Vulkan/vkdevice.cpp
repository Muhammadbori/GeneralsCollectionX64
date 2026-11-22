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

// TheSuperHackers @feature O138 DX12Device implemented with Vulkan (RTS_RENDERER=vulkan), in place of
// dx12device.cpp. See vkinternal.h for the overall design.
//
// Frames: two frame slots, each with a command pool, an upload command buffer (texture uploads and the first
// layout of new textures, submitted first), the main command buffer, a fence and frame memory (host visible
// pages for dynamic buffers, constants and upload staging). The game draws into an offscreen back buffer
// (B8G8R8A8); a present copies it to the acquired swap chain image, so Read_Back_Buffer can read the last frame
// at any time. Drawing uses dynamic rendering (VK_KHR_dynamic_rendering) and one push descriptor set
// (VK_KHR_push_descriptor) per draw.
//
// Layouts: textures that are only uploaded stay in SHADER_READ_ONLY_OPTIMAL outside the upload command buffer.
// Render and depth targets are tracked on the main command buffer (attachment, shader read, transfer).
//
// Not yet (O138 part 1): the brightness ramp (Set_Gamma_Ramp is kept, not applied), uploads into render targets,
// tearing presents, the stats log. -dx12debug enables the validation layer and writes vkdebug.log.

#include "WW3D2/Backend/DX12/dx12device.h"
#include "WW3D2/Backend/DX12/dx12ffp.h"
#include "WW3D2/Backend/DX12/dx12gameshaders.h"
#include "WW3D2/Backend/DX12/dx12resource.h"
#include "WW3D2/Backend/Vulkan/vkinternal.h"
#include "WWDebug/wwdebug.h"

#include <windows.h>
#include <dxc/dxcapi.h>

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

#define VK_DEFINE_FUNCTION(name) PFN_##name name = nullptr;
VK_GLOBAL_FUNCTIONS(VK_DEFINE_FUNCTION)
VK_INSTANCE_FUNCTIONS(VK_DEFINE_FUNCTION)
VK_DEBUG_FUNCTIONS(VK_DEFINE_FUNCTION)
VK_DEVICE_FUNCTIONS(VK_DEFINE_FUNCTION)
#undef VK_DEFINE_FUNCTION

DX12FrameCounters DX12Device_Counters;
bool DX12Device_DebugLayer = false;
bool DX12Device_Stats = false;
bool DX12Device_NoShaderCache = false;

bool DX12Device::Requested = false;
bool DX12Device::UnlockedPresent = false;
ID3D12Device * DX12Device::Device = nullptr;
unsigned DX12Device::Width = 0;
unsigned DX12Device::Height = 0;
unsigned long long DX12Device::FrameNumber = 0;

namespace
{

enum
{
	FRAME_COUNT = 2,
	FRAME_PAGE_SIZE = 32 * 1024 * 1024,
	FRAME_EXTRA_PAGE_SIZE = 8 * 1024 * 1024,
	CONSTANT_ALIGNMENT = 256,
	MAX_PENDING_UPLOAD_BYTES = 64 * 1024 * 1024,
	MAX_INPUT_ELEMENTS = 16,
	MEMORY_BLOCK_SIZE = 64 * 1024 * 1024,
	BINDING_TEXTURES = 4,
	BINDING_SAMPLERS = BINDING_TEXTURES + DX12_MAX_STAGES,
	BINDING_COUNT = BINDING_SAMPLERS + DX12_MAX_STAGES,
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

enum
{
	BLEND_BOTHSRCALPHA = 12,
	BLEND_BOTHINVSRCALPHA = 13,
	STENCILOP_KEEP = 1,
	STENCILOP_DECR = 8,
	CMP_NEVER = 1,
	CMP_ALWAYS = 8,
};

// DXGI_FORMAT values of the input layouts (dx12gameshaders.cpp), spelled out so this file does not need dxgiformat.h.
enum
{
	DXGI_R32G32B32A32_FLOAT = 2,
	DXGI_R32G32B32_FLOAT = 6,
	DXGI_R16G16B16A16_SINT = 14,
	DXGI_R32G32_FLOAT = 16,
	DXGI_R8G8B8A8_UNORM = 28,
	DXGI_R8G8B8A8_UINT = 30,
	DXGI_R16G16_SINT = 38,
	DXGI_R32_FLOAT = 41,
	DXGI_B8G8R8A8_UNORM = 87,
};

const int ZBIAS_TO_DEPTH_BIAS = -84;
const unsigned MAX_FRAME_ENDS_WITHOUT_PRESENT = 16;
const unsigned long long NO_SERIAL = ~0ull;

// ---------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------

struct FramePage
{
	VkBuffer Buffer;
	VkDeviceMemory Memory;
	unsigned char * CPU;
	unsigned Table;
	VkDeviceSize Size;
	VkDeviceSize Offset;
};

struct DeferredRelease
{
	unsigned long long Serial;	// free once this submission has completed
	VkImage Image;
	VkImageView Views[2];
	VkBuffer Buffer;
	VKAllocation Allocation;
	bool HostAllocation;
	unsigned Table;				// buffer table entry to free, or ~0u
	VKImageData * ImageData;
	VKBufferData * BufferData;
};

struct FrameSlot
{
	VkCommandPool Pool;
	VkCommandBuffer Main;
	VkCommandBuffer Upload;
	VkFence Fence;
	VkSemaphore Acquire;
	bool Submitted;
	unsigned long long Serial;
	bool UploadOpen;
	std::vector<FramePage> Pages;
	size_t PageIndex;
};

struct PendingUpload
{
	DX12TextureClass * Texture;
	unsigned Level;
	std::vector<unsigned char> Data;
};

struct VertexElement
{
	const char * Semantic;
	unsigned Index;
	VkFormat Format;
	unsigned Offset;
};

struct VertexLayout
{
	VertexElement Elements[MAX_INPUT_ELEMENTS];
	unsigned Count;
	unsigned Size;
};

struct InterfaceVar
{
	std::string Semantic;	// upper case name without the index
	unsigned Index;
	unsigned Location;
	size_t LocationWord;	// word index of the Location literal in the code
};

struct ShaderEntry
{
	ShaderEntry() : Id(0) {}
	std::vector<unsigned> Code;	// SPIR-V, empty when the compile failed
	unsigned Id;
	std::vector<InterfaceVar> Inputs;
	std::vector<InterfaceVar> Outputs;
};

struct PSOKey
{
	unsigned VertexShader;
	unsigned PixelShader;
	unsigned FVF;
	unsigned InputLayout;
	unsigned VertexStride;
	unsigned Topology;
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
	unsigned StencilEnable;
	unsigned StencilFail;
	unsigned StencilZFail;
	unsigned StencilPass;
	unsigned StencilFunc;
	unsigned StencilReadMask;
	unsigned StencilWriteMask;
	unsigned DepthFormat;	// VkFormat of the depth attachment, 0 without one
};

struct PSOKeyLess
{
	bool operator()(const PSOKey & a, const PSOKey & b) const { return memcmp(&a, &b, sizeof(PSOKey)) < 0; }
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

struct GameShaderCache
{
	GameShaderCache() : VertexCompiled(false) {}
	ShaderEntry Vertex;
	bool VertexCompiled;
	std::map<DX12GamePSVariant, ShaderEntry, GamePSVariantLess> Pixel;
};

struct GameLayout
{
	DX12GameInputLayout Source;
	VertexLayout Layout;
};

struct MemoryBlock
{
	VkDeviceMemory Memory;
	VkDeviceSize Size;
	unsigned Type;
	bool Host;
	unsigned char * Mapped;
	std::vector<std::pair<VkDeviceSize, VkDeviceSize>> Free;	// offset, size; sorted by offset
};

HMODULE VulkanLibrary = nullptr;
HMODULE CompilerLibrary = nullptr;
DxcCreateInstanceProc DxcCreateInstanceFunc = nullptr;
IDxcCompiler3 * Compiler = nullptr;
IDxcUtils * CompilerUtils = nullptr;

VkInstance Instance = VK_NULL_HANDLE;
VkDebugUtilsMessengerEXT Messenger = VK_NULL_HANDLE;
VkSurfaceKHR Surface = VK_NULL_HANDLE;
VkPhysicalDevice Physical = VK_NULL_HANDLE;
VkPhysicalDeviceProperties PhysicalProperties;
VkPhysicalDeviceMemoryProperties MemoryProperties;
VkPhysicalDeviceFeatures EnabledFeatures;
VkDevice VK = VK_NULL_HANDLE;
VkQueue Queue = VK_NULL_HANDLE;
unsigned QueueFamily = 0;
VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
bool ColorVertexFormatSupported = true;
bool MirrorOnceSupported = false;
char AdapterName[256];
unsigned long long DedicatedVideoMemory = 0;
std::vector<DX12Device::DisplayMode> DisplayModes;
HWND Window = nullptr;
DWORD MainThreadID = 0;
char FailureReason[256];
char UserDataDirectory[MAX_PATH];
char DebugLogDirectory[MAX_PATH];

VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;
VkExtent2D SwapchainExtent = { 0, 0 };
std::vector<VkImage> SwapchainImages;
std::vector<VkSemaphore> RenderDone;	// per swap chain image
bool SwapchainStale = false;

VKImageData * BackBuffer = nullptr;		// offscreen back buffer
VKImageData * DepthBuffer = nullptr;	// default depth buffer
bool HasPresented = false;

FrameSlot Frames[FRAME_COUNT];
unsigned FrameIndex = 0;
bool FrameSlotPrepared = false;
bool Recording = false;
bool RenderingActive = false;
unsigned FrameEndsWithoutPresent = 0;
unsigned long long NextSerial = 1;
unsigned long long CompletedSerial = 0;
VkCommandPool OneShotPool = VK_NULL_HANDLE;
VkCommandBuffer OneShot = VK_NULL_HANDLE;

std::mutex ResourceMutex;	// allocator, buffer table, deferred releases, pending uploads
std::vector<MemoryBlock> Blocks;
std::vector<VkBuffer> BufferTable;
std::vector<unsigned> FreeTableEntries;
std::vector<DeferredRelease> DeferredReleases;
std::vector<PendingUpload> PendingUploads;
std::vector<DX12TextureClass *> InitialLayouts;
size_t PendingUploadBytes = 0;
std::atomic<bool> HasPendingUploads(false);
std::vector<DX12TextureClass *> DirtyTextures;
std::vector<VKImageData *> LiveImages;	// images not yet released, so Release_All can destroy what the game still holds
bool UploadingDirtyTextures = false;

VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
std::map<PSOKey, VkPipeline, PSOKeyLess> Pipelines;
std::map<unsigned, VkSampler> Samplers;
VkSampler DefaultSampler = VK_NULL_HANDLE;
VKImageData * WhiteTexture = nullptr;

VertexShaderMap VertexShaders;
PixelShaderMap PixelShaders;
std::vector<GameShaderCache> GameShaderCaches;
std::vector<GameLayout> GameLayouts;
unsigned NextShaderId = 1;
const ShaderEntry * LastVertexShader = nullptr;
DX12VSKey LastVSKey;
const ShaderEntry * LastPixelShader = nullptr;
DX12PSKey LastPSKey;

DX12TextureClass * TargetColor = nullptr;
DX12TextureClass * TargetDepth = nullptr;
bool TargetDefaultDepth = true;
bool TargetHasDepth = false;
VkViewport CurrentViewport;		// Direct3D convention (positive height)
VkRect2D CurrentScissor;

VkPipeline BoundPipeline = VK_NULL_HANDLE;
unsigned BoundStencilRef = ~0u;

FILE * DebugLog = nullptr;
unsigned ValidationErrors = 0;
unsigned ValidationWarnings = 0;
bool LoggedTriangleFan = false;
bool LoggedOverflow = false;
bool LoggedRenderTargetUpload = false;
unsigned LastBadFVF = ~0u;

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

bool Is_Main_Thread()
{
	return MainThreadID == 0 || GetCurrentThreadId() == MainThreadID;
}

void Debug_Log(const char * format, ...)
{
	if (!DebugLog)
	{
		return;
	}
	va_list args;
	va_start(args, format);
	vfprintf(DebugLog, format, args);
	va_end(args);
	fputc('\n', DebugLog);
	fflush(DebugLog);
}

void Set_Failure_Reason(const char * format, ...)
{
	if (FailureReason[0] != '\0')
	{
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(FailureReason, sizeof(FailureReason), format, args);
	va_end(args);
	WWDEBUG_SAY(("Vulkan: %s", FailureReason));
	Debug_Log("failure: %s", FailureReason);
}

bool Check(VkResult result, const char * what)
{
	if (result == VK_SUCCESS)
	{
		return true;
	}
	Set_Failure_Reason("%s failed (VkResult %d)", what, (int)result);
	return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL Debug_Callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT * data, void *)
{
	const char * kind = "info";
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		kind = "ERROR";
		++ValidationErrors;
	}
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		kind = "warning";
		++ValidationWarnings;
	}
	if (ValidationErrors + ValidationWarnings < 2000 || (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && ValidationErrors < 5000)
	{
		Debug_Log("frame %llu: validation %s: %s", DX12Device::Get_Frame_Number(), kind, data && data->pMessage ? data->pMessage : "");
	}
	return VK_FALSE;
}

void Open_Debug_Log()
{
	if (DebugLog)
	{
		return;
	}
	char path[MAX_PATH * 2];
	if (DebugLogDirectory[0] != '\0')
	{
		size_t length = strlen(DebugLogDirectory);
		const char * separator = (DebugLogDirectory[length - 1] == '\\' || DebugLogDirectory[length - 1] == '/') ? "" : "\\";
		snprintf(path, sizeof(path), "%s%svkdebug.log", DebugLogDirectory, separator);
		DebugLog = fopen(path, "w");
	}
	if (!DebugLog)
	{
		DebugLog = fopen("vkdebug.log", "w");
	}
}

unsigned long long Query_Ticks()
{
	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	return (unsigned long long)ticks.QuadPart;
}

int Find_Memory_Type(unsigned type_bits, VkMemoryPropertyFlags flags)
{
	for (unsigned i = 0; i < MemoryProperties.memoryTypeCount; ++i)
	{
		if ((type_bits & (1u << i)) && (MemoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
		{
			return (int)i;
		}
	}
	return -1;
}

// Block allocator; ResourceMutex held.
bool Allocate_Memory(const VkMemoryRequirements & requirements, bool host, VKAllocation & out)
{
	VkMemoryPropertyFlags flags = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	int type = Find_Memory_Type(requirements.memoryTypeBits, flags);
	if (type < 0 && !host)
	{
		type = Find_Memory_Type(requirements.memoryTypeBits, 0);
	}
	if (type < 0)
	{
		return false;
	}
	const VkDeviceSize alignment = requirements.alignment ? requirements.alignment : 1;
	for (size_t b = 0; b < Blocks.size(); ++b)
	{
		MemoryBlock & block = Blocks[b];
		if (block.Type != (unsigned)type || block.Host != host)
		{
			continue;
		}
		for (size_t f = 0; f < block.Free.size(); ++f)
		{
			const VkDeviceSize start = block.Free[f].first;
			const VkDeviceSize end = start + block.Free[f].second;
			const VkDeviceSize aligned = (start + alignment - 1) / alignment * alignment;
			if (aligned + requirements.size > end)
			{
				continue;
			}
			// Split the free range into the part before the allocation and the part after it.
			std::vector<std::pair<VkDeviceSize, VkDeviceSize>> pieces;
			if (aligned > start)
				pieces.push_back(std::make_pair(start, aligned - start));
			if (aligned + requirements.size < end)
				pieces.push_back(std::make_pair(aligned + requirements.size, end - aligned - requirements.size));
			block.Free.erase(block.Free.begin() + f);
			block.Free.insert(block.Free.begin() + f, pieces.begin(), pieces.end());
			out.Memory = block.Memory;
			out.Offset = aligned;
			out.Size = requirements.size;
			out.Block = (unsigned)b;
			out.Mapped = block.Mapped ? block.Mapped + aligned : nullptr;
			return true;
		}
	}

	MemoryBlock block;
	block.Size = requirements.size + alignment > MEMORY_BLOCK_SIZE ? requirements.size + alignment : MEMORY_BLOCK_SIZE;
	block.Type = (unsigned)type;
	block.Host = host;
	block.Mapped = nullptr;
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	info.allocationSize = block.Size;
	info.memoryTypeIndex = (unsigned)type;
	if (vkAllocateMemory(VK, &info, nullptr, &block.Memory) != VK_SUCCESS)
	{
		WWDEBUG_SAY(("Vulkan: out of memory (%llu bytes)", (unsigned long long)block.Size));
		return false;
	}
	if (host)
	{
		void * mapped = nullptr;
		if (vkMapMemory(VK, block.Memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		{
			vkFreeMemory(VK, block.Memory, nullptr);
			return false;
		}
		block.Mapped = static_cast<unsigned char *>(mapped);
	}
	block.Free.push_back(std::make_pair((VkDeviceSize)0, block.Size));
	Blocks.push_back(block);
	return Allocate_Memory(requirements, host, out);
}

// ResourceMutex held.
void Free_Memory(const VKAllocation & allocation)
{
	if (allocation.Memory == VK_NULL_HANDLE || allocation.Block >= Blocks.size())
	{
		return;
	}
	MemoryBlock & block = Blocks[allocation.Block];
	std::vector<std::pair<VkDeviceSize, VkDeviceSize>> & free_list = block.Free;
	std::pair<VkDeviceSize, VkDeviceSize> range(allocation.Offset, allocation.Size);
	std::vector<std::pair<VkDeviceSize, VkDeviceSize>>::iterator it = std::lower_bound(free_list.begin(), free_list.end(), range);
	it = free_list.insert(it, range);
	// Merge with the neighbours.
	if (it + 1 != free_list.end() && it->first + it->second == (it + 1)->first)
	{
		it->second += (it + 1)->second;
		free_list.erase(it + 1);
	}
	if (it != free_list.begin() && (it - 1)->first + (it - 1)->second == it->first)
	{
		(it - 1)->second += it->second;
		free_list.erase(it);
	}
}

// ResourceMutex held.
unsigned Add_Table_Entry(VkBuffer buffer)
{
	if (!FreeTableEntries.empty())
	{
		unsigned index = FreeTableEntries.back();
		FreeTableEntries.pop_back();
		BufferTable[index] = buffer;
		return index;
	}
	BufferTable.push_back(buffer);
	return (unsigned)BufferTable.size() - 1;
}

unsigned long long Make_Address(unsigned table, VkDeviceSize offset)
{
	return ((unsigned long long)(table + 1) << VK_ADDRESS_SHIFT) | (unsigned long long)offset;
}

bool Resolve_Address(unsigned long long address, VkBuffer & buffer, VkDeviceSize & offset)
{
	const unsigned long long index = (address >> VK_ADDRESS_SHIFT);
	if (index == 0)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(ResourceMutex);
	if (index - 1 >= BufferTable.size() || BufferTable[(size_t)(index - 1)] == VK_NULL_HANDLE)
	{
		return false;
	}
	buffer = BufferTable[(size_t)(index - 1)];
	offset = address & ((1ull << VK_ADDRESS_SHIFT) - 1);
	return true;
}

bool Create_Frame_Page(VkDeviceSize size, FramePage & page)
{
	memset(&page, 0, sizeof(page));
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
		| VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!Check(vkCreateBuffer(VK, &info, nullptr, &page.Buffer), "vkCreateBuffer (frame memory)"))
	{
		return false;
	}
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(VK, page.Buffer, &requirements);
	int type = Find_Memory_Type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (unsigned)type;
	void * mapped = nullptr;
	if (type < 0 || vkAllocateMemory(VK, &allocate, nullptr, &page.Memory) != VK_SUCCESS
		|| vkBindBufferMemory(VK, page.Buffer, page.Memory, 0) != VK_SUCCESS
		|| vkMapMemory(VK, page.Memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		Set_Failure_Reason("frame memory of %llu bytes", (unsigned long long)size);
		vkDestroyBuffer(VK, page.Buffer, nullptr);
		if (page.Memory)
			vkFreeMemory(VK, page.Memory, nullptr);
		memset(&page, 0, sizeof(page));
		return false;
	}
	page.CPU = static_cast<unsigned char *>(mapped);
	page.Size = size;
	page.Offset = 0;
	std::lock_guard<std::mutex> lock(ResourceMutex);
	page.Table = Add_Table_Entry(page.Buffer);
	return true;
}

// Frees deferred releases whose submission has completed. Main thread.
void Process_Deferred(bool all)
{
	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		FrameSlot & frame = Frames[i];
		if (frame.Submitted && vkGetFenceStatus(VK, frame.Fence) == VK_SUCCESS && frame.Serial > CompletedSerial)
		{
			CompletedSerial = frame.Serial;
		}
	}
	std::vector<DeferredRelease> ready;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		for (size_t i = 0; i < DeferredReleases.size();)
		{
			if (all || DeferredReleases[i].Serial <= CompletedSerial)
			{
				ready.push_back(DeferredReleases[i]);
				DeferredReleases[i] = DeferredReleases.back();
				DeferredReleases.pop_back();
			}
			else
			{
				++i;
			}
		}
		for (size_t i = 0; i < ready.size(); ++i)
		{
			const DeferredRelease & r = ready[i];
			if (r.Table != ~0u && r.Table < BufferTable.size())
			{
				BufferTable[r.Table] = VK_NULL_HANDLE;
				FreeTableEntries.push_back(r.Table);
			}
			Free_Memory(r.Allocation);
		}
	}
	for (size_t i = 0; i < ready.size(); ++i)
	{
		const DeferredRelease & r = ready[i];
		for (int v = 0; v < 2; ++v)
		{
			if (r.Views[v] != VK_NULL_HANDLE && (v == 0 || r.Views[1] != r.Views[0]))
				vkDestroyImageView(VK, r.Views[v], nullptr);
		}
		if (r.Image != VK_NULL_HANDLE)
			vkDestroyImage(VK, r.Image, nullptr);
		if (r.Buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(VK, r.Buffer, nullptr);
		delete r.ImageData;
		delete r.BufferData;
	}
}

void Wait_Idle()
{
	if (VK)
	{
		vkDeviceWaitIdle(VK);
		for (int i = 0; i < FRAME_COUNT; ++i)
		{
			if (Frames[i].Submitted && Frames[i].Serial > CompletedSerial)
				CompletedSerial = Frames[i].Serial;
		}
		if (NextSerial > 0 && CompletedSerial < NextSerial - 1)
			CompletedSerial = NextSerial - 1;
	}
}

// A barrier with conservative stages and access masks.
void Image_Barrier(VkCommandBuffer cb, VKImageData * image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image->Image;
	barrier.subresourceRange.aspectMask = image->Aspect;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	++DX12Device_Counters.Barriers;
}

void Raw_Image_Barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
	VKImageData data;
	memset(&data, 0, sizeof(data));
	data.Image = image;
	data.Aspect = aspect;
	Image_Barrier(cb, &data, from, to);
}

void End_Rendering()
{
	if (RenderingActive)
	{
		vkCmdEndRenderingKHR(Frames[FrameIndex].Main);
		RenderingActive = false;
	}
}

// Moves a render or depth target on the main command buffer, clearing it first when it is new.
void Use_Target_Image(VKImageData * image, DX12TextureClass * texture, VkImageLayout layout)
{
	VkCommandBuffer cb = Frames[FrameIndex].Main;
	if (texture && texture->Needs_Initial_Clear())
	{
		End_Rendering();
		Image_Barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageSubresourceRange range = { image->Aspect, 0, 1, 0, 1 };
		if (image->Aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
		{
			VkClearDepthStencilValue value = { 1.0f, 0 };
			vkCmdClearDepthStencilImage(cb, image->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
		}
		else
		{
			VkClearColorValue value;
			memset(&value, 0, sizeof(value));
			vkCmdClearColorImage(cb, image->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
		}
		image->Layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		texture->Set_Initialized();
	}
	if (image->Layout != layout)
	{
		End_Rendering();
		Image_Barrier(cb, image, image->Layout, layout);
		image->Layout = layout;
	}
}

VKImageData * Current_Color_Image()
{
	return TargetColor ? TargetColor->Peek_VK_Image() : BackBuffer;
}

unsigned Target_Width()
{
	return TargetColor ? TargetColor->Get_Width() : DX12Device::Get_Width();
}

unsigned Target_Height()
{
	return TargetColor ? TargetColor->Get_Height() : DX12Device::Get_Height();
}

// The depth image of the current targets, or null; as Direct3D 8, a depth buffer smaller than the color target
// is not bound.
VKImageData * Current_Depth_Image(DX12TextureClass ** texture)
{
	*texture = nullptr;
	if (!TargetColor)
	{
		return DepthBuffer;
	}
	const unsigned width = TargetColor->Get_Width();
	const unsigned height = TargetColor->Get_Height();
	if (TargetDepth)
	{
		if (TargetDepth->Get_Width() >= width && TargetDepth->Get_Height() >= height && TargetDepth->Peek_VK_Image())
		{
			*texture = TargetDepth;
			return TargetDepth->Peek_VK_Image();
		}
		return nullptr;
	}
	if (TargetDefaultDepth && DepthBuffer && DepthBuffer->Width >= width && DepthBuffer->Height >= height)
	{
		return DepthBuffer;
	}
	return nullptr;
}

void Apply_Viewport()
{
	if (!RenderingActive)
	{
		return;
	}
	VkCommandBuffer cb = Frames[FrameIndex].Main;
	// Direct3D's y axis points up in clip space: a viewport with a negative height keeps the shaders as they are.
	VkViewport viewport = CurrentViewport;
	if (viewport.width < 1.0f)
		viewport.width = 1.0f;
	if (viewport.height < 1.0f)
		viewport.height = 1.0f;
	viewport.y = viewport.y + viewport.height;
	viewport.height = -viewport.height;
	vkCmdSetViewport(cb, 0, 1, &viewport);

	VkRect2D scissor = CurrentScissor;
	const int width = (int)Target_Width();
	const int height = (int)Target_Height();
	if (scissor.offset.x < 0)
		scissor.offset.x = 0;
	if (scissor.offset.y < 0)
		scissor.offset.y = 0;
	if (scissor.offset.x > width)
		scissor.offset.x = width;
	if (scissor.offset.y > height)
		scissor.offset.y = height;
	if ((int)scissor.extent.width > width - scissor.offset.x)
		scissor.extent.width = (unsigned)(width - scissor.offset.x);
	if ((int)scissor.extent.height > height - scissor.offset.y)
		scissor.extent.height = (unsigned)(height - scissor.offset.y);
	vkCmdSetScissor(cb, 0, 1, &scissor);
}

void Set_Full_Viewport()
{
	CurrentViewport.x = 0.0f;
	CurrentViewport.y = 0.0f;
	CurrentViewport.width = (float)Target_Width();
	CurrentViewport.height = (float)Target_Height();
	CurrentViewport.minDepth = 0.0f;
	CurrentViewport.maxDepth = 1.0f;
	CurrentScissor.offset.x = 0;
	CurrentScissor.offset.y = 0;
	CurrentScissor.extent.width = Target_Width();
	CurrentScissor.extent.height = Target_Height();
	Apply_Viewport();
}

// Starts dynamic rendering into the current targets. Recording only.
bool Begin_Rendering()
{
	if (RenderingActive)
	{
		return true;
	}
	VKImageData * color = Current_Color_Image();
	if (!color)
	{
		return false;
	}
	Use_Target_Image(color, TargetColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	DX12TextureClass * depth_texture = nullptr;
	VKImageData * depth = Current_Depth_Image(&depth_texture);
	if (depth)
	{
		Use_Target_Image(depth, depth_texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	TargetHasDepth = depth != nullptr;

	VkRenderingAttachmentInfoKHR color_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	color_attachment.imageView = color->View;
	color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingAttachmentInfoKHR depth_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	if (depth)
	{
		depth_attachment.imageView = depth->View;
		depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	}
	VkRenderingInfoKHR info = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
	info.renderArea.offset.x = 0;
	info.renderArea.offset.y = 0;
	info.renderArea.extent.width = color->Width;
	info.renderArea.extent.height = color->Height;
	info.layerCount = 1;
	info.colorAttachmentCount = 1;
	info.pColorAttachments = &color_attachment;
	info.pDepthAttachment = depth ? &depth_attachment : nullptr;
	info.pStencilAttachment = depth ? &depth_attachment : nullptr;
	vkCmdBeginRenderingKHR(Frames[FrameIndex].Main, &info);
	RenderingActive = true;
	BoundPipeline = VK_NULL_HANDLE;
	BoundStencilRef = ~0u;
	Apply_Viewport();
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame slots, frame memory, uploads
// ---------------------------------------------------------------------------------------------------------------

void Prepare_Frame_Slot()
{
	if (FrameSlotPrepared)
	{
		return;
	}
	FrameSlot & frame = Frames[FrameIndex];
	if (frame.Submitted)
	{
		const unsigned long long start = Query_Ticks();
		vkWaitForFences(VK, 1, &frame.Fence, VK_TRUE, UINT64_MAX);
		++DX12Device_Counters.FenceWaits[DX12_WAIT_FRAME_SLOT];
		DX12Device_Counters.FenceWaitTicks[DX12_WAIT_FRAME_SLOT] += Query_Ticks() - start;
		if (frame.Serial > CompletedSerial)
			CompletedSerial = frame.Serial;
		frame.Submitted = false;
	}
	for (size_t i = 0; i < frame.Pages.size(); ++i)
	{
		frame.Pages[i].Offset = 0;
	}
	frame.PageIndex = 0;
	vkResetCommandPool(VK, frame.Pool, 0);
	frame.UploadOpen = false;
	FrameSlotPrepared = true;
	Process_Deferred(false);
}

VkCommandBuffer Upload_Command_Buffer()
{
	FrameSlot & frame = Frames[FrameIndex];
	if (!frame.UploadOpen)
	{
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(frame.Upload, &begin);
		frame.UploadOpen = true;
	}
	return frame.Upload;
}

void Flush_Dirty_Textures()
{
	if (DirtyTextures.empty() || UploadingDirtyTextures)
	{
		return;
	}
	std::vector<DX12TextureClass *> textures;
	textures.swap(DirtyTextures);
	UploadingDirtyTextures = true;
	for (size_t i = 0; i < textures.size(); ++i)
	{
		if (textures[i]->Upload_Dirty_Levels())
			DirtyTextures.push_back(textures[i]);
		else
			textures[i]->Release_Ref();
	}
	UploadingDirtyTextures = false;
}

// Records the first layouts of new textures and the queued level uploads into the upload command buffer of the
// frame slot. Main thread.
void Record_Uploads()
{
	Flush_Dirty_Textures();
	std::vector<DX12TextureClass *> initial;
	std::vector<PendingUpload> uploads;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		initial.swap(InitialLayouts);
		if (HasPendingUploads.load())
		{
			uploads.swap(PendingUploads);
			PendingUploadBytes = 0;
			HasPendingUploads = false;
		}
	}
	if (initial.empty() && uploads.empty())
	{
		return;
	}
	DX12StatsScope stats_scope(DX12Device_Counters.UploadRecordTicks);
	Prepare_Frame_Slot();
	VkCommandBuffer cb = Upload_Command_Buffer();

	for (size_t i = 0; i < initial.size(); ++i)
	{
		VKImageData * image = initial[i]->Peek_VK_Image();
		if (image && !image->InitialLayoutDone)
		{
			// New images are zero, as Direct3D 12 committed resources are.
			Image_Barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			const bool block = image->Format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || image->Format == VK_FORMAT_BC2_UNORM_BLOCK
				|| image->Format == VK_FORMAT_BC3_UNORM_BLOCK;
			if (!block)
			{
				VkClearColorValue value;
				memset(&value, 0, sizeof(value));
				VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
				vkCmdClearColorImage(cb, image->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
			}
			Image_Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			image->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			image->InitialLayoutDone = true;
		}
		initial[i]->Release_Ref();
	}

	for (size_t i = 0; i < uploads.size(); ++i)
	{
		PendingUpload & upload = uploads[i];
		DX12TextureClass * texture = upload.Texture;
		VKImageData * image = texture->Peek_VK_Image();
		if (image && texture->Is_Render_Target())
		{
			if (!LoggedRenderTargetUpload)
			{
				WWDEBUG_SAY(("Vulkan: uploads into render targets are not supported yet, upload skipped"));
				LoggedRenderTargetUpload = true;
			}
			image = nullptr;
		}
		if (image && image->InitialLayoutDone && upload.Level < image->Levels && !upload.Data.empty())
		{
			void * cpu = nullptr;
			unsigned long long address = 0;
			VkBuffer buffer;
			VkDeviceSize offset;
			if (DX12Device::Allocate_Frame_Memory((unsigned)upload.Data.size(), &cpu, &address) && Resolve_Address(address, buffer, offset))
			{
				memcpy(cpu, upload.Data.data(), upload.Data.size());
				unsigned width = image->Width >> upload.Level;
				unsigned height = image->Height >> upload.Level;
				VkBufferImageCopy region;
				memset(&region, 0, sizeof(region));
				region.bufferOffset = offset;
				region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				region.imageSubresource.mipLevel = upload.Level;
				region.imageSubresource.layerCount = 1;
				region.imageExtent.width = width ? width : 1;
				region.imageExtent.height = height ? height : 1;
				region.imageExtent.depth = 1;
				Image_Barrier(cb, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vkCmdCopyBufferToImage(cb, buffer, image->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				Image_Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
		}
		texture->Set_Initialized();
		texture->Release_Ref();
	}
}

// Submits the upload command buffer of the slot (if any) and the main command buffer (if recording).
void Submit(bool with_main, VkSemaphore wait, VkSemaphore signal)
{
	FrameSlot & frame = Frames[FrameIndex];
	VkCommandBuffer buffers[2];
	unsigned count = 0;
	if (frame.UploadOpen)
	{
		vkEndCommandBuffer(frame.Upload);
		buffers[count++] = frame.Upload;
		frame.UploadOpen = false;
	}
	if (with_main)
	{
		vkEndCommandBuffer(frame.Main);
		buffers[count++] = frame.Main;
	}
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	info.waitSemaphoreCount = wait ? 1 : 0;
	info.pWaitSemaphores = wait ? &wait : nullptr;
	info.pWaitDstStageMask = wait ? &wait_stage : nullptr;
	info.commandBufferCount = count;
	info.pCommandBuffers = buffers;
	info.signalSemaphoreCount = signal ? 1 : 0;
	info.pSignalSemaphores = signal ? &signal : nullptr;
	vkResetFences(VK, 1, &frame.Fence);
	VkResult result = vkQueueSubmit(Queue, 1, &info, frame.Fence);
	if (result != VK_SUCCESS)
	{
		WWDEBUG_SAY(("Vulkan: vkQueueSubmit failed (%d)", (int)result));
		Debug_Log("vkQueueSubmit failed (%d)", (int)result);
	}
	frame.Submitted = true;
	frame.Serial = NextSerial++;
}

// Uploads outside a frame: submitted at once and waited for. Main thread.
void Execute_Uploads_Now()
{
	if (Recording)
	{
		return;
	}
	bool pending;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		pending = HasPendingUploads.load() || !InitialLayouts.empty();
	}
	if (!pending && DirtyTextures.empty())
	{
		return;
	}
	DX12StatsScope stats_scope(DX12Device_Counters.UploadExecuteTicks);
	++DX12Device_Counters.UploadExecutes;
	Prepare_Frame_Slot();
	Record_Uploads();
	FrameSlot & frame = Frames[FrameIndex];
	if (frame.UploadOpen)
	{
		Submit(false, VK_NULL_HANDLE, VK_NULL_HANDLE);
		vkWaitForFences(VK, 1, &frame.Fence, VK_TRUE, UINT64_MAX);
		++DX12Device_Counters.FenceWaits[DX12_WAIT_UPLOADS];
		CompletedSerial = frame.Serial;
		frame.Submitted = false;
		// The slot's frame memory and command pool start over.
		FrameSlotPrepared = false;
		Prepare_Frame_Slot();
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------------------------

unsigned long long Hash64(const void * data, size_t size, unsigned long long hash = 1469598103934665603ull)
{
	const unsigned char * bytes = static_cast<const unsigned char *>(data);
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

void Shader_Cache_Path(char * path, size_t size, unsigned long long key)
{
	size_t length = strlen(UserDataDirectory);
	const char * separator = (length > 0 && (UserDataDirectory[length - 1] == '\\' || UserDataDirectory[length - 1] == '/')) ? "" : "\\";
	snprintf(path, size, "%s%sShaderCache\\vk_%016llx.spv", UserDataDirectory, separator, key);
}

bool Load_Cached(unsigned long long key, std::vector<unsigned> & code)
{
	if (DX12Device_NoShaderCache || UserDataDirectory[0] == '\0')
	{
		return false;
	}
	char path[MAX_PATH * 2];
	Shader_Cache_Path(path, sizeof(path), key);
	FILE * file = fopen(path, "rb");
	if (!file)
	{
		return false;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	bool ok = false;
	if (size > 20 && (size % 4) == 0)
	{
		code.resize(size / 4);
		ok = fread(code.data(), 1, size, file) == (size_t)size && code[0] == 0x07230203u;
	}
	fclose(file);
	if (!ok)
		code.clear();
	return ok;
}

void Store_Cached(unsigned long long key, const std::vector<unsigned> & code)
{
	if (DX12Device_NoShaderCache || UserDataDirectory[0] == '\0')
	{
		return;
	}
	char path[MAX_PATH * 2];
	Shader_Cache_Path(path, sizeof(path), key);
	char directory[MAX_PATH * 2];
	strncpy(directory, path, sizeof(directory));
	directory[sizeof(directory) - 1] = '\0';
	char * slash = strrchr(directory, '\\');
	if (slash)
	{
		*slash = '\0';
		CreateDirectoryA(directory, nullptr);
	}
	FILE * file = fopen(path, "wb");
	if (file)
	{
		fwrite(code.data(), 4, code.size(), file);
		fclose(file);
	}
}

// Splits a semantic such as TEXCOORD3 into TEXCOORD and 3.
void Split_Semantic(const char * text, std::string & name, unsigned & index)
{
	std::string upper(text);
	for (size_t i = 0; i < upper.size(); ++i)
		upper[i] = (char)toupper((unsigned char)upper[i]);
	size_t digits = upper.size();
	while (digits > 0 && isdigit((unsigned char)upper[digits - 1]))
		--digits;
	name = upper.substr(0, digits);
	index = digits < upper.size() ? (unsigned)atoi(upper.c_str() + digits) : 0;
}

// Reads the stage inputs and outputs (in.var.* and out.var.* names, as dxc writes them) and their locations.
void Reflect(ShaderEntry & entry)
{
	const std::vector<unsigned> & code = entry.Code;
	std::map<unsigned, std::string> names;
	std::map<unsigned, std::pair<unsigned, size_t>> locations;
	for (size_t i = 5; i < code.size();)
	{
		const unsigned opcode = code[i] & 0xffff;
		const unsigned count = code[i] >> 16;
		if (count == 0)
			break;
		if (opcode == 5 && count >= 3)	// OpName
		{
			names[code[i + 1]] = std::string(reinterpret_cast<const char *>(&code[i + 2]));
		}
		else if (opcode == 71 && count >= 4 && code[i + 2] == 30)	// OpDecorate Location
		{
			locations[code[i + 1]] = std::make_pair(code[i + 3], i + 3);
		}
		i += count;
	}
	for (std::map<unsigned, std::pair<unsigned, size_t>>::iterator it = locations.begin(); it != locations.end(); ++it)
	{
		std::map<unsigned, std::string>::iterator name = names.find(it->first);
		if (name == names.end())
			continue;
		const bool input = name->second.compare(0, 7, "in.var.") == 0;
		const bool output = name->second.compare(0, 8, "out.var.") == 0;
		if (!input && !output)
			continue;
		InterfaceVar var;
		Split_Semantic(name->second.c_str() + (input ? 7 : 8), var.Semantic, var.Index);
		var.Location = it->second.first;
		var.LocationWord = it->second.second;
		(input ? entry.Inputs : entry.Outputs).push_back(var);
	}
}

bool Compile_Shader(const std::string & source, const char * name, const char * entry, const char * target, ShaderEntry & out)
{
	// Shader model 5 targets of the generated HLSL become shader model 6 for dxc.
	char profile[16];
	snprintf(profile, sizeof(profile), "%c%c_6_0", target[0], target[1]);

	static const char * const options = "spirv vulkan1.1 dx-layout dx-position-w b0 t4 s12 hv2018 O3 v1";
	unsigned long long key = Hash64(source.data(), source.size());
	key = Hash64(entry, strlen(entry), key);
	key = Hash64(profile, strlen(profile), key);
	key = Hash64(options, strlen(options), key);
	if (Load_Cached(key, out.Code))
	{
		++DX12Device_Counters.ShaderCacheHits;
		Reflect(out);
		return true;
	}
	if (!Compiler)
	{
		return false;
	}

	std::wstring wide_entry(entry, entry + strlen(entry));
	std::wstring wide_profile(profile, profile + strlen(profile));
	const wchar_t * args[] =
	{
		L"-E", wide_entry.c_str(), L"-T", wide_profile.c_str(),
		L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fvk-use-dx-layout", L"-fvk-use-dx-position-w",
		L"-fvk-b-shift", L"0", L"0", L"-fvk-t-shift", L"4", L"0", L"-fvk-s-shift", L"12", L"0",
		L"-HV", L"2018", L"-O3",
	};
	DxcBuffer buffer;
	buffer.Ptr = source.data();
	buffer.Size = source.size();
	buffer.Encoding = DXC_CP_UTF8;
	IDxcResult * result = nullptr;
	const unsigned long long start = Query_Ticks();
	HRESULT hr = Compiler->Compile(&buffer, args, sizeof(args) / sizeof(args[0]), nullptr, __uuidof(IDxcResult), (void **)&result);
	++DX12Device_Counters.ShaderCompiles;
	DX12Device_Counters.ShaderCompileTicks += Query_Ticks() - start;
	HRESULT status = E_FAIL;
	if (SUCCEEDED(hr) && result)
	{
		result->GetStatus(&status);
	}
	if (FAILED(hr) || FAILED(status))
	{
		std::string message = "no compiler output";
		IDxcBlobUtf8 * errors = nullptr;
		if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void **)&errors, nullptr)) && errors)
		{
			if (errors->GetStringLength() > 0)
				message = errors->GetStringPointer();
			errors->Release();
		}
		WWDEBUG_SAY(("Vulkan: compiling %s (%s) failed: %s", name, profile, message.c_str()));
		Debug_Log("frame %llu: compiling %s (%s) failed:\n%s\n----- source -----\n%s\n-----", DX12Device::Get_Frame_Number(), name, profile,
			message.c_str(), source.c_str());
		if (result)
			result->Release();
		return false;
	}
	IDxcBlob * object = nullptr;
	if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void **)&object, nullptr)) && object && object->GetBufferSize() >= 20)
	{
		out.Code.resize(object->GetBufferSize() / 4);
		memcpy(out.Code.data(), object->GetBufferPointer(), out.Code.size() * 4);
	}
	if (object)
		object->Release();
	result->Release();
	if (out.Code.empty())
	{
		return false;
	}
	Store_Cached(key, out.Code);
	++DX12Device_Counters.ShaderCacheStores;
	Reflect(out);
	return true;
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
	if (Compile_Shader(source, name, "VSMain", "vs_5_0", entry))
		Debug_Log("frame %llu: compiled %s (FVF 0x%x)", DX12Device::Get_Frame_Number(), name, key.FVF);
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
	if (Compile_Shader(source, name, "PSMain", "ps_5_0", entry))
		Debug_Log("frame %llu: compiled %s", DX12Device::Get_Frame_Number(), name);
	return PixelShaders.insert(PixelShaderMap::value_type(key, entry)).first->second;
}

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
			return cache.Vertex;
		cache.VertexCompiled = true;
		entry = &cache.Vertex;
	}
	else
	{
		if (variant)
			key = *variant;
		std::map<DX12GamePSVariant, ShaderEntry, GamePSVariantLess>::iterator it = cache.Pixel.find(key);
		if (it != cache.Pixel.end())
			return it->second;
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
	if (Compile_Shader(source, name, entry_point, target, *entry))
		Debug_Log("frame %llu: compiled %s", DX12Device::Get_Frame_Number(), name);
	return *entry;
}

VkFormat Map_DXGI_Format(unsigned format)
{
	switch (format)
	{
	case DXGI_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
	case DXGI_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
	case DXGI_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
	case DXGI_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case DXGI_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
	case DXGI_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
	case DXGI_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
	case DXGI_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
	case DXGI_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
	default: return VK_FORMAT_UNDEFINED;
	}
}

unsigned Intern_Game_Layout(const DX12GameInputLayout & source)
{
	for (size_t i = 0; i < GameLayouts.size(); ++i)
	{
		if (memcmp(&GameLayouts[i].Source, &source, sizeof(source)) == 0)
			return (unsigned)i + 1;
	}
	GameLayout layout;
	memset(&layout, 0, sizeof(layout));
	layout.Source = source;
	for (unsigned i = 0; i < source.ElementCount; ++i)
	{
		VertexElement & element = layout.Layout.Elements[i];
		element.Semantic = "V";
		element.Index = source.Elements[i].Register;
		element.Format = Map_DXGI_Format(source.Elements[i].Format);
		element.Offset = source.Elements[i].Offset;
	}
	layout.Layout.Count = source.ElementCount;
	layout.Layout.Size = source.Size;
	GameLayouts.push_back(layout);
	return (unsigned)GameLayouts.size();
}

// The vertex layout of a Direct3D 8 FVF (as Decode_FVF of dx12device.cpp).
bool Decode_FVF(unsigned fvf, VertexLayout & layout)
{
	static const VkFormat texcoord_formats[4] = { VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_SFLOAT };
	static const unsigned texcoord_sizes[4] = { 8, 12, 16, 4 };
	memset(&layout, 0, sizeof(layout));
	unsigned offset = 0;
	unsigned count = 0;
	const unsigned position = fvf & FVF_POSITION_MASK;
	VertexElement element = { "POSITION", 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
	if (position == FVF_XYZ)
	{
		offset = 12;
	}
	else if (position == FVF_XYZRHW)
	{
		element.Format = VK_FORMAT_R32G32B32A32_SFLOAT;
		offset = 16;
	}
	else if (position >= FVF_XYZB1)
	{
		offset = 12 + 4 * (((position - FVF_XYZB1) >> 1) + 1);
	}
	else
	{
		return false;
	}
	layout.Elements[count++] = element;
	if (fvf & FVF_NORMAL)
	{
		VertexElement normal = { "NORMAL", 0, VK_FORMAT_R32G32B32_SFLOAT, offset };
		layout.Elements[count++] = normal;
		offset += 12;
	}
	if (fvf & FVF_PSIZE)
	{
		offset += 4;
	}
	if (fvf & FVF_DIFFUSE)
	{
		VertexElement diffuse = { "COLOR", 0, VK_FORMAT_R8G8B8A8_UNORM, offset };
		layout.Elements[count++] = diffuse;
		offset += 4;
	}
	if (fvf & FVF_SPECULAR)
	{
		VertexElement specular = { "COLOR", 1, VK_FORMAT_R8G8B8A8_UNORM, offset };
		layout.Elements[count++] = specular;
		offset += 4;
	}
	const unsigned texcount = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
	if (texcount > FVF_MAX_TEXCOORDS)
	{
		return false;
	}
	for (unsigned i = 0; i < texcount; ++i)
	{
		const unsigned size_bits = (fvf >> (16 + i * 2)) & 3;
		VertexElement texcoord = { "TEXCOORD", i, texcoord_formats[size_bits], offset };
		layout.Elements[count++] = texcoord;
		offset += texcoord_sizes[size_bits];
	}
	layout.Count = count;
	layout.Size = offset;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Pipelines and samplers
// ---------------------------------------------------------------------------------------------------------------

VkBlendFactor Map_Blend(unsigned blend, bool alpha, VkBlendFactor fallback)
{
	switch (blend)
	{
	case 1: return VK_BLEND_FACTOR_ZERO;
	case 2: return VK_BLEND_FACTOR_ONE;
	case 3: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
	case 4: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
	case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 7: return VK_BLEND_FACTOR_DST_ALPHA;
	case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case 9: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
	case 10: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default: return fallback;
	}
}

VkShaderModule Create_Module(const std::vector<unsigned> & code)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = code.size() * 4;
	info.pCode = code.data();
	VkShaderModule module = VK_NULL_HANDLE;
	vkCreateShaderModule(VK, &info, nullptr, &module);
	return module;
}

VkPipeline Get_Pipeline(const PSOKey & key, const VertexLayout & layout, const ShaderEntry & vs, const ShaderEntry & ps)
{
	std::map<PSOKey, VkPipeline, PSOKeyLess>::iterator it = Pipelines.find(key);
	if (it != Pipelines.end())
	{
		return it->second;
	}

	VkPipeline pipeline = VK_NULL_HANDLE;

	// Vertex attributes: the vertex shader's inputs found in the layout, matched by semantic.
	VkVertexInputAttributeDescription attributes[MAX_INPUT_ELEMENTS];
	unsigned attribute_count = 0;
	bool inputs_ok = true;
	for (size_t i = 0; i < vs.Inputs.size(); ++i)
	{
		const InterfaceVar & input = vs.Inputs[i];
		bool found = false;
		for (unsigned e = 0; e < layout.Count && attribute_count < MAX_INPUT_ELEMENTS; ++e)
		{
			if (input.Semantic == layout.Elements[e].Semantic && input.Index == layout.Elements[e].Index && layout.Elements[e].Format != VK_FORMAT_UNDEFINED)
			{
				attributes[attribute_count].location = input.Location;
				attributes[attribute_count].binding = 0;
				attributes[attribute_count].format = layout.Elements[e].Format;
				attributes[attribute_count].offset = layout.Elements[e].Offset;
				++attribute_count;
				found = true;
				break;
			}
		}
		if (!found)
		{
			Debug_Log("frame %llu: vertex shader %u reads %s%u, which the vertex layout (FVF 0x%x) does not have",
				DX12Device::Get_Frame_Number(), vs.Id, input.Semantic.c_str(), input.Index, key.FVF);
			inputs_ok = false;
		}
	}

	if (inputs_ok)
	{
		// The pixel shader reads its inputs from the locations the vertex shader writes them to.
		std::vector<unsigned> ps_code = ps.Code;
		for (size_t i = 0; i < ps.Inputs.size(); ++i)
		{
			for (size_t o = 0; o < vs.Outputs.size(); ++o)
			{
				if (ps.Inputs[i].Semantic == vs.Outputs[o].Semantic && ps.Inputs[i].Index == vs.Outputs[o].Index)
				{
					ps_code[ps.Inputs[i].LocationWord] = vs.Outputs[o].Location;
					break;
				}
			}
		}
		VkShaderModule vs_module = Create_Module(vs.Code);
		VkShaderModule ps_module = Create_Module(ps_code);

		VkPipelineShaderStageCreateInfo stages[2];
		memset(stages, 0, sizeof(stages));
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs_module;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = ps_module;
		stages[1].pName = "main";
		// dxc keeps the HLSL entry point name.
		for (size_t i = 5; i < vs.Code.size();)
		{
			const unsigned opcode = vs.Code[i] & 0xffff;
			const unsigned count = vs.Code[i] >> 16;
			if (count == 0)
				break;
			if (opcode == 15)	// OpEntryPoint
				stages[0].pName = reinterpret_cast<const char *>(&vs.Code[i + 3]);
			i += count;
		}
		for (size_t i = 5; i < ps_code.size();)
		{
			const unsigned opcode = ps_code[i] & 0xffff;
			const unsigned count = ps_code[i] >> 16;
			if (count == 0)
				break;
			if (opcode == 15)
				stages[1].pName = reinterpret_cast<const char *>(&ps.Code[i + 3]);
			i += count;
		}

		VkVertexInputBindingDescription binding = { 0, key.VertexStride, VK_VERTEX_INPUT_RATE_VERTEX };
		VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &binding;
		vertex_input.vertexAttributeDescriptionCount = attribute_count;
		vertex_input.pVertexAttributeDescriptions = attributes;

		VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		assembly.topology = (VkPrimitiveTopology)key.Topology;

		VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewport.viewportCount = 1;
		viewport.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		raster.polygonMode = (key.FillMode == 2 && EnabledFeatures.fillModeNonSolid) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
		raster.cullMode = key.CullMode == 2 ? VK_CULL_MODE_FRONT_BIT : (key.CullMode == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
		raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
		raster.depthBiasEnable = key.DepthBias != 0 ? VK_TRUE : VK_FALSE;
		raster.depthBiasConstantFactor = (float)(key.DepthBias * ZBIAS_TO_DEPTH_BIAS);
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depth.depthTestEnable = key.ZEnable ? VK_TRUE : VK_FALSE;
		depth.depthWriteEnable = key.ZWriteEnable ? VK_TRUE : VK_FALSE;
		depth.depthCompareOp = (key.ZFunc >= 1 && key.ZFunc <= 8) ? (VkCompareOp)(key.ZFunc - 1) : VK_COMPARE_OP_LESS_OR_EQUAL;
		VkStencilOpState stencil;
		memset(&stencil, 0, sizeof(stencil));
		stencil.failOp = VK_STENCIL_OP_KEEP;
		stencil.passOp = VK_STENCIL_OP_KEEP;
		stencil.depthFailOp = VK_STENCIL_OP_KEEP;
		stencil.compareOp = VK_COMPARE_OP_ALWAYS;
		if (key.StencilEnable)
		{
			depth.stencilTestEnable = VK_TRUE;
			stencil.failOp = (VkStencilOp)(key.StencilFail - 1);
			stencil.depthFailOp = (VkStencilOp)(key.StencilZFail - 1);
			stencil.passOp = (VkStencilOp)(key.StencilPass - 1);
			stencil.compareOp = (VkCompareOp)(key.StencilFunc - 1);
			stencil.compareMask = key.StencilReadMask;
			stencil.writeMask = key.StencilWriteMask;
		}
		depth.front = stencil;
		depth.back = stencil;

		VkPipelineColorBlendAttachmentState blend;
		memset(&blend, 0, sizeof(blend));
		VkBlendFactor src_color, dest_color, src_alpha, dest_alpha;
		if (key.SrcBlend == BLEND_BOTHSRCALPHA)
		{
			src_color = src_alpha = VK_BLEND_FACTOR_SRC_ALPHA;
			dest_color = dest_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}
		else if (key.SrcBlend == BLEND_BOTHINVSRCALPHA)
		{
			src_color = src_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			dest_color = dest_alpha = VK_BLEND_FACTOR_SRC_ALPHA;
		}
		else
		{
			src_color = Map_Blend(key.SrcBlend, false, VK_BLEND_FACTOR_ONE);
			src_alpha = Map_Blend(key.SrcBlend, true, VK_BLEND_FACTOR_ONE);
			dest_color = Map_Blend(key.DestBlend, false, VK_BLEND_FACTOR_ZERO);
			dest_alpha = Map_Blend(key.DestBlend, true, VK_BLEND_FACTOR_ZERO);
		}
		static const VkBlendOp blend_ops[6] = { VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
		const VkBlendOp blend_op = (key.BlendOp >= 1 && key.BlendOp <= 5) ? blend_ops[key.BlendOp] : VK_BLEND_OP_ADD;
		blend.blendEnable = key.AlphaBlendEnable ? VK_TRUE : VK_FALSE;
		blend.srcColorBlendFactor = src_color;
		blend.dstColorBlendFactor = dest_color;
		blend.colorBlendOp = blend_op;
		blend.srcAlphaBlendFactor = src_alpha;
		blend.dstAlphaBlendFactor = dest_alpha;
		blend.alphaBlendOp = blend_op;
		// D3DCOLORWRITEENABLE: red 1, green 2, blue 4, alpha 8, as VkColorComponentFlags.
		blend.colorWriteMask = key.ColorWriteMask & 0xf;
		VkPipelineColorBlendStateCreateInfo blend_state = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		blend_state.attachmentCount = 1;
		blend_state.pAttachments = &blend;

		VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
		VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamic.dynamicStateCount = 3;
		dynamic.pDynamicStates = dynamic_states;

		VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
		VkPipelineRenderingCreateInfoKHR rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachmentFormats = &color_format;
		rendering.depthAttachmentFormat = (VkFormat)key.DepthFormat;
		rendering.stencilAttachmentFormat = (VkFormat)key.DepthFormat;

		VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		info.pNext = &rendering;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertex_input;
		info.pInputAssemblyState = &assembly;
		info.pViewportState = &viewport;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pDepthStencilState = &depth;
		info.pColorBlendState = &blend_state;
		info.pDynamicState = &dynamic;
		info.layout = PipelineLayout;

		const unsigned long long start = Query_Ticks();
		VkResult result = (vs_module && ps_module) ? vkCreateGraphicsPipelines(VK, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) : VK_ERROR_UNKNOWN;
		++DX12Device_Counters.PipelineCreates;
		DX12Device_Counters.PipelineCreateTicks += Query_Ticks() - start;
		if (result != VK_SUCCESS)
		{
			WWDEBUG_SAY(("Vulkan: vkCreateGraphicsPipelines failed (%d) for FVF 0x%x", (int)result, key.FVF));
			Debug_Log("frame %llu: vkCreateGraphicsPipelines failed (%d) for FVF 0x%x, shaders %u/%u", DX12Device::Get_Frame_Number(),
				(int)result, key.FVF, key.VertexShader, key.PixelShader);
			pipeline = VK_NULL_HANDLE;
		}
		if (vs_module)
			vkDestroyShaderModule(VK, vs_module, nullptr);
		if (ps_module)
			vkDestroyShaderModule(VK, ps_module, nullptr);
	}
	Pipelines[key] = pipeline;
	return pipeline;
}

VkSampler Get_Sampler(unsigned min_filter, unsigned mag_filter, unsigned mip_filter, unsigned address_u, unsigned address_v, unsigned max_anisotropy, unsigned min_lod)
{
	if (min_lod > 15)
		min_lod = 15;
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
	const unsigned key = min_filter | (mag_filter << 2) | (mip_filter << 4) | (address_u << 6) | (address_v << 9) | ((max_anisotropy - 1) << 12) | (min_lod << 16);
	std::map<unsigned, VkSampler>::iterator it = Samplers.find(key);
	if (it != Samplers.end())
	{
		return it->second;
	}
	static const VkSamplerAddressMode modes[6] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE };
	VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	info.magFilter = (anisotropic || mag_filter == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.minFilter = (anisotropic || min_filter == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.mipmapMode = (anisotropic || mip_filter >= 2) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = modes[address_u];
	info.addressModeV = modes[address_v];
	if (!MirrorOnceSupported)
	{
		if (info.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE)
			info.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		if (info.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE)
			info.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	}
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.anisotropyEnable = (anisotropic && EnabledFeatures.samplerAnisotropy) ? VK_TRUE : VK_FALSE;
	info.maxAnisotropy = (float)max_anisotropy;
	info.compareOp = VK_COMPARE_OP_NEVER;
	info.minLod = (float)min_lod;
	info.maxLod = (mip_filter == 0) ? (float)min_lod : VK_LOD_CLAMP_NONE;
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(VK, &info, nullptr, &sampler);
	++DX12Device_Counters.SamplerCreates;
	Samplers[key] = sampler;
	return sampler;
}

// Frame memory for constants: the descriptor of the copy.
bool Upload_Constants(const void * data, unsigned size, VkDescriptorBufferInfo & out)
{
	void * cpu = nullptr;
	unsigned long long address = 0;
	if (!DX12Device::Allocate_Frame_Memory(size, &cpu, &address))
	{
		return false;
	}
	memcpy(cpu, data, size);
	++DX12Device_Counters.ConstantUploads;
	DX12Device_Counters.ConstantBytes += size;
	VkDeviceSize offset;
	if (!Resolve_Address(address, out.buffer, offset))
	{
		return false;
	}
	out.offset = offset;
	out.range = size;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Device creation
// ---------------------------------------------------------------------------------------------------------------

bool Load_Libraries()
{
	if (!VulkanLibrary)
	{
		VulkanLibrary = LoadLibraryA("vulkan-1.dll");
	}
	if (!VulkanLibrary)
	{
		Set_Failure_Reason("vulkan-1.dll is missing (no Vulkan driver is installed)");
		return false;
	}
	PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)GetProcAddress(VulkanLibrary, "vkGetInstanceProcAddr");
	if (!get)
	{
		Set_Failure_Reason("vulkan-1.dll has no vkGetInstanceProcAddr");
		return false;
	}
#define VK_LOAD_GLOBAL(name) name = (PFN_##name)get(VK_NULL_HANDLE, #name);
	VK_GLOBAL_FUNCTIONS(VK_LOAD_GLOBAL)
#undef VK_LOAD_GLOBAL
	if (!vkCreateInstance)
	{
		Set_Failure_Reason("vkCreateInstance is missing");
		return false;
	}

	// The shader compiler, beside the exe. Without it only shaders in the cache can be used.
	if (!CompilerLibrary)
	{
		CompilerLibrary = LoadLibraryA("dxcompiler.dll");
	}
	if (CompilerLibrary && !Compiler)
	{
		DxcCreateInstanceFunc = (DxcCreateInstanceProc)GetProcAddress(CompilerLibrary, "DxcCreateInstance");
		if (DxcCreateInstanceFunc)
		{
			DxcCreateInstanceFunc(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void **)&Compiler);
			DxcCreateInstanceFunc(CLSID_DxcUtils, __uuidof(IDxcUtils), (void **)&CompilerUtils);
		}
	}
	if (!Compiler)
	{
		WWDEBUG_SAY(("Vulkan: dxcompiler.dll is missing, shaders come from the shader cache only"));
		Debug_Log("dxcompiler.dll is missing: shaders come from the shader cache only");
	}
	return true;
}

bool Has_Extension(const std::vector<VkExtensionProperties> & extensions, const char * name)
{
	for (size_t i = 0; i < extensions.size(); ++i)
	{
		if (strcmp(extensions[i].extensionName, name) == 0)
			return true;
	}
	return false;
}

bool Create_Instance()
{
	unsigned count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	if (count)
		vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());

	std::vector<const char *> enabled;
	enabled.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
	enabled.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
	bool debug_utils = false;
	std::vector<const char *> layers;
	if (DX12Device_DebugLayer)
	{
		unsigned layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> available(layer_count);
		if (layer_count)
			vkEnumerateInstanceLayerProperties(&layer_count, available.data());
		for (size_t i = 0; i < available.size(); ++i)
		{
			if (strcmp(available[i].layerName, "VK_LAYER_KHRONOS_validation") == 0)
				layers.push_back("VK_LAYER_KHRONOS_validation");
		}
		if (layers.empty())
			Debug_Log("VK_LAYER_KHRONOS_validation is not installed: no validation");
		if (Has_Extension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		{
			enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			debug_utils = true;
		}
	}

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "Command & Conquer Generals";
	app.pEngineName = "WW3D";
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	info.pApplicationInfo = &app;
	info.enabledExtensionCount = (unsigned)enabled.size();
	info.ppEnabledExtensionNames = enabled.data();
	info.enabledLayerCount = (unsigned)layers.size();
	info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
	if (!Check(vkCreateInstance(&info, nullptr, &Instance), "vkCreateInstance"))
	{
		return false;
	}
	PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)GetProcAddress(VulkanLibrary, "vkGetInstanceProcAddr");
#define VK_LOAD_INSTANCE(name) name = (PFN_##name)get(Instance, #name);
	VK_INSTANCE_FUNCTIONS(VK_LOAD_INSTANCE)
	VK_DEBUG_FUNCTIONS(VK_LOAD_INSTANCE)
#undef VK_LOAD_INSTANCE
	if (debug_utils && vkCreateDebugUtilsMessengerEXT)
	{
		VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
		messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger.pfnUserCallback = Debug_Callback;
		vkCreateDebugUtilsMessengerEXT(Instance, &messenger, nullptr, &Messenger);
	}
	Debug_Log("instance created, validation %s", layers.empty() ? "off" : "on");
	return true;
}

bool Create_Device()
{
	unsigned count = 0;
	vkEnumeratePhysicalDevices(Instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	if (count)
		vkEnumeratePhysicalDevices(Instance, &count, devices.data());
	if (devices.empty())
	{
		Set_Failure_Reason("no Vulkan adapter");
		return false;
	}

	// The first discrete adapter that can present to the window, else the first that can.
	int best = -1;
	int best_score = -1;
	unsigned best_family = 0;
	for (size_t d = 0; d < devices.size(); ++d)
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(devices[d], &properties);
		unsigned ext_count = 0;
		vkEnumerateDeviceExtensionProperties(devices[d], nullptr, &ext_count, nullptr);
		std::vector<VkExtensionProperties> extensions(ext_count);
		if (ext_count)
			vkEnumerateDeviceExtensionProperties(devices[d], nullptr, &ext_count, extensions.data());
		if (!Has_Extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !Has_Extension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)
			|| !Has_Extension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
		{
			continue;
		}
		unsigned family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		if (family_count)
			vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &family_count, families.data());
		for (unsigned f = 0; f < family_count; ++f)
		{
			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], f, Surface, &present);
			if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
			{
				int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
				if (score > best_score)
				{
					best = (int)d;
					best_score = score;
					best_family = f;
				}
				break;
			}
		}
	}
	if (best < 0)
	{
		Set_Failure_Reason("no Vulkan adapter with swap chains, push descriptors and dynamic rendering");
		return false;
	}
	Physical = devices[best];
	QueueFamily = best_family;
	vkGetPhysicalDeviceProperties(Physical, &PhysicalProperties);
	vkGetPhysicalDeviceMemoryProperties(Physical, &MemoryProperties);
	strncpy(AdapterName, PhysicalProperties.deviceName, sizeof(AdapterName) - 1);
	DedicatedVideoMemory = 0;
	for (unsigned h = 0; h < MemoryProperties.memoryHeapCount; ++h)
	{
		if (MemoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			DedicatedVideoMemory += MemoryProperties.memoryHeaps[h].size;
	}

	VkPhysicalDeviceFeatures supported;
	vkGetPhysicalDeviceFeatures(Physical, &supported);
	memset(&EnabledFeatures, 0, sizeof(EnabledFeatures));
	EnabledFeatures.samplerAnisotropy = supported.samplerAnisotropy;
	EnabledFeatures.fillModeNonSolid = supported.fillModeNonSolid;

	unsigned ext_count = 0;
	vkEnumerateDeviceExtensionProperties(Physical, nullptr, &ext_count, nullptr);
	std::vector<VkExtensionProperties> extensions(ext_count);
	if (ext_count)
		vkEnumerateDeviceExtensionProperties(Physical, nullptr, &ext_count, extensions.data());
	std::vector<const char *> enabled;
	enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	enabled.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	enabled.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
	if (Has_Extension(extensions, "VK_KHR_portability_subset"))
		enabled.push_back("VK_KHR_portability_subset");
	MirrorOnceSupported = Has_Extension(extensions, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	if (MirrorOnceSupported)
		enabled.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);

	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
	dynamic_rendering.dynamicRendering = VK_TRUE;

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue.queueFamilyIndex = QueueFamily;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	info.pNext = &dynamic_rendering;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue;
	info.enabledExtensionCount = (unsigned)enabled.size();
	info.ppEnabledExtensionNames = enabled.data();
	info.pEnabledFeatures = &EnabledFeatures;
	if (!Check(vkCreateDevice(Physical, &info, nullptr, &VK), "vkCreateDevice"))
	{
		return false;
	}
#define VK_LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(VK, #name);
	VK_DEVICE_FUNCTIONS(VK_LOAD_DEVICE)
#undef VK_LOAD_DEVICE
	if (!vkCmdBeginRenderingKHR || !vkCmdPushDescriptorSetKHR || !vkCreateSwapchainKHR)
	{
		Set_Failure_Reason("Vulkan device functions are missing");
		return false;
	}
	vkGetDeviceQueue(VK, QueueFamily, 0, &Queue);

	// Depth: D24S8 as Direct3D 12 uses, else D32S8 (Apple GPUs have no D24S8).
	const VkFormat depth_formats[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
	for (int i = 0; i < 2 && DepthFormat == VK_FORMAT_UNDEFINED; ++i)
	{
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(Physical, depth_formats[i], &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			&& (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
			DepthFormat = depth_formats[i];
	}
	if (DepthFormat == VK_FORMAT_UNDEFINED)
	{
		Set_Failure_Reason("no depth stencil format");
		return false;
	}
	VkFormatProperties color_vertex;
	vkGetPhysicalDeviceFormatProperties(Physical, VK_FORMAT_B8G8R8A8_UNORM, &color_vertex);
	ColorVertexFormatSupported = (color_vertex.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;

	Debug_Log("adapter %s, vendor 0x%04x device 0x%04x, Vulkan %u.%u.%u, depth format %d, mirror once %d", AdapterName,
		PhysicalProperties.vendorID, PhysicalProperties.deviceID, VK_VERSION_MAJOR(PhysicalProperties.apiVersion),
		VK_VERSION_MINOR(PhysicalProperties.apiVersion), VK_VERSION_PATCH(PhysicalProperties.apiVersion), (int)DepthFormat, MirrorOnceSupported ? 1 : 0);
	return true;
}

bool Create_Layouts()
{
	VkDescriptorSetLayoutBinding bindings[BINDING_COUNT];
	memset(bindings, 0, sizeof(bindings));
	for (unsigned i = 0; i < BINDING_COUNT; ++i)
	{
		bindings[i].binding = i;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		if (i < BINDING_TEXTURES)
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		else if (i < BINDING_SAMPLERS)
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		else
			bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	}
	VkDescriptorSetLayoutCreateInfo set = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	set.bindingCount = BINDING_COUNT;
	set.pBindings = bindings;
	if (!Check(vkCreateDescriptorSetLayout(VK, &set, nullptr, &SetLayout), "vkCreateDescriptorSetLayout"))
	{
		return false;
	}
	VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &SetLayout;
	return Check(vkCreatePipelineLayout(VK, &layout, nullptr, &PipelineLayout), "vkCreatePipelineLayout");
}

bool Create_Frames()
{
	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		FrameSlot & frame = Frames[i];
		frame.Submitted = false;
		frame.Serial = 0;
		frame.UploadOpen = false;
		frame.PageIndex = 0;
		VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pool.queueFamilyIndex = QueueFamily;
		if (!Check(vkCreateCommandPool(VK, &pool, nullptr, &frame.Pool), "vkCreateCommandPool"))
			return false;
		VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocate.commandPool = frame.Pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 2;
		VkCommandBuffer buffers[2];
		if (!Check(vkAllocateCommandBuffers(VK, &allocate, buffers), "vkAllocateCommandBuffers"))
			return false;
		frame.Main = buffers[0];
		frame.Upload = buffers[1];
		VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		if (!Check(vkCreateFence(VK, &fence, nullptr, &frame.Fence), "vkCreateFence"))
			return false;
		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (!Check(vkCreateSemaphore(VK, &semaphore, nullptr, &frame.Acquire), "vkCreateSemaphore"))
			return false;
		FramePage page;
		if (!Create_Frame_Page(FRAME_PAGE_SIZE, page))
			return false;
		frame.Pages.push_back(page);
	}
	VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = QueueFamily;
	if (!Check(vkCreateCommandPool(VK, &pool, nullptr, &OneShotPool), "vkCreateCommandPool"))
		return false;
	VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocate.commandPool = OneShotPool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	return Check(vkAllocateCommandBuffers(VK, &allocate, &OneShot), "vkAllocateCommandBuffers");
}

void Enumerate_Display_Modes()
{
	DisplayModes.clear();
	DEVMODEA mode;
	memset(&mode, 0, sizeof(mode));
	mode.dmSize = sizeof(mode);
	for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &mode); ++i)
	{
		if (mode.dmBitsPerPel == 32)
		{
			DX12Device::DisplayMode entry = { (unsigned)mode.dmPelsWidth, (unsigned)mode.dmPelsHeight };
			DisplayModes.push_back(entry);
		}
	}
	if (DisplayModes.empty())
	{
		DX12Device::DisplayMode entry = { (unsigned)GetSystemMetrics(SM_CXSCREEN), (unsigned)GetSystemMetrics(SM_CYSCREEN) };
		DisplayModes.push_back(entry);
	}
	std::sort(DisplayModes.begin(), DisplayModes.end(), [](const DX12Device::DisplayMode & a, const DX12Device::DisplayMode & b)
		{ return a.Width != b.Width ? a.Width < b.Width : a.Height < b.Height; });
	DisplayModes.erase(std::unique(DisplayModes.begin(), DisplayModes.end(), [](const DX12Device::DisplayMode & a, const DX12Device::DisplayMode & b)
		{ return a.Width == b.Width && a.Height == b.Height; }), DisplayModes.end());
}

void Destroy_Swapchain()
{
	for (size_t i = 0; i < RenderDone.size(); ++i)
	{
		vkDestroySemaphore(VK, RenderDone[i], nullptr);
	}
	RenderDone.clear();
	SwapchainImages.clear();
	if (Swapchain)
	{
		vkDestroySwapchainKHR(VK, Swapchain, nullptr);
		Swapchain = VK_NULL_HANDLE;
	}
}

bool Create_Swapchain(unsigned width, unsigned height)
{
	VkSurfaceCapabilitiesKHR caps;
	if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Physical, Surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
	{
		return false;
	}
	unsigned format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(Physical, Surface, &format_count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	if (format_count)
		vkGetPhysicalDeviceSurfaceFormatsKHR(Physical, Surface, &format_count, formats.data());
	VkSurfaceFormatKHR format = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	bool found = false;
	for (size_t i = 0; i < formats.size(); ++i)
	{
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
		{
			format = formats[i];
			found = true;
			break;
		}
	}
	if (!found)
	{
		for (size_t i = 0; i < formats.size(); ++i)
		{
			if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
			{
				format = formats[i];
				found = true;
				break;
			}
		}
	}
	if (!found && !formats.empty())
		format = formats[0];

	VkExtent2D extent = { width, height };
	if (caps.currentExtent.width != 0xffffffffu)
		extent = caps.currentExtent;
	if (extent.width == 0 || extent.height == 0)
	{
		// Minimized: no swap chain until the window has a size again.
		SwapchainStale = true;
		return true;
	}

	unsigned image_count = caps.minImageCount + 1;
	if (caps.maxImageCount && image_count > caps.maxImageCount)
		image_count = caps.maxImageCount;
	VkSwapchainKHR old = Swapchain;
	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = Surface;
	info.minImageCount = image_count;
	info.imageFormat = format.format;
	info.imageColorSpace = format.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
		: (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	VkSwapchainKHR created = VK_NULL_HANDLE;
	if (!Check(vkCreateSwapchainKHR(VK, &info, nullptr, &created), "vkCreateSwapchainKHR"))
	{
		return false;
	}
	Destroy_Swapchain();
	Swapchain = created;
	SwapchainFormat = format.format;
	SwapchainExtent = extent;
	unsigned count = 0;
	vkGetSwapchainImagesKHR(VK, Swapchain, &count, nullptr);
	SwapchainImages.resize(count);
	vkGetSwapchainImagesKHR(VK, Swapchain, &count, SwapchainImages.data());
	RenderDone.resize(count);
	for (unsigned i = 0; i < count; ++i)
	{
		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		vkCreateSemaphore(VK, &semaphore, nullptr, &RenderDone[i]);
	}
	SwapchainStale = false;
	Debug_Log("swap chain %ux%u, %u images, format %d", extent.width, extent.height, count, (int)format.format);
	return true;
}

void Release_Image_Now(VKImageData *& image)
{
	if (image)
	{
		VKDevice_Release_Image(image);
		image = nullptr;
	}
}

void Release_All()
{
	if (VK)
	{
		vkDeviceWaitIdle(VK);
	}
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
	for (size_t i = 0; i < DirtyTextures.size(); ++i)
		DirtyTextures[i]->Release_Ref();
	DirtyTextures.clear();
	{
		std::vector<PendingUpload> uploads;
		std::vector<DX12TextureClass *> initial;
		{
			std::lock_guard<std::mutex> lock(ResourceMutex);
			uploads.swap(PendingUploads);
			initial.swap(InitialLayouts);
			HasPendingUploads = false;
		}
		for (size_t i = 0; i < uploads.size(); ++i)
			uploads[i].Texture->Release_Ref();
		for (size_t i = 0; i < initial.size(); ++i)
			initial[i]->Release_Ref();
	}
	if (VK)
	{
		Release_Image_Now(BackBuffer);
		Release_Image_Now(DepthBuffer);
		Release_Image_Now(WhiteTexture);
		Process_Deferred(true);
		// Images the game still holds (textures released after the device): their Vulkan objects go now, the
		// VKImageData when the texture is released.
		for (size_t i = 0; i < LiveImages.size(); ++i)
		{
			VKImageData * image = LiveImages[i];
			if (image->SampleView != image->View)
				vkDestroyImageView(VK, image->SampleView, nullptr);
			vkDestroyImageView(VK, image->View, nullptr);
			vkDestroyImage(VK, image->Image, nullptr);
			image->Image = VK_NULL_HANDLE;
			image->View = VK_NULL_HANDLE;
			image->SampleView = VK_NULL_HANDLE;
		}
		LiveImages.clear();
		for (std::map<PSOKey, VkPipeline, PSOKeyLess>::iterator it = Pipelines.begin(); it != Pipelines.end(); ++it)
		{
			if (it->second)
				vkDestroyPipeline(VK, it->second, nullptr);
		}
		for (std::map<unsigned, VkSampler>::iterator it = Samplers.begin(); it != Samplers.end(); ++it)
			vkDestroySampler(VK, it->second, nullptr);
		// DefaultSampler is one of Samplers.
		Destroy_Swapchain();
		for (int i = 0; i < FRAME_COUNT; ++i)
		{
			FrameSlot & frame = Frames[i];
			for (size_t p = 0; p < frame.Pages.size(); ++p)
			{
				vkDestroyBuffer(VK, frame.Pages[p].Buffer, nullptr);
				vkFreeMemory(VK, frame.Pages[p].Memory, nullptr);
			}
			frame.Pages.clear();
			if (frame.Pool)
				vkDestroyCommandPool(VK, frame.Pool, nullptr);
			if (frame.Fence)
				vkDestroyFence(VK, frame.Fence, nullptr);
			if (frame.Acquire)
				vkDestroySemaphore(VK, frame.Acquire, nullptr);
			frame.Pool = VK_NULL_HANDLE;
			frame.Fence = VK_NULL_HANDLE;
			frame.Acquire = VK_NULL_HANDLE;
			frame.Submitted = false;
		}
		if (OneShotPool)
			vkDestroyCommandPool(VK, OneShotPool, nullptr);
		if (PipelineLayout)
			vkDestroyPipelineLayout(VK, PipelineLayout, nullptr);
		if (SetLayout)
			vkDestroyDescriptorSetLayout(VK, SetLayout, nullptr);
		for (size_t b = 0; b < Blocks.size(); ++b)
			vkFreeMemory(VK, Blocks[b].Memory, nullptr);
		vkDestroyDevice(VK, nullptr);
	}
	Pipelines.clear();
	Samplers.clear();
	Blocks.clear();
	BufferTable.clear();
	FreeTableEntries.clear();
	DeferredReleases.clear();
	VertexShaders.clear();
	PixelShaders.clear();
	GameShaderCaches.clear();
	GameLayouts.clear();
	LastVertexShader = nullptr;
	LastPixelShader = nullptr;
	DefaultSampler = VK_NULL_HANDLE;
	OneShotPool = VK_NULL_HANDLE;
	OneShot = VK_NULL_HANDLE;
	PipelineLayout = VK_NULL_HANDLE;
	SetLayout = VK_NULL_HANDLE;
	VK = VK_NULL_HANDLE;
	if (Instance)
	{
		if (Surface)
			vkDestroySurfaceKHR(Instance, Surface, nullptr);
		if (Messenger && vkDestroyDebugUtilsMessengerEXT)
			vkDestroyDebugUtilsMessengerEXT(Instance, Messenger, nullptr);
		vkDestroyInstance(Instance, nullptr);
	}
	Surface = VK_NULL_HANDLE;
	Messenger = VK_NULL_HANDLE;
	Instance = VK_NULL_HANDLE;
	Recording = false;
	RenderingActive = false;
	FrameSlotPrepared = false;
	DepthFormat = VK_FORMAT_UNDEFINED;
}

bool Init_Device(HWND hwnd)
{
	Window = hwnd;
	MainThreadID = GetCurrentThreadId();
	if (!Load_Libraries() || !Create_Instance())
	{
		return false;
	}
#ifdef _WIN32
	VkWin32SurfaceCreateInfoKHR surface = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	surface.hinstance = GetModuleHandleA(nullptr);
	surface.hwnd = hwnd;
	if (!Check(vkCreateWin32SurfaceKHR(Instance, &surface, nullptr, &Surface), "vkCreateWin32SurfaceKHR"))
	{
		return false;
	}
#endif
	if (!Create_Device() || !Create_Layouts() || !Create_Frames())
	{
		return false;
	}
	Enumerate_Display_Modes();
	DefaultSampler = Get_Sampler(2, 2, 0, 1, 1, 1, 0);

	// The 1x1 opaque white texture, bound for stages without a texture.
	WhiteTexture = VKDevice_Create_Image(1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT, false);
	if (!WhiteTexture)
	{
		return false;
	}
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(OneShot, &begin);
	Image_Barrier(OneShot, WhiteTexture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkClearColorValue white = { { 1.0f, 1.0f, 1.0f, 1.0f } };
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(OneShot, WhiteTexture->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
	Image_Barrier(OneShot, WhiteTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(OneShot);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &OneShot;
	vkQueueSubmit(Queue, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(Queue);
	WhiteTexture->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	WhiteTexture->InitialLayoutDone = true;
	return true;
}

// Runs commands on the one shot command buffer and waits for them. Main thread, outside a frame or after the
// frame's commands were submitted.
template <class Record>
void Run_One_Shot(Record record)
{
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(OneShot, &begin);
	record(OneShot);
	vkEndCommandBuffer(OneShot);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &OneShot;
	vkQueueSubmit(Queue, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(Queue);
	++DX12Device_Counters.FenceWaits[DX12_WAIT_FLUSH];
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Functions for vkresource.cpp
// ---------------------------------------------------------------------------------------------------------------

VkDevice VKDevice_Get()
{
	return VK;
}

VKImageData * VKDevice_Create_Image(unsigned width, unsigned height, unsigned levels, VkFormat format, VkImageUsageFlags usage,
	VkImageAspectFlags aspect, bool alpha_one)
{
	if (!VK)
	{
		return nullptr;
	}
	if (format == VK_FORMAT_UNDEFINED)
	{
		format = DepthFormat;
	}
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKImageData * image = new VKImageData;
	memset(image, 0, sizeof(*image));
	if (vkCreateImage(VK, &info, nullptr, &image->Image) != VK_SUCCESS)
	{
		delete image;
		return nullptr;
	}
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(VK, image->Image, &requirements);
	bool allocated;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		allocated = Allocate_Memory(requirements, false, image->Allocation);
	}
	if (!allocated || vkBindImageMemory(VK, image->Image, image->Allocation.Memory, image->Allocation.Offset) != VK_SUCCESS)
	{
		if (allocated)
		{
			std::lock_guard<std::mutex> lock(ResourceMutex);
			Free_Memory(image->Allocation);
		}
		vkDestroyImage(VK, image->Image, nullptr);
		delete image;
		return nullptr;
	}
	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	view.image = image->Image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange.aspectMask = aspect;
	view.subresourceRange.levelCount = levels;
	view.subresourceRange.layerCount = 1;
	vkCreateImageView(VK, &view, nullptr, &image->View);
	image->SampleView = image->View;
	if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) || alpha_one)
	{
		view.subresourceRange.aspectMask = aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT;
		if (alpha_one)
			view.components.a = VK_COMPONENT_SWIZZLE_ONE;
		vkCreateImageView(VK, &view, nullptr, &image->SampleView);
	}
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		LiveImages.push_back(image);
	}
	image->Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image->Format = format;
	image->Aspect = aspect;
	image->Width = width;
	image->Height = height;
	image->Levels = levels;
	return image;
}

void VKDevice_Release_Image(VKImageData * image)
{
	if (!image)
	{
		return;
	}
	if (!VK)
	{
		// Released after the device: Release_All has destroyed its objects already.
		delete image;
		return;
	}
	DeferredRelease release;
	memset(&release, 0, sizeof(release));
	release.Image = image->Image;
	release.Views[0] = image->View;
	release.Views[1] = image->SampleView;
	release.Allocation = image->Allocation;
	release.Table = ~0u;
	release.ImageData = image;
	std::lock_guard<std::mutex> lock(ResourceMutex);
	std::vector<VKImageData *>::iterator live = std::find(LiveImages.begin(), LiveImages.end(), image);
	if (live != LiveImages.end())
	{
		*live = LiveImages.back();
		LiveImages.pop_back();
	}
	release.Serial = NextSerial;
	DeferredReleases.push_back(release);
}

VKBufferData * VKDevice_Create_Host_Buffer(unsigned size, void ** cpu, unsigned long long * address)
{
	if (!VK)
	{
		return nullptr;
	}
	VKBufferData * buffer = new VKBufferData;
	memset(buffer, 0, sizeof(*buffer));
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(VK, &info, nullptr, &buffer->Buffer) != VK_SUCCESS)
	{
		delete buffer;
		return nullptr;
	}
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(VK, buffer->Buffer, &requirements);
	std::lock_guard<std::mutex> lock(ResourceMutex);
	if (!Allocate_Memory(requirements, true, buffer->Allocation)
		|| vkBindBufferMemory(VK, buffer->Buffer, buffer->Allocation.Memory, buffer->Allocation.Offset) != VK_SUCCESS)
	{
		vkDestroyBuffer(VK, buffer->Buffer, nullptr);
		delete buffer;
		return nullptr;
	}
	buffer->TableIndex = Add_Table_Entry(buffer->Buffer);
	*cpu = buffer->Allocation.Mapped;
	*address = Make_Address(buffer->TableIndex, 0);
	return buffer;
}

void VKDevice_Release_Buffer(VKBufferData * buffer)
{
	if (!buffer)
	{
		return;
	}
	if (!VK)
	{
		delete buffer;
		return;
	}
	DeferredRelease release;
	memset(&release, 0, sizeof(release));
	release.Buffer = buffer->Buffer;
	release.Allocation = buffer->Allocation;
	release.Table = buffer->TableIndex;
	release.BufferData = buffer;
	std::lock_guard<std::mutex> lock(ResourceMutex);
	release.Serial = NextSerial;
	DeferredReleases.push_back(release);
}

void VKDevice_Queue_Texture_Upload(DX12TextureClass * texture, unsigned level, std::vector<unsigned char> & data)
{
	if (!VK || !texture || data.empty())
	{
		return;
	}
	bool too_many;
	{
		std::lock_guard<std::mutex> lock(ResourceMutex);
		texture->Add_Ref();
		PendingUploads.push_back(PendingUpload());
		PendingUpload & pending = PendingUploads.back();
		pending.Texture = texture;
		pending.Level = level;
		pending.Data.swap(data);
		PendingUploadBytes += pending.Data.size();
		HasPendingUploads = true;
		too_many = PendingUploadBytes > MAX_PENDING_UPLOAD_BYTES;
		++DX12Device_Counters.TextureUploads;
		DX12Device_Counters.TextureUploadBytes += pending.Data.size();
	}
	if (too_many && Is_Main_Thread() && !UploadingDirtyTextures)
	{
		if (Recording)
			Record_Uploads();
		else
			Execute_Uploads_Now();
	}
}

void VKDevice_Queue_Initial_Layout(DX12TextureClass * texture)
{
	if (!VK || !texture)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(ResourceMutex);
	texture->Add_Ref();
	InitialLayouts.push_back(texture);
}

// ---------------------------------------------------------------------------------------------------------------
// DX12Device
// ---------------------------------------------------------------------------------------------------------------

unsigned long long DX12Device_Query_Ticks()
{
	return Query_Ticks();
}

bool DX12Device_Is_Main_Thread()
{
	return VK != VK_NULL_HANDLE && GetCurrentThreadId() == MainThreadID;
}

void DX12Device_Set_User_Data_Directory(const char * directory)
{
	strncpy(UserDataDirectory, directory ? directory : "", sizeof(UserDataDirectory) - 1);
	UserDataDirectory[sizeof(UserDataDirectory) - 1] = '\0';
}

void DX12Device_Set_Debug_Log_Directory(const char * directory)
{
	strncpy(DebugLogDirectory, directory ? directory : "", sizeof(DebugLogDirectory) - 1);
	DebugLogDirectory[sizeof(DebugLogDirectory) - 1] = '\0';
}

bool DX12Device::Init(void * hwnd)
{
	if (Device)
	{
		return true;
	}
	FailureReason[0] = '\0';
	if (!hwnd)
	{
		Set_Failure_Reason("there is no game window");
		return false;
	}
	WWDEBUG_SAY(("Vulkan: initializing"));
	if (DX12Device_DebugLayer)
	{
		Open_Debug_Log();
		Debug_Log("initializing Vulkan");
	}
	if (!Init_Device((HWND)hwnd))
	{
		WWDEBUG_SAY(("Vulkan: initialization failed: %s", FailureReason));
		Release_All();
		return false;
	}
	Device = reinterpret_cast<ID3D12Device *>(VK);
	Width = 0;
	Height = 0;
	DX12_Register_Game_Shaders();
	Debug_Log("%d game shaders registered", DX12GameShaders_Get_Count());
	return true;
}

void DX12Device::Shutdown()
{
	if (!VK)
	{
		Device = nullptr;
		return;
	}
	if (Recording)
	{
		Submit_Frame(false);
	}
	Execute_Uploads_Now();
	Wait_Idle();
	Debug_Log("shutdown at frame %llu: validation errors %u, warnings %u; shaders compiled %llu, from the cache %llu",
		FrameNumber, ValidationErrors, ValidationWarnings, DX12Device_Counters.ShaderCompiles, DX12Device_Counters.ShaderCacheHits);
	Device = nullptr;
	Release_All();
	if (DebugLog)
	{
		fclose(DebugLog);
		DebugLog = nullptr;
	}
	Width = 0;
	Height = 0;
}

const char * DX12Device::Get_Failure_Reason()
{
	return FailureReason;
}

const char * DX12Device::Get_Adapter_Name()
{
	return AdapterName;
}

unsigned DX12Device::Get_Vendor_Id()
{
	return PhysicalProperties.vendorID;
}

unsigned DX12Device::Get_Device_Id()
{
	return PhysicalProperties.deviceID;
}

unsigned long long DX12Device::Get_Dedicated_Video_Memory()
{
	return DedicatedVideoMemory;
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
	if (BackBuffer && width == Width && height == Height && !SwapchainStale)
	{
		return true;
	}
	if (Recording)
	{
		Submit_Frame(false);
	}
	Wait_Idle();
	Process_Deferred(false);

	if (!Create_Swapchain(width, height))
	{
		return false;
	}
	Release_Image_Now(BackBuffer);
	Release_Image_Now(DepthBuffer);
	BackBuffer = VKDevice_Create_Image(width, height, 1, VK_FORMAT_B8G8R8A8_UNORM,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT, false);
	DepthBuffer = VKDevice_Create_Image(width, height, 1, VK_FORMAT_UNDEFINED, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, false);
	if (!BackBuffer || !DepthBuffer)
	{
		Set_Failure_Reason("back buffer %ux%u", width, height);
		return false;
	}
	// Both start cleared, as the Direct3D 12 back buffer and depth buffer do.
	Run_One_Shot([](VkCommandBuffer cb)
	{
		Image_Barrier(cb, BackBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue black;
		memset(&black, 0, sizeof(black));
		VkImageSubresourceRange color_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cb, BackBuffer->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &color_range);
		Image_Barrier(cb, BackBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		Image_Barrier(cb, DepthBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearDepthStencilValue depth = { 1.0f, 0 };
		VkImageSubresourceRange depth_range = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
		vkCmdClearDepthStencilImage(cb, DepthBuffer->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &depth_range);
		Image_Barrier(cb, DepthBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	});
	BackBuffer->Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	DepthBuffer->Layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	HasPresented = false;
	Width = width;
	Height = height;
	Set_Full_Viewport();
	WWDEBUG_SAY(("Vulkan: back buffer %ux%u", width, height));
	return true;
}

void DX12Device::Begin_Frame()
{
	if (!Device || Recording || !BackBuffer)
	{
		return;
	}
	Prepare_Frame_Slot();
	FrameSlot & frame = Frames[FrameIndex];
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(frame.Main, &begin) != VK_SUCCESS)
	{
		return;
	}
	Recording = true;
	RenderingActive = false;
	BoundPipeline = VK_NULL_HANDLE;
	Record_Uploads();
}

void DX12Device::End_Frame(bool present)
{
	if (!Device)
	{
		return;
	}
	if (Recording && !present && ++FrameEndsWithoutPresent < MAX_FRAME_ENDS_WITHOUT_PRESENT)
	{
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
	FrameEndsWithoutPresent = 0;
	const unsigned long long submit_start = Query_Ticks();
	if (!Recording)
	{
		bool pending;
		{
			std::lock_guard<std::mutex> lock(ResourceMutex);
			pending = HasPendingUploads.load() || !InitialLayouts.empty();
		}
		if (!present && !pending && DirtyTextures.empty())
		{
			return;
		}
		Begin_Frame();
		if (!Recording)
		{
			return;
		}
	}
	Record_Uploads();
	End_Rendering();

	FrameSlot & frame = Frames[FrameIndex];
	VkCommandBuffer cb = frame.Main;
	if (BackBuffer->Layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	{
		Image_Barrier(cb, BackBuffer, BackBuffer->Layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		BackBuffer->Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	}

	unsigned image_index = ~0u;
	bool acquired = false;
	if (present)
	{
		if (SwapchainStale || !Swapchain)
		{
			Create_Swapchain(Width, Height);
		}
		if (Swapchain)
		{
			VkResult result = vkAcquireNextImageKHR(VK, Swapchain, UINT64_MAX, frame.Acquire, VK_NULL_HANDLE, &image_index);
			acquired = (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
				SwapchainStale = true;
		}
	}
	if (acquired)
	{
		// The back buffer goes to the swap chain image, scaled when the window size differs.
		VkImage target = SwapchainImages[image_index];
		Raw_Image_Barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		if (SwapchainFormat == VK_FORMAT_B8G8R8A8_UNORM && SwapchainExtent.width == Width && SwapchainExtent.height == Height)
		{
			VkImageCopy copy;
			memset(&copy, 0, sizeof(copy));
			copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.srcSubresource.layerCount = 1;
			copy.dstSubresource = copy.srcSubresource;
			copy.extent.width = Width;
			copy.extent.height = Height;
			copy.extent.depth = 1;
			vkCmdCopyImage(cb, BackBuffer->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		}
		else
		{
			VkImageBlit blit;
			memset(&blit, 0, sizeof(blit));
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.layerCount = 1;
			blit.dstSubresource = blit.srcSubresource;
			blit.srcOffsets[1].x = (int)Width;
			blit.srcOffsets[1].y = (int)Height;
			blit.srcOffsets[1].z = 1;
			blit.dstOffsets[1].x = (int)SwapchainExtent.width;
			blit.dstOffsets[1].y = (int)SwapchainExtent.height;
			blit.dstOffsets[1].z = 1;
			vkCmdBlitImage(cb, BackBuffer->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		}
		Raw_Image_Barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}

	Submit(true, acquired ? frame.Acquire : VK_NULL_HANDLE, acquired ? RenderDone[image_index] : VK_NULL_HANDLE);
	Recording = false;
	RenderingActive = false;

	if (acquired)
	{
		const unsigned long long present_start = Query_Ticks();
		VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &RenderDone[image_index];
		info.swapchainCount = 1;
		info.pSwapchains = &Swapchain;
		info.pImageIndices = &image_index;
		VkResult result = vkQueuePresentKHR(Queue, &info);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
			SwapchainStale = true;
		DX12Device_Counters.PresentTicks += Query_Ticks() - present_start;
		extern double WW3D_PresentMsTotal;
		static double s_tickMs = 0.0;
		if (s_tickMs == 0.0)
		{
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			s_tickMs = 1000.0 / (double)frequency.QuadPart;
		}
		WW3D_PresentMsTotal += (double)(Query_Ticks() - present_start) * s_tickMs;
		HasPresented = true;
	}

	FrameIndex = (FrameIndex + 1) % FRAME_COUNT;
	FrameSlotPrepared = false;
	Process_Deferred(false);
	DX12Device_Counters.SubmitTicks += Query_Ticks() - submit_start;
	if (present && (FrameNumber % 300) == 0)
	{
		Debug_Log("frame %llu: presented; validation errors %u, warnings %u; pipelines %llu", FrameNumber, ValidationErrors, ValidationWarnings,
			DX12Device_Counters.PipelineCreates);
	}
	if (present)
	{
		memset(&DX12Device_Counters, 0, sizeof(DX12Device_Counters));
	}
	++FrameNumber;
}

void DX12Device::Clear(bool clear_color, bool clear_z_stencil, unsigned argb, float z, unsigned stencil)
{
	if (!Device || (!clear_color && !clear_z_stencil))
	{
		return;
	}
	if (!Recording)
	{
		Begin_Frame();
	}
	if (!Recording || !Begin_Rendering())
	{
		return;
	}
	int left = CurrentScissor.offset.x < 0 ? 0 : CurrentScissor.offset.x;
	int top = CurrentScissor.offset.y < 0 ? 0 : CurrentScissor.offset.y;
	int right = CurrentScissor.offset.x + (int)CurrentScissor.extent.width;
	int bottom = CurrentScissor.offset.y + (int)CurrentScissor.extent.height;
	if (right > (int)Target_Width())
		right = (int)Target_Width();
	if (bottom > (int)Target_Height())
		bottom = (int)Target_Height();
	if (right <= left || bottom <= top)
	{
		return;
	}
	VkClearRect rect;
	rect.rect.offset.x = left;
	rect.rect.offset.y = top;
	rect.rect.extent.width = (unsigned)(right - left);
	rect.rect.extent.height = (unsigned)(bottom - top);
	rect.baseArrayLayer = 0;
	rect.layerCount = 1;
	VkClearAttachment attachments[2];
	unsigned count = 0;
	if (clear_color)
	{
		attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachments[count].colorAttachment = 0;
		attachments[count].clearValue.color.float32[0] = ((argb >> 16) & 0xff) / 255.0f;
		attachments[count].clearValue.color.float32[1] = ((argb >> 8) & 0xff) / 255.0f;
		attachments[count].clearValue.color.float32[2] = (argb & 0xff) / 255.0f;
		attachments[count].clearValue.color.float32[3] = ((argb >> 24) & 0xff) / 255.0f;
		++count;
	}
	if (clear_z_stencil && TargetHasDepth)
	{
		attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		attachments[count].colorAttachment = 0;
		attachments[count].clearValue.depthStencil.depth = z < 0.0f ? 0.0f : (z > 1.0f ? 1.0f : z);
		attachments[count].clearValue.depthStencil.stencil = stencil & 0xff;
		++count;
	}
	if (count)
	{
		vkCmdClearAttachments(Frames[FrameIndex].Main, count, attachments, 1, &rect);
	}
}

void DX12Device::Set_Viewport(unsigned x, unsigned y, unsigned width, unsigned height, float min_z, float max_z)
{
	if (!Device)
	{
		return;
	}
	if (!Recording)
	{
		Begin_Frame();
	}
	min_z = min_z < 0.0f ? 0.0f : (min_z > 1.0f ? 1.0f : min_z);
	max_z = max_z < 0.0f ? 0.0f : (max_z > 1.0f ? 1.0f : max_z);
	CurrentViewport.x = (float)x;
	CurrentViewport.y = (float)y;
	CurrentViewport.width = (float)width;
	CurrentViewport.height = (float)height;
	CurrentViewport.minDepth = min_z;
	CurrentViewport.maxDepth = max_z;
	CurrentScissor.offset.x = (int)x;
	CurrentScissor.offset.y = (int)y;
	CurrentScissor.extent.width = width;
	CurrentScissor.extent.height = height;
	Apply_Viewport();
}

void DX12Device::Draw(const DX12DrawDesc & desc)
{
	if (!Device)
	{
		return;
	}
	DX12StatsScope stats_scope(DX12Device_Counters.DrawTicks);
	++DX12Device_Counters.DrawCalls;
	if (desc.VertexAddress == 0 || desc.VertexStride == 0 || desc.VertexBufferSize == 0)
	{
		return;
	}
	const bool indexed = desc.IndexAddress != 0;
	if (indexed)
	{
		if (desc.IndexCount == 0 || desc.IndexBufferSize == 0)
			return;
		if ((unsigned long long)desc.StartIndex + desc.IndexCount > desc.IndexBufferSize / 2
			|| desc.BaseVertex < 0 || (unsigned long long)desc.BaseVertex * desc.VertexStride >= desc.VertexBufferSize)
		{
			if (!LoggedOverflow)
			{
				WWDEBUG_SAY(("Vulkan: draw out of range, skipped"));
				LoggedOverflow = true;
			}
			return;
		}
	}
	else
	{
		if (desc.VertexCount == 0)
			return;
		if (((unsigned long long)desc.StartVertex + desc.VertexCount) * desc.VertexStride > desc.VertexBufferSize)
		{
			if (!LoggedOverflow)
			{
				WWDEBUG_SAY(("Vulkan: draw out of range, skipped"));
				LoggedOverflow = true;
			}
			return;
		}
	}

	VkPrimitiveTopology topology;
	switch (desc.PrimitiveType)
	{
	case PRIM_POINTLIST: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
	case PRIM_LINELIST: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
	case PRIM_LINESTRIP: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
	case PRIM_TRIANGLELIST: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
	case PRIM_TRIANGLESTRIP: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
	case PRIM_TRIANGLEFAN:
		if (!LoggedTriangleFan)
		{
			WWDEBUG_SAY(("Vulkan: triangle fans must be converted to lists before DX12Device::Draw, draw skipped"));
			LoggedTriangleFan = true;
		}
		return;
	default:
		return;
	}

	int game_vs = -1;
	int game_ps = -1;
	unsigned game_layout_id = 0;
	if (desc.VertexShader != 0 && !DX12GameShaders_Resolve_Handle(desc.VertexShader, true, &game_vs, &game_layout_id))
		return;
	if (desc.PixelShader != 0 && !DX12GameShaders_Resolve_Handle(desc.PixelShader, false, &game_ps, nullptr))
		return;

	VertexLayout fvf_layout;
	const VertexLayout * layout = &fvf_layout;
	if (game_vs >= 0)
	{
		if (game_layout_id == 0 || game_layout_id > GameLayouts.size())
			return;
		layout = &GameLayouts[game_layout_id - 1].Layout;
		if (layout->Size > desc.VertexStride)
			return;
	}
	else if (!Decode_FVF(desc.FVF, fvf_layout) || fvf_layout.Size > desc.VertexStride)
	{
		if (LastBadFVF != desc.FVF)
		{
			WWDEBUG_SAY(("Vulkan: unsupported FVF 0x%x with stride %u, draw skipped", desc.FVF, desc.VertexStride));
			LastBadFVF = desc.FVF;
		}
		return;
	}

	if (!Recording)
	{
		Begin_Frame();
	}
	if (!Recording)
	{
		return;
	}
	Record_Uploads();

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
		ffp.PS.Fog = render_states[FFP_RS_FOGENABLE] != 0 ? 1 : 0;
		ffp.PS.FogTableMode = 0;
		if (game_ps < 0)
			ffp.PS.Specular = render_states[FFP_RS_SPECULARENABLE] != 0 ? 1 : 0;
	}

	const ShaderEntry * vertex = nullptr;
	if (game_vs >= 0)
	{
		vertex = &Get_Game_Shader(game_vs, nullptr);
	}
	else
	{
		if (!LastVertexShader || memcmp(&ffp.VS, &LastVSKey, sizeof(LastVSKey)) != 0)
		{
			LastVertexShader = &Get_Vertex_Shader(ffp.VS);
			LastVSKey = ffp.VS;
		}
		vertex = LastVertexShader;
	}
	const ShaderEntry * pixel = nullptr;
	if (game_ps >= 0)
	{
		DX12GamePSVariant variant;
		memset(&variant, 0, sizeof(variant));
		for (unsigned stage = 0; stage < DX12_MAX_STAGES; ++stage)
			variant.Projected[stage] = ffp.PS.Stages[stage].Projected;
		variant.AlphaFunc = ffp.PS.AlphaFunc;
		variant.Fog = ffp.PS.Fog;
		variant.FogTableMode = ffp.PS.FogTableMode;
		pixel = &Get_Game_Shader(game_ps, &variant);
	}
	else
	{
		if (!LastPixelShader || memcmp(&ffp.PS, &LastPSKey, sizeof(LastPSKey)) != 0)
		{
			LastPixelShader = &Get_Pixel_Shader(ffp.PS);
			LastPSKey = ffp.PS;
		}
		pixel = LastPixelShader;
	}
	if (!vertex || !pixel || vertex->Code.empty() || pixel->Code.empty())
	{
		return;
	}

	// Textures first: moving a render target to the shader read layout ends the rendering.
	VkDescriptorImageInfo images[DX12_MAX_STAGES];
	VkDescriptorImageInfo samplers[DX12_MAX_STAGES];
	VKImageData * color_image = Current_Color_Image();
	for (int stage = 0; stage < DX12_MAX_STAGES; ++stage)
	{
		const bool sampled = game_ps >= 0 || ffp.PS.Stages[stage].Texture == FFP_TEXTURE_SAMPLED;
		DX12TextureClass * texture = sampled ? desc.Textures[stage] : nullptr;
		VKImageData * image = texture ? texture->Peek_VK_Image() : nullptr;
		if (image && (image == color_image || !image->InitialLayoutDone && !texture->Is_Render_Target() && !texture->Is_Depth_Target()))
		{
			// A target cannot be sampled while it is drawn into; a texture without its first layout is not ready.
			image = nullptr;
		}
		if (image && (texture->Is_Render_Target() || texture->Is_Depth_Target()))
		{
			Use_Target_Image(image, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		if (!image)
		{
			image = WhiteTexture;
		}
		images[stage].sampler = VK_NULL_HANDLE;
		images[stage].imageView = image->SampleView;
		images[stage].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		samplers[stage].sampler = sampled ? Get_Sampler(desc.MinFilter[stage], desc.MagFilter[stage], desc.MipFilter[stage],
			desc.AddressU[stage], desc.AddressV[stage], desc.MaxAnisotropy[stage], texture ? texture->Get_Min_LOD() : 0) : DefaultSampler;
		samplers[stage].imageView = VK_NULL_HANDLE;
		samplers[stage].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	if (!Begin_Rendering())
	{
		return;
	}

	PSOKey key;
	memset(&key, 0, sizeof(key));
	key.VertexShader = vertex->Id;
	key.PixelShader = pixel->Id;
	key.FVF = game_vs >= 0 ? 0 : desc.FVF;
	key.InputLayout = game_vs >= 0 ? game_layout_id : 0;
	key.VertexStride = desc.VertexStride;
	key.Topology = (unsigned)topology;
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
	DX12TextureClass * depth_texture = nullptr;
	VKImageData * depth_image = Current_Depth_Image(&depth_texture);
	key.DepthFormat = depth_image ? (unsigned)depth_image->Format : 0;
	if (!TargetHasDepth)
	{
		key.ZEnable = 0;
		key.ZWriteEnable = 0;
		key.ZFunc = 0;
		key.DepthBias = 0;
	}
	const bool stencil = TargetHasDepth && render_states[FFP_RS_STENCILENABLE] != 0;
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
	VkPipeline pipeline = Get_Pipeline(key, *layout, *vertex, *pixel);
	if (!pipeline)
	{
		return;
	}

	DX12FFPViewport viewport;
	viewport.X = CurrentViewport.x;
	viewport.Y = CurrentViewport.y;
	viewport.Width = CurrentViewport.width;
	viewport.Height = CurrentViewport.height;
	viewport.MinZ = CurrentViewport.minDepth;
	viewport.MaxZ = CurrentViewport.maxDepth;
	DX12VSConstants vs;
	DX12PSConstants ps;
	DX12FFP_Fill_Constants(desc, render_states, ffp, viewport, vs, ps);

	VkDescriptorBufferInfo constants[4];
	if (!Upload_Constants(&vs, sizeof(vs), constants[0]) || !Upload_Constants(&ps, sizeof(ps), constants[1]))
	{
		return;
	}
	constants[2] = constants[1];
	constants[3] = constants[0];
	if (game_ps >= 0)
	{
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
		if (!Upload_Constants(&game_ps_constants, sizeof(game_ps_constants), constants[2]))
			return;
	}
	if (game_vs >= 0)
	{
		DX12GameVSConstants game_vs_constants;
		if (desc.VertexShaderConstants)
			memcpy(game_vs_constants.C, desc.VertexShaderConstants, sizeof(game_vs_constants.C));
		else
			memset(game_vs_constants.C, 0, sizeof(game_vs_constants.C));
		if (!Upload_Constants(&game_vs_constants, sizeof(game_vs_constants), constants[3]))
			return;
	}

	VkBuffer vertex_buffer;
	VkDeviceSize vertex_offset;
	if (!Resolve_Address(desc.VertexAddress, vertex_buffer, vertex_offset))
	{
		return;
	}
	VkBuffer index_buffer = VK_NULL_HANDLE;
	VkDeviceSize index_offset = 0;
	if (indexed && !Resolve_Address(desc.IndexAddress, index_buffer, index_offset))
	{
		return;
	}

	VkCommandBuffer cb = Frames[FrameIndex].Main;
	if (pipeline != BoundPipeline)
	{
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		BoundPipeline = pipeline;
		++DX12Device_Counters.PipelineBinds;
	}
	if (stencil)
	{
		const unsigned stencil_ref = render_states[FFP_RS_STENCILREF] & 0xff;
		if (stencil_ref != BoundStencilRef)
		{
			vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
			BoundStencilRef = stencil_ref;
		}
	}

	VkWriteDescriptorSet writes[BINDING_COUNT];
	memset(writes, 0, sizeof(writes));
	for (unsigned i = 0; i < BINDING_COUNT; ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		if (i < BINDING_TEXTURES)
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writes[i].pBufferInfo = &constants[i];
		}
		else if (i < BINDING_SAMPLERS)
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			writes[i].pImageInfo = &images[i - BINDING_TEXTURES];
		}
		else
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			writes[i].pImageInfo = &samplers[i - BINDING_SAMPLERS];
		}
	}
	vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 0, BINDING_COUNT, writes);
	DX12Device_Counters.RootParameterSets += BINDING_COUNT;

	vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer, &vertex_offset);
	++DX12Device_Counters.DrawsRecorded;
	if (indexed)
	{
		vkCmdBindIndexBuffer(cb, index_buffer, index_offset, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(cb, desc.IndexCount, 1, desc.StartIndex, desc.BaseVertex, 0);
	}
	else
	{
		vkCmdDraw(cb, desc.VertexCount, 1, desc.StartVertex, 0);
	}
}

unsigned DX12Device::Allocate_SRV_Slot() { return 1; }
void DX12Device::Free_SRV_Slot(unsigned) {}
unsigned DX12Device::Allocate_RTV_Slot() { return 1; }
void DX12Device::Free_RTV_Slot(unsigned) {}
unsigned DX12Device::Allocate_DSV_Slot() { return 1; }
void DX12Device::Free_DSV_Slot(unsigned) {}
void DX12Device::Release_When_Idle(ID3D12Resource *) {}
void DX12Device::Queue_Texture_Upload(DX12TextureClass *, unsigned, ID3D12Resource *) {}

void DX12Device::Set_Render_Target(DX12TextureClass * color, DX12TextureClass * depth, bool use_default_depth)
{
	if (!Device)
	{
		return;
	}
	if (color && (!color->Is_Render_Target() || !color->Peek_VK_Image()))
	{
		color = nullptr;
	}
	if (depth && (!depth->Is_Depth_Target() || !depth->Peek_VK_Image()))
	{
		depth = nullptr;
		use_default_depth = false;
	}
	if (!color)
	{
		depth = nullptr;
		use_default_depth = true;
	}
	if (color != TargetColor || depth != TargetDepth || use_default_depth != TargetDefaultDepth)
	{
		End_Rendering();
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
	if (!Device || !dest || dest == TargetColor || !dest->Peek_VK_Image() || dest->Peek_VK_Image()->Format != VK_FORMAT_B8G8R8A8_UNORM)
	{
		return false;
	}
	if (!Recording)
	{
		Begin_Frame();
	}
	if (!Recording)
	{
		return false;
	}
	VKImageData * source = Current_Color_Image();
	VKImageData * target = dest->Peek_VK_Image();
	if (!source)
	{
		return false;
	}
	const RECT * rect = reinterpret_cast<const RECT *>(src_rect);
	int left = rect ? rect->left : 0;
	int top = rect ? rect->top : 0;
	int right = rect ? rect->right : (int)Target_Width();
	int bottom = rect ? rect->bottom : (int)Target_Height();
	if (left < 0) { dest_x -= left; left = 0; }
	if (top < 0) { dest_y -= top; top = 0; }
	if (dest_x < 0) { left -= dest_x; dest_x = 0; }
	if (dest_y < 0) { top -= dest_y; dest_y = 0; }
	if (right > (int)Target_Width())
		right = (int)Target_Width();
	if (bottom > (int)Target_Height())
		bottom = (int)Target_Height();
	if (right - left > (int)target->Width - dest_x)
		right = left + (int)target->Width - dest_x;
	if (bottom - top > (int)target->Height - dest_y)
		bottom = top + (int)target->Height - dest_y;
	if (right <= left || bottom <= top)
	{
		return false;
	}

	End_Rendering();
	VkCommandBuffer cb = Frames[FrameIndex].Main;
	Use_Target_Image(source, TargetColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	const bool target_is_render_target = dest->Is_Render_Target();
	VkImageLayout target_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (target_is_render_target)
	{
		Use_Target_Image(target, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}
	else
	{
		if (!target->InitialLayoutDone)
			return false;
		Image_Barrier(cb, target, target_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}
	VkImageCopy copy;
	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = 1;
	copy.dstSubresource = copy.srcSubresource;
	copy.srcOffset.x = left;
	copy.srcOffset.y = top;
	copy.dstOffset.x = dest_x;
	copy.dstOffset.y = dest_y;
	copy.extent.width = (unsigned)(right - left);
	copy.extent.height = (unsigned)(bottom - top);
	copy.extent.depth = 1;
	vkCmdCopyImage(cb, source->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	if (!target_is_render_target)
	{
		Image_Barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target_layout);
	}
	return true;
}

void DX12Device::Set_Gamma_Ramp(const unsigned short *, const unsigned short *, const unsigned short *)
{
	// TheSuperHackers @todo O138 part 2: the brightness pass.
}

DX12TextureClass * DX12Device::Read_Back_Buffer()
{
	if (!Device || !BackBuffer)
	{
		return nullptr;
	}
	const unsigned width = Width;
	const unsigned height = Height;
	void * cpu = nullptr;
	unsigned long long address = 0;
	VKBufferData * readback = VKDevice_Create_Host_Buffer(width * height * 4, &cpu, &address);
	if (!readback)
	{
		return nullptr;
	}
	VkBufferImageCopy region;
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;
	region.bufferOffset = 0;

	if (Recording)
	{
		// The frame so far: its commands are submitted and waited for, and the frame continues.
		End_Rendering();
		FrameSlot & frame = Frames[FrameIndex];
		VkCommandBuffer cb = frame.Main;
		const VkImageLayout layout = BackBuffer->Layout;
		Image_Barrier(cb, BackBuffer, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkCmdCopyImageToBuffer(cb, BackBuffer->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->Buffer, 1, &region);
		Image_Barrier(cb, BackBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : layout);
		if (layout == VK_IMAGE_LAYOUT_UNDEFINED)
			BackBuffer->Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		Submit(true, VK_NULL_HANDLE, VK_NULL_HANDLE);
		vkWaitForFences(VK, 1, &frame.Fence, VK_TRUE, UINT64_MAX);
		++DX12Device_Counters.FenceWaits[DX12_WAIT_FLUSH];
		CompletedSerial = frame.Serial;
		frame.Submitted = false;
		// Continue the frame in the same slot; its frame memory stays valid.
		vkResetCommandPool(VK, frame.Pool, 0);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cb, &begin);
		BoundPipeline = VK_NULL_HANDLE;
	}
	else
	{
		const VkImageLayout layout = BackBuffer->Layout;
		VkBuffer buffer = readback->Buffer;
		Run_One_Shot([&](VkCommandBuffer cb)
		{
			if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
				Image_Barrier(cb, BackBuffer, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			vkCmdCopyImageToBuffer(cb, BackBuffer->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
		});
		BackBuffer->Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	}

	DX12TextureClass * texture = DX12TextureClass::Create(width, height, WW3D_FORMAT_A8R8G8B8, 1, true, true);
	if (texture)
	{
		int pitch = 0;
		unsigned char * dst = static_cast<unsigned char *>(texture->Lock(0, &pitch, nullptr, false));
		if (dst)
		{
			const unsigned char * src = static_cast<const unsigned char *>(cpu);
			for (unsigned y = 0; y < height; ++y)
				memcpy(dst + (size_t)y * pitch, src + (size_t)y * width * 4, width * 4);
			texture->Unlock(0);
		}
	}
	VKDevice_Release_Buffer(readback);
	return texture;
}

void DX12Device::Queue_Dirty_Texture(DX12TextureClass * texture)
{
	if (!texture)
	{
		return;
	}
	if (!Device)
	{
		texture->Upload_Dirty_Levels();
		return;
	}
	texture->Add_Ref();
	DirtyTextures.push_back(texture);
}

bool DX12Device::Allocate_Frame_Memory(unsigned size, void ** cpu, unsigned long long * gpu)
{
	if (!Device || !cpu || !gpu)
	{
		return false;
	}
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
	while (frame.PageIndex < frame.Pages.size() && aligned > frame.Pages[frame.PageIndex].Size - frame.Pages[frame.PageIndex].Offset)
	{
		++frame.PageIndex;
	}
	if (frame.PageIndex >= frame.Pages.size())
	{
		FramePage page;
		if (!Create_Frame_Page(aligned > FRAME_EXTRA_PAGE_SIZE ? aligned : FRAME_EXTRA_PAGE_SIZE, page))
		{
			return false;
		}
		frame.Pages.push_back(page);
		frame.PageIndex = frame.Pages.size() - 1;
	}
	FramePage & page = frame.Pages[frame.PageIndex];
	*cpu = page.CPU + page.Offset;
	*gpu = Make_Address(page.Table, page.Offset);
	page.Offset += aligned;
	DX12Device_Counters.FrameMemoryBytes += aligned;
	return true;
}

void DX12Device::Register_Game_Shader(const char * name, bool vertex_shader, const char * hlsl_source, const char * entry_point)
{
	const int index = DX12GameShaders_Register(name, vertex_shader, hlsl_source, entry_point);
	if (index < 0)
	{
		return;
	}
	if ((size_t)index >= GameShaderCaches.size())
		GameShaderCaches.resize(index + 1);
	else
		GameShaderCaches[index] = GameShaderCache();
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
	const int index = DX12GameShaders_Find(name, vertex_shader);
	if (index < 0)
	{
		return 0;
	}
	unsigned layout_id = 0;
	if (vertex_shader)
	{
		DX12GameInputLayout source;
		std::string error;
		if (!DX12GameShaders_Parse_Declaration(declaration, source, error))
		{
			Debug_Log("game vertex shader %s: unsupported vertex declaration: %s", name, error.c_str());
			return 0;
		}
		for (unsigned i = 0; i < source.ElementCount; ++i)
		{
			if (source.Elements[i].Format == DXGI_B8G8R8A8_UNORM && !ColorVertexFormatSupported)
				return 0;
		}
		layout_id = Intern_Game_Layout(source);
	}
	const ShaderEntry & entry = Get_Game_Shader(index, nullptr);
	if (entry.Code.empty())
	{
		Debug_Log("game %s shader %s does not compile; the game falls back", vertex_shader ? "vertex" : "pixel", name);
		return 0;
	}
	return DX12GameShaders_Allocate_Handle(index, vertex_shader, layout_id);
}

void DX12Device::Delete_Game_Shader(unsigned handle)
{
	DX12GameShaders_Free_Handle(handle);
}

bool DX12Device::Is_Game_Shader(unsigned handle, bool vertex_shader)
{
	return DX12GameShaders_Resolve_Handle(handle, vertex_shader, nullptr, nullptr);
}

