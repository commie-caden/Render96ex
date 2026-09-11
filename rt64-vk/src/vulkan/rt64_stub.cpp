/*
 * rt64_stub — the complete RT64 C ABI, failing cleanly.
 *
 * Phase 1 milestone. Building this proves the ported public header, the
 * dlopen loader path and the .so layout all line up with what the game
 * expects, before a single line of Vulkan exists. The real backend replaces
 * this file behind the identical ABI, so the game side never changes again.
 *
 * Every creator returns null and records a message retrievable through
 * RT64_GetLastError, which is exactly how the original signals failure.
 */
#include "rt64/rt64.h"
#include "rt64_device_vk.h"
#include "rt64_mesh_vk.h"
#include "rt64_texture_vk.h"
#include "rt64_scene_vk.h"
#include "rt64_shader_vk.h"
#include "rt64_shader_compiler_vk.h"
#include "rt64_swapchain_vk.h"
#include "rt64_paths_vk.h"
#include "rt64_view_vk.h"
#include "rt64_descriptor_layout_vk.h"

#include <vector>
#include <cstdio>
#include "rt64_raytracing_vk.h"

#include <memory>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#   define DLLEXPORT __declspec(dllexport)
#else
#   define DLLEXPORT __attribute__((visibility("default")))
#endif

namespace {
    /* The original keeps a global last-error string; match that behaviour. */
    std::string g_lastError;
}

extern "C" {

DLLEXPORT const char *RT64_GetLastError(void) {
    return g_lastError.c_str();
}

/* -------------------------------------------------------------- device */
namespace {
/* Owns the device and the per-device acceleration structure builder. The
   public API hands out an opaque RT64_DEVICE*, so this can grow without
   touching the ABI. */
struct DeviceContext;

struct DeviceContext {
    RT64::DeviceVK device;
    RT64::AccelerationStructureBuilder builder;
    RT64::ShaderCompilerVK shaderCompiler;
    RT64::DescriptorLayouts layouts;
    bool rayTracingReady = false;
    bool compilerReady = false;
    bool layoutsReady = false;

    /* Every material's hit groups live in one pipeline, so the device tracks
       them and views rebuild when the set changes. */
    std::vector<RT64::ShaderVK *> materials;
    /* Slot N here is gTextures[N] in the shaders. Destroyed textures leave a
       hole rather than shifting every later index. */
    std::vector<RT64::TextureVK *> textures;
    std::vector<RT64::ViewVK *> views;
    std::string shaderDir = "shaders";

    /* Per-frame synchronisation for RT64_DrawDevice. */
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;
    bool frameResourcesReady = false;
};

/* Set RT64_SHADER_DIR to point at the compiled SPIR-V if it is not in
   "shaders" beside the executable. */
static void initFrameResources(DeviceContext *ctx) {
    if (ctx->frameResourcesReady || ctx->device.getSwapchain() == nullptr) {
        return;
    }
    VkDevice vk = ctx->device.getDevice();
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx->device.getGraphicsFamily();
    vkCreateCommandPool(vk, &poolInfo, nullptr, &ctx->commandPool);

    VkCommandBufferAllocateInfo cbInfo = {};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = ctx->commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk, &cbInfo, &ctx->commandBuffer);

    VkSemaphoreCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateSemaphore(vk, &si, nullptr, &ctx->acquired);
    vkCreateFence(vk, &fi, nullptr, &ctx->inFlight);
    ctx->rendered.resize(ctx->device.getSwapchain()->getImageCount());
    for (VkSemaphore &s : ctx->rendered) {
        vkCreateSemaphore(vk, &si, nullptr, &s);
    }
    ctx->frameResourcesReady = true;
}
} /* namespace */

