#include "rt64_descriptor_layout_vk.h"
#include "rt64_shader_bindings.h"

namespace RT64 {

namespace {

VkSamplerAddressMode addressModeFor(uint32_t mode) {
    /* Matches RT64::ShaderVK::AddressingMode. */
    switch (mode) {
        case 0:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;           /* Wrap */
        case 1:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;  /* Mirror */
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;    /* Clamp */
    }
}

} /* namespace */

bool DescriptorLayouts::createImmutableSamplers(std::string &error) {
    /* The generator picks a sampler register with
           1 + filter * 9 + hAddr * 3 + vAddr
       (uniqueSamplerRegisterIndex), so register N encodes one filter and two
       addressing modes. D3D12 declared these as static samplers in the root
       signature; the Vulkan equivalent is an immutable sampler baked into the
       layout, which needs no descriptor write and cannot be bound wrongly.

       Inverting the formula here keeps the two definitions in step — if the
       generator's formula changes, this must change with it. */
    materialSamplers.assign(19, VK_NULL_HANDLE);   /* 1-based, 1..18 */
    for (uint32_t reg = 1; reg <= 18; reg++) {
        const uint32_t encoded = reg - 1;
        const uint32_t filter = encoded / 9;
        const uint32_t hAddr = (encoded % 9) / 3;
        const uint32_t vAddr = encoded % 3;

        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        /* Filter: 0 = Point, 1 = Linear. */
        info.magFilter = filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.minFilter = info.magFilter;
        info.mipmapMode = filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = addressModeFor(hAddr);
        info.addressModeV = addressModeFor(vAddr);
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &info, nullptr,
                            &materialSamplers[reg]) != VK_SUCCESS) {
            error = "vkCreateSampler failed for material sampler register " +
                    std::to_string(reg);
            return false;
        }
    }
    return true;
}

bool DescriptorLayouts::create(VkDevice dev, std::string &error) {
    device = dev;
    if (!createImmutableSamplers(error)) {
        return false;
    }

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
            /* Per-material samplers are immutable, so no descriptor write is
               needed and the wrong sampler cannot be bound by accident. */
            if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER &&
                b.binding >= kFirstMaterialSampler &&
                b.binding <= kLastMaterialSampler) {
                out.pImmutableSamplers =
                    &materialSamplers[b.binding - kFirstMaterialSampler + 1];
            }
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
    for (VkSampler s : materialSamplers) {
        if (s != VK_NULL_HANDLE) {
            vkDestroySampler(device, s, nullptr);
        }
    }
}

} /* namespace RT64 */
