/*
 * rt64_texture_vk — Vulkan replacement for the D3D12 Texture.
 *
 * Handles RT64_TEXTURE_FORMAT_RGBA8 (with a generated mipmap chain) and
 * RT64_TEXTURE_FORMAT_DDS.
 *
 * DDS is uploaded compressed rather than decoded. Every GPU that can run this
 * port supports BC1/BC2/BC3 natively, so decoding on the CPU would cost time,
 * memory and image quality for nothing. The parser reads the surface header,
 * maps the FourCC (and the DX10 extension header) to a VkFormat, and copies
 * each mip level with block-aligned extents.
 */
#ifndef RT64_TEXTURE_VK_H
#define RT64_TEXTURE_VK_H

#include <cstdint>
#include <string>
#include <vector>

#include "rt64_raytracing_vk.h"

namespace RT64 {

class DeviceVK;

class TextureVK {
public:
    explicit TextureVK(DeviceVK *device);
    ~TextureVK();

    bool setRGBA8(const void *bytes, int byteCount, int width, int height,
                  int rowPitch, bool generateMipmaps, std::string &error);

    /* Uploads a .dds as-is. Desktop GPUs decode BC natively, so the block data
       goes straight to the image: no CPU decode, no quality loss, and a
       quarter to a sixth of the memory an expanded RGBA8 copy would take. */
    bool setDDS(const void *bytes, int byteCount, std::string &error);

    VkImage getImage() const { return image; }
    VkImageView getView() const { return view; }
    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    uint32_t getMipLevels() const { return mipLevels; }

    /* Slot in the shaders' gTextures[512] array, or -1 when unassigned. The
       game passes textures as pointers but the shader addresses them by index
       through MaterialProperties, so something has to translate. */
    int getIndex() const { return index; }
    void setIndex(int i) { index = i; }

private:
    bool createImage(VkFormat format, uint32_t levels, std::string &error);
    bool uploadAndGenerateMips(const void *bytes, VkDeviceSize byteCount,
                               int rowPitch, std::string &error);
    /* Copies pre-built mip levels verbatim. Unlike the RGBA8 path this does
       not blit to generate a chain: the file already has one, and BC data
       cannot be filtered by vkCmdBlitImage anyway. */
    bool uploadCompressedLevels(const uint8_t *base,
                                const std::vector<VkDeviceSize> &offsets,
                                const std::vector<VkDeviceSize> &sizes,
                                std::string &error);
    void destroy();

    DeviceVK *device = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    int index = -1;
};

} /* namespace RT64 */

#endif /* RT64_TEXTURE_VK_H */
