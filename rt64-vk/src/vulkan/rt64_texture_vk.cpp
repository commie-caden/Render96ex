#include "rt64_texture_vk.h"
#include "rt64_device_vk.h"

#include <algorithm>
#include <cstring>
#include <vector>

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


/*
 * DDS support.
 *
 * The file is a 124-byte DDSURFACEDESC2 after a four-byte magic, optionally
 * followed by a DDS_HEADER_DXT10 when the FourCC is "DX10". Everything after
 * that is surface data, mip level 0 first, each level a quarter the size of
 * the last, and never smaller than one 4x4 block.
 */
namespace {

constexpr uint32_t DDS_MAGIC = 0x20534444u;   /* "DDS " */
constexpr uint32_t DDPF_FOURCC = 0x4u;
constexpr uint32_t DDPF_RGB = 0x40u;
constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000u;

constexpr uint32_t fourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

struct DDSPixelFormat {
    uint32_t size, flags, fourCC, rgbBitCount;
    uint32_t rBitMask, gBitMask, bBitMask, aBitMask;
};

struct DDSHeader {
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    uint32_t reserved1[11];
    DDSPixelFormat pixelFormat;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};

struct DDSHeaderDX10 {
    uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};

/* Only the DXGI values a BC-compressed .dds can actually carry. */
VkFormat formatFromDXGI(uint32_t dxgi) {
    switch (dxgi) {
        case 71: case 72: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* BC1 */
        case 74: case 75: return VK_FORMAT_BC2_UNORM_BLOCK;       /* BC2 */
        case 77: case 78: return VK_FORMAT_BC3_UNORM_BLOCK;       /* BC3 */
        case 80: case 81: return VK_FORMAT_BC4_UNORM_BLOCK;       /* BC4 */
        case 83: case 84: return VK_FORMAT_BC5_UNORM_BLOCK;       /* BC5 */
        case 98: case 99: return VK_FORMAT_BC7_UNORM_BLOCK;       /* BC7 */
        case 28: case 29: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

uint32_t blockBytesFor(VkFormat format) {
    switch (format) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return 8;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return 16;
        default:
            return 0;   /* uncompressed */
    }
}

/* Compressed levels are measured in 4x4 blocks, and a 1- or 2-pixel mip still
   occupies a whole block — the single most common way a DDS loader ends up
   reading past the end of the file. */
VkDeviceSize levelBytes(VkFormat format, uint32_t w, uint32_t h) {
    const uint32_t blockBytes = blockBytesFor(format);
    if (blockBytes == 0) {
        return (VkDeviceSize)w * h * 4;
    }
    const uint32_t bw = (w + 3) / 4;
    const uint32_t bh = (h + 3) / 4;
    return (VkDeviceSize)(bw > 0 ? bw : 1) * (bh > 0 ? bh : 1) * blockBytes;
}

} /* namespace */

bool TextureVK::setDDS(const void *bytes, int byteCount, std::string &error) {
    const uint8_t *data = (const uint8_t *)bytes;
    if ((data == nullptr) || (byteCount < (int)(4 + sizeof(DDSHeader)))) {
        error = "DDS data is too small to contain a header";
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != DDS_MAGIC) {
        error = "DDS magic missing";
        return false;
    }

    DDSHeader header = {};
    std::memcpy(&header, data + 4, sizeof(header));
    if (header.size != 124) {
        error = "DDS header size is not 124";
        return false;
    }

    size_t offset = 4 + sizeof(DDSHeader);
    VkFormat format = VK_FORMAT_UNDEFINED;

    if ((header.pixelFormat.flags & DDPF_FOURCC) != 0) {
        switch (header.pixelFormat.fourCC) {
            case fourCC('D','X','T','1'): format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
            case fourCC('D','X','T','3'): format = VK_FORMAT_BC2_UNORM_BLOCK; break;
            case fourCC('D','X','T','5'): format = VK_FORMAT_BC3_UNORM_BLOCK; break;
            case fourCC('A','T','I','1'):
            case fourCC('B','C','4','U'): format = VK_FORMAT_BC4_UNORM_BLOCK; break;
            case fourCC('A','T','I','2'):
            case fourCC('B','C','5','U'): format = VK_FORMAT_BC5_UNORM_BLOCK; break;
            case fourCC('D','X','1','0'): {
                if (byteCount < (int)(offset + sizeof(DDSHeaderDX10))) {
                    error = "DDS claims a DX10 header but the file ends first";
                    return false;
                }
                DDSHeaderDX10 ext = {};
                std::memcpy(&ext, data + offset, sizeof(ext));
                offset += sizeof(DDSHeaderDX10);
                format = formatFromDXGI(ext.dxgiFormat);
                if (format == VK_FORMAT_UNDEFINED) {
                    error = "DDS uses unsupported DXGI format " +
                            std::to_string(ext.dxgiFormat);
                    return false;
                }
                break;
            }
            default: {
                char cc[5] = {};
                std::memcpy(cc, &header.pixelFormat.fourCC, 4);
                error = std::string("DDS uses unsupported FourCC '") + cc + "'";
                return false;
            }
        }
    } else if ((header.pixelFormat.flags & DDPF_RGB) != 0 &&
               header.pixelFormat.rgbBitCount == 32) {
        /* Uncompressed BGRA, which some tools emit for textures with alpha. */
        format = (header.pixelFormat.rBitMask == 0x00ff0000u)
                     ? VK_FORMAT_B8G8R8A8_UNORM
                     : VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        error = "DDS is neither FourCC-compressed nor 32-bit uncompressed";
        return false;
    }

    if (!device->supportsSampledFormat(format)) {
        error = "the device cannot sample this DDS format";
        return false;
    }

    width = header.width;
    height = header.height;
    mipLevels = ((header.flags & DDSD_MIPMAPCOUNT) != 0 && header.mipMapCount > 0)
                    ? header.mipMapCount : 1;
    if ((width == 0) || (height == 0)) {
        error = "DDS reports a zero dimension";
        return false;
    }

    /* Trust the file's own extents over its mip count: a truncated chain is
       more common than a wrong header, and reading past the buffer is worse
       than dropping a level. */
    std::vector<VkDeviceSize> levelSizes;
    std::vector<VkDeviceSize> levelOffsets;
    VkDeviceSize cursor = 0;
    uint32_t usableLevels = 0;
    for (uint32_t level = 0; level < mipLevels; level++) {
        const uint32_t lw = (width >> level) > 0 ? (width >> level) : 1;
        const uint32_t lh = (height >> level) > 0 ? (height >> level) : 1;
        const VkDeviceSize size = levelBytes(format, lw, lh);
        if ((VkDeviceSize)byteCount < offset + cursor + size) {
            break;
        }
        levelOffsets.push_back(cursor);
        levelSizes.push_back(size);
        cursor += size;
        usableLevels++;
    }
    if (usableLevels == 0) {
        error = "DDS contains no complete mip level";
        return false;
    }
    mipLevels = usableLevels;

    if (!createImage(format, mipLevels, error)) {
        return false;
    }
    return uploadCompressedLevels(data + offset, levelOffsets, levelSizes, error);
}

bool TextureVK::uploadCompressedLevels(const uint8_t *base,
                                       const std::vector<VkDeviceSize> &offsets,
                                       const std::vector<VkDeviceSize> &sizes,
                                       std::string &error) {
    VkDeviceSize total = 0;
    for (VkDeviceSize s : sizes) { total += s; }
    if (total == 0) {
        error = "no compressed data to upload";
        return false;
    }

    VkDevice vk = device->getDevice();
    VmaAllocator allocator = device->getAllocator();

    BufferVK staging;
    if (!createBuffer(allocator, vk, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      true, staging, error)) {
        return false;
    }
    std::memcpy(staging.mapped, base, (size_t)total);

    VkCommandPool pool = VK_NULL_HANDLE;
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

    /* Whole chain to TRANSFER_DST, copy every level, then the whole chain to
       SHADER_READ_ONLY. No blitting: the mips came from the file. */
    VkImageMemoryBarrier toDst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toDst);

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(offsets.size());
    for (size_t level = 0; level < offsets.size(); level++) {
        const uint32_t lw = (width >> level) > 0 ? (width >> (uint32_t)level) : 1;
        const uint32_t lh = (height >> level) > 0 ? (height >> (uint32_t)level) : 1;
        VkBufferImageCopy region = {};
        region.bufferOffset = offsets[level];
        /* Zero means "tightly packed", which is what a DDS surface is. Setting
           these to the pixel extents would be wrong for block formats. */
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                   (uint32_t)level, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {lw, lh, 1};
        regions.push_back(region);
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(device->getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->getGraphicsQueue());

    vkDestroyCommandPool(vk, pool, nullptr);
    staging.destroy(allocator);
    return true;
}

} /* namespace RT64 */
