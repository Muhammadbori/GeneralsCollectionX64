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

// TheSuperHackers @feature O138 Vulkan renderer (RTS_RENDERER=vulkan), shared between vkdevice.cpp and
// vkresource.cpp.
//
// A Vulkan build implements the renderer interface of Backend/DX12 (DX12Device in dx12device.h,
// DX12TextureClass and DX12BufferClass in dx12resource.h) with Vulkan instead of Direct3D 12, so DX8Wrapper
// and the rest of the game do not change. The fixed-function emulation (dx12ffp) and the game shader ports
// (dx12gameshaders) are shared: their HLSL is compiled to SPIR-V by the DirectX Shader Compiler (dxcompiler,
// "-spirv"), with the Direct3D registers mapped to the bindings of one push descriptor set:
//     b0..b3 -> bindings 0..3 (uniform buffers), t0..t7 -> 4..11 (sampled images), s0..s7 -> 12..19 (samplers).
//
// Vulkan is loaded at run time (vulkan-1.dll; libvulkan / MoltenVK on macOS), so the exe has no link time
// dependency on it. "GPU addresses" (DX12DrawDesc::VertexAddress, frame memory) are 64-bit values that encode a
// buffer of the device's buffer table and an offset: ((index + 1) << VK_ADDRESS_SHIFT) | offset.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif
#include <vulkan/vulkan.h>

#include <vector>

class DX12TextureClass;

enum
{
	VK_ADDRESS_SHIFT = 36,
};

// A piece of device memory from the device's block allocator.
struct VKAllocation
{
	VkDeviceMemory Memory;
	VkDeviceSize Offset;
	VkDeviceSize Size;
	unsigned Block;		// allocator block index
	void * Mapped;		// host visible allocations: the CPU address of Offset
};

struct VKImageData
{
	VkImage Image;
	VkImageView View;		// all aspects, identity swizzle: render and depth attachments, copies
	VkImageView SampleView;	// sampled view: depth only for depth targets, alpha one for targets without alpha
	VKAllocation Allocation;
	VkImageLayout Layout;	// main thread tracking
	VkFormat Format;
	VkImageAspectFlags Aspect;
	unsigned Width;			// size of level 0 of the image (rounded to 4 for block formats)
	unsigned Height;
	unsigned Levels;
	bool InitialLayoutDone;	// the first transition out of UNDEFINED was recorded
};

struct VKBufferData
{
	VkBuffer Buffer;
	VKAllocation Allocation;
	unsigned TableIndex;
};

// Function pointers, loaded by vkdevice.cpp.
#define VK_GLOBAL_FUNCTIONS(X) \
	X(vkCreateInstance) \
	X(vkEnumerateInstanceExtensionProperties) \
	X(vkEnumerateInstanceLayerProperties)

#ifdef _WIN32
#define VK_PLATFORM_INSTANCE_FUNCTIONS(X) X(vkCreateWin32SurfaceKHR)
#else
#define VK_PLATFORM_INSTANCE_FUNCTIONS(X)
#endif

#define VK_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceFeatures) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceFormatProperties) \
	X(vkEnumerateDeviceExtensionProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr) \
	X(vkDestroySurfaceKHR) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
	VK_PLATFORM_INSTANCE_FUNCTIONS(X)

#define VK_DEBUG_FUNCTIONS(X) \
	X(vkCreateDebugUtilsMessengerEXT) \
	X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FUNCTIONS(X) \
	X(vkDestroyDevice) \
	X(vkGetDeviceQueue) \
	X(vkDeviceWaitIdle) \
	X(vkQueueSubmit) \
	X(vkQueueWaitIdle) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkMapMemory) \
	X(vkUnmapMemory) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkBindBufferMemory) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkResetCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkGetFenceStatus) \
	X(vkCreateSemaphore) \
	X(vkDestroySemaphore) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer) \
	X(vkCmdCopyImage) \
	X(vkCmdBlitImage) \
	X(vkCmdClearColorImage) \
	X(vkCmdClearDepthStencilImage) \
	X(vkCmdClearAttachments) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdBindIndexBuffer) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdSetStencilReference) \
	X(vkCmdDraw) \
	X(vkCmdDrawIndexed) \
	X(vkCmdBeginRenderingKHR) \
	X(vkCmdEndRenderingKHR) \
	X(vkCmdPushDescriptorSetKHR) \
	X(vkCreateSwapchainKHR) \
	X(vkDestroySwapchainKHR) \
	X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) \
	X(vkQueuePresentKHR)

#define VK_DECLARE_FUNCTION(name) extern PFN_##name name;
VK_GLOBAL_FUNCTIONS(VK_DECLARE_FUNCTION)
VK_INSTANCE_FUNCTIONS(VK_DECLARE_FUNCTION)
VK_DEBUG_FUNCTIONS(VK_DECLARE_FUNCTION)
VK_DEVICE_FUNCTIONS(VK_DECLARE_FUNCTION)
#undef VK_DECLARE_FUNCTION

// Implemented in vkdevice.cpp for vkresource.cpp.
VkDevice VKDevice_Get();
// Creates an image with a view (one 2D image, all levels) and device local memory. Main thread only.
VKImageData * VKDevice_Create_Image(unsigned width, unsigned height, unsigned levels, VkFormat format, VkImageUsageFlags usage,
	VkImageAspectFlags aspect, bool alpha_one);
// Releases the image when the GPU no longer uses it. Any thread.
void VKDevice_Release_Image(VKImageData * image);
// A host visible, persistently mapped buffer in the buffer table, for static vertex and index buffers.
VKBufferData * VKDevice_Create_Host_Buffer(unsigned size, void ** cpu, unsigned long long * address);
void VKDevice_Release_Buffer(VKBufferData * buffer);
// Queues a level upload: data holds the level in the image's layout (tight rows of texels or 4x4 blocks)
// and is taken over (swapped out). Any thread.
void VKDevice_Queue_Texture_Upload(DX12TextureClass * texture, unsigned level, std::vector<unsigned char> & data);
// Records the first transition of a new texture image (and its zero clear) with the next uploads. Main thread.
void VKDevice_Queue_Initial_Layout(DX12TextureClass * texture);
