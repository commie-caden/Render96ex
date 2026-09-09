/*
 * pipeline_test — assemble the real ray tracing pipeline.
 *
 * Five raygen shaders, two miss shaders from inside PrimaryRayGen, and hit
 * groups generated per material by ShaderVK, built into one
 * VkRayTracingPipelineKHR with a shader binding table sized from the device's
 * own alignment properties.
 *
 * Runs with validation enabled and asserts zero errors.
 */
#include "rt64_descriptor_layout_vk.h"
#include "rt64_device_vk.h"
#include "rt64_rt_pipeline_vk.h"
#include "rt64_shader_compiler_vk.h"
#include "rt64_shader_vk.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

bool loadSpirv(const std::string &path, std::vector<uint32_t> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    if (size <= 0 || (size % 4) != 0) return false;
    out.resize((size_t)size / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(out.data()), size);
    return true;
}

} /* namespace */

int main(int argc, char **argv) {
    const std::string shaderDir = (argc > 1) ? argv[1] : "shaders";
    const std::string dxcLib = (argc > 2) ? argv[2] : "";

    RT64::DeviceVK device;
    device.setValidationEnabled(true);
    std::string error;
    if (!device.initialize(nullptr, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  device: %s\n", device.getProperties().deviceName);

    RT64::AccelerationStructureBuilder builder;
    if (!builder.initialize(&device, error)) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    RT64::DescriptorLayouts layouts;
    if (!layouts.create(device.getDevice(), error)) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    const RT64::DescriptorSetLayoutInfo *rt = layouts.find("RayTracing");
    if (rt == nullptr) { std::fprintf(stderr, "  no RayTracing layout\n"); return 1; }

    device.resetValidationCounters();

    RT64::RayTracingPipeline pipeline;

    struct PassFile { RT64::RayPass pass; const char *file; const char *entry; };
    const PassFile passes[] = {
        { RT64::RayPass::Primary,    "PrimaryRayGen",    "PrimaryRayGen" },
        { RT64::RayPass::Direct,     "DirectRayGen",     "DirectRayGen" },
        { RT64::RayPass::Indirect,   "IndirectRayGen",   "IndirectRayGen" },
        { RT64::RayPass::Reflection, "ReflectionRayGen", "ReflectionRayGen" },
        { RT64::RayPass::Refraction, "RefractionRayGen", "RefractionRayGen" },
    };

    std::vector<uint32_t> primarySpirv;
    bool allLoaded = true;
    for (const PassFile &p : passes) {
        std::vector<uint32_t> spirv;
        if (!loadSpirv(shaderDir + "/" + p.file + ".spv", spirv)) {
            std::printf("   \033[31mFAIL\033[0m cannot load %s.spv\n", p.file);
            allLoaded = false;
            continue;
        }
        if (p.pass == RT64::RayPass::Primary) { primarySpirv = spirv; }
        if (!pipeline.setRaygen(p.pass, spirv, p.entry, error)) {
            std::printf("   \033[31mFAIL\033[0m %s: %s\n", p.file, error.c_str());
            allLoaded = false;
        }
    }
    expect(allLoaded, "five raygen shaders supplied");

    /* Both miss shaders live inside PrimaryRayGen. */
    expect(pipeline.setMissShaders(primarySpirv, "SurfaceMiss", "ShadowMiss",
                                   error), "miss shaders supplied");

    /* Generate a few materials, as a scene would. */
    RT64::ShaderCompilerVK compiler;
    if (!compiler.initialize(dxcLib, error)) {
        std::fprintf(stderr, "  compiler init failed: %s\n", error.c_str());
        return 1;
    }
    const unsigned int materialIds[] = { 0x01200200, 0x00000045, 0x0000038d };
    const int flags = RT64_SHADER_RAYTRACE_ENABLED;
    std::vector<RT64::ShaderVK *> shaders;
    for (unsigned int id : materialIds) {
        RT64::ShaderVK *s = new RT64::ShaderVK(&compiler, id,
            RT64::ShaderVK::Filter::Linear,
            RT64::ShaderVK::AddressingMode::Wrap,
            RT64::ShaderVK::AddressingMode::Wrap, flags);
        if (!s->isValid()) {
            std::printf("   \033[31mFAIL\033[0m material %08x: %s\n", id,
                        s->getLastError().substr(0, 80).c_str());
            failures++;
            delete s;
            continue;
        }
        uint32_t first = 0;
        if (!pipeline.addMaterial(*s, first, error)) {
            std::printf("   \033[31mFAIL\033[0m addMaterial: %s\n", error.c_str());
            failures++;
        } else {
            std::printf("        material %08x -> hit groups %u and %u\n", id,
                        first, first + 1);
        }
        shaders.push_back(s);
    }
    expect(pipeline.getMaterialCount() == 3, "three materials added");

    const bool built = pipeline.build(&device, builder.functions(), rt->layout,
                                      error);
    if (!built) {
        std::printf("   \033[31mFAIL\033[0m pipeline build: %s\n", error.c_str());
        failures++;
    } else {
        std::printf("   \033[32m ok \033[0m pipeline built: %u groups "
                    "(5 raygen + 2 miss + %u hit)\n", pipeline.getGroupCount(),
                    pipeline.getMaterialCount() * 2);
        const RT64::ShaderBindingTable &sbt = pipeline.getSBT();
        std::printf("        SBT stride %llu, raygen records %u\n",
                    (unsigned long long)sbt.raygenRegion().stride,
                    sbt.getRaygenCount());

        /* Each pass must address a distinct raygen record. */
        auto a = sbt.raygenRegion(0);
        auto b = sbt.raygenRegion(4);
        expect(b.deviceAddress - a.deviceAddress == 4 * a.stride,
               "each pass addresses its own raygen record");
        expect(a.size == a.stride,
               "raygen region size equals its stride, as the spec requires");
        expect(sbt.missRegion().size == 2 * sbt.missRegion().stride,
               "miss region holds both miss shaders");
        expect(sbt.hitRegion().size == 6 * sbt.hitRegion().stride,
               "hit region holds two groups per material");
    }

    const uint32_t errs = device.getValidationErrorCount();
    std::printf("   %s validation: %u errors, %u warnings\n",
                errs == 0 ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m",
                errs, device.getValidationWarningCount());
    if (errs != 0) failures++;

    pipeline.destroy(&device);
    for (RT64::ShaderVK *s : shaders) delete s;
    builder.shutdown();
    std::printf("  %s\n", failures == 0 ? "all pipeline checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
