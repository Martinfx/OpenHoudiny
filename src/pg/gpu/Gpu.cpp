#include "pg/gpu/Gpu.h"

#include "pg/gpu/Shaders.h"
#include "pg/gpu/Vulkan.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace pg::gpu {
namespace {

/// What is copied through the CPU-visible memory at a time.
constexpr size_t kStagingBytes = size_t(64) << 20;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string versionText(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

const char* kindOf(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "other";
    }
}

/// How much a device is preferred when none is named: a discrete GPU most,
/// a CPU pretending to be one least -- and only when asked for.
int preference(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

/// An instance, its functions, and the devices it sees: destroyed with it.
struct Instance {
    vk::Api api;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;  ///< the validation layer's findings
    std::vector<VkPhysicalDevice> physical;
    ~Instance() {
        if (messenger) api.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        if (instance) api.vkDestroyInstance(instance, nullptr);
    }
};

/// What the validation layer finds, to stderr: it is there to be read.
VKAPI_ATTR VkBool32 VKAPI_CALL reportFinding(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    std::fprintf(stderr, "vulkan %s: %s\n", error ? "error" : "warning", data && data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

/// An instance for compute alone: no surfaces. PG_GPU_VALIDATE=1 adds the
/// validation layer, if there is one.
bool createInstance(Instance& in, std::string& why) {
    if (!vk::loadGlobal(in.api, why)) return false;
    uint32_t count = 0;
    in.api.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    in.api.vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    std::vector<const char*> enabled;
    VkInstanceCreateFlags flags = 0;
    // MoltenVK lists its devices only to those who say they take what it is.
    if (hasExtension(extensions, "VK_KHR_portability_enumeration")) {
        enabled.push_back("VK_KHR_portability_enumeration");
        flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    std::vector<const char*> layers;
    if (const char* v = std::getenv("PG_GPU_VALIDATE"); v && *v && std::strcmp(v, "0") != 0) {
        in.api.vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> found(count);
        in.api.vkEnumerateInstanceLayerProperties(&count, found.data());
        for (const VkLayerProperties& l : found) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        if (layers.empty()) std::fprintf(stderr, "PG_GPU_VALIDATE: no validation layer (VK_LAYER_KHRONOS_validation)\n");
    }
    // Its findings told through VK_EXT_debug_utils -- the loader's, or the
    // layer's own.
    bool report = false;
    if (!layers.empty()) {
        report = hasExtension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (!report) {
            in.api.vkEnumerateInstanceExtensionProperties(layers[0], &count, nullptr);
            std::vector<VkExtensionProperties> own(count);
            in.api.vkEnumerateInstanceExtensionProperties(layers[0], &count, own.data());
            report = hasExtension(own, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        if (report) enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkDebugUtilsMessengerCreateInfoEXT messenger{};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = reportFinding;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Prototype";
    app.pEngineName = "Prototype";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = report ? &messenger : nullptr;  // what creating the instance finds, too
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (const VkResult r = in.api.vkCreateInstance(&info, nullptr, &in.instance); r != VK_SUCCESS) {
        in.instance = VK_NULL_HANDLE;
        why = r == VK_ERROR_INCOMPATIBLE_DRIVER ? "the Vulkan loader found no driver (VK_ERROR_INCOMPATIBLE_DRIVER)"
                                                : std::string("Vulkan would not start: ") + vk::resultName(r);
        return false;
    }
    if (!vk::loadInstance(in.api, in.instance, why)) return false;
    if (report) {
        in.api.vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            in.api.vkGetInstanceProcAddr(in.instance, "vkCreateDebugUtilsMessengerEXT"));
        in.api.vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            in.api.vkGetInstanceProcAddr(in.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (in.api.vkCreateDebugUtilsMessengerEXT && in.api.vkDestroyDebugUtilsMessengerEXT &&
            in.api.vkCreateDebugUtilsMessengerEXT(in.instance, &messenger, nullptr, &in.messenger) != VK_SUCCESS) {
            in.messenger = VK_NULL_HANDLE;
        }
    }
    in.api.vkEnumeratePhysicalDevices(in.instance, &count, nullptr);
    in.physical.resize(count);
    in.api.vkEnumeratePhysicalDevices(in.instance, &count, in.physical.data());
    in.physical.resize(count);
    return true;
}

/// A device as DeviceInfo says it, and the queue family it would compute
/// on.
DeviceInfo describe(const vk::Api& api, VkPhysicalDevice pd, int index, uint32_t& family) {
    DeviceInfo d;
    d.index = index;
    VkPhysicalDeviceProperties props{};
    api.vkGetPhysicalDeviceProperties(pd, &props);
    d.name = props.deviceName;
    d.kind = kindOf(props.deviceType);
    d.cpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    d.api = versionText(props.apiVersion);
    // Its subgroups (Vulkan 1.1), its driver's name and version (1.2).
    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    const bool v11 = props.apiVersion >= VK_API_VERSION_1_1, v12 = props.apiVersion >= VK_API_VERSION_1_2;
    if (v11) {
        props2.pNext = &subgroup;
        if (v12) subgroup.pNext = &driver;
        api.vkGetPhysicalDeviceProperties2(pd, &props2);
        d.subgroup = subgroup.subgroupSize;
    }
    if (v12 && driver.driverName[0]) {
        d.driver = std::string(driver.driverName) + " " + driver.driverInfo;
    } else if (props.vendorID == 0x10de) {
        const uint32_t v = props.driverVersion;  // NVIDIA's own way: 10.8.8.6 bits
        d.driver = "NVIDIA " + std::to_string(v >> 22) + "." + std::to_string((v >> 14) & 0xff);
    } else {
        d.driver = versionText(props.driverVersion);
    }
    VkPhysicalDeviceMemoryProperties memory{};
    api.vkGetPhysicalDeviceMemoryProperties(pd, &memory);
    for (uint32_t h = 0; h < memory.memoryHeapCount; ++h) {
        if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) d.memory += memory.memoryHeaps[h].size;
    }
    // What the kernels need: Vulkan 1.1, a queue that computes, buffers
    // bound as they are dispatched (push descriptors), and room for them.
    std::string missing;
    if (!v11) missing = "Vulkan 1.1";
    uint32_t count = 0;
    api.vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    api.vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
    family = UINT32_MAX;
    for (uint32_t f = 0; f < count; ++f) {
        if ((families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && family == UINT32_MAX) family = f;
    }
    if (family == UINT32_MAX) missing = "a queue that computes";
    api.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    api.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, extensions.data());
    if (!hasExtension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) missing = "VK_KHR_push_descriptor";
    const VkPhysicalDeviceLimits& l = props.limits;
    if (l.maxPerStageDescriptorStorageBuffers < static_cast<uint32_t>(Device::kMaxBuffers) ||
        l.maxPushConstantsSize < Device::kMaxPush || l.maxComputeWorkGroupInvocations < 256) {
        missing = "room for the kernels' buffers and work groups";
    }
    d.usable = missing.empty();
    d.missing = missing;
    return d;
}

uint32_t memoryType(const VkPhysicalDeviceMemoryProperties& memory, uint32_t allowed, VkMemoryPropertyFlags want,
                    VkMemoryPropertyFlags rather) {
    // What is wanted and rather had, else what is wanted.
    for (const VkMemoryPropertyFlags flags : {want | rather, want}) {
        for (uint32_t t = 0; t < memory.memoryTypeCount; ++t) {
            if ((allowed & (1u << t)) && (memory.memoryTypes[t].propertyFlags & flags) == flags) return t;
        }
    }
    return UINT32_MAX;
}

}  // namespace

std::vector<DeviceInfo> devices(std::string* why) {
    Instance in;
    std::string error;
    std::vector<DeviceInfo> out;
    if (!createInstance(in, error)) {
        if (why) *why = error;
        return out;
    }
    for (size_t i = 0; i < in.physical.size(); ++i) {
        uint32_t family = 0;
        out.push_back(describe(in.api, in.physical[i], static_cast<int>(i), family));
    }
    if (why) *why = out.empty() ? "Vulkan sees no device" : "";
    return out;
}

// --- the device ------------------------------------------------------------------------

struct Device::Impl {
    Instance instance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;  ///< two: when a batch began and ended; none if it cannot tell
    double timestampNs = 0.0;                  ///< nanoseconds a tick
    uint64_t timestampMask = 0;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::map<std::string, VkPipeline> pipelines;
    std::unique_ptr<Buffer> staging;  ///< what the CPU writes and reads, kStagingBytes

    vk::Api& api() { return instance.api; }
    /// The kernel `name`'s pipeline, made the first time; null, with `why`,
    /// for a kernel the program does not carry.
    VkPipeline pipeline(const std::string& name, std::string& why);
};

struct Buffer::Handles {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;  ///< the CPU's view of it, for the staging buffer alone
};

Device::Device() : impl_(std::make_unique<Impl>()) {}

Device::~Device() {
    Impl& m = *impl_;
    if (!m.device) return;
    vk::Api& api = m.api();
    api.vkDeviceWaitIdle(m.device);
    m.staging.reset();
    for (const auto& [name, pipeline] : m.pipelines) api.vkDestroyPipeline(m.device, pipeline, nullptr);
    if (m.layout) api.vkDestroyPipelineLayout(m.device, m.layout, nullptr);
    if (m.setLayout) api.vkDestroyDescriptorSetLayout(m.device, m.setLayout, nullptr);
    if (m.timestamps) api.vkDestroyQueryPool(m.device, m.timestamps, nullptr);
    if (m.fence) api.vkDestroyFence(m.device, m.fence, nullptr);
    if (m.pool) api.vkDestroyCommandPool(m.device, m.pool, nullptr);
    api.vkDestroyDevice(m.device, nullptr);
}

bool Device::fail(const std::string& what) {
    if (error_.empty()) error_ = what;
    return false;
}

std::unique_ptr<Device> Device::open(const std::string& choice, std::string& why, bool cpuToo) {
    std::string want = choice;
    if (want.empty()) {
        if (const char* env = std::getenv("PG_GPU")) want = env;
    }
    want = lower(want);
    if (want == "none" || want == "cpu" || want == "off") {
        why = "the GPU is turned off (" + std::string(choice.empty() ? "PG_GPU=" : "") + want + ")";
        return nullptr;
    }
    std::unique_ptr<Device> d(new Device());
    Impl& m = *d->impl_;
    if (!createInstance(m.instance, why)) return nullptr;
    vk::Api& api = m.api();
    if (m.instance.physical.empty()) {
        why = "Vulkan sees no device";
        return nullptr;
    }
    std::vector<DeviceInfo> infos;
    std::vector<uint32_t> families;
    for (size_t i = 0; i < m.instance.physical.size(); ++i) {
        uint32_t family = 0;
        infos.push_back(describe(api, m.instance.physical[i], static_cast<int>(i), family));
        families.push_back(family);
    }
    // The one named -- by its place in the list, or a part of its name -- or
    // the best that will do.
    int chosen = -1;
    if (!want.empty()) {
        const bool number = std::all_of(want.begin(), want.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        for (const DeviceInfo& i : infos) {
            if (number ? std::to_string(i.index) == want : lower(i.name).find(want) != std::string::npos) {
                chosen = i.index;
                break;
            }
        }
        if (chosen < 0) {
            why = "no Vulkan device is \"" + want + "\": there are";
            for (const DeviceInfo& i : infos) why += " " + std::to_string(i.index) + " " + i.name + ";";
            why.pop_back();
            return nullptr;
        }
    } else {
        int best = 0;
        for (size_t i = 0; i < infos.size(); ++i) {
            VkPhysicalDeviceProperties props{};
            api.vkGetPhysicalDeviceProperties(m.instance.physical[i], &props);
            const int score = infos[i].usable && (!infos[i].cpu || cpuToo) ? preference(props.deviceType) : 0;
            if (score > best) {
                best = score;
                chosen = static_cast<int>(i);
            }
        }
        if (chosen < 0) {
            why = "no GPU that will do: there are";
            for (const DeviceInfo& i : infos) why += " " + std::to_string(i.index) + " " + i.name + (i.usable ? " (" + i.kind + ");" : " (no " + i.missing + ");");
            why.pop_back();
            return nullptr;
        }
    }
    d->info_ = infos[static_cast<size_t>(chosen)];
    if (!d->info_.usable) {
        why = d->info_.name + " lacks " + d->info_.missing;
        return nullptr;
    }
    m.physical = m.instance.physical[static_cast<size_t>(chosen)];
    m.family = families[static_cast<size_t>(chosen)];
    api.vkGetPhysicalDeviceMemoryProperties(m.physical, &m.memory);

    // One queue, that computes; buffers bound as they are dispatched.
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = m.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    if (const VkResult r = api.vkCreateDevice(m.physical, &info, nullptr, &m.device); r != VK_SUCCESS) {
        m.device = VK_NULL_HANDLE;
        why = d->info_.name + " would not open: " + vk::resultName(r);
        return nullptr;
    }
    if (!vk::loadDevice(api, m.device, why)) return nullptr;
    api.vkGetDeviceQueue(m.device, m.family, 0, &m.queue);

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = m.family;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (api.vkCreateCommandPool(m.device, &pool, nullptr, &m.pool) != VK_SUCCESS ||
        api.vkCreateFence(m.device, &fence, nullptr, &m.fence) != VK_SUCCESS) {
        why = "the device would not make a command pool";
        return nullptr;
    }
    // Timestamps, where the queue keeps them.
    uint32_t count = 0;
    api.vkGetPhysicalDeviceQueueFamilyProperties(m.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families2(count);
    api.vkGetPhysicalDeviceQueueFamilyProperties(m.physical, &count, families2.data());
    VkPhysicalDeviceProperties props{};
    api.vkGetPhysicalDeviceProperties(m.physical, &props);
    const uint32_t bits = families2[m.family].timestampValidBits;
    if (bits > 0 && props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 2;
        if (api.vkCreateQueryPool(m.device, &q, nullptr, &m.timestamps) != VK_SUCCESS) m.timestamps = VK_NULL_HANDLE;
        m.timestampNs = props.limits.timestampPeriod;
        m.timestampMask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    }
    // Every kernel the same layout: kMaxBuffers storage buffers, kMaxPush
    // bytes of push constants.
    VkDescriptorSetLayoutBinding bindings[kMaxBuffers]{};
    for (int b = 0; b < kMaxBuffers; ++b) {
        bindings[b].binding = static_cast<uint32_t>(b);
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set.bindingCount = kMaxBuffers;
    set.pBindings = bindings;
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = kMaxPush;
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m.setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (api.vkCreateDescriptorSetLayout(m.device, &set, nullptr, &m.setLayout) != VK_SUCCESS ||
        api.vkCreatePipelineLayout(m.device, &layout, nullptr, &m.layout) != VK_SUCCESS) {
        why = "the device would not make the kernels' layout";
        return nullptr;
    }
    why.clear();
    return d;
}

VkPipeline Device::Impl::pipeline(const std::string& name, std::string& why) {
    if (const auto it = pipelines.find(name); it != pipelines.end()) return it->second;
    const Spirv* code = nullptr;
    for (size_t i = 0; i < kSpirvCount; ++i) {
        if (name == kSpirv[i].name) code = &kSpirv[i];
    }
    if (!code) {
        why = "no kernel " + name;
        return VK_NULL_HANDLE;
    }
    vk::Api& a = api();
    VkShaderModuleCreateInfo module{};
    module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize = code->count * sizeof(uint32_t);
    module.pCode = code->words;
    VkShaderModule shader = VK_NULL_HANDLE;
    if (const VkResult r = a.vkCreateShaderModule(device, &module, nullptr, &shader); r != VK_SUCCESS) {
        why = "kernel " + name + " would not load: " + vk::resultName(r);
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = a.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    a.vkDestroyShaderModule(device, shader, nullptr);
    if (r != VK_SUCCESS) {
        why = "kernel " + name + " would not build: " + vk::resultName(r);
        return VK_NULL_HANDLE;
    }
    pipelines[name] = pipeline;
    return pipeline;
}

// --- buffers ---------------------------------------------------------------------------

Buffer::Buffer(Device& device, size_t bytes) : device_(device), bytes_(bytes), handles_(std::make_unique<Handles>()) {}

Buffer::~Buffer() {
    Device::Impl& m = device_.impl();
    if (!m.device) return;
    vk::Api& api = m.api();
    if (handles_->mapped) api.vkUnmapMemory(m.device, handles_->memory);
    if (handles_->buffer) api.vkDestroyBuffer(m.device, handles_->buffer, nullptr);
    if (handles_->memory) api.vkFreeMemory(m.device, handles_->memory, nullptr);
}

namespace {

/// Makes `b`'s buffer and memory: on the device, or where the CPU sees it.
bool allocate(Device::Impl& m, VkBuffer& buffer, VkDeviceMemory& memory, void** mapped, size_t bytes, bool host,
              std::string& why) {
    vk::Api& api = m.api();
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = std::max<size_t>(bytes, 4);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (const VkResult r = api.vkCreateBuffer(m.device, &info, nullptr, &buffer); r != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        why = std::string("no buffer of ") + std::to_string(bytes) + " bytes: " + vk::resultName(r);
        return false;
    }
    VkMemoryRequirements need{};
    api.vkGetBufferMemoryRequirements(m.device, buffer, &need);
    const uint32_t type = host ? memoryType(m.memory, need.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                               : memoryType(m.memory, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) {
        why = "no memory of the kind a buffer needs";
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = need.size;
    alloc.memoryTypeIndex = type;
    if (const VkResult r = api.vkAllocateMemory(m.device, &alloc, nullptr, &memory); r != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
        why = std::string("no memory for ") + std::to_string(bytes) + " bytes on the device: " + vk::resultName(r);
        return false;
    }
    if (api.vkBindBufferMemory(m.device, buffer, memory, 0) != VK_SUCCESS) {
        why = "a buffer would not take its memory";
        return false;
    }
    if (host && api.vkMapMemory(m.device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        why = "the CPU cannot see the staging memory";
        return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<Buffer> Device::buffer(size_t bytes) {
    if (!ok()) return nullptr;
    std::unique_ptr<Buffer> b(new Buffer(*this, bytes));
    std::string why;
    if (!allocate(*impl_, b->handles_->buffer, b->handles_->memory, nullptr, bytes, false, why)) {
        fail(why);
        return nullptr;
    }
    return b;
}

bool Device::staging() {
    Impl& m = *impl_;
    if (m.staging) return true;
    std::unique_ptr<Buffer> s(new Buffer(*this, kStagingBytes));
    std::string why;
    if (!allocate(m, s->handles_->buffer, s->handles_->memory, &s->handles_->mapped, kStagingBytes, true, why)) {
        return fail(why);
    }
    m.staging = std::move(s);
    return true;
}

bool Device::upload(Buffer& to, const void* from, size_t bytes, size_t offset) {
    if (!ok()) return false;
    if (offset + bytes > to.bytes()) return fail("an upload past the end of a buffer");
    Impl& m = *impl_;
    if (!staging()) return false;
    const auto* src = static_cast<const uint8_t*>(from);
    for (size_t done = 0; done < bytes;) {
        const size_t n = std::min(bytes - done, kStagingBytes);
        std::memcpy(m.staging->handles_->mapped, src + done, n);
        Batch batch(*this);
        batch.copy(*m.staging, to, n, 0, offset + done);
        if (batch.run() < 0.0) return false;
        done += n;
    }
    return true;
}

bool Device::download(const Buffer& from, void* to, size_t bytes, size_t offset) {
    if (!ok()) return false;
    if (offset + bytes > from.bytes()) return fail("a download past the end of a buffer");
    Impl& m = *impl_;
    if (!staging()) return false;
    auto* dst = static_cast<uint8_t*>(to);
    for (size_t done = 0; done < bytes;) {
        const size_t n = std::min(bytes - done, kStagingBytes);
        Batch batch(*this);
        batch.copy(from, *m.staging, n, offset + done, 0);
        if (batch.run() < 0.0) return false;
        std::memcpy(dst + done, m.staging->handles_->mapped, n);
        done += n;
    }
    return true;
}

// --- batches ---------------------------------------------------------------------------

struct Batch::State {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool recording = false;
};

Batch::Batch(Device& device) : device_(device), state_(std::make_unique<State>()) {
    if (!device_.ok()) return;
    Device::Impl& m = device_.impl();
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = m.pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    if (m.api().vkAllocateCommandBuffers(m.device, &info, &state_->cmd) != VK_SUCCESS) {
        state_->cmd = VK_NULL_HANDLE;
        device_.fail("the device would not give a command buffer");
    }
}

Batch::~Batch() {
    Device::Impl& m = device_.impl();
    if (state_->cmd && m.device) m.api().vkFreeCommandBuffers(m.device, m.pool, 1, &state_->cmd);
}

namespace {

/// Everything before it written, before anything after it reads or writes:
/// kernels and copies alike.
void barrier(vk::Api& api, VkCommandBuffer cmd, VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
             VkAccessFlags dstAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                       VK_ACCESS_TRANSFER_WRITE_BIT) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = dstAccess;
    api.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage, 0, 1, &b,
                             0, nullptr, 0, nullptr);
}

}  // namespace

void Batch::begin() {
    if (state_->recording || !device_.ok() || !state_->cmd) return;
    Device::Impl& m = device_.impl();
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (m.api().vkBeginCommandBuffer(state_->cmd, &info) != VK_SUCCESS) {
        device_.fail("a command buffer would not begin");
        return;
    }
    if (m.timestamps) {
        m.api().vkCmdResetQueryPool(state_->cmd, m.timestamps, 0, 2);
        m.api().vkCmdWriteTimestamp(state_->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m.timestamps, 0);
    }
    state_->recording = true;
}

Batch& Batch::dispatch(const char* shader, std::initializer_list<const Buffer*> buffers, const void* push,
                       size_t pushBytes, uint32_t x, uint32_t y, uint32_t z) {
    begin();
    if (!state_->recording) return *this;
    Device::Impl& m = device_.impl();
    vk::Api& api = m.api();
    if (buffers.size() > static_cast<size_t>(Device::kMaxBuffers) || pushBytes > Device::kMaxPush || pushBytes % 4 != 0) {
        device_.fail(std::string("kernel ") + shader + ": too many buffers or push constants");
        return *this;
    }
    std::string why;
    const VkPipeline pipeline = m.pipeline(shader, why);
    if (!pipeline) {
        device_.fail(why);
        return *this;
    }
    api.vkCmdBindPipeline(state_->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDescriptorBufferInfo infos[Device::kMaxBuffers]{};
    VkWriteDescriptorSet writes[Device::kMaxBuffers]{};
    uint32_t n = 0;
    for (const Buffer* b : buffers) {
        infos[n].buffer = b->handles_->buffer;
        infos[n].range = VK_WHOLE_SIZE;
        writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[n].dstBinding = n;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[n].pBufferInfo = &infos[n];
        ++n;
    }
    if (n > 0) api.vkCmdPushDescriptorSetKHR(state_->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, n, writes);
    if (pushBytes > 0) {
        api.vkCmdPushConstants(state_->cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(pushBytes), push);
    }
    api.vkCmdDispatch(state_->cmd, x, y, z);
    barrier(api, state_->cmd);
    return *this;
}

Batch& Batch::copy(const Buffer& from, Buffer& to, size_t bytes, size_t fromOffset, size_t toOffset) {
    begin();
    if (!state_->recording || bytes == 0) return *this;
    if (fromOffset + bytes > from.bytes() || toOffset + bytes > to.bytes()) {
        device_.fail("a copy past the end of a buffer");
        return *this;
    }
    vk::Api& api = device_.impl().api();
    VkBufferCopy region{};
    region.srcOffset = fromOffset;
    region.dstOffset = toOffset;
    region.size = bytes;
    api.vkCmdCopyBuffer(state_->cmd, from.handles_->buffer, to.handles_->buffer, 1, &region);
    barrier(api, state_->cmd);
    return *this;
}

Batch& Batch::fill(Buffer& buffer, uint32_t word, size_t bytes, size_t offset) {
    begin();
    if (!state_->recording || bytes == 0) return *this;
    if (offset % 4 != 0 || bytes % 4 != 0 || offset + bytes > buffer.bytes()) {
        device_.fail("a fill not of whole words, or past the end of a buffer");
        return *this;
    }
    vk::Api& api = device_.impl().api();
    api.vkCmdFillBuffer(state_->cmd, buffer.handles_->buffer, offset, bytes, word);
    barrier(api, state_->cmd);
    return *this;
}

double Batch::run() {
    if (!device_.ok()) return -1.0;
    if (!state_->recording) return 0.0;
    Device::Impl& m = device_.impl();
    vk::Api& api = m.api();
    // What it wrote, for the CPU to read once it is done.
    barrier(api, state_->cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if (m.timestamps) api.vkCmdWriteTimestamp(state_->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m.timestamps, 1);
    state_->recording = false;
    if (api.vkEndCommandBuffer(state_->cmd) != VK_SUCCESS) {
        device_.fail("a command buffer would not end");
        return -1.0;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &state_->cmd;
    const auto start = std::chrono::steady_clock::now();
    if (const VkResult r = api.vkQueueSubmit(m.queue, 1, &submit, m.fence); r != VK_SUCCESS) {
        device_.fail(std::string("the device would not take the work: ") + vk::resultName(r));
        return -1.0;
    }
    const VkResult waited = api.vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, UINT64_MAX);
    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (waited != VK_SUCCESS) {
        device_.fail(std::string("the device did not finish: ") + vk::resultName(waited));
        return -1.0;
    }
    api.vkResetFences(m.device, 1, &m.fence);
    if (m.timestamps) {
        uint64_t ticks[2] = {0, 0};
        if (api.vkGetQueryPoolResults(m.device, m.timestamps, 0, 2, sizeof ticks, ticks, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint64_t span = (ticks[1] - ticks[0]) & m.timestampMask;
            return static_cast<double>(span) * m.timestampNs * 1e-6;
        }
    }
    return wall;
}

std::vector<std::string> kernels() {
    std::vector<std::string> out;
    for (size_t i = 0; i < kSpirvCount; ++i) out.emplace_back(kSpirv[i].name);
    return out;
}

}  // namespace pg::gpu
