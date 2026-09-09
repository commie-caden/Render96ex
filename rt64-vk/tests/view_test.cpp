/*
 * view_test — render targets and the ray tracing descriptor set.
 *
 * Runs with validation enabled and asserts zero validation errors, because a
 * descriptor type or format mismatch is reported by the layers rather than by
 * a return code — vkUpdateDescriptorSets returns void.
 */
#include "rt64_descriptor_layout_vk.h"
#include "rt64_device_vk.h"
#include "rt64_scene_vk.h"
#include "rt64_view_vk.h"

#include <cstdio>
#include <string>

namespace {

/* A check that prints only a verdict cannot be diagnosed remotely, so name
   the formats rather than comparing opaque enum values silently. */
const char *formatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_UNDEFINED:             return "UNDEFINED";
        case VK_FORMAT_R32G32B32A32_SFLOAT:   return "R32G32B32A32_SFLOAT";
        case VK_FORMAT_R32G32_SFLOAT:         return "R32G32_SFLOAT";
        case VK_FORMAT_R32_SFLOAT:            return "R32_SFLOAT";
        case VK_FORMAT_R32_SINT:              return "R32_SINT";
        case VK_FORMAT_R32_UINT:              return "R32_UINT";
        case VK_FORMAT_R16G16B16A16_SNORM:    return "R16G16B16A16_SNORM";
        case VK_FORMAT_R16G16B16A16_SFLOAT:   return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
        case VK_FORMAT_R16_UINT:              return "R16_UINT";
        default:                              return "other";
    }
}

int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}
} /* namespace */

int main() {
    RT64::DeviceVK device;
    device.setValidationEnabled(true);
    std::string error;
    if (!device.initialize(nullptr, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  device: %s\n", device.getProperties().deviceName);

    RT64::DescriptorLayouts layouts;
    if (!layouts.create(device.getDevice(), error)) {
        std::fprintf(stderr, "  layout creation failed: %s\n", error.c_str());
        return 1;
    }
    const RT64::DescriptorSetLayoutInfo *rt = layouts.find("RayTracing");
    expect(rt != nullptr, "RayTracing layout found");
    if (rt == nullptr) return 1;

    RT64::AccelerationStructureBuilder builder;
    if (!builder.initialize(&device, error)) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    RT64::SceneVK scene(&device, &builder);
    RT64::ViewVK view(&device, &scene);

    device.resetValidationCounters();

    if (!view.resize(1280, 720, error)) {
        std::printf("   \033[31mFAIL\033[0m resize: %s\n", error.c_str());
        return 1;
    }
    std::printf("   \033[32m ok \033[0m %zu targets at 1280x720, %.0f MB\n",
                view.getTargets().size(), view.totalBytes() / 1048576.0);

    /* Spot-check that formats came from the shaders, not from assumptions. */
    struct Check { const char *name; VkFormat format; };
    const Check checks[] = {
        { "gViewDirection",  VK_FORMAT_R32G32B32A32_SFLOAT },
        { "gInstanceId",     VK_FORMAT_R32_SINT },
        { "gFlow",           VK_FORMAT_R32G32_SFLOAT },
        { "gDepth",          VK_FORMAT_R32_SFLOAT },
        { "gHitColor",       VK_FORMAT_R8G8B8A8_UNORM },
        { "gHitNormal",      VK_FORMAT_R16G16B16A16_SNORM },
        { "gHitInstanceId",  VK_FORMAT_R16_UINT },
    };
    for (const Check &c : checks) {
        const RT64::RenderTarget *t = view.findTarget(c.name);
        if (t == nullptr) {
            std::printf("   \033[31mFAIL\033[0m %-18s target not created\n",
                        c.name);
            failures++;
            continue;
        }
        const bool ok = (t->format == c.format);
        if (ok) {
            std::printf("   \033[32m ok \033[0m %-18s %s\n", c.name,
                        formatName(t->format));
        } else {
            std::printf("   \033[31mFAIL\033[0m %-18s got %s (%d), expected "
                        "%s (%d)\n", c.name, formatName(t->format),
                        (int)t->format, formatName(c.format), (int)c.format);
            failures++;
        }
    }

    /* Dump every target, so a disagreement between the shaders and the
       targets is visible rather than inferred. */
    std::printf("\n  all targets:\n");
    for (const RT64::RenderTarget &t : view.getTargets()) {
        std::printf("    %-24s binding %-3u %-20s %s\n", t.name.c_str(),
                    t.binding, formatName(t.format),
                    t.image != VK_NULL_HANDLE ? "image" : "texel buffer");
    }
    std::printf("\n");

    expect(view.updateDescriptorSet(rt->layout, error), "descriptor set written");
    expect(view.getDescriptorSet() != VK_NULL_HANDLE, "descriptor set allocated");

    /* Resizing must release and rebuild every target cleanly. */
    expect(view.resize(1920, 1080, error), "resized to 1920x1080");
    std::printf("        %.0f MB at 1080p\n", view.totalBytes() / 1048576.0);
    expect(view.updateDescriptorSet(rt->layout, error),
           "descriptor set rewritten after resize");

    expect(view.resize(0, 0, error) == false, "zero-size resize rejected");

    const uint32_t errs = device.getValidationErrorCount();
    const uint32_t warns = device.getValidationWarningCount();
    std::printf("   %s validation: %u errors, %u warnings\n",
                errs == 0 ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m",
                errs, warns);
    if (errs != 0) failures++;

    builder.shutdown();
    std::printf("  %s\n", failures == 0 ? "all view checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
