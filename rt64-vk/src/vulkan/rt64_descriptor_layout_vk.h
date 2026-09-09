/*
 * rt64_descriptor_layout_vk — descriptor set layouts, one per pass.
 *
 * Built from the generated rt64_shader_bindings.h, which is reflected out of
 * the compiled SPIR-V. Nothing here transcribes binding numbers by hand.
 */
#ifndef RT64_DESCRIPTOR_LAYOUT_VK_H
#define RT64_DESCRIPTOR_LAYOUT_VK_H

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace RT64 {

struct DescriptorSetLayoutInfo {
    std::string name;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    uint32_t bindingCount = 0;
    uint32_t descriptorTotal = 0;   /* counts array elements, not bindings */
    bool hasLargeArray = false;     /* a binding with count > 1 */
};

class DescriptorLayouts {
public:
    ~DescriptorLayouts();

    /* Bindings 301..318 correspond to the generator's per-combination
       samplers; 300 is the tracer's own sampler and is written normally. */
    static const uint32_t kFirstMaterialSampler = 301;
    static const uint32_t kLastMaterialSampler = 318;

    bool create(VkDevice device, std::string &error);
    const DescriptorSetLayoutInfo *find(const std::string &name) const;
    const std::vector<DescriptorSetLayoutInfo> &all() const { return layouts; }

private:
    bool createImmutableSamplers(std::string &error);

    VkDevice device = VK_NULL_HANDLE;
    std::vector<DescriptorSetLayoutInfo> layouts;
    /* Indexed by register number, 1..18. */
    std::vector<VkSampler> materialSamplers;
};

} /* namespace RT64 */

#endif /* RT64_DESCRIPTOR_LAYOUT_VK_H */