DLLEXPORT RT64_DEVICE *RT64_CreateDevice(void *hwnd) {
    DeviceContext *ctx = new DeviceContext();
    std::string error;
    if (!ctx->device.initialize(hwnd, error)) {
        g_lastError = "Failed to create RT64 device: " + error;
        delete ctx;
        return nullptr;
    }
    /* Ray tracing is optional at this stage: a device without it can still
       serve the raster paths, so failure here is recorded, not fatal. */
    if (ctx->builder.initialize(&ctx->device, error)) {
        ctx->rayTracingReady = true;
    } else {
        g_lastError = "Ray tracing unavailable: " + error;
    }
    /* A stale librt64.so next to the executable is indistinguishable from a
       fresh one at runtime, and has cost several debugging cycles. Say which
       build is actually loaded. */
    fprintf(stderr, "RT64: librt64.so built " __DATE__ " " __TIME__
                    " (executable dir: %s)\n",
            RT64::executableDirectory().c_str());

    /* Loaded lazily so a device that never creates a shader does not need
       libdxcompiler.so present. */
    if (ctx->shaderCompiler.initialize("", error)) {
        ctx->compilerReady = true;
        fprintf(stderr, "RT64: shader compiler: %s\n",
                ctx->shaderCompiler.getLibraryPath().c_str());
    } else {
        fprintf(stderr, "RT64: shader compiler unavailable: %s\n",
                error.c_str());
    }
    if (ctx->layouts.create(ctx->device.getDevice(), error)) {
        ctx->layoutsReady = true;
    } else {
        g_lastError = "Descriptor layouts unavailable: " + error;
    }
    /* Shaders live beside the executable unless told otherwise, for the same
       reason the libraries do. */
    if (const char *dir = std::getenv("RT64_SHADER_DIR")) {
        ctx->shaderDir = dir;
    } else {
        ctx->shaderDir = RT64::besideExecutable("shaders");
    }
    fprintf(stderr, "RT64: shader directory: %s\n", ctx->shaderDir.c_str());
    initFrameResources(ctx);
    return (RT64_DEVICE *)ctx;
}
DLLEXPORT void RT64_DestroyDevice(RT64_DEVICE *device) {
    DeviceContext *ctx = (DeviceContext *)device;
    if (ctx != nullptr) {
        ctx->builder.shutdown();
        delete ctx;
    }
}
DLLEXPORT void RT64_DrawDevice(RT64_DEVICE *device, int vsyncInterval,
                               float deltaTimeMs) {
    (void)deltaTimeMs;
    DeviceContext *ctx = (DeviceContext *)device;
    if (ctx == nullptr || !ctx->frameResourcesReady) {
        return;
    }
    RT64::SwapchainVK *swapchain = ctx->device.getSwapchain();
    if (swapchain == nullptr) {
        return;
    }
    VkDevice vk = ctx->device.getDevice();
    std::string error;

    const RT64::DescriptorSetLayoutInfo *rayLayout =
        ctx->layouts.find("RayTracing");
    const RT64::DescriptorSetLayoutInfo *composeLayout =
        ctx->layouts.find("Compose");
    if (rayLayout == nullptr || composeLayout == nullptr) {
        g_lastError = "descriptor layouts missing";
        return;
    }

    vkWaitForFences(vk, 1, &ctx->inFlight, VK_TRUE, UINT64_MAX);

    /* Scene state first: the TLAS is rebuilt whenever instances move, and
       rebuilding it invalidates any descriptor that names it. */
    for (RT64::ViewVK *view : ctx->views) {
        RT64::SceneVK *scene = view->getScene();
        if (scene == nullptr) { continue; }
        if (!scene->updateInstanceBuffers(error) ||
            !scene->updateTopLevel(error)) {
            g_lastError = "scene update failed: " + error;
            return;
        }
        /* Pipeline rebuilds touch objects that may still be in flight. */
        if (!view->ensurePipeline(ctx->materials, rayLayout->layout,
                                  composeLayout->layout,
                                  swapchain->getFormat(), ctx->shaderDir,
                                  ctx->builder.functions(), error)) {
            g_lastError = "pipeline build failed: " + error;
            return;
        }
        if (!view->updateDescriptorSet(rayLayout->layout, error) ||
            !view->getCompose().bindTargets(*view, error)) {
            g_lastError = "descriptor update failed: " + error;
            return;
        }

        /* Hit records are per instance and carry that instance's mesh
           addresses, so they are rebuilt alongside the TLAS. */
        std::vector<RT64::HitRecord> records;
        uint32_t index = 0;
        for (RT64::InstanceVK *instance : scene->getInstances()) {
            const RT64_INSTANCE_DESC &desc = instance->getDescription();
            if (desc.mesh == nullptr) { continue; }
            RT64::MeshVK *mesh = (RT64::MeshVK *)desc.mesh;
            if (mesh->accelerationStructure().handle == VK_NULL_HANDLE) {
                continue;
            }
            uint32_t material = 0;
            for (uint32_t m = 0; m < ctx->materials.size(); m++) {
                if ((RT64_SHADER *)ctx->materials[m] == desc.shader) {
                    material = m;
                    break;
                }
            }
            RT64::HitRecord surface;
            surface.groupIndex = view->getPipeline().getMaterialGroupIndex(material);
            surface.vertexAddress = mesh->getVertexAddress();
            surface.indexAddress = mesh->getIndexAddress();
            RT64::HitRecord shadow = surface;
            shadow.groupIndex = surface.groupIndex + 1;
            records.push_back(surface);
            records.push_back(shadow);
            index++;
        }
        /* A hit record whose mesh addresses are zero makes the hit group
           dereference address 0, which the GPU reports as
           "GPUVM fault detected at address 0x00000000" and then loses the
           context. Drop such records rather than dispatch them. */
        size_t dropped = 0;
        for (size_t i = 0; i < records.size(); ) {
            if (records[i].vertexAddress == 0 || records[i].indexAddress == 0) {
                records.erase(records.begin() + i);
                dropped++;
            } else {
                i++;
            }
        }
        if (dropped > 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                fprintf(stderr, "RT64: dropped %zu hit record(s) with null mesh "
                                "addresses\n", dropped);
            }
        }

        if (!records.empty() &&
            !view->getPipeline().buildShaderBindingTable(
                &ctx->device, ctx->builder.functions(), records, error)) {
            g_lastError = "shader binding table build failed: " + error;
            return;
        }
        view->setTraceable(!records.empty() &&
                           scene->getTopLevel().handle != VK_NULL_HANDLE);
    }

    uint32_t imageIndex = 0;
    VkResult res = vkAcquireNextImageKHR(vk, swapchain->getSwapchain(),
                                         UINT64_MAX, ctx->acquired,
                                         VK_NULL_HANDLE, &imageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        swapchain->recreate(ctx->device.getGraphicsFamily(), vsyncInterval,
                            error);
        return;
    }
    vkResetFences(vk, 1, &ctx->inFlight);

    VkCommandBuffer cmd = ctx->commandBuffer;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

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

    for (RT64::ViewVK *view : ctx->views) {
        if (!view->render(cmd, swapchain->getImageView(imageIndex),
                          swapchain->getExtent(), error)) {
            g_lastError = "view render failed: " + error;
        }
    }

    VkImageMemoryBarrier toPresent = toColor;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toPresent);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &ctx->acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &ctx->rendered[imageIndex];
    vkQueueSubmit(ctx->device.getGraphicsQueue(), 1, &submit, ctx->inFlight);

    VkSwapchainKHR chain = swapchain->getSwapchain();
    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &ctx->rendered[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &chain;
    present.pImageIndices = &imageIndex;
    vkQueuePresentKHR(ctx->device.getGraphicsQueue(), &present);
}

