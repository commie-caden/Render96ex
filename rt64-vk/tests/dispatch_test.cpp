/*
 * dispatch_test — the five ray passes, actually dispatched.
 *
 * Everything the port has built comes together here: a scene with real
 * geometry, its acceleration structures, the view's G-buffer, the descriptor
 * set, the pipeline assembled from RT64's raygen and generated hit groups, and
 * five vkCmdTraceRaysKHR calls with barriers between them.
 *
 * Headless and with validation on. The GPU work is submitted and waited on, so
 * a device loss or a synchronisation error surfaces here rather than later.
 */
#include "rt64_descriptor_layout_vk.h"
#include "rt64_device_vk.h"
#include "rt64_mesh_vk.h"
#include "rt64_rt_pipeline_vk.h"
#include "rt64_scene_vk.h"
#include "rt64_shader_compiler_vk.h"
#include "rt64_shader_vk.h"
#include "rt64_view_vk.h"

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

/* 44 bytes, the stride combiner 0x01200200 asks for. Laid out by the generator
   rather than guessed: attribute offsets come from the colour combiner. */
struct Vertex { float f[11]; };

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
    RT64::DescriptorLayouts layouts;
    if (!builder.initialize(&device, error) ||
        !layouts.create(device.getDevice(), error)) {
        std::fprintf(stderr, "  setup failed: %s\n", error.c_str());
        return 1;
    }
    const RT64::DescriptorSetLayoutInfo *rt = layouts.find("RayTracing");
    if (rt == nullptr) return 1;

    device.resetValidationCounters();

    /* --- scene with real geometry ------------------------------------- */
    RT64::SceneVK scene(&device, &builder);
    RT64::MeshVK mesh(&device, &builder, RT64_MESH_RAYTRACE_ENABLED);
    const unsigned int idx[3] = { 0, 1, 2 };
    const float positions[3][3] = { {0,1,0}, {1,-1,0}, {-1,-1,0} };
    Vertex verts[3] = {};
    /* Filled in below, once the material has told us where its attributes
       live — an all-zero vertex has zero shade, and the combiner derives alpha
       from shade, so the geometry would be invisible however well it traces. */

    RT64::InstanceVK instance(&scene);
    RT64_INSTANCE_DESC desc = {};
    desc.mesh = (RT64_MESH *)&mesh;
    for (int i = 0; i < 4; i++) desc.transform.m[i][i] = 1.0f;
    desc.previousTransform = desc.transform;
    /* A zeroed material is not neutral. PrimaryRayGen skips any recorded hit
       whose alpha contribution is below EPSILON, so an all-zero material makes
       a perfectly good intersection invisible — resInstanceId stays -1 and the
       result is indistinguishable from missing the geometry entirely. */
    desc.material.solidAlphaMultiplier = 1.0f;
    desc.material.shadowAlphaMultiplier = 1.0f;
    desc.material.diffuseColorMix = { 1.0f, 1.0f, 1.0f, 1.0f };
    desc.material.selfLight = { 1.0f, 1.0f, 1.0f };
    desc.material.specularColor = { 1.0f, 1.0f, 1.0f };
    desc.material.specularExponent = 1.0f;
    /* Non-zero, and this is load-bearing. PrimaryRayGen only marks a hit as
       the primary one when storeHit becomes true, and the paths that set it
       are lighting, reflection and transparency:

           bool usesLighting  = (lightGroupMaskBits > 0);
           bool applyLighting = usesLighting && (hitColor.a > MINIMUM_ALPHA);

       With lightGroupMaskBits at 0 and no reflection, a perfectly good opaque
       hit is recorded into the hit buffers and then never stored, leaving
       resInstanceId at -1. */
    desc.material.lightGroupMaskBits = 0xFFFF;
    desc.material.diffuseTexIndex = -1;
    desc.material.normalTexIndex = -1;
    desc.material.specularTexIndex = -1;
    instance.setDescription(desc);
    expect(scene.updateTopLevel(error), "TLAS built from the scene");

    RT64_LIGHT light = {};
    light.diffuseColor = { 1.0f, 1.0f, 1.0f };
    light.attenuationRadius = 100.0f;
    light.groupBits = 0xFFFF;
    expect(scene.setLights(&light, 1, error), "scene lights uploaded");
    expect(scene.updateInstanceBuffers(error), "instance transforms and materials packed");
    std::printf("        %u instance(s) packed\n", scene.getPackedInstanceCount());

    /* --- view --------------------------------------------------------- */
    RT64::ViewVK view(&device, &scene);
    expect(view.resize(640, 360, error), "view targets created at 640x360");
    /* A right-handed view matrix for a camera at (0,0,3) looking down -Z at
       the triangle in the z=0 plane. The view matrix translates the world by
       the negated camera position. Projection is left null so setCamera builds
       XMMatrixPerspectiveFovRH itself, matching the original. */
    float viewMatrix[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,-3,1 };
    view.setCamera(viewMatrix, nullptr, 1.047f, 0.1f, 100.0f);
    expect(view.updateDescriptorSet(rt->layout, error), "descriptor set written");

    /* --- pipeline ------------------------------------------------------ */
    RT64::RayTracingPipeline pipeline;
    struct PassFile { RT64::RayPass pass; const char *file; };
    const PassFile passes[] = {
        { RT64::RayPass::Primary,    "PrimaryRayGen" },
        { RT64::RayPass::Direct,     "DirectRayGen" },
        { RT64::RayPass::Indirect,   "IndirectRayGen" },
        { RT64::RayPass::Reflection, "ReflectionRayGen" },
        { RT64::RayPass::Refraction, "RefractionRayGen" },
    };
    std::vector<uint32_t> primarySpirv;
    for (const PassFile &p : passes) {
        std::vector<uint32_t> spirv;
        if (!loadSpirv(shaderDir + "/" + std::string(p.file) + ".spv", spirv)) {
            std::printf("   \033[31mFAIL\033[0m cannot load %s.spv\n", p.file);
            return 1;
        }
        if (p.pass == RT64::RayPass::Primary) primarySpirv = spirv;
        pipeline.setRaygen(p.pass, spirv, p.file, error);
    }
    pipeline.setMissShaders(primarySpirv, "SurfaceMiss", "ShadowMiss", error);

    RT64::ShaderCompilerVK compiler;
    if (!compiler.initialize(dxcLib, error)) {
        std::fprintf(stderr, "  compiler init failed: %s\n", error.c_str());
        return 1;
    }
    RT64::ShaderVK material(&compiler, 0x01200200,
        RT64::ShaderVK::Filter::Linear,
        RT64::ShaderVK::AddressingMode::Wrap,
        RT64::ShaderVK::AddressingMode::Wrap,
        /* Both groups, matching what RT64_CreateShader requests. The attribute
           offsets come from the raster path, and the hit groups read the same
           vertex data, so a raytrace-only shader would leave them unavailable. */
        RT64_SHADER_RASTER_ENABLED | RT64_SHADER_RAYTRACE_ENABLED);
    expect(material.isValid(), "material shader generated");

    /* Lay out the vertices using the offsets the generator computed. */
    {
        const RT64::ShaderVK::RasterGroup &raster = material.getRasterGroup();
        std::printf("        vertex stride %u, %zu attributes\n",
                    raster.vertexStride, raster.attributes.size());
        for (const auto &attr : raster.attributes) {
            const char *kind = "?";
            switch (attr.attribute) {
                case RT64::ShaderVK::VertexAttribute::Position: kind = "position"; break;
                case RT64::ShaderVK::VertexAttribute::Normal:   kind = "normal";   break;
                case RT64::ShaderVK::VertexAttribute::TexCoord: kind = "texcoord"; break;
                case RT64::ShaderVK::VertexAttribute::Color:    kind = "color";    break;
            }
            std::printf("          %-9s offset %2u, %u component(s)\n",
                        kind, attr.offset, attr.componentCount);
        }
        for (int v = 0; v < 3; v++) {
            for (const auto &attr : raster.attributes) {
                const uint32_t base = attr.offset / 4;
                switch (attr.attribute) {
                    case RT64::ShaderVK::VertexAttribute::Position:
                        for (uint32_t c = 0; c < 3; c++) {
                            verts[v].f[base + c] = positions[v][c];
                        }
                        break;
                    case RT64::ShaderVK::VertexAttribute::Normal:
                        verts[v].f[base + 2] = 1.0f;   /* facing +Z */
                        break;
                    case RT64::ShaderVK::VertexAttribute::Color:
                        /* Opaque white shade: this is what gives the hit a
                           non-zero alpha for the raygen to keep. */
                        for (uint32_t c = 0; c < attr.componentCount; c++) {
                            verts[v].f[base + c] = 1.0f;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        std::printf("        vertex 0 floats:");
        for (int i = 0; i < 11; i++) { std::printf(" %.0f", verts[0].f[i]); }
        std::printf("\n");
        expect(mesh.setMesh(verts, 3, (int)raster.vertexStride, idx, 3, error),
               "mesh built a BLAS from combiner-laid-out vertices");
        expect(scene.updateTopLevel(error), "TLAS rebuilt");
        expect(scene.updateInstanceBuffers(error), "instance buffers repacked");
        std::printf("        %u instance(s) packed after the rebuild\n",
                    scene.getPackedInstanceCount());
        /* updateTopLevel destroys and recreates the acceleration structure, so
           the descriptor written earlier now names a freed handle. Rewriting
           is safe here because nothing has been submitted yet. */
        expect(view.updateDescriptorSet(rt->layout, error),
               "descriptor set rewritten for the new TLAS");
    }
    uint32_t firstGroup = 0;
    expect(pipeline.addMaterial(material, firstGroup, error), "material added");
    expect(pipeline.build(&device, builder.functions(), rt->layout, error),
           "ray tracing pipeline built");

    /* One instance, so two hit records, carrying its mesh's addresses. */
    std::vector<RT64::HitRecord> records(2);
    records[0].groupIndex = pipeline.getMaterialGroupIndex(0);
    records[0].vertexAddress = mesh.getVertexAddress();
    records[0].indexAddress = mesh.getIndexAddress();
    records[1] = records[0];
    records[1].groupIndex += 1;
    expect(pipeline.buildShaderBindingTable(&device, builder.functions(),
                                            records, error),
           "shader binding table carries the mesh addresses");

    /* --- dispatch ------------------------------------------------------ */
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = device.getGraphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device.getDevice(), &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo cbInfo = {};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.getDevice(), &cbInfo, &cmd);

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    view.transitionTargets(cmd);
    view.dispatchRayPasses(cmd, pipeline, builder.functions());
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const VkResult submitted = vkQueueSubmit(device.getGraphicsQueue(), 1,
                                             &submit, VK_NULL_HANDLE);
    expect(submitted == VK_SUCCESS, "five ray passes submitted");
    const VkResult waited = vkQueueWaitIdle(device.getGraphicsQueue());
    expect(waited == VK_SUCCESS, "GPU completed the work without device loss");

    /* The point of the whole exercise: did any ray actually hit the triangle?
       gInstanceId is R32_SINT and PrimaryRayGen writes the instance index on a
       hit. A dispatch that runs cleanly but hits nothing looks identical to a
       working one from the outside, so this is the check that distinguishes
       them. */
    {
        /* Report each stage separately. A single hit/miss number cannot
           distinguish "the camera is wrong" from "rays hit but the material
           made them invisible", and both look like zero. */
        auto countNonZeroFloat4 = [&](const char *name, size_t &nonZero,
                                      size_t &total) -> bool {
            std::vector<uint8_t> pixels;
            if (!view.readTarget(name, pixels, error)) { return false; }
            const float *f = reinterpret_cast<const float *>(pixels.data());
            total = pixels.size() / (sizeof(float) * 4);
            nonZero = 0;
            for (size_t i = 0; i < total; i++) {
                if (f[i * 4] != 0.0f || f[i * 4 + 1] != 0.0f ||
                    f[i * 4 + 2] != 0.0f) {
                    nonZero++;
                }
            }
            return true;
        };

        size_t n = 0, total = 0;
        if (countNonZeroFloat4("gViewDirection", n, total)) {
            std::printf("        gViewDirection:   %6zu / %zu non-zero%s\n",
                        n, total, n > 0 ? "  (raygen ran, camera valid)" : "");
            expect(n > 0, "ray generation produced valid ray directions");
        }
        if (countNonZeroFloat4("gShadingPosition", n, total)) {
            std::printf("        gShadingPosition: %6zu / %zu non-zero%s\n",
                        n, total, n > 0 ? "  (hits were shaded)" : "");
        }
        if (countNonZeroFloat4("gDiffuse", n, total)) {
            std::printf("        gDiffuse:         %6zu / %zu non-zero\n",
                        n, total);
        }

        /* The hit buffers record every intersection the anyhit sees, before
           any alpha test. This is what separates "no rays hit" from "rays hit
           but the material discarded them" — the image targets cannot. */
        std::vector<uint8_t> hitLayer;
        if (view.readHitLayer("gHitDistAndFlow", 0, hitLayer, error)) {
            const float *d = reinterpret_cast<const float *>(hitLayer.data());
            const size_t count = hitLayer.size() / (sizeof(float) * 4);
            size_t intersections = 0;
            for (size_t i = 0; i < count; i++) {
                if (d[i * 4] > 0.0f) { intersections++; }
            }
            std::printf("        gHitDistAndFlow:  %6zu / %zu intersections"
                        "  (anyhit, pre-alpha)\n", intersections, count);
            expect(intersections > 0, "rays intersected the triangle");
        } else {
            std::printf("        hit buffer readback: %s\n", error.c_str());
        }

        /* gHitColor is what the anyhit stored and what the raygen alpha-tests
           against. Non-zero alpha here means the material and vertex path are
           correct and the loss is between the anyhit and the raygen — most
           likely payload.nhits. Zero alpha means the combiner produced a
           transparent result despite the inputs looking right. */
        size_t opaqueHits = 0;
        std::vector<uint8_t> hitColor;
        if (view.readHitLayer("gHitColor", 0, hitColor, error)) {
            const uint8_t *c = hitColor.data();
            const size_t count = hitColor.size() / 4;
            size_t opaque = 0, anyColor = 0;
            for (size_t i = 0; i < count; i++) {
                if (c[i * 4 + 3] > 0) { opaque++; }
                if (c[i * 4] || c[i * 4 + 1] || c[i * 4 + 2]) { anyColor++; }
            }
            std::printf("        gHitColor:        %6zu / %zu with alpha>0, "
                        "%zu with colour\n", opaque, count, anyColor);
            opaqueHits = opaque;
            if (opaque == 0) {
                std::printf("          -> anyhit produced zero alpha; the "
                            "combiner or material path is at fault\n");
            }
        }

        std::vector<uint8_t> pixels;
        if (view.readTarget("gInstanceId", pixels, error)) {
            const int32_t *ids = reinterpret_cast<const int32_t *>(pixels.data());
            const size_t count = pixels.size() / sizeof(int32_t);
            size_t hits = 0;
            for (size_t i = 0; i < count; i++) {
                if (ids[i] >= 0) { hits++; }
            }
            std::printf("        gInstanceId:      %6zu / %zu hit (%.1f%%)\n",
                        hits, count, 100.0 * (double)hits / (double)count);
            expect(hits > 0, "primary rays hit the triangle");
        } else {
            std::printf("   \033[31mFAIL\033[0m readback: %s\n", error.c_str());
            failures++;
        }
    }

    const uint32_t errs = device.getValidationErrorCount();
    std::printf("   %s validation: %u errors, %u warnings\n",
                errs == 0 ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m",
                errs, device.getValidationWarningCount());
    if (errs != 0) failures++;

    vkDestroyCommandPool(device.getDevice(), pool, nullptr);
    pipeline.destroy(&device);
    builder.shutdown();
    if (failures == 0) {
        std::printf("  RT64 renders: rays traced, hits shaded, G-buffer "
                    "written\n");
    } else {
        std::printf("  SOME CHECKS FAILED\n");
    }
    return failures == 0 ? 0 : 1;
}
