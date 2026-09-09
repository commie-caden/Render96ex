/*
 * raster_test — the first real pixels through RT64's own shaders.
 *
 * Draws a fullscreen triangle with FullScreenVS + PostProcessPS, sampling a
 * procedural checkerboard, straight into the swapchain via dynamic rendering.
 *
 * This is the Phase 2 milestone because it exercises everything the raster
 * passes need and nothing they don't: descriptor set layout matching the
 * -fvk-*-shift binding scheme, a uniform buffer matching the gParams cbuffer
 * layout, image upload with layout transitions, and a graphics pipeline built
 * from unmodified RT64 SPIR-V.
 *
 * With motionBlurStrength = 0 the shader is a straight passthrough of gOutput,
 * so a correct run shows the checkerboard crisply. A blurred or black result
 * means the uniform buffer layout is wrong.
 */
#include "rt64_device_vk.h"
#include "rt64_global_params.h"
#include "rt64_shaders_vk.h"
#include "rt64_swapchain_vk.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

namespace {

const uint32_t kTextureSize = 256;

bool check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "  %s failed (%d)\n", what, (int)r);
        return false;
    }
    return true;
}

uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t bits,
                        VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem = {};
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

} /* namespace */

int main(int argc, char **argv) {
    std::string shaderDir = "shaders";
    bool validation = false;
    bool motionBlur = false;
    int framesToRun = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--validation") { validation = true; }
        else if (a == "--motionblur") { motionBlur = true; }
        else if (a == "--frames" && i + 1 < argc) { framesToRun = std::atoi(argv[++i]); }
        else if (a == "--shaders" && i + 1 < argc) { shaderDir = argv[++i]; }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "RT64 Vulkan — fullscreen triangle", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 960, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    RT64::DeviceVK device;
    device.setValidationEnabled(validation);
    std::string error;
    if (!device.initialize(window, error)) {
        std::fprintf(stderr, "device init failed: %s\n", error.c_str());
        return 1;
    }
    RT64::SwapchainVK *swapchain = device.getSwapchain();
    VkDevice vk = device.getDevice();
    VkQueue queue = device.getGraphicsQueue();

    std::printf("  device:    %s\n", device.getProperties().deviceName);
    std::printf("  swapchain: %ux%u, %u images, format %d\n",
                swapchain->getExtent().width, swapchain->getExtent().height,
                swapchain->getImageCount(), (int)swapchain->getFormat());

    RT64::ShaderLibraryVK shaders;
    if (!shaders.load(vk, shaderDir, error)) {
        std::fprintf(stderr, "  shader load failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  shaders:   %zu modules\n", shaders.count());

    /* ------------------------------------------------ command infrastructure */
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.getGraphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateCommandPool(vk, &poolInfo, nullptr, &pool), "CreateCommandPool")) return 1;

    auto oneShot = [&](const std::function<void(VkCommandBuffer)> &record) -> bool {
        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(vk, &ai, &cb) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        record(cb);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(vk, pool, 1, &cb);
        return true;
    };

    /* ------------------------------------------------------ checkerboard image */
    std::vector<uint8_t> pixels(kTextureSize * kTextureSize * 4);
    for (uint32_t y = 0; y < kTextureSize; y++) {
        for (uint32_t x = 0; x < kTextureSize; x++) {
            const bool on = ((x / 32) + (y / 32)) % 2 == 0;
            uint8_t *p = &pixels[(y * kTextureSize + x) * 4];
            p[0] = on ? 230 : 30;
            p[1] = (uint8_t)(x * 255 / kTextureSize);
            p[2] = on ? 60 : 200;
            p[3] = 255;
        }
    }

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = { kTextureSize, kTextureSize, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imgAlloc = {};
    imgAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage texture = VK_NULL_HANDLE;
    VmaAllocation textureAlloc = VK_NULL_HANDLE;
    if (!check(vmaCreateImage(device.getAllocator(), &imgInfo, &imgAlloc,
                              &texture, &textureAlloc, nullptr),
               "vmaCreateImage")) return 1;

    /* Staging upload. */
    VkBufferCreateInfo stageInfo = {};
    stageInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stageInfo.size = pixels.size();
    stageInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stageAlloc = {};
    stageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stageAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo = {};
    if (!check(vmaCreateBuffer(device.getAllocator(), &stageInfo, &stageAlloc,
                               &staging, &stagingAlloc, &stagingInfo),
               "vmaCreateBuffer(staging)")) return 1;
    std::memcpy(stagingInfo.pMappedData, pixels.data(), pixels.size());

    oneShot([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = texture;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);

        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { kTextureSize, kTextureSize, 1 };
        vkCmdCopyBufferToImage(cb, staging, texture,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    });
    vmaDestroyBuffer(device.getAllocator(), staging, stagingAlloc);

    /* A 4x4 constant flow texture: R=1.0 means "one pixel of horizontal
       motion", which motionBlurStrength then scales into a visible smear. */
    std::vector<uint8_t> flowPixels(4 * 4 * 4);
    for (size_t i = 0; i < flowPixels.size(); i += 4) {
        flowPixels[i + 0] = 255;   /* flow.x = 1.0 */
        flowPixels[i + 1] = 0;     /* flow.y = 0.0 */
        flowPixels[i + 2] = 0;
        flowPixels[i + 3] = 255;
    }
    VkImageCreateInfo flowInfo = imgInfo;
    flowInfo.extent = { 4, 4, 1 };
    VkImage flowImage = VK_NULL_HANDLE;
    VmaAllocation flowAlloc = VK_NULL_HANDLE;
    if (!check(vmaCreateImage(device.getAllocator(), &flowInfo, &imgAlloc,
                              &flowImage, &flowAlloc, nullptr),
               "vmaCreateImage(flow)")) return 1;

    VkBufferCreateInfo flowStageInfo = stageInfo;
    flowStageInfo.size = flowPixels.size();
    VkBuffer flowStaging = VK_NULL_HANDLE;
    VmaAllocation flowStagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo flowStagingInfo = {};
    if (!check(vmaCreateBuffer(device.getAllocator(), &flowStageInfo, &stageAlloc,
                               &flowStaging, &flowStagingAlloc, &flowStagingInfo),
               "vmaCreateBuffer(flow staging)")) return 1;
    std::memcpy(flowStagingInfo.pMappedData, flowPixels.data(), flowPixels.size());

    oneShot([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = flowImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { 4, 4, 1 };
        vkCmdCopyBufferToImage(cb, flowStaging, flowImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    });
    vmaDestroyBuffer(device.getAllocator(), flowStaging, flowStagingAlloc);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView textureView = VK_NULL_HANDLE;
    if (!check(vkCreateImageView(vk, &viewInfo, nullptr, &textureView),
               "CreateImageView")) return 1;

    viewInfo.image = flowImage;
    VkImageView flowView = VK_NULL_HANDLE;
    if (!check(vkCreateImageView(vk, &viewInfo, nullptr, &flowView),
               "CreateImageView(flow)")) return 1;

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (!check(vkCreateSampler(vk, &sampInfo, nullptr, &sampler),
               "CreateSampler")) return 1;

    /* --------------------------------------------------------- uniform buffer */
    RT64::GlobalParams params = {};
    params.resolution[0] = (float)swapchain->getExtent().width;
    params.resolution[1] = (float)swapchain->getExtent().height;
    /* This is the real test of the gParams layout. With the struct nearly all
       zeros, a WRONG offset still reads zero, still takes the passthrough
       branch, and still looks correct — so passthrough proves very little.
       Setting these two fields makes the shader take its other branch, and it
       can only do that if it finds them at the offsets we wrote them to
       (644 and 680). Blur appearing is positive evidence; no blur means the
       layout is wrong. */
    if (motionBlur) {
        params.motionBlurStrength = 60.0f;  /* flow is 1.0 == one pixel */
        params.motionBlurSamples = 16;
    } else {
        params.motionBlurStrength = 0.0f;
        params.motionBlurSamples = 0;
    }

    VkBufferCreateInfo uboInfo = {};
    uboInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    uboInfo.size = sizeof(RT64::GlobalParams);
    uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo uboAlloc = {};
    uboAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    uboAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer ubo = VK_NULL_HANDLE;
    VmaAllocation uboAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo uboAllocInfo = {};
    if (!check(vmaCreateBuffer(device.getAllocator(), &uboInfo, &uboAlloc, &ubo,
                               &uboAllocation, &uboAllocInfo),
               "vmaCreateBuffer(ubo)")) return 1;
    std::memcpy(uboAllocInfo.pMappedData, &params, sizeof(params));

    /* ------------------------------------------------------- descriptor layout */
    /* DXC emits separate images and samplers, not combined ones, so these must
       be SAMPLED_IMAGE and SAMPLER rather than COMBINED_IMAGE_SAMPLER. The
       binding numbers come from the -fvk-*-shift scheme. */
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0].binding = RT64::RT64_BINDING_SRV_BASE + 0;      /* gOutput  */
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = RT64::RT64_BINDING_SRV_BASE + 1;      /* gFlow    */
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = RT64::RT64_BINDING_CBV_BASE + 0;      /* gParams  */
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = RT64::RT64_BINDING_SAMPLER_BASE + 0;  /* gSampler */
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo setLayoutInfo = {};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 4;
    setLayoutInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (!check(vkCreateDescriptorSetLayout(vk, &setLayoutInfo, nullptr, &setLayout),
               "CreateDescriptorSetLayout")) return 1;

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  2 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
    };
    VkDescriptorPoolCreateInfo poolCreate = {};
    poolCreate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreate.maxSets = 1;
    poolCreate.poolSizeCount = 3;
    poolCreate.pPoolSizes = poolSizes;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    if (!check(vkCreateDescriptorPool(vk, &poolCreate, nullptr, &descPool),
               "CreateDescriptorPool")) return 1;

    VkDescriptorSetAllocateInfo setAlloc = {};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = descPool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &setLayout;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    if (!check(vkAllocateDescriptorSets(vk, &setAlloc, &descSet),
               "AllocateDescriptorSets")) return 1;

    VkDescriptorImageInfo texInfo = {};
    texInfo.imageView = textureView;
    texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo sampDesc = {};
    sampDesc.sampler = sampler;
    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = ubo;
    bufInfo.range = sizeof(RT64::GlobalParams);

    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet;
        writes[i].descriptorCount = 1;
    }
    writes[0].dstBinding = bindings[0].binding;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &texInfo;
    /* gFlow must be populated even when motion blur is off: the shader
       declares it, so the descriptor set would otherwise be incomplete. */
    VkDescriptorImageInfo flowDesc = {};
    flowDesc.imageView = flowView;
    flowDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[1].dstBinding = bindings[1].binding;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo = &flowDesc;
    writes[2].dstBinding = bindings[2].binding;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &bufInfo;
    writes[3].dstBinding = bindings[3].binding;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[3].pImageInfo = &sampDesc;
    vkUpdateDescriptorSets(vk, 4, writes, 0, nullptr);

    /* ------------------------------------------------------------- pipeline */
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (!check(vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &pipelineLayout),
               "CreatePipelineLayout")) return 1;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shaders.get("FullScreenVS");
    stages[0].pName = "VSMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shaders.get("PostProcessPS");
    stages[1].pName = "PSMain";

    /* No vertex buffers: FullScreenVS builds the triangle from SV_VertexID. */
    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    /* Dynamic rendering: no VkRenderPass, no VkFramebuffer. */
    VkFormat colorFormat = swapchain->getFormat();
    VkPipelineRenderingCreateInfo renderingInfo = {};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!check(vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipelineInfo,
                                         nullptr, &pipeline),
               "CreateGraphicsPipelines")) return 1;
    std::printf("  pipeline:  FullScreenVS + PostProcessPS created\n");
    std::printf("  mode:      %s\n", motionBlur
        ? "motion blur ON — expect a horizontal smear (validates gParams offsets)"
        : "passthrough — expect a crisp checkerboard");

    /* ----------------------------------------------------------- frame loop */
    const int kFramesInFlight = 2;
    std::vector<VkCommandBuffer> cmds(kFramesInFlight);
    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = pool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(vk, &cmdAlloc, cmds.data());

    std::vector<VkSemaphore> acquired(kFramesInFlight);
    std::vector<VkFence> inFlight(kFramesInFlight);
    for (int i = 0; i < kFramesInFlight; i++) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateSemaphore(vk, &si, nullptr, &acquired[i]);
        vkCreateFence(vk, &fi, nullptr, &inFlight[i]);
    }
    std::vector<VkSemaphore> rendered(swapchain->getImageCount());
    for (VkSemaphore &s : rendered) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(vk, &si, nullptr, &s);
    }

    bool running = true;
    int frame = 0, presented = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        if (!running) break;

        vkWaitForFences(vk, 1, &inFlight[frame], VK_TRUE, UINT64_MAX);
        uint32_t imageIndex = 0;
        VkResult res = vkAcquireNextImageKHR(vk, swapchain->getSwapchain(),
                                             UINT64_MAX, acquired[frame],
                                             VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain->recreate(device.getGraphicsFamily(), 1, error);
            continue;
        }
        vkResetFences(vk, 1, &inFlight[frame]);

        VkCommandBuffer cb = cmds[frame];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        VkImageMemoryBarrier toColor = {};
        toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = swapchain->getImage(imageIndex);
        toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toColor.srcAccessMask = 0;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toColor);

        VkRenderingAttachmentInfo colorAttachment = {};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchain->getImageView(imageIndex);
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

        VkRenderingInfo rendering = {};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = swapchain->getExtent();
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;
        vkCmdBeginRendering(cb, &rendering);

        VkViewport vp = {};
        vp.width = (float)swapchain->getExtent().width;
        vp.height = (float)swapchain->getExtent().height;
        vp.maxDepth = 1.0f;
        VkRect2D scissor = {};
        scissor.extent = swapchain->getExtent();
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 0, 1, &descSet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);          /* the fullscreen triangle */
        vkCmdEndRendering(cb);

        VkImageMemoryBarrier toPresent = toColor;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toPresent);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[frame];
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[imageIndex];
        vkQueueSubmit(queue, 1, &submit, inFlight[frame]);

        VkSwapchainKHR chain = swapchain->getSwapchain();
        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &chain;
        present.pImageIndices = &imageIndex;
        res = vkQueuePresentKHR(queue, &present);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
            swapchain->recreate(device.getGraphicsFamily(), 1, error);
        }

        presented++;
        frame = (frame + 1) % kFramesInFlight;
        if (framesToRun > 0 && presented >= framesToRun) running = false;
    }

    vkDeviceWaitIdle(vk);
    for (VkSemaphore s : rendered) vkDestroySemaphore(vk, s, nullptr);
    for (int i = 0; i < kFramesInFlight; i++) {
        vkDestroySemaphore(vk, acquired[i], nullptr);
        vkDestroyFence(vk, inFlight[i], nullptr);
    }
    vkDestroyPipeline(vk, pipeline, nullptr);
    vkDestroyPipelineLayout(vk, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(vk, descPool, nullptr);
    vkDestroyDescriptorSetLayout(vk, setLayout, nullptr);
    vkDestroySampler(vk, sampler, nullptr);
    vkDestroyImageView(vk, textureView, nullptr);
    vkDestroyImageView(vk, flowView, nullptr);
    vmaDestroyImage(device.getAllocator(), texture, textureAlloc);
    vmaDestroyImage(device.getAllocator(), flowImage, flowAlloc);
    vmaDestroyBuffer(device.getAllocator(), ubo, uboAllocation);
    vkDestroyCommandPool(vk, pool, nullptr);

    std::printf("  presented %d frames through RT64's shaders\n", presented);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