/* ------------------------------------------------------------ inspector */
DLLEXPORT RT64_INSPECTOR *RT64_CreateInspector(RT64_DEVICE *device) {
    (void)device;
    return nullptr;
}
DLLEXPORT bool RT64_HandleMessageInspector(RT64_INSPECTOR *inspector,
                                           unsigned int msg,
                                           unsigned long long wParam,
                                           long long lParam) {
    (void)inspector; (void)msg; (void)wParam; (void)lParam;
    return false;
}
DLLEXPORT void RT64_SetSceneInspector(RT64_INSPECTOR *inspector,
                                      RT64_SCENE_DESC *sceneDesc) {
    (void)inspector; (void)sceneDesc;
}
DLLEXPORT void RT64_SetMaterialInspector(RT64_INSPECTOR *inspector,
                                         RT64_MATERIAL *material,
                                         const char *materialName) {
    (void)inspector; (void)material; (void)materialName;
}
DLLEXPORT void RT64_SetLightsInspector(RT64_INSPECTOR *inspector,
                                       RT64_LIGHT *lights, int *lightCount,
                                       int maxLightCount) {
    (void)inspector; (void)lights; (void)lightCount; (void)maxLightCount;
}
DLLEXPORT void RT64_PrintClearInspector(RT64_INSPECTOR *inspector) {
    (void)inspector;
}
DLLEXPORT void RT64_PrintMessageInspector(RT64_INSPECTOR *inspector,
                                          const char *message) {
    (void)inspector; (void)message;
}
DLLEXPORT void RT64_DestroyInspector(RT64_INSPECTOR *inspector) {
    (void)inspector;
}

