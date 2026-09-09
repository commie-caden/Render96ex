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

    bool create(VkDevice device, std::string &error);
    const DescriptorSetLayoutInfo *find(const std::string &name) const;
    const std::vector<DescriptorSetLayoutInfo> &all() const { return layouts; }

private:
    VkDevice device = VK_NULL_HANDLE;
    std::vector<DescriptorSetLayoutInfo> layouts;
};

} /* namespace RT64 */

#endif /* RT64_DESCRIPTOR_LAYOUT_VK_H */
