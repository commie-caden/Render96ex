#include "rt64_texture_vk.h"
#include "rt64_device_vk.h"

#include <algorithm>
#include <cstring>

namespace RT64 {

TextureVK::TextureVK(DeviceVK *dev) : device(dev) {}

TextureVK::~TextureVK() { destroy(); }

void TextureVK::destroy() {
    if (device == nullptr) {
        return;
    }
    VkDevice vk = device->getDevice();
    if (view != VK_NULL_HANDLE)  { vkDestroyImageView(vk, view, nullptr); view = VK_NULL_HANDLE; }
    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(device->getAllocator(), image, allocation);
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
    if (pool != VK_NULL_HANDLE)  { vkDestroyCommandPool(vk, pool, nullptr); pool = VK_NULL_HANDLE; }
}

bool TextureVK::createImage(VkFormat format, uint32_t levels,
                            std::string &error) {
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* TRANSFER_SRC as well as DST: generating mips blits level n to n+1, so
       each level is read from as well as written to. */
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc = {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VkResult res = vmaCreateImage(device->getAllocator(), &info, &alloc, &image,
                                  &allocation, nullptr);
    if (res != VK_SUCCESS) {
        error = "vmaCreateImage failed (" + std::to_string((int)res) + ")";
        return false;
    }
    return true;
}

bool TextureVK::uploadAndGenerateMips(const void *bytes, VkDeviceSize byteCount,
                                      int rowPitch, std::string &error) {
    VkDevice vk = device->getDevice();
    VmaAllocator allocator = device->getAllocator();

    BufferVK staging;
    if (!createBuffer(allocator, vk, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      true, staging, error)) {
        return false;
    }
    std::memcpy(staging.mapped, bytes, (size_t)byteCount);

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = device->getGraphicsFamily();
    if (vkCreateCommandPool(vk, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        staging.destroy(allocator);
        return false;
    }

    VkCommandBufferAllocateInfo cbInfo = {};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk, &cbInfo, &cmd);

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    /* Level 0 receives the upload. */
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    /* rowPitch is in bytes; bufferRowLength wants texels. Zero means tightly
       packed, which is the common case. */
    if (rowPitch > 0 && (uint32_t)rowPitch != width * 4) {
        copy.bufferRowLength = (uint32_t)rowPitch / 4;
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    /* Mip chain by successive halving blits. The original used the
       GenerateMipsCS compute shader; blits are simpler and equivalent for
       RGBA8, and keep this independent of the compute pipeline. */
    int32_t mipWidth = (int32_t)width;
    int32_t mipHeight = (int32_t)height;
    for (uint32_t level = 1; level < mipLevels; level++) {
        /* Previous level: transfer dst -> transfer src. */
        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);

        /* This level: undefined -> transfer dst. */
        barrier.subresourceRange.baseMipLevel = level;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);

        const int32_t nextWidth = std::max(mipWidth / 2, 1);
        const int32_t nextHeight = std::max(mipHeight / 2, 1);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
        blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);

        /* Previous level is finished with: hand it to the shader. */
        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }

    /* The last level is still TRANSFER_DST. */
    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(device->getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->getGraphicsQueue());

    staging.destroy(allocator);
    return true;
}

bool TextureVK::setRGBA8(const void *bytes, int byteCount, int w, int h,
                         int rowPitch, bool generateMipmaps,
                         std::string &error) {
    if (bytes == nullptr || byteCount <= 0) {
        error = "texture has no data";
        return false;
    }
    if (w <= 0 || h <= 0) {
        error = "texture has non-positive dimensions";
        return false;
    }
    const int expectedPitch = (rowPitch > 0) ? rowPitch : w * 4;
    if ((int64_t)byteCount < (int64_t)expectedPitch * h) {
        error = "texture byteCount " + std::to_string(byteCount) +
                " is smaller than " + std::to_string(w) + "x" +
                std::to_string(h) + " at pitch " + std::to_string(expectedPitch);
        return false;
    }

    destroy();
    width = (uint32_t)w;
    height = (uint32_t)h;

    mipLevels = 1;
    if (generateMipmaps) {
        uint32_t largest = std::max(width, height);
        while (largest > 1) { largest >>= 1; mipLevels++; }
    }

    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    /* Blit-based mip generation needs the format to support linear filtering
       when used as a blit source. Checking beats a driver-specific surprise. */
    if (mipLevels > 1) {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(device->getPhysicalDevice(), format,
                                            &props);
        if (!(props.optimalTilingFeatures &
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            mipLevels = 1;   /* fall back to a single level rather than fail */
        }
    }

    if (!createImage(format, mipLevels, error)) {
        return false;
    }
    if (!uploadAndGenerateMips(bytes, (VkDeviceSize)byteCount, rowPitch, error)) {
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 };
    VkResult res = vkCreateImageView(device->getDevice(), &viewInfo, nullptr,
                                     &view);
    if (res != VK_SUCCESS) {
        error = "vkCreateImageView failed (" + std::to_string((int)res) + ")";
        return false;
    }
    return true;
}

} /* namespace RT64 */