/* ------------------------------------------------------------- instance */
DLLEXPORT RT64_INSTANCE *RT64_CreateInstance(RT64_SCENE *scene) {
    RT64::SceneVK *s = (RT64::SceneVK *)scene;
    if (s == nullptr) {
        g_lastError = "RT64_CreateInstance called with a null scene";
        return nullptr;
    }
    return (RT64_INSTANCE *)new RT64::InstanceVK(s);
}
DLLEXPORT void RT64_SetInstanceDescription(RT64_INSTANCE *instance,
                                           RT64_INSTANCE_DESC desc) {
    RT64::InstanceVK *i = (RT64::InstanceVK *)instance;
    if (i == nullptr) {
        g_lastError = "RT64_SetInstanceDescription called with a null instance";
        return;
    }
    i->setDescription(desc);
}
DLLEXPORT void RT64_DestroyInstance(RT64_INSTANCE *instance) {
    delete (RT64::InstanceVK *)instance;
}

/* ----------------------------------------------------------------- mesh */
DLLEXPORT RT64_MESH *RT64_CreateMesh(RT64_DEVICE *device, int flags) {
    DeviceContext *ctx = (DeviceContext *)device;
    if (ctx == nullptr) {
        g_lastError = "RT64_CreateMesh called with a null device";
        return nullptr;
    }
    if ((flags & RT64_MESH_RAYTRACE_ENABLED) && !ctx->rayTracingReady) {
        g_lastError = "Mesh requests ray tracing but the device has none";
        return nullptr;
    }
    return (RT64_MESH *)new RT64::MeshVK(&ctx->device, &ctx->builder, flags);
}
DLLEXPORT void RT64_SetMesh(RT64_MESH *mesh, void *vertexArray,
                            int vertexCount, int vertexStride,
                            unsigned int *indexArray, int indexCount) {
    RT64::MeshVK *m = (RT64::MeshVK *)mesh;
    if (m == nullptr) {
        g_lastError = "RT64_SetMesh called with a null mesh";
        return;
    }
    std::string error;
    if (!m->setMesh(vertexArray, vertexCount, vertexStride, indexArray,
                    indexCount, error)) {
        g_lastError = "RT64_SetMesh failed: " + error;
    }
}
DLLEXPORT void RT64_DestroyMesh(RT64_MESH *mesh) {
    delete (RT64::MeshVK *)mesh;
}

/* ---------------------------------------------------------------- scene */
DLLEXPORT RT64_SCENE *RT64_CreateScene(RT64_DEVICE *device) {
    DeviceContext *ctx = (DeviceContext *)device;
    if (ctx == nullptr) {
        g_lastError = "RT64_CreateScene called with a null device";
        return nullptr;
    }
    RT64::SceneVK *scene = new RT64::SceneVK(&ctx->device, &ctx->builder);
    scene->setOwnerHandle(ctx);
    return (RT64_SCENE *)scene;
}
DLLEXPORT void RT64_SetSceneDescription(RT64_SCENE *scene,
                                        RT64_SCENE_DESC sceneDesc) {
    RT64::SceneVK *s = (RT64::SceneVK *)scene;
    if (s == nullptr) {
        g_lastError = "RT64_SetSceneDescription called with a null scene";
        return;
    }
    s->setDescription(sceneDesc);

    /* The scene stored it, but the shaders read GlobalParams, which the view
       owns — so it has to be pushed through. Ambient and the camera-attached
       eye light live here, and without them a scene is lit only by its
       explicit lights. */
    DeviceContext *ctx = (DeviceContext *)s->getOwnerHandle();
    if (ctx != nullptr) {
        for (RT64::ViewVK *view : ctx->views) {
            if (view->getScene() == s) {
                view->setSceneDescription(sceneDesc);
            }
        }
    }
}
DLLEXPORT void RT64_SetSceneLights(RT64_SCENE *scene, RT64_LIGHT *lightArray,
                                   int lightCount) {
    RT64::SceneVK *s = (RT64::SceneVK *)scene;
    if (s == nullptr) {
        g_lastError = "RT64_SetSceneLights called with a null scene";
        return;
    }
    std::string error;
    if (!s->setLights(lightArray, lightCount, error)) {
        g_lastError = "RT64_SetSceneLights failed: " + error;
    }
}
DLLEXPORT void RT64_DestroyScene(RT64_SCENE *scene) {
    delete (RT64::SceneVK *)scene;
}

