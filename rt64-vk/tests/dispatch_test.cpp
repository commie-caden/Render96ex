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

struct Vertex { float x, y, z; float pad[8]; };   /* 44-byte stride */

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
    const Vertex verts[3] = { {0,1,0,{0}}, {1,-1,0,{0}}, {-1,-1,0,{0}} };
    const unsigned int idx[3] = { 0, 1, 2 };
    expect(mesh.setMesh(verts, 3, (int)sizeof(Vertex), idx, 3, error),
           "mesh built a BLAS");

    RT64::InstanceVK instance(&scene);
    RT64_INSTANCE_DESC desc = {};
    desc.mesh = (RT64_MESH *)&mesh;
    for (int i = 0; i < 4; i++) desc.transform.m[i][i] = 1.0f;
    instance.setDescription(desc);
    expect(scene.updateTopLevel(error), "TLAS built from the scene");

    RT64_LIGHT light = {};
    light.diffuseColor = { 1.0f, 1.0f, 1.0f };
    light.attenuationRadius = 100.0f;
    light.groupBits = 0xFFFF;
    expect(scene.setLights(&light, 1, error), "scene lights uploaded");

    /* --- view --------------------------------------------------------- */
    RT64::ViewVK view(&device, &scene);
    expect(view.resize(640, 360, error), "view targets created at 640x360");
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
        RT64::ShaderVK::AddressingMode::Wrap, RT64_SHADER_RAYTRACE_ENABLED);
    expect(material.isValid(), "material shader generated");
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

    const uint32_t errs = device.getValidationErrorCount();
    std::printf("   %s validation: %u errors, %u warnings\n",
                errs == 0 ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m",
                errs, device.getValidationWarningCount());
    if (errs != 0) failures++;

    vkDestroyCommandPool(device.getDevice(), pool, nullptr);
    pipeline.destroy(&device);
    builder.shutdown();
    std::printf("  %s\n", failures == 0
                ? "PrimaryRayGen and all four secondary passes dispatched"
                : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
