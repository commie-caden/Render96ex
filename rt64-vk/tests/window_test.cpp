/*
 * window_test — the Phase 1 milestone: open a window, clear it, present.
 *
 * Exercises the parts that cannot be tested headless: SDL surface creation,
 * swapchain setup, per-frame synchronisation, and swapchain recreation on
 * resize. Deliberately uses vkCmdClearColorImage rather than a render pass,
 * so a failure here is a swapchain or sync problem and nothing else.
 *
 * Run with validation:
 *   VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./rt64_window_test
 */
#include "rt64_device_vk.h"
#include "rt64_swapchain_vk.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

namespace {

const int kMaxFramesInFlight = 2;

struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence     inFlight       = VK_NULL_HANDLE;
};

bool check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "  %s failed (%d)\n", what, (int)r);
        return false;
    }
    return true;
}

} /* namespace */

int main(int argc, char **argv) {
    bool validation = false;
    int framesToRun = 0;                   /* 0 = until the window closes */
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--validation") { validation = true; }
        if (std::string(argv[i]) == "--frames" && i + 1 < argc) {
            framesToRun = std::atoi(argv[++i]);
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "RT64 Vulkan — Phase 1", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 960, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    std::printf("  SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

    RT64::DeviceVK device;
    device.setValidationEnabled(validation);
    std::string error;
    if (!device.initialize(window, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    RT64::SwapchainVK *swapchain = device.getSwapchain();
    std::printf("  device:       %s\n", device.getProperties().deviceName);
    std::printf("  swapchain:    %ux%u, %u images\n",
                swapchain->getExtent().width, swapchain->getExtent().height,
                swapchain->getImageCount());
    std::printf("  present mode: %s\n",
                swapchain->getPresentMode() == VK_PRESENT_MODE_FIFO_KHR
                    ? "FIFO"
                    : (swapchain->getPresentMode() == VK_PRESENT_MODE_MAILBOX_KHR
                           ? "MAILBOX" : "IMMEDIATE"));

    VkDevice vk = device.getDevice();

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.getGraphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateCommandPool(vk, &poolInfo, nullptr, &pool),
               "vkCreateCommandPool")) { return 1; }

    std::vector<VkCommandBuffer> cmds(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    if (!check(vkAllocateCommandBuffers(vk, &allocInfo, cmds.data()),
               "vkAllocateCommandBuffers")) { return 1; }

    std::vector<FrameSync> frames(kMaxFramesInFlight);
    for (FrameSync &f : frames) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateSemaphore(vk, &si, nullptr, &f.imageAvailable);
        vkCreateFence(vk, &fi, nullptr, &f.inFlight);
    }
    /* One render-finished semaphore per swapchain image, not per frame in
       flight: vkQueuePresentKHR waits on it, and reusing a frame-indexed
       semaphore while an older present still references it is a race that
       validation flags. */
    std::vector<VkSemaphore> renderFinished(swapchain->getImageCount());
    for (VkSemaphore &s : renderFinished) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(vk, &si, nullptr, &s);
    }

    VkQueue queue = device.getGraphicsQueue();
    bool running = true;
    int frameIndex = 0;
    int presented = 0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { running = false; }
            if (event.type == SDL_KEYDOWN &&
                event.key.keysym.sym == SDLK_ESCAPE) { running = false; }
        }
        if (!running) { break; }

        FrameSync &frame = frames[frameIndex];
        vkWaitForFences(vk, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult res = vkAcquireNextImageKHR(vk, swapchain->getSwapchain(),
                                             UINT64_MAX, frame.imageAvailable,
                                             VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!swapchain->recreate(device.getGraphicsFamily(), 1, error)) {
                std::fprintf(stderr, "recreate failed: %s\n", error.c_str());
                break;
            }
            continue;
        }
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            std::fprintf(stderr, "vkAcquireNextImageKHR failed (%d)\n", (int)res);
            break;
        }

        vkResetFences(vk, 1, &frame.inFlight);
        VkCommandBuffer cmd = cmds[frameIndex];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;

        /* UNDEFINED -> TRANSFER_DST. D3D12 would have promoted this state
           implicitly; Vulkan requires it spelled out. */
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = swapchain->getImage(imageIndex);
        toTransfer.subresourceRange = range;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toTransfer);

        float t = (float)presented * 0.02f;
        VkClearColorValue color = {};
        color.float32[0] = 0.5f + 0.5f * std::sin(t);
        color.float32[1] = 0.25f;
        color.float32[2] = 0.5f + 0.5f * std::cos(t);
        color.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, swapchain->getImage(imageIndex),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color,
                             1, &range);

        VkImageMemoryBarrier toPresent = toTransfer;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toPresent);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished[imageIndex];
        if (!check(vkQueueSubmit(queue, 1, &submit, frame.inFlight),
                   "vkQueueSubmit")) { break; }

        VkSwapchainKHR chain = swapchain->getSwapchain();
        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &chain;
        present.pImageIndices = &imageIndex;
        res = vkQueuePresentKHR(queue, &present);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
            if (!swapchain->recreate(device.getGraphicsFamily(), 1, error)) {
                std::fprintf(stderr, "recreate failed: %s\n", error.c_str());
                break;
            }
        } else if (res != VK_SUCCESS) {
            std::fprintf(stderr, "vkQueuePresentKHR failed (%d)\n", (int)res);
            break;
        }

        presented++;
        frameIndex = (frameIndex + 1) % kMaxFramesInFlight;
        if (framesToRun > 0 && presented >= framesToRun) { running = false; }
    }

    vkDeviceWaitIdle(vk);
    for (VkSemaphore s : renderFinished) { vkDestroySemaphore(vk, s, nullptr); }
    for (FrameSync &f : frames) {
        vkDestroySemaphore(vk, f.imageAvailable, nullptr);
        vkDestroyFence(vk, f.inFlight, nullptr);
    }
    vkDestroyCommandPool(vk, pool, nullptr);

    std::printf("  presented %d frames cleanly\n", presented);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