/* --------------------------------------------------------------- shader */
DLLEXPORT RT64_SHADER *RT64_CreateShader(RT64_DEVICE *device,
                                         unsigned int shaderId, int filter,
                                         int hAddr, int vAddr,
                                         bool normalMapEnabled,
                                         bool specularMapEnabled) {
    DeviceContext *ctx = (DeviceContext *)device;
    if (ctx == nullptr) {
        g_lastError = "RT64_CreateShader called with a null device";
        return nullptr;
    }
    if (!ctx->compilerReady) {
        g_lastError = "RT64_CreateShader needs libdxcompiler.so, which was not "
                      "found. Set RT64_DXC_LIB or place it beside the "
                      "executable.";
        return nullptr;
    }

    int flags = RT64_SHADER_RASTER_ENABLED | RT64_SHADER_RAYTRACE_ENABLED;
    if (normalMapEnabled)   { flags |= RT64_SHADER_NORMAL_MAP_ENABLED; }
    if (specularMapEnabled) { flags |= RT64_SHADER_SPECULAR_MAP_ENABLED; }

    RT64::ShaderVK *shader = new RT64::ShaderVK(
        &ctx->shaderCompiler, shaderId,
        RT64::convertFilter((unsigned int)filter),
        RT64::convertAddressingMode((unsigned int)hAddr),
        RT64::convertAddressingMode((unsigned int)vAddr), flags);
    if (!shader->isValid()) {
        g_lastError = "RT64_CreateShader failed for shader " +
                      std::to_string(shaderId) + ": " + shader->getLastError();
        delete shader;
        return nullptr;
    }
    /* A new material means new hit groups, so every view's pipeline is stale. */
    ctx->materials.push_back(shader);
    for (RT64::ViewVK *view : ctx->views) {
        view->invalidatePipeline();
    }
    return (RT64_SHADER *)shader;
}
DLLEXPORT void RT64_DestroyShader(RT64_SHADER *shader) {
    delete (RT64::ShaderVK *)shader;
}

