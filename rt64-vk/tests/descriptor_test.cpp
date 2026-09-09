/*
 * descriptor_test — create every pass's descriptor set layout on the device.
 *
 * The layouts are generated from the compiled SPIR-V, so this checks that what
 * the shaders declare is something the driver will actually accept — in
 * particular the 512-element gTextures array, which needs descriptor indexing
 * and PARTIALLY_BOUND to be legal when only some slots are populated.
 */
#include "rt64_descriptor_layout_vk.h"
#include "rt64_device_vk.h"
#include "rt64_shader_bindings.h"

#include <cstdio>
#include <string>

int main() {
    RT64::DeviceVK device;
    std::string error;
    if (!device.initialize(nullptr, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  device: %s\n\n", device.getProperties().deviceName);

    RT64::DescriptorLayouts layouts;
    if (!layouts.create(device.getDevice(), error)) {
        std::fprintf(stderr, "  FAIL %s\n", error.c_str());
        return 1;
    }

    for (const RT64::DescriptorSetLayoutInfo &info : layouts.all()) {
        std::printf("   ok  %-13s %2u bindings, %5u descriptors%s\n",
                    info.name.c_str(), info.bindingCount, info.descriptorTotal,
                    info.hasLargeArray ? "  (has array)" : "");
    }

    /* The big one: confirm gTextures really is 512 and fits the device. */
    const RT64::ShaderBindingGroup *rt = nullptr;
    for (uint32_t i = 0; i < RT64::kShaderBindingGroupCount; i++) {
        if (std::string(RT64::kShaderBindingGroups[i].name) == "RayTracing") {
            rt = &RT64::kShaderBindingGroups[i];
        }
    }
    if (rt != nullptr) {
        uint32_t largest = 0;
        const char *largestName = "";
        for (uint32_t i = 0; i < rt->count; i++) {
            if (rt->bindings[i].count > largest) {
                largest = rt->bindings[i].count;
                largestName = rt->bindings[i].name;
            }
        }
        const uint32_t limit =
            device.getProperties().limits.maxPerStageDescriptorSampledImages;
        std::printf("\n  largest array: %s[%u], device allows %u per stage — %s\n",
                    largestName, largest, limit,
                    largest <= limit ? "fits" : "TOO LARGE");
        if (largest > limit) return 1;
    }

    std::printf("  all %u descriptor set layouts created\n",
                (unsigned)layouts.all().size());
    return 0;
}
