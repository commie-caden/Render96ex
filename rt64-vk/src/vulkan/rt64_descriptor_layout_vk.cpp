#include "rt64_descriptor_layout_vk.h"
#include "rt64_shader_bindings.h"

namespace RT64 {

bool DescriptorLayouts::create(VkDevice dev, std::string &error) {
    device = dev;

    for (uint32_t g = 0; g < kShaderBindingGroupCount; g++) {
        const ShaderBindingGroup &group = kShaderBindingGroups[g];

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags> flags;
        DescriptorSetLayoutInfo info;
        info.name = group.name;

        for (uint32_t i = 0; i < group.count; i++) {
            const ShaderBinding &b = group.bindings[i];
            VkDescriptorSetLayoutBinding out = {};
            out.binding = b.binding;
            out.descriptorType = b.type;
            out.descriptorCount = b.count;
            out.stageFlags = b.stages;
            bindings.push_back(out);

            info.descriptorTotal += b.count;
            if (b.count > 1) {
                info.hasLargeArray = true;
            }

            /* gTextures is a fixed 512-element array that is only ever
               partially populated — a scene rarely has 512 textures loaded.
               PARTIALLY_BOUND says the unwritten entries are legal as long as
               the shader does not read them, which is what descriptor
               indexing is for. Without it, every slot would need a valid
               descriptor before the set could be used. */
            VkDescriptorBindingFlags f = 0;
            if (b.count > 1) {
                f |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            }
            flags.push_back(f);
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {};
        flagsInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = (uint32_t)flags.size();
        flagsInfo.pBindingFlags = flags.data();

        VkDescriptorSetLayoutCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        createInfo.bindingCount = (uint32_t)bindings.size();
        createInfo.pBindings = bindings.data();
        if (info.hasLargeArray) {
            createInfo.pNext = &flagsInfo;
        }

        VkResult res = vkCreateDescriptorSetLayout(device, &createInfo, nullptr,
                                                   &info.layout);
        if (res != VK_SUCCESS) {
            error = std::string("vkCreateDescriptorSetLayout failed for ") +
                    group.name + " (" + std::to_string((int)res) + ")";
            return false;
        }
        info.bindingCount = (uint32_t)bindings.size();
        layouts.push_back(info);
    }
    return true;
}

const DescriptorSetLayoutInfo *DescriptorLayouts::find(
    const std::string &name) const {
    for (const DescriptorSetLayoutInfo &info : layouts) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

DescriptorLayouts::~DescriptorLayouts() {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    for (DescriptorSetLayoutInfo &info : layouts) {
        if (info.layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, info.layout, nullptr);
        }
    }
}

} /* namespace RT64 */
