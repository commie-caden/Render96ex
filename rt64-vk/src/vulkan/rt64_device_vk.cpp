/*
 * rt64_device_vk — Vulkan instance and device creation.
 *
 * Replaces the D3D12 object graph in the original rt64_device.cpp: instance
 * and physical-device selection stand in for the DXGI factory and adapter
 * enumeration, and VMA stands in for D3D12MemoryAllocator.
 *
 * Device selection is strict about ray tracing. A machine can expose several
 * Vulkan devices (a discrete GPU alongside llvmpipe, say), so we score them
 * and reject any that cannot path trace rather than taking the first hit.
 */
#include "rt64_device_vk.h"
#include "rt64_swapchain_vk.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

namespace RT64 {

namespace {

/* Extensions that are still extensions in 1.4, so must be requested. */
const char *const kRequiredDeviceExtensions[] = {
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

bool hasExtension(const std::vector<VkExtensionProperties> &available,
                  const char *name) {
    for (const VkExtensionProperties &e : available) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data());
    return list;
}

} /* namespace */

/* ------------------------------------------------------------------ setup */

bool DeviceVK::gatherInstanceExtensions(std::string &error) {
    /* A Wayland session with XWayland running exposes both surface paths, and
       SDL picks one at runtime. Asking SDL which extensions it needs is the
       only correct answer; hardcoding VK_KHR_wayland_surface would break the
       moment SDL_VIDEODRIVER=x11, and vice versa. */
    if (windowHandle == nullptr) {
        return true;                      /* headless: no surface needed */
    }
    SDL_Window *window = (SDL_Window *)windowHandle;

    unsigned int count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) {
        error = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") +
                SDL_GetError();
        return false;
    }
    std::vector<const char *> names(count);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &count, names.data())) {
        error = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") +
                SDL_GetError();
        return false;
    }
    for (unsigned int i = 0; i < count; i++) {
        /* SDL owns these strings for the window's lifetime; copy so the device
           does not depend on that. */
        ownedExtensionNames.emplace_back(names[i]);
    }
    for (const std::string &name : ownedExtensionNames) {
        instanceExtensions.push_back(name.c_str());
    }
    return true;
}

bool DeviceVK::createInstance(std::string &error) {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerateVersion =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            nullptr, "vkEnumerateInstanceVersion");
    if (enumerateVersion != nullptr) {
        enumerateVersion(&loaderVersion);
    }

    /* Ask for 1.4 but do not hard-fail below it: ray tracing is an extension
       at every version, so a 1.2 loader can still run this. */
    if (loaderVersion < VK_API_VERSION_1_3) {
        error = "Vulkan 1.3 or newer is required; loader reports " +
                std::to_string(VK_VERSION_MAJOR(loaderVersion)) + "." +
                std::to_string(VK_VERSION_MINOR(loaderVersion));
        return false;
    }
    apiVersion = std::min<uint32_t>(loaderVersion, RT64_VULKAN_API_VERSION);

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "RT64";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "RT64";
    appInfo.apiVersion = apiVersion;

    std::vector<const char *> layers;
    if (validationEnabled) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> available(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, available.data());
        for (const VkLayerProperties &l : available) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                break;
            }
        }
        if (layers.empty()) {
            /* Not fatal — just means the layers are not installed. */
            validationEnabled = false;
        }
    }

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = (uint32_t)layers.size();
    createInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    createInfo.enabledExtensionCount = (uint32_t)instanceExtensions.size();
    createInfo.ppEnabledExtensionNames =
        instanceExtensions.empty() ? nullptr : instanceExtensions.data();

    VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
    if (res != VK_SUCCESS) {
        error = "vkCreateInstance failed (" + std::to_string((int)res) + ")";
        return false;
    }
    return true;
}

int DeviceVK::scoreDevice(VkPhysicalDevice candidate,
                          std::string &reason) const {
    std::vector<VkExtensionProperties> available = deviceExtensions(candidate);

    for (const char *name : kRequiredDeviceExtensions) {
        if (!hasExtension(available, name)) {
            reason = std::string("missing ") + name;
            return -1;
        }
    }

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(candidate, &props);

    /* gTextures is declared as a fixed 512-element array in the shaders. */
    if (props.limits.maxPerStageDescriptorSampledImages < 512) {
        reason = "maxPerStageDescriptorSampledImages < 512";
        return -1;
    }
    /* Dynamic rendering (core in 1.3) lets us skip VkRenderPass and
       VkFramebuffer entirely. Any GPU that can do KHR ray tracing has a 1.3
       driver, so requiring it costs nothing and removes a lot of code. */
    if (VK_VERSION_MAJOR(props.apiVersion) == 1 &&
        VK_VERSION_MINOR(props.apiVersion) < 3) {
        reason = "device API older than 1.3 (need dynamic rendering)";
        return -1;
    }

    /* Prefer real hardware. A software device (llvmpipe) is a valid debug
       target but should never win over a discrete GPU. */
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 1000;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 250;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 10;
        default:                                     return 1;
    }
}

