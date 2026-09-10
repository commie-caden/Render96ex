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

#include "rt64_raytracing_vk.h"

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

    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    const std::vector<RenderTarget> &getTargets() const { return targets; }
    VkDescriptorSet getDescriptorSet() const { return descriptorSet; }
    const RenderTarget *findTarget(const std::string &name) const;

    /* Total bytes across every target, for reporting. */
    uint64_t totalBytes() const;

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
};

} /* namespace RT64 */

#endif /* RT64_VIEW_VK_H */
