#pragma once
//
// The Vulkan functions pg::gpu calls, found at run time in the loader
// (libvulkan.so.1, vulkan-1.dll) -- nothing is linked against it, so a
// machine without Vulkan runs everything on the CPU. Inside pg::gpu alone:
// the rest of the program sees Gpu.h.
//
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <string>

namespace pg::gpu::vk {

// Before there is an instance.
#define PG_VK_GLOBAL(X)                       \
    X(vkCreateInstance)                       \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

// Of an instance.
#define PG_VK_INSTANCE(X)                       \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceProperties2)           \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkEnumerateDeviceExtensionProperties)     \
    X(vkCreateDevice)                           \
    X(vkGetDeviceProcAddr)

// Of a device.
#define PG_VK_DEVICE(X)               \
    X(vkDestroyDevice)                \
    X(vkGetDeviceQueue)               \
    X(vkDeviceWaitIdle)               \
    X(vkCreateBuffer)                 \
    X(vkDestroyBuffer)                \
    X(vkGetBufferMemoryRequirements)  \
    X(vkAllocateMemory)               \
    X(vkFreeMemory)                   \
    X(vkBindBufferMemory)             \
    X(vkMapMemory)                    \
    X(vkUnmapMemory)                  \
    X(vkCreateShaderModule)           \
    X(vkDestroyShaderModule)          \
    X(vkCreateDescriptorSetLayout)    \
    X(vkDestroyDescriptorSetLayout)   \
    X(vkCreatePipelineLayout)         \
    X(vkDestroyPipelineLayout)        \
    X(vkCreateComputePipelines)       \
    X(vkDestroyPipeline)              \
    X(vkCreateCommandPool)            \
    X(vkDestroyCommandPool)           \
    X(vkAllocateCommandBuffers)       \
    X(vkFreeCommandBuffers)           \
    X(vkBeginCommandBuffer)           \
    X(vkEndCommandBuffer)             \
    X(vkCmdBindPipeline)              \
    X(vkCmdPushConstants)             \
    X(vkCmdPushDescriptorSetKHR)      \
    X(vkCmdDispatch)                  \
    X(vkCmdPipelineBarrier)           \
    X(vkCmdCopyBuffer)                \
    X(vkCmdFillBuffer)                \
    X(vkCmdWriteTimestamp)            \
    X(vkCmdResetQueryPool)            \
    X(vkCreateQueryPool)              \
    X(vkDestroyQueryPool)             \
    X(vkGetQueryPoolResults)          \
    X(vkQueueSubmit)                  \
    X(vkCreateFence)                  \
    X(vkDestroyFence)                 \
    X(vkResetFences)                  \
    X(vkWaitForFences)

struct Api {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define PG_VK_DECLARE(name) PFN_##name name = nullptr;
    PG_VK_GLOBAL(PG_VK_DECLARE)
    PG_VK_INSTANCE(PG_VK_DECLARE)
    PG_VK_DEVICE(PG_VK_DECLARE)
#undef PG_VK_DECLARE
    // VK_EXT_debug_utils, with the validation layer alone: null without it.
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
};

/// The loader's functions before there is an instance -- opened once; false,
/// with `why`, without Vulkan.
bool loadGlobal(Api& api, std::string& why);
/// ... those of `instance`, those of `device`; false, with `why`, if one is
/// missing.
bool loadInstance(Api& api, VkInstance instance, std::string& why);
bool loadDevice(Api& api, VkDevice device, std::string& why);

/// A VkResult said in words: "VK_ERROR_DEVICE_LOST".
const char* resultName(VkResult result);

}  // namespace pg::gpu::vk