bool DeviceVK::pickPhysicalDevice(std::string &error) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        error = "no Vulkan physical devices found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    int bestScore = -1;
    std::string rejections;
    for (VkPhysicalDevice candidate : devices) {
        std::string reason;
        int score = scoreDevice(candidate, reason);
        if (score < 0) {
            VkPhysicalDeviceProperties props = {};
            vkGetPhysicalDeviceProperties(candidate, &props);
            rejections += std::string("\n  ") + props.deviceName + ": " + reason;
            continue;
        }
        if (score > bestScore) {
            bestScore = score;
            physicalDevice = candidate;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        error = "no device supports ray tracing." + rejections;
        return false;
    }

    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);

    /* Cache the ray tracing limits. The shader binding table layout in the
       pipeline builder depends on these, and they differ from D3D12's fixed
       32-byte record / 64-byte table alignment. */
    rtProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    asProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    asProperties.pNext = &rtProperties;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &asProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

    return true;
}

bool DeviceVK::findQueueFamilies(std::string &error) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count,
                                             families.data());

    VkSurfaceKHR surface =
        (swapchain != nullptr) ? swapchain->getSurface() : VK_NULL_HANDLE;

    for (uint32_t i = 0; i < count; i++) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            continue;
        }
        /* When presenting, the family must also support the surface. RADV
           exposes one universal family so this normally picks 0, but that is
           not guaranteed across drivers. */
        if (surface != VK_NULL_HANDLE) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface,
                                                 &present);
            if (!present) {
                continue;
            }
        }
        graphicsFamily = i;
        break;
    }
    if (graphicsFamily == UINT32_MAX) {
        error = (surface != VK_NULL_HANDLE)
                    ? "no queue family supports both graphics and present"
                    : "no graphics queue family";
        return false;
    }
    return true;
}

bool DeviceVK::createLogicalDevice(std::string &error) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    /* Feature chain. Everything here is needed by the ported shaders:
       buffer device address for acceleration structure references, descriptor
       indexing for the 512-entry texture array, scalar block layout because
       the HLSL-derived SPIR-V uses tightly packed structures. */
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures = {};
    rtFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtFeatures.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = {};
    asFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.accelerationStructure = VK_TRUE;
    asFeatures.pNext = &rtFeatures;

    VkPhysicalDeviceVulkan13Features vk13 = {};
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13.dynamicRendering = VK_TRUE;
    vk13.synchronization2 = VK_TRUE;
    vk13.pNext = &asFeatures;

    VkPhysicalDeviceVulkan12Features vk12 = {};
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12.bufferDeviceAddress = VK_TRUE;
    vk12.descriptorIndexing = VK_TRUE;
    vk12.runtimeDescriptorArray = VK_TRUE;
    vk12.descriptorBindingPartiallyBound = VK_TRUE;
    vk12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vk12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vk12.scalarBlockLayout = VK_TRUE;
    vk12.pNext = &vk13;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vk12;
    features2.features.samplerAnisotropy = VK_TRUE;
    features2.features.shaderInt64 = VK_TRUE;
    features2.features.geometryShader = VK_TRUE;   /* im3d points and lines */

    std::vector<const char *> extensions;
    for (const char *name : kRequiredDeviceExtensions) {
        extensions.push_back(name);
    }
    if (windowHandle != nullptr) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    for (const char *name : extraDeviceExtensions) {
        extensions.push_back(name);
    }

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkResult res = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
    if (res != VK_SUCCESS) {
        error = "vkCreateDevice failed (" + std::to_string((int)res) + ")";
        return false;
    }

    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    return true;
}

bool DeviceVK::createAllocator(std::string &error) {
    VmaVulkanFunctions functions = {};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info = {};
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.instance = instance;
    info.vulkanApiVersion = apiVersion;
    info.pVulkanFunctions = &functions;
    /* Acceleration structures and the SBT are addressed by device address. */
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VkResult res = vmaCreateAllocator(&info, &allocator);
    if (res != VK_SUCCESS) {
        error = "vmaCreateAllocator failed (" + std::to_string((int)res) + ")";
        return false;
    }
    return true;
}

bool DeviceVK::initialize(void *window, std::string &error) {
    windowHandle = window;
    if (!gatherInstanceExtensions(error)) { return false; }
    if (!createInstance(error))           { return false; }

    /* The surface must exist before the physical device is chosen, so that
       present support can be part of the decision. */
    if (windowHandle != nullptr) {
        swapchain = new SwapchainVK();
        if (!swapchain->createSurface(instance, windowHandle, error)) {
            return false;
        }
    }

    if (!pickPhysicalDevice(error))   { return false; }
    if (!findQueueFamilies(error))    { return false; }
    if (!createLogicalDevice(error))  { return false; }
    if (!createAllocator(error))      { return false; }

    if (swapchain != nullptr) {
        if (!swapchain->create(physicalDevice, device, graphicsFamily,
                               1 /* vsync on by default */, error)) {
            return false;
        }
    }
    return true;
}

DeviceVK::~DeviceVK() {
    if (device != VK_NULL_HANDLE) { vkDeviceWaitIdle(device); }
    /* Swapchain owns the surface, so it must go before the instance. */
    delete swapchain;
    swapchain = nullptr;
    if (allocator != VK_NULL_HANDLE) { vmaDestroyAllocator(allocator); }
    if (device != VK_NULL_HANDLE)    { vkDestroyDevice(device, nullptr); }
    if (instance != VK_NULL_HANDLE)  { vkDestroyInstance(instance, nullptr); }
}

} /* namespace RT64 */
