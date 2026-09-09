#include "rt64_view_vk.h"
#include "rt64_device_vk.h"
#include "rt64_global_params.h"
#include "rt64_scene_vk.h"
#include "rt64_shader_bindings.h"

#include <cstring>

namespace RT64 {

namespace {

/* Bytes per element for the texel buffer formats RT64 uses. */
uint32_t formatBytes(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:       return 4;
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:            return 2;
        default:                            return 4;
    }
}

const ShaderBindingGroup *rayTracingGroup() {
    for (uint32_t i = 0; i < kShaderBindingGroupCount; i++) {
        if (std::strcmp(kShaderBindingGroups[i].name, "RayTracing") == 0) {
            return &kShaderBindingGroups[i];
        }
    }
    return nullptr;
}

} /* namespace */

ViewVK::ViewVK(DeviceVK *dev, SceneVK *sc) : device(dev), scene(sc) {}

ViewVK::~ViewVK() {
    VkDevice vk = device->getDevice();
    VmaAllocator allocator = device->getAllocator();
    releaseTargets();
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk, descriptorPool, nullptr);
    }
    if (placeholderView != VK_NULL_HANDLE) {
        vkDestroyImageView(vk, placeholderView, nullptr);
    }
    if (placeholderImage != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, placeholderImage, placeholderAlloc);
    }
    if (sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk, sampler, nullptr);
    }
    paramsBuffer.destroy(allocator);
    instanceMaterials.destroy(allocator);
    emptyStorage.destroy(allocator);
}

void ViewVK::releaseTargets() {
    VkDevice vk = device->getDevice();
    VmaAllocator allocator = device->getAllocator();
    for (RenderTarget &t : targets) {
        if (t.imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(vk, t.imageView, nullptr);
        }
        if (t.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, t.image, t.allocation);
        }
        if (t.bufferView != VK_NULL_HANDLE) {
            vkDestroyBufferView(vk, t.bufferView, nullptr);
        }
        t.buffer.destroy(allocator);
    }
    targets.clear();
}

