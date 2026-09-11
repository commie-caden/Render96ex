/*
 * rt64_view_vk — the ray tracing render targets and their descriptor set.
 *
 * Every target is created from the generated binding table rather than a
 * hand-written list, so the G-buffer cannot drift from what the shaders
 * declare. That includes each storage image's format, which Vulkan requires to
 * match the shader's declaration exactly.
 */
#ifndef RT64_VIEW_VK_H
#define RT64_VIEW_VK_H

#include <cstdint>
#include <string>
#include <vector>

#include "rt64_compose_vk.h"
#include "rt64_raytracing_vk.h"
#include "rt64_rt_pipeline_vk.h"

#include "rt64/rt64.h"
#include "rt64_global_params.h"

namespace RT64 {

class DeviceVK;
class SceneVK;
class RayTracingPipeline;

/* Matches MAX_HIT_QUERIES in GlobalHitBuffers.hlsli, plus the extra slot the
   original allocated (MaxQueries = 16 + 1 in rt64_view.cpp). */
static const uint32_t RT64_MAX_HIT_QUERIES = 16;
static const uint32_t RT64_HIT_LAYERS = RT64_MAX_HIT_QUERIES + 1;

struct RenderTarget {
    std::string name;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkFormat format = VK_FORMAT_UNDEFINED;

    /* Storage images. */
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;

    /* Storage texel buffers. */
    BufferVK buffer;
    VkBufferView bufferView = VK_NULL_HANDLE;
};

class ViewVK {
public:
    ViewVK(DeviceVK *device, SceneVK *scene);
    ~ViewVK();

    /* Allocates every target at this resolution. Safe to call again to
       resize; existing targets are released first. */
    bool resize(uint32_t width, uint32_t height, std::string &error);

    /* Allocates the descriptor set and writes every binding. Call after
       resize and whenever the scene's acceleration structure is rebuilt. */
    bool updateDescriptorSet(VkDescriptorSetLayout layout, std::string &error);

    /* The device owns the texture list; the view writes it into gTextures. */
    void setTextureArray(const std::vector<class TextureVK *> *textures) {
        textureArray = textures;
    }

    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    const std::vector<RenderTarget> &getTargets() const { return targets; }
    VkDescriptorSet getDescriptorSet() const { return descriptorSet; }
    const RenderTarget *findTarget(const std::string &name) const;
    SceneVK *getScene() const { return scene; }

    /* Total bytes across every target, for reporting. */
    uint64_t totalBytes() const;

    /* Per-frame rendering, driven by RT64_DrawDevice. Rebuilds whatever the
       scene invalidated, traces the five passes, and composes into the given
       swapchain image. */
    void setRayFunctions(const RayTracingFunctions *fn) { rayFunctions = fn; }
    bool render(VkCommandBuffer cmd, VkImageView swapchainView,
                VkExtent2D swapchainExtent, std::string &error);

    /* Applies the game's view settings — sample counts, light budget, motion
       blur — to the persistent parameter block. */
    void setDescription(const RT64_VIEW_DESC &desc);

    /* The scene description carries ambient light, the camera-attached eye
       light and the sky contribution. Without it a scene is lit only by its
       explicit lights, which is why everything looked far darker than the
       rasterised reference. */
    void setSceneDescription(const RT64_SCENE_DESC &desc);

    /* Tracing with no acceleration structure or an empty hit region reads
       unwritten descriptors, which on RADV shows up as a GPUVM fault rather
       than a clean error. */
    void setTraceable(bool value) { traceable = value; }
    RayTracingPipeline &getPipeline() { return pipeline; }
    ComposePass &getCompose() { return compose; }

    /* The pipeline embeds every material's hit groups, so it is rebuilt when
       the material set changes rather than every frame. */
    void invalidatePipeline() { pipelineDirty = true; }
    bool ensurePipeline(const std::vector<class ShaderVK *> &materials,
                        VkDescriptorSetLayout rayLayout,
                        VkDescriptorSetLayout composeLayout,
                        VkFormat colorFormat, const std::string &shaderDir,
                        const RayTracingFunctions &fn, std::string &error);

    /* Fills gParams. The camera basis is what PrimaryRayGen builds rays from,
       so leaving it zeroed produces degenerate rays that hit nothing — which
       looks exactly like a broken acceleration structure. */
    void setCamera(const float viewMatrix[16], const float projectionMatrix[16],
                   float fovRadians, float nearDist, float farDist);

    /* Copies a storage target back to host memory. For tests and debugging:
       it stalls the queue. */
    bool readTarget(const std::string &name, std::vector<uint8_t> &out,
                    std::string &error);

    /* One layer of a gHit* texel buffer. These record every intersection the
       anyhit sees, before any alpha test, so they distinguish "no rays hit"
       from "rays hit but were discarded as transparent" — which the image
       targets cannot. */
    bool readHitLayer(const std::string &name, uint32_t layer,
                      std::vector<uint8_t> &out, std::string &error);

    /* Puts every storage image into GENERAL, which is the layout ray tracing
       shaders read and write them in. Only needed after a resize. */
    void transitionTargets(VkCommandBuffer cmd);

    /* Records the five ray passes. Each reads what the previous wrote, so a
       barrier separates them. */
    void dispatchRayPasses(VkCommandBuffer cmd, const RayTracingPipeline &pipeline,
                           const RayTracingFunctions &fn);

private:
    bool createStorageImage(RenderTarget &target, std::string &error);
    bool createTexelBuffer(RenderTarget &target, std::string &error);
    bool createPlaceholders(std::string &error);
    void releaseTargets();

    DeviceVK *device = nullptr;
    SceneVK *scene = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    std::vector<RenderTarget> targets;

    /* Resources the shaders require but which are supplied elsewhere; a 1x1
       stand-in keeps the descriptor set complete until they exist. */
    VkImage placeholderImage = VK_NULL_HANDLE;
    VmaAllocation placeholderAlloc = VK_NULL_HANDLE;
    VkImageView placeholderView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    BufferVK paramsBuffer;
    BufferVK instanceMaterials;
    BufferVK emptyStorage;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    RayTracingPipeline pipeline;
    ComposePass compose;
    RT64_VIEW_DESC description = {};
    /* Persistent: setCamera and setDescription each own a subset of the fields
       and must not clobber the other's. Rebuilding the whole struct in
       setCamera reset maxLights to 1 every frame, which starved the lighting
       no matter what the game asked for. */
    GlobalParams params = {};
    bool pipelineDirty = true;
    const RayTracingFunctions *rayFunctions = nullptr;
    bool targetsTransitioned = false;
    bool traceable = false;
    const std::vector<class TextureVK *> *textureArray = nullptr;
    uint32_t builtMaterialCount = 0;
};

} /* namespace RT64 */

#endif /* RT64_VIEW_VK_H */
