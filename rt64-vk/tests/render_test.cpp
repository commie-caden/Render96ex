/*
 * render_test — the whole thing, on screen.
 *
 * Scene, acceleration structures, five ray passes, then ComposePS resolving
 * the G-buffer into the swapchain. This is what RT64 rendering looks like end
 * to end on Vulkan.
 */
#include "rt64_descriptor_layout_vk.h"
#include "rt64_device_vk.h"
#include "rt64_mesh_vk.h"
#include "rt64_rt_pipeline_vk.h"
#include "rt64_scene_vk.h"
#include "rt64_shader_compiler_vk.h"
#include "rt64_shader_vk.h"
#include "rt64_view_vk.h"

#include "rt64_compose_vk.h"
#include "rt64_swapchain_vk.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

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

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "RT64 Vulkan — ray traced and composed", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    RT64::DeviceVK device;
    device.setValidationEnabled(true);
    std::string error;
    if (!device.initialize(window, error)) {
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
    RT64::SwapchainVK *swapchain = device.getSwapchain();
    const VkExtent2D swapExtent = swapchain->getExtent();
    std::printf("  swapchain: %ux%u\n", swapExtent.width, swapExtent.height);

    RT64::ViewVK view(&device, &scene);
    expect(view.resize(swapExtent.width, swapExtent.height, error),
           "view targets sized to the swapchain");
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

    /* --- compose ------------------------------------------------------ */
    const RT64::DescriptorSetLayoutInfo *composeLayout = layouts.find("Compose");
    if (composeLayout == nullptr) {
        std::fprintf(stderr, "  no Compose layout\n");
        return 1;
    }
    RT64::ComposePass compose;
    expect(compose.create(&device, composeLayout->layout, swapchain->getFormat(),
                          shaderDir, error), "compose pipeline created");
    expect(compose.bindTargets(view, error), "compose bound to the G-buffer");

    /* --- render loop --------------------------------------------------- */
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
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

    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateSemaphore(device.getDevice(), &si, nullptr, &acquired);
    vkCreateFence(device.getDevice(), &fi, nullptr, &inFlight);
    std::vector<VkSemaphore> rendered(swapchain->getImageCount());
    for (VkSemaphore &s : rendered) {
        vkCreateSemaphore(device.getDevice(), &si, nullptr, &s);
    }

    std::printf("\n  Rendering. Escape or close the window to exit.\n");
    std::printf("  Expect a lit triangle, shaded by the ray passes and\n");
    std::printf("  resolved by ComposePS.\n\n");

    bool running = true;
    int presented = 0;
    bool targetsReady = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        if (!running) break;

        vkWaitForFences(device.getDevice(), 1, &inFlight, VK_TRUE, UINT64_MAX);
        uint32_t imageIndex = 0;
        VkResult res = vkAcquireNextImageKHR(device.getDevice(),
                                             swapchain->getSwapchain(),
                                             UINT64_MAX, acquired,
                                             VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) break;
        vkResetFences(device.getDevice(), 1, &inFlight);

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        /* The targets only need the UNDEFINED -> GENERAL transition once;
           repeating it every frame would discard the previous contents. */
        if (!targetsReady) {
            view.transitionTargets(cmd);
            targetsReady = true;
        }
        view.dispatchRayPasses(cmd, pipeline, builder.functions());

        /* The ray passes write the G-buffer as storage images; compose samples
           them. Same GENERAL layout, but the writes must be visible first. */
        VkMemoryBarrier toSample = {};
        toSample.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toSample.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             1, &toSample, 0, nullptr, 0, nullptr);

        VkImageMemoryBarrier toColor = {};
        toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = swapchain->getImage(imageIndex);
        toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toColor);

        compose.record(cmd, swapchain->getImageView(imageIndex), swapExtent);

        VkImageMemoryBarrier toPresent = toColor;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toPresent);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[imageIndex];
        vkQueueSubmit(device.getGraphicsQueue(), 1, &submit, inFlight);

        VkSwapchainKHR chain = swapchain->getSwapchain();
        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &chain;
        present.pImageIndices = &imageIndex;
        vkQueuePresentKHR(device.getGraphicsQueue(), &present);
        presented++;
    }

    vkDeviceWaitIdle(device.getDevice());
    std::printf("  presented %d frames, %u validation errors\n", presented,
                device.getValidationErrorCount());
    if (device.getValidationErrorCount() != 0) { failures++; }

    for (VkSemaphore s : rendered) vkDestroySemaphore(device.getDevice(), s, nullptr);
    vkDestroySemaphore(device.getDevice(), acquired, nullptr);
    vkDestroyFence(device.getDevice(), inFlight, nullptr);
    vkDestroyCommandPool(device.getDevice(), pool, nullptr);
    compose.destroy();
    pipeline.destroy(&device);
    builder.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("  %s\n", failures == 0 ? "rendered without validation errors"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