bool ViewVK::createStorageImage(RenderTarget &target, std::string &error) {
    /* A storage image must carry exactly the format its shader declares, which
       is why the format comes from reflection rather than a guess. */
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = target.format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkFormatProperties props = {};
    vkGetPhysicalDeviceFormatProperties(device->getPhysicalDevice(),
                                        target.format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        error = target.name + ": format does not support storage images";
        return false;
    }

    VmaAllocationCreateInfo alloc = {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VkResult res = vmaCreateImage(device->getAllocator(), &info, &alloc,
                                  &target.image, &target.allocation, nullptr);
    if (res != VK_SUCCESS) {
        error = target.name + ": vmaCreateImage failed (" +
                std::to_string((int)res) + ")";
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = target.format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    res = vkCreateImageView(device->getDevice(), &viewInfo, nullptr,
                            &target.imageView);
    if (res != VK_SUCCESS) {
        error = target.name + ": vkCreateImageView failed";
        return false;
    }
    return true;
}

bool ViewVK::createTexelBuffer(RenderTarget &target, std::string &error) {
    /* One element per pixel per hit layer, exactly as the original sized
       these: rtWidth * rtHeight * MaxQueries. */
    const VkDeviceSize elements =
        (VkDeviceSize)width * height * RT64_HIT_LAYERS;
    const VkDeviceSize bytes = elements * formatBytes(target.format);

    VkFormatProperties props = {};
    vkGetPhysicalDeviceFormatProperties(device->getPhysicalDevice(),
                                        target.format, &props);
    if (!(props.bufferFeatures &
          VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT)) {
        error = target.name + ": format does not support storage texel buffers";
        return false;
    }

    if (!createBuffer(device->getAllocator(), device->getDevice(), bytes,
                      VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      false, target.buffer, error)) {
        return false;
    }

    VkBufferViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    viewInfo.buffer = target.buffer.buffer;
    viewInfo.format = target.format;
    viewInfo.range = VK_WHOLE_SIZE;
    VkResult res = vkCreateBufferView(device->getDevice(), &viewInfo, nullptr,
                                      &target.bufferView);
    if (res != VK_SUCCESS) {
        error = target.name + ": vkCreateBufferView failed (" +
                std::to_string((int)res) + ")";
        return false;
    }
    return true;
}

bool ViewVK::createPlaceholders(std::string &error) {
    if (sampler != VK_NULL_HANDLE) {
        return true;                     /* already built */
    }
    VkDevice vk = device->getDevice();

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(vk, &sampInfo, nullptr, &sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = { 1, 1, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc = {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device->getAllocator(), &imgInfo, &alloc,
                       &placeholderImage, &placeholderAlloc, nullptr) != VK_SUCCESS) {
        error = "placeholder image creation failed";
        return false;
    }
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = placeholderImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(vk, &viewInfo, nullptr, &placeholderView) != VK_SUCCESS) {
        error = "placeholder view creation failed";
        return false;
    }

    if (!createBuffer(device->getAllocator(), vk, sizeof(GlobalParams),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, paramsBuffer,
                      error)) {
        return false;
    }
    GlobalParams params = {};
    params.resolution[0] = (float)width;
    params.resolution[1] = (float)height;
    std::memcpy(paramsBuffer.mapped, &params, sizeof(params));

    /* instanceMaterials is filled per frame from the scene's instances; a
       modest initial allocation avoids a null descriptor. */
    if (!createBuffer(device->getAllocator(), vk, 1024,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      true, instanceMaterials, error)) {
        return false;
    }
    /* Stand-in for a scene with no lights: a storage buffer binding cannot be
       left null. */
    if (!createBuffer(device->getAllocator(), vk, 1024,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      true, emptyStorage, error)) {
        return false;
    }
    return true;
}

bool ViewVK::resize(uint32_t w, uint32_t h, std::string &error) {
    if (w == 0 || h == 0) {
        error = "view resize to zero";
        return false;
    }
    releaseTargets();
    width = w;
    height = h;

    const ShaderBindingGroup *group = rayTracingGroup();
    if (group == nullptr) {
        error = "no RayTracing binding group in the generated table";
        return false;
    }

    for (uint32_t i = 0; i < group->count; i++) {
        const ShaderBinding &b = group->bindings[i];
        if (b.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            b.type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            continue;   /* supplied by the scene or elsewhere */
        }
        RenderTarget target;
        target.name = b.name;
        target.binding = b.binding;
        target.type = b.type;
        target.format = b.format;
        if (target.format == VK_FORMAT_UNDEFINED) {
            error = target.name + ": shader declares no storage format";
            return false;
        }

        const bool ok = (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                            ? createStorageImage(target, error)
                            : createTexelBuffer(target, error);
        if (!ok) {
            targets.push_back(target);   /* so the destructor frees it */
            return false;
        }
        targets.push_back(target);
    }

    return createPlaceholders(error);
}

const RenderTarget *ViewVK::findTarget(const std::string &name) const {
    for (const RenderTarget &t : targets) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

uint64_t ViewVK::totalBytes() const {
    uint64_t total = 0;
    for (const RenderTarget &t : targets) {
        if (t.image != VK_NULL_HANDLE) {
            total += (uint64_t)width * height * formatBytes(t.format);
        } else {
            total += (uint64_t)width * height * RT64_HIT_LAYERS *
                     formatBytes(t.format);
        }
    }
    return total;
}

bool ViewVK::updateDescriptorSet(VkDescriptorSetLayout layout,
                                 std::string &error) {
    const ShaderBindingGroup *group = rayTracingGroup();
    if (group == nullptr) {
        error = "no RayTracing binding group";
        return false;
    }
    VkDevice vk = device->getDevice();

    if (descriptorPool == VK_NULL_HANDLE) {
        /* Pool sizes come from the same table the layout does, so they cannot
           disagree with it. */
        std::vector<VkDescriptorPoolSize> sizes;
        for (uint32_t i = 0; i < group->count; i++) {
            const ShaderBinding &b = group->bindings[i];
            bool merged = false;
            for (VkDescriptorPoolSize &s : sizes) {
                if (s.type == b.type) { s.descriptorCount += b.count; merged = true; break; }
            }
            if (!merged) {
                sizes.push_back({ b.type, b.count });
            }
        }
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = (uint32_t)sizes.size();
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(vk, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            error = "vkCreateDescriptorPool failed";
            return false;
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(vk, &allocInfo, &descriptorSet) != VK_SUCCESS) {
            error = "vkAllocateDescriptorSets failed";
            return false;
        }
    }

    /* Storage kept alive until vkUpdateDescriptorSets returns. */
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkBufferView> bufferViews;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> asInfos;
    imageInfos.reserve(group->count);
    bufferInfos.reserve(group->count);
    bufferViews.reserve(group->count);
    asInfos.reserve(2);

    VkAccelerationStructureKHR tlas = scene->getTopLevel().handle;

    for (uint32_t i = 0; i < group->count; i++) {
        const ShaderBinding &b = group->bindings[i];
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = b.binding;
        write.descriptorType = b.type;
        write.descriptorCount = 1;

        switch (b.type) {
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                const RenderTarget *t = findTarget(b.name);
                if (t == nullptr) { continue; }
                imageInfos.push_back({ VK_NULL_HANDLE, t->imageView,
                                       VK_IMAGE_LAYOUT_GENERAL });
                write.pImageInfo = &imageInfos.back();
                break;
            }
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                const RenderTarget *t = findTarget(b.name);
                if (t == nullptr) { continue; }
                bufferViews.push_back(t->bufferView);
                write.pTexelBufferView = &bufferViews.back();
                break;
            }
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                if (tlas == VK_NULL_HANDLE) {
                    continue;   /* nothing to trace yet; leave unwritten */
                }
                VkWriteDescriptorSetAccelerationStructureKHR asWrite = {};
                asWrite.sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                asWrite.accelerationStructureCount = 1;
                asWrite.pAccelerationStructures = &scene->getTopLevel().handle;
                asInfos.push_back(asWrite);
                write.pNext = &asInfos.back();
                break;
            }
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                VkBuffer buffer = emptyStorage.buffer;
                if (std::strcmp(b.name, "SceneLights") == 0 &&
                    scene->getLightBuffer() != VK_NULL_HANDLE) {
                    buffer = scene->getLightBuffer();
                } else if (std::strcmp(b.name, "instanceMaterials") == 0) {
                    buffer = instanceMaterials.buffer;
                }
                bufferInfos.push_back({ buffer, 0, VK_WHOLE_SIZE });
                write.pBufferInfo = &bufferInfos.back();
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
                bufferInfos.push_back({ paramsBuffer.buffer, 0,
                                        sizeof(GlobalParams) });
                write.pBufferInfo = &bufferInfos.back();
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER: {
                imageInfos.push_back({ sampler, VK_NULL_HANDLE,
                                       VK_IMAGE_LAYOUT_UNDEFINED });
                write.pImageInfo = &imageInfos.back();
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
                /* gTextures is PARTIALLY_BOUND, so leaving its 512 slots
                   unwritten is legal as long as the shader does not read them.
                   The single-image bindings still need something. */
                if (b.count > 1) { continue; }
                imageInfos.push_back({ VK_NULL_HANDLE, placeholderView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                write.pImageInfo = &imageInfos.back();
                break;
            }
            default:
                continue;
        }
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(vk, (uint32_t)writes.size(), writes.data(), 0,
                           nullptr);
    return true;
}

} /* namespace RT64 */
