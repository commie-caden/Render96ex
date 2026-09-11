/*
 * rt64_texture_vk — Vulkan replacement for the D3D12 Texture.
 *
 * Currently handles RT64_TEXTURE_FORMAT_RGBA8 including the mipmap chain.
 * RT64_TEXTURE_FORMAT_DDS is rejected with a clear message rather than
 * half-supported: Render96 ships .dds assets, so it needs a real BC-format
 * parser rather than a guess.
 */
#ifndef RT64_TEXTURE_VK_H
#define RT64_TEXTURE_VK_H

#include <cstdint>
#include <string>

#include "rt64_raytracing_vk.h"

namespace RT64 {

class DeviceVK;

class TextureVK {
public:
    /* Index into the shaders' gTextures[512] array. The game passes textures
       around as pointers, but the shader addresses them by index through
       MaterialProperties, so something has to assign and translate. */
    int arrayIndex = -1;

    explicit TextureVK(DeviceVK *device);
    ~TextureVK();

    bool setRGBA8(const void *bytes, int byteCount, int width, int height,
                  int rowPitch, bool generateMipmaps, std::string &error);

    VkImage getImage() const { return image; }
    VkImageView getView() const { return view; }
    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    uint32_t getMipLevels() const { return mipLevels; }

    /* Slot in the shaders' gTextures[512] array, or -1 when unassigned. */
    int getIndex() const { return index; }
    void setIndex(int i) { index = i; }

private:
    bool createImage(VkFormat format, uint32_t levels, std::string &error);
    bool uploadAndGenerateMips(const void *bytes, VkDeviceSize byteCount,
                               int rowPitch, std::string &error);
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
