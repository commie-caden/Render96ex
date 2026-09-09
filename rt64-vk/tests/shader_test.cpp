/*
 * shader_test — create every RT64 shader module on the real device.
 *
 * spirv-val proves a module is well-formed; this proves the driver accepts it,
 * which is a different question and the one that matters for Phase 2.
 */
#include "rt64_device_vk.h"
#include "rt64_shaders_vk.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    const std::string dir = (argc > 1) ? argv[1] : "shaders";

    RT64::DeviceVK device;
    std::string error;
    if (!device.initialize(nullptr, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  device: %s\n", device.getProperties().deviceName);

    RT64::ShaderLibraryVK library;
    if (!library.load(device.getDevice(), dir, error)) {
        std::fprintf(stderr, "  FAIL %s\n", error.c_str());
        return 1;
    }

    for (const RT64::ShaderInfo &info : RT64::allShaders()) {
        const char *kind = "?";
        switch (info.kind) {
            case RT64::ShaderKind::Vertex:        kind = "vertex";   break;
            case RT64::ShaderKind::Pixel:         kind = "pixel";    break;
            case RT64::ShaderKind::Geometry:      kind = "geometry"; break;
            case RT64::ShaderKind::Compute:       kind = "compute";  break;
            case RT64::ShaderKind::RayTracingLib: kind = "rt lib";   break;
        }
        std::printf("   ok  %-24s %s\n", info.name, kind);
    }
    std::printf("  %zu shader modules created and accepted by the driver\n",
                library.count());
    return 0;
}