/* -------------------------------------------------------------- texture */
DLLEXPORT RT64_TEXTURE *RT64_CreateTexture(RT64_DEVICE *device,
                                           RT64_TEXTURE_DESC desc) {
    DeviceContext *ctx = (DeviceContext *)device;
    if (ctx == nullptr) {
        g_lastError = "RT64_CreateTexture called with a null device";
        return nullptr;
    }

    RT64::TextureVK *texture = new RT64::TextureVK(&ctx->device);
    texture->arrayIndex = -1;
    std::string error;
    bool ok = false;
    switch (desc.format) {
        case RT64_TEXTURE_FORMAT_RGBA8:
            ok = texture->setRGBA8(desc.bytes, desc.byteCount, desc.width,
                                   desc.height, desc.rowPitch, true, error);
            break;
        case RT64_TEXTURE_FORMAT_DDS:
            /* Render96 ships .dds assets, so this needs a real BC-format
               parser rather than a guess. Not yet implemented. */
            error = "RT64_TEXTURE_FORMAT_DDS is not implemented in the Vulkan "
                    "port yet";
            break;
        default:
            error = "unknown texture format " + std::to_string(desc.format);
            break;
    }

    if (!ok) {
        g_lastError = "RT64_CreateTexture failed: " + error;
        delete texture;
        return nullptr;
    }
    /* Claim a slot, reusing one freed by a destroyed texture if there is
       one, so long sessions do not exhaust the 512 the shaders declare. */
    texture->arrayIndex = -1;
    for (size_t i = 0; i < ctx->textures.size(); i++) {
        if (ctx->textures[i] == nullptr) {
            ctx->textures[i] = texture;
            texture->arrayIndex = (int)i;
            break;
        }
    }
    if (texture->arrayIndex < 0) {
        if (ctx->textures.size() >= 512) {
            g_lastError = "RT64_CreateTexture: all 512 texture slots are in use";
            delete texture;
            return nullptr;
        }
        texture->arrayIndex = (int)ctx->textures.size();
        ctx->textures.push_back(texture);
    }
    /* No dirty flag needed: RT64_DrawDevice rewrites the descriptor set every
       frame, so a texture created now is visible on the next one. */
    return (RT64_TEXTURE *)texture;
}
DLLEXPORT void RT64_DestroyTexture(RT64_TEXTURE *texture) {
    /* The slot is freed but not compacted: other materials hold indices. */
    delete (RT64::TextureVK *)texture;
}

/* ----------------------------------------------------------------- view */
DLLEXPORT RT64_VIEW *RT64_CreateView(RT64_SCENE *scene) {
    RT64::SceneVK *s = (RT64::SceneVK *)scene;
    if (s == nullptr) {
        g_lastError = "RT64_CreateView called with a null scene";
        return nullptr;
    }
    DeviceContext *ctx = (DeviceContext *)s->getOwnerHandle();
    if (ctx == nullptr) {
        g_lastError = "scene has no device";
        return nullptr;
    }

    RT64::ViewVK *view = new RT64::ViewVK(&ctx->device, s);
    view->setRayFunctions(&ctx->builder.functions());

    RT64::SwapchainVK *swapchain = ctx->device.getSwapchain();
    std::string error;
    const VkExtent2D extent = (swapchain != nullptr)
                                  ? swapchain->getExtent()
                                  : VkExtent2D{ 640, 480 };
    if (!view->resize(extent.width, extent.height, error)) {
        g_lastError = "RT64_CreateView failed: " + error;
        delete view;
        return nullptr;
    }
    view->setTextureArray(&ctx->textures);
    ctx->views.push_back(view);
    return (RT64_VIEW *)view;
}
DLLEXPORT void RT64_SetViewPerspective(RT64_VIEW *view, RT64_MATRIX4 viewMatrix,
                                       float fovRadians, float nearDist,
                                       float farDist, bool canReproject) {
    RT64::ViewVK *v = (RT64::ViewVK *)view;
    if (v == nullptr) {
        g_lastError = "RT64_SetViewPerspective called with a null view";
        return;
    }
    (void)canReproject;
    v->setCamera(&viewMatrix.m[0][0], nullptr, fovRadians, nearDist, farDist);
}
DLLEXPORT void RT64_SetViewDescription(RT64_VIEW *view, RT64_VIEW_DESC desc) {
    RT64::ViewVK *v = (RT64::ViewVK *)view;
    if (v != nullptr) {
        v->setDescription(desc);
    }
}
DLLEXPORT void RT64_SetViewSkyPlane(RT64_VIEW *view, RT64_TEXTURE *texture) {
    (void)view; (void)texture;
}
DLLEXPORT RT64_INSTANCE *RT64_GetViewRaytracedInstanceAt(RT64_VIEW *view,
                                                         int x, int y) {
    (void)view; (void)x; (void)y;
    return nullptr;
}
DLLEXPORT bool RT64_GetViewUpscalerSupport(RT64_VIEW *view, int upscaler) {
    /* Upscalers were dropped from this port: native resolution only. */
    (void)view; (void)upscaler;
    return false;
}
DLLEXPORT void RT64_DestroyView(RT64_VIEW *view) {
    delete (RT64::ViewVK *)view;
}

} /* extern "C" */
