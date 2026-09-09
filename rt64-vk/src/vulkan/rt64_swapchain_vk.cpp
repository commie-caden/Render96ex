/*
 * rt64_swapchain_vk — surface and swapchain, replacing the DXGI swapchain.
 *
 * Two things differ meaningfully from the D3D12 original.
 *
 * The surface extension cannot be chosen at compile time. A Wayland session
 * with XWayland running exposes both paths, and SDL decides which it uses at
 * runtime, so the instance extension list comes from SDL rather than from us.
 *
 * Extent is not always dictated by the surface. On Wayland the compositor
 * usually reports currentExtent as 0xFFFFFFFF, meaning the application picks
 * the size; on X11 the surface reports a real extent that must be obeyed.
 * Getting this wrong yields a zero-sized or mismatched swapchain, so both
 * cases are handled explicitly.
 */
#include "rt64_swapchain_vk.h"

#include <algorithm>
#include <cstdint>

#include <SDL2/SDL_vulkan.h>

namespace RT64 {

namespace {

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR> &formats) {
    /* RT64 composes in linear space and writes an already-tonemapped image,
       so an UNORM target is what we want, not SRGB. */
    for (const VkSurfaceFormatKHR &f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM ||
             f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats[0];
}

bool hasMode(const std::vector<VkPresentModeKHR> &modes, VkPresentModeKHR m) {
    return std::find(modes.begin(), modes.end(), m) != modes.end();
}

} /* namespace */

bool SwapchainVK::createSurface(VkInstance instance, void *window,
                                std::string &error) {
    if (window == nullptr) {
        error = "no window supplied";
        return false;
    }
    sdlWindow = (SDL_Window *)window;
    if (!SDL_Vulkan_CreateSurface(sdlWindow, instance, &surface)) {
        error = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
        return false;
    }
    this->instance = instance;
    return true;
}

VkExtent2D SwapchainVK::chooseExtent(
    const VkSurfaceCapabilitiesKHR &caps) const {
    /* 0xFFFFFFFF means "you choose" — the usual Wayland case. Anything else
       is binding and must be used verbatim. */
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }

    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(sdlWindow, &w, &h);
    VkExtent2D extent = { (uint32_t)w, (uint32_t)h };
    extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                               caps.maxImageExtent.height);
    return extent;
}

VkPresentModeKHR SwapchainVK::choosePresentMode(
    const std::vector<VkPresentModeKHR> &modes, int vsyncInterval) const {
    /* RT64_DrawDevice takes a vsync interval: 0 means uncapped. FIFO is the
       only mode required to exist, so everything else is a preference. */
    if (vsyncInterval == 0) {
        if (hasMode(modes, VK_PRESENT_MODE_MAILBOX_KHR)) {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        if (hasMode(modes, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool SwapchainVK::create(VkPhysicalDevice physicalDevice, VkDevice device,
                         uint32_t graphicsFamily, int vsyncInterval,
                         std::string &error) {
    this->physicalDevice = physicalDevice;
    this->device = device;

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, graphicsFamily,
                                         surface, &supported);
    if (!supported) {
        error = "graphics queue family cannot present to this surface";
        return false;
    }

    VkSurfaceCapabilitiesKHR caps = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                         nullptr);
    if (formatCount == 0) {
        error = "surface reports no formats";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                         formats.data());

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                              &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                              &modeCount, modes.data());

    VkSurfaceFormatKHR format = chooseFormat(formats);
    extent = chooseExtent(caps);
    if (extent.width == 0 || extent.height == 0) {
        error = "window has zero drawable size (minimised?)";
        return false;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    /* TRANSFER_DST so the compose pass can blit into it, COLOR_ATTACHMENT for
       the imgui/im3d overlay drawn on top. */
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = choosePresentMode(modes, vsyncInterval);
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(device, &info, nullptr, &created);
    if (res != VK_SUCCESS) {
        error = "vkCreateSwapchainKHR failed (" + std::to_string((int)res) + ")";
        return false;
    }

    destroyViews();
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
    swapchain = created;
    imageFormat = format.format;
    presentMode = info.presentMode;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

    imageViews.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        res = vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        if (res != VK_SUCCESS) {
            error = "vkCreateImageView failed (" + std::to_string((int)res) + ")";
            return false;
        }
    }
    return true;
}

bool SwapchainVK::recreate(uint32_t graphicsFamily, int vsyncInterval,
                           std::string &error) {
    vkDeviceWaitIdle(device);
    return create(physicalDevice, device, graphicsFamily, vsyncInterval, error);
}

void SwapchainVK::destroyViews() {
    for (VkImageView view : imageViews) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
    }
    imageViews.clear();
}

SwapchainVK::~SwapchainVK() {
    if (device != VK_NULL_HANDLE) {
        destroyViews();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }
    }
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}

} /* namespace RT64 */
