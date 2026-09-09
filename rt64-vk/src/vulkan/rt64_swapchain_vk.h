/*
 * rt64_swapchain_vk — surface and swapchain management.
 */
#ifndef RT64_SWAPCHAIN_VK_H
#define RT64_SWAPCHAIN_VK_H

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace RT64 {

class SwapchainVK {
public:
    ~SwapchainVK();

    bool createSurface(VkInstance instance, void *window, std::string &error);
    bool create(VkPhysicalDevice physicalDevice, VkDevice device,
                uint32_t graphicsFamily, int vsyncInterval,
                std::string &error);
    bool recreate(uint32_t graphicsFamily, int vsyncInterval,
                  std::string &error);

    VkSurfaceKHR   getSurface()   const { return surface; }
    VkSwapchainKHR getSwapchain() const { return swapchain; }
    VkFormat       getFormat()    const { return imageFormat; }
    VkExtent2D     getExtent()    const { return extent; }
    VkPresentModeKHR getPresentMode() const { return presentMode; }
    uint32_t       getImageCount() const { return (uint32_t)images.size(); }
    VkImage        getImage(uint32_t i)     const { return images[i]; }
    VkImageView    getImageView(uint32_t i) const { return imageViews[i]; }

private:
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR &caps) const;
    VkPresentModeKHR choosePresentMode(
        const std::vector<VkPresentModeKHR> &modes, int vsyncInterval) const;
    void destroyViews();

    SDL_Window      *sdlWindow      = nullptr;
    VkInstance       instance       = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice         device         = VK_NULL_HANDLE;
    VkSurfaceKHR     surface        = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain      = VK_NULL_HANDLE;
    VkFormat         imageFormat    = VK_FORMAT_UNDEFINED;
    VkExtent2D       extent         = { 0, 0 };
    VkPresentModeKHR presentMode    = VK_PRESENT_MODE_FIFO_KHR;

    std::vector<VkImage>     images;
    std::vector<VkImageView> imageViews;
};

} /* namespace RT64 */

#endif /* RT64_SWAPCHAIN_VK_H */
