/*
 * rt64_compose_vk — resolves the ray traced G-buffer into a presentable image.
 *
 * FullScreenVS + ComposePS, drawn with dynamic rendering straight into the
 * swapchain. This is the last step between a correct G-buffer and something
 * visible.
 */
#ifndef RT64_COMPOSE_VK_H
#define RT64_COMPOSE_VK_H

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace RT64 {

class DeviceVK;
class ViewVK;

class ComposePass {
public:
    ~ComposePass();

    bool create(DeviceVK *device, VkDescriptorSetLayout setLayout,
                VkFormat colorFormat, const std::string &shaderDir,
                std::string &error);

    /* Points the descriptor set at the view's current targets. Call again
       after a resize, since the image views are recreated. */
    bool bindTargets(ViewVK &view, std::string &error);

    /* Draws the fullscreen triangle into the given attachment. The G-buffer
       stays in GENERAL, which is a legal layout to sample from. */
    void record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);

    void destroy();

private:
    DeviceVK *device = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule pixelModule = VK_NULL_HANDLE;
};

} /* namespace RT64 */

#endif /* RT64_COMPOSE_VK_H */
