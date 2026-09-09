/*
 * device_info — create a headless RT64 device and print what it selected.
 *
 * This exercises the real selection path (scoring, extension checks, feature
 * chain, VMA) rather than probing Vulkan independently, so it reports what
 * RT64 will actually use at runtime.
 */
#include "rt64_device_vk.h"

#include <cstdio>
#include <string>

int main() {
    RT64::DeviceVK device;
    std::string error;
    if (!device.initialize(nullptr, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        return 1;
    }

    const VkPhysicalDeviceProperties &p = device.getProperties();
    const auto &rt = device.getRayTracingProperties();
    const auto &as = device.getAccelerationStructureProperties();

    std::printf("  selected      %s\n", p.deviceName);
    std::printf("  device API    %u.%u.%u\n", VK_VERSION_MAJOR(p.apiVersion),
                VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    std::printf("  using API     %u.%u\n",
                VK_VERSION_MAJOR(device.getApiVersion()),
                VK_VERSION_MINOR(device.getApiVersion()));
    std::printf("  graphics queue family %u\n", device.getGraphicsFamily());
    std::printf("\n  SBT layout inputs (Phase 3):\n");
    std::printf("    shaderGroupHandleSize      %u\n", rt.shaderGroupHandleSize);
    std::printf("    shaderGroupBaseAlignment   %u\n", rt.shaderGroupBaseAlignment);
    std::printf("    shaderGroupHandleAlignment %u\n", rt.shaderGroupHandleAlignment);
    std::printf("    maxRayRecursionDepth       %u\n", rt.maxRayRecursionDepth);
    std::printf("    maxGeometryCount           %llu\n",
                (unsigned long long)as.maxGeometryCount);
    std::printf("\n  descriptor budget:\n");
    std::printf("    maxPerStageDescriptorSampledImages %u (need 512)\n",
                p.limits.maxPerStageDescriptorSampledImages);
    std::printf("\n  VMA allocator %s\n",
                device.getAllocator() ? "created" : "MISSING");
    return 0;
}
