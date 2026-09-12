/*
 * rt64_device_vk — Vulkan replacement for the D3D12 Device object graph.
 */
#ifndef RT64_DEVICE_VK_H
#define RT64_DEVICE_VK_H

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

/* Target the newest API the *build* headers know about. The runtime clamps
   this to whatever the loader actually offers, so building against a 1.3 SDK
   and running on a 1.4 loader is fine. Hardcoding 1.4 would simply fail to
   compile on older SDKs. */
#ifndef RT64_VULKAN_API_VERSION
#   if defined(VK_API_VERSION_1_4)
#       define RT64_VULKAN_API_VERSION VK_API_VERSION_1_4
#   elif defined(VK_API_VERSION_1_3)
#       define RT64_VULKAN_API_VERSION VK_API_VERSION_1_3
#   else
#       define RT64_VULKAN_API_VERSION VK_API_VERSION_1_2
#   endif
#endif

namespace RT64 {

class SwapchainVK;

class DeviceVK {
public:
    ~DeviceVK();

    /* window may be null for a headless device — useful for tests and for
       bringing the backend up before the swapchain exists. */
    bool initialize(void *window, std::string &error);

    VkInstance         getInstance()       const { return instance; }
    VkPhysicalDevice   getPhysicalDevice() const { return physicalDevice; }

    /* BC support is universal on desktop but not guaranteed by the spec, so
       the DDS path asks before assuming. */
    bool supportsSampledFormat(VkFormat format) const;
    VkDevice           getDevice()         const { return device; }
    VkQueue            getGraphicsQueue()  const { return graphicsQueue; }
    uint32_t           getGraphicsFamily() const { return graphicsFamily; }
    VmaAllocator       getAllocator()      const { return allocator; }
    SwapchainVK       *getSwapchain()      const { return swapchain; }
    uint32_t           getApiVersion()     const { return apiVersion; }

    const VkPhysicalDeviceProperties &getProperties() const {
        return deviceProperties;
    }
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &
    getRayTracingProperties() const { return rtProperties; }
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR &
    getAccelerationStructureProperties() const { return asProperties; }

    void setValidationEnabled(bool enabled) { validationEnabled = enabled; }
    /* Validation messages are counted rather than merely printed, so tests can
       assert on them instead of relying on someone reading stderr. */
    uint32_t getValidationErrorCount() const { return validationErrors; }
    uint32_t getValidationWarningCount() const { return validationWarnings; }
    void resetValidationCounters() { validationErrors = 0; validationWarnings = 0; }
    void addInstanceExtension(const char *name) {
        instanceExtensions.push_back(name);
    }
    void addDeviceExtension(const char *name) {
        extraDeviceExtensions.push_back(name);
    }

private:
    bool gatherInstanceExtensions(std::string &error);
    bool createInstance(std::string &error);
    bool pickPhysicalDevice(std::string &error);
    bool findQueueFamilies(std::string &error);
    bool createLogicalDevice(std::string &error);
    bool createAllocator(std::string &error);
    int  scoreDevice(VkPhysicalDevice candidate, std::string &reason) const;

    VkInstance       instance       = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice         device         = VK_NULL_HANDLE;
    VkQueue          graphicsQueue  = VK_NULL_HANDLE;
    VmaAllocator     allocator      = VK_NULL_HANDLE;

    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t apiVersion     = 0;
    void    *windowHandle   = nullptr;
    bool     validationEnabled = false;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    mutable uint32_t validationErrors = 0;
    mutable uint32_t validationWarnings = 0;

public:
    /* Called from the debug callback; public only for that reason. */
    void recordValidationMessage(bool isError) const {
        if (isError) { validationErrors++; } else { validationWarnings++; }
    }

private:
    bool createDebugMessenger(std::string &error);

    VkPhysicalDeviceProperties deviceProperties = {};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties = {};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties = {};

    SwapchainVK *swapchain = nullptr;

    std::vector<const char *> instanceExtensions;
    std::vector<const char *> extraDeviceExtensions;
    /* Backing storage for extension names handed to us by SDL. */
    std::vector<std::string> ownedExtensionNames;
};

} /* namespace RT64 */

#endif /* RT64_DEVICE_VK_H */
