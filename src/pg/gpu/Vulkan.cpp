#include "pg/gpu/Vulkan.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <mutex>

namespace pg::gpu::vk {
namespace {

/// The loader's vkGetInstanceProcAddr, the library opened once and kept
/// open: what is found in it stays good. Null, with `why`, without one.
PFN_vkGetInstanceProcAddr findLoader(std::string& why) {
    static std::once_flag once;
    static PFN_vkGetInstanceProcAddr get = nullptr;
    static std::string error;
    std::call_once(once, [] {
#ifdef _WIN32
        HMODULE lib = LoadLibraryA("vulkan-1.dll");
        if (lib) get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
#else
        void* lib = nullptr;
        for (const char* name : {"libvulkan.so.1", "libvulkan.so", "libvulkan.1.dylib", "libMoltenVK.dylib"}) {
            lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (lib) break;
        }
        if (lib) get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
        if (!lib) error = "no Vulkan loader on this machine (libvulkan.so.1)";
        else if (!get) error = "the Vulkan loader has no vkGetInstanceProcAddr";
    });
    why = error;
    return get;
}

}  // namespace

bool loadGlobal(Api& api, std::string& why) {
    api.vkGetInstanceProcAddr = findLoader(why);
    if (!api.vkGetInstanceProcAddr) return false;
#define PG_VK_LOAD(name)                                                                       \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name)); \
    if (!api.name) {                                                                           \
        why = "the Vulkan loader has no " #name;                                               \
        return false;                                                                          \
    }
    PG_VK_GLOBAL(PG_VK_LOAD)
#undef PG_VK_LOAD
    return true;
}

bool loadInstance(Api& api, VkInstance instance, std::string& why) {
#define PG_VK_LOAD(name)                                                                 \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetInstanceProcAddr(instance, #name)); \
    if (!api.name) {                                                                     \
        why = "this Vulkan has no " #name;                                               \
        return false;                                                                    \
    }
    PG_VK_INSTANCE(PG_VK_LOAD)
#undef PG_VK_LOAD
    return true;
}

bool loadDevice(Api& api, VkDevice device, std::string& why) {
#define PG_VK_LOAD(name)                                                             \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetDeviceProcAddr(device, #name)); \
    if (!api.name) {                                                                 \
        why = "the device's driver has no " #name;                                   \
        return false;                                                                \
    }
    PG_VK_DEVICE(PG_VK_LOAD)
#undef PG_VK_LOAD
    return true;
}

const char* resultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        default: return "a Vulkan error";
    }
}

}  // namespace pg::gpu::vk
