/*
 * raytrace_test — Phase 3 milestone: rays actually traced against a BLAS/TLAS.
 *
 * Builds a triangle bottom level structure, a top level structure over it, a
 * ray tracing pipeline with raygen/miss/hit groups, and a shader binding table
 * laid out from the device's own alignment properties. Traces into a storage
 * image and blits that to the swapchain.
 *
 * A barycentric-coloured triangle on a dark blue field means the whole chain
 * works. Anything wrong in the acceleration structures or SBT tends to produce
 * an all-miss (uniform dark blue) result rather than a crash, so the triangle
 * appearing is the signal — not the absence of errors.
 */
#include "rt64_device_vk.h"
#include "rt64_raytracing_vk.h"
#include "rt64_shaders_vk.h"
#include "rt64_swapchain_vk.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

namespace {

bool check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "  %s failed (%d)\n", what, (int)r);
        return false;
    }
    return true;
}

struct Vertex { float x, y, z; };

} /* namespace */

int main(int argc, char **argv) {
    std::string shaderDir = "test_shaders";
    bool validation = false;
    int framesToRun = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--validation") validation = true;
        else if (a == "--frames" && i + 1 < argc) framesToRun = std::atoi(argv[++i]);
        else if (a == "--shaders" && i + 1 < argc) shaderDir = argv[++i];
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "RT64 Vulkan — ray traced triangle", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 960, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

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
    VmaAllocator allocator = device.getAllocator();
    std::printf("  device:    %s\n", device.getProperties().deviceName);
    std::printf("  swapchain format %d, storage image format %d%s\n",
                (int)swapchain->getFormat(), (int)VK_FORMAT_R8G8B8A8_UNORM,
                swapchain->getFormat() == VK_FORMAT_R8G8B8A8_UNORM
                    ? " (same)" : " (differ — blit converts)");

    RT64::AccelerationStructureBuilder builder;
    if (!builder.initialize(&device, error)) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    const RT64::RayTracingFunctions &fn = builder.functions();
    std::printf("  loaded 8 KHR ray tracing entry points via vkGetDeviceProcAddr\n");

    /* ------------------------------------------------------------ geometry */
    const Vertex vertices[3] = {
        {  0.0f,  0.7f, 0.0f },
        {  0.7f, -0.5f, 0.0f },
        { -0.7f, -0.5f, 0.0f },
    };
    const uint32_t indices[3] = { 0, 1, 2 };

    RT64::BufferVK vertexBuffer, indexBuffer;
    const VkBufferUsageFlags asInput =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (!RT64::createBuffer(allocator, vk, sizeof(vertices), asInput, true,
                            vertexBuffer, error) ||
        !RT64::createBuffer(allocator, vk, sizeof(indices), asInput, true,
                            indexBuffer, error)) {
        std::fprintf(stderr, "  buffer creation failed: %s\n", error.c_str());
        return 1;
    }
    std::memcpy(vertexBuffer.mapped, vertices, sizeof(vertices));
    std::memcpy(indexBuffer.mapped, indices, sizeof(indices));

    RT64::TriangleGeometry geo = {};
    geo.vertexAddress = vertexBuffer.address;
    geo.indexAddress = indexBuffer.address;
    geo.vertexCount = 3;
    geo.vertexStride = sizeof(Vertex);
    geo.indexCount = 3;

    RT64::AccelerationStructureVK blas, tlas;
    if (!builder.buildBottomLevel({ geo }, blas, error)) {
        std::fprintf(stderr, "  BLAS build failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  BLAS:      built (1 triangle)\n");

    VkAccelerationStructureInstanceKHR instance = {};
    /* Row-major 3x4 identity. */
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = blas.address;

    if (!builder.buildTopLevel({ instance }, tlas, error)) {
        std::fprintf(stderr, "  TLAS build failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  TLAS:      built (1 instance)\n");

    /* -------------------------------------------------------- storage image */
    VkExtent2D extent = swapchain->getExtent();
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = { extent.width, extent.height, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo imgAlloc = {};
    imgAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage storageImage = VK_NULL_HANDLE;
    VmaAllocation storageAlloc = VK_NULL_HANDLE;
    if (!check(vmaCreateImage(allocator, &imgInfo, &imgAlloc, &storageImage,
                              &storageAlloc, nullptr), "vmaCreateImage")) return 1;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = storageImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView storageView = VK_NULL_HANDLE;
    if (!check(vkCreateImageView(vk, &viewInfo, nullptr, &storageView),
               "CreateImageView")) return 1;

    /* --------------------------------------------------------- descriptors */
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;                    /* gOutput  (u0 -> 0)   */
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[1].binding = 100;                  /* SceneBVH (t0 -> 100) */
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo setInfo = {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (!check(vkCreateDescriptorSetLayout(vk, &setInfo, nullptr, &setLayout),
               "CreateDescriptorSetLayout")) return 1;

    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
    };
    VkDescriptorPoolCreateInfo poolCreate = {};
    poolCreate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreate.maxSets = 1;
    poolCreate.poolSizeCount = 2;
    poolCreate.pPoolSizes = sizes;
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

    VkDescriptorImageInfo imageDesc = {};
    imageDesc.imageView = storageView;
    imageDesc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    /* The acceleration structure is written through a pNext chain, not through
       pImageInfo/pBufferInfo. */
    VkWriteDescriptorSetAccelerationStructureKHR asWrite = {};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas.handle;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imageDesc;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].pNext = &asWrite;
    writes[1].dstSet = descSet;
    writes[1].dstBinding = 100;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);

    /* ----------------------------------------------------------- pipeline */
    RT64::ShaderLibraryVK shaders;
    VkShaderModule rtModule = VK_NULL_HANDLE;
    {
        const std::string path = shaderDir + "/TestRayTracing.spv";
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "  cannot open %s\n", path.c_str());
            return 1;
        }
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<char> code((size_t)size);
        if (std::fread(code.data(), 1, (size_t)size, f) != (size_t)size) {
            std::fclose(f); std::fprintf(stderr, "  short read on %s\n", path.c_str()); return 1;
        }
        std::fclose(f);
        VkShaderModuleCreateInfo mi = {};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = (size_t)size;
        mi.pCode = reinterpret_cast<const uint32_t *>(code.data());
        if (!check(vkCreateShaderModule(vk, &mi, nullptr, &rtModule),
                   "CreateShaderModule")) return 1;
    }

    VkPipelineShaderStageCreateInfo stages[3] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rtModule;
    stages[0].pName = "RayGen";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rtModule;
    stages[1].pName = "Miss";
    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rtModule;
    stages[2].pName = "ClosestHit";

    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};
    for (int i = 0; i < 3; i++) {
        groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[i].generalShader = VK_SHADER_UNUSED_KHR;
        groups[i].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (!check(vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &pipelineLayout),
               "CreatePipelineLayout")) return 1;

    VkRayTracingPipelineCreateInfoKHR pipeInfo = {};
    pipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipeInfo.stageCount = 3;
    pipeInfo.pStages = stages;
    pipeInfo.groupCount = 3;
    pipeInfo.pGroups = groups;
    pipeInfo.maxPipelineRayRecursionDepth = 1;
    pipeInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!check(fn.createRayTracingPipelines(vk, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                            1, &pipeInfo, nullptr, &pipeline),
               "vkCreateRayTracingPipelinesKHR")) return 1;
    std::printf("  pipeline:  raygen + miss + hit group\n");

    RT64::ShaderBindingTable sbt;
    /* One hit record naming group 2, the triangle hit group. This test uses no
       shader record data, so the addresses stay zero. */
    std::vector<RT64::HitRecord> hitRecords(1);
    hitRecords[0].groupIndex = 2;
    if (!sbt.build(&device, fn, pipeline, 1, 1, 3, hitRecords, error)) {
        std::fprintf(stderr, "  SBT build failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  SBT:       handleSize %u, handleAlign %u, baseAlign %u,"
                " stride %llu\n", sbt.handleSize, sbt.handleAlignment,
                sbt.baseAlignment,
                (unsigned long long)sbt.raygenRegion().stride);

    /* --------------------------------------------------------- frame loop */
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.getGraphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(vk, &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = pool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk, &cmdAlloc, &cmd);

    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateSemaphore(vk, &si, nullptr, &acquired);
    vkCreateFence(vk, &fi, nullptr, &inFlight);
    std::vector<VkSemaphore> rendered(swapchain->getImageCount());
    for (VkSemaphore &s : rendered) vkCreateSemaphore(vk, &si, nullptr, &s);

    bool running = true;
    int presented = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        if (!running) break;

        vkWaitForFences(vk, 1, &inFlight, VK_TRUE, UINT64_MAX);
        uint32_t imageIndex = 0;
        VkResult res = vkAcquireNextImageKHR(vk, swapchain->getSwapchain(),
                                             UINT64_MAX, acquired,
                                             VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) break;   /* resize unsupported here */
        vkResetFences(vk, 1, &inFlight);

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier toGeneral = {};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = storageImage;
        toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
                             0, nullptr, 0, nullptr, 1, &toGeneral);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                pipelineLayout, 0, 1, &descSet, 0, nullptr);
        VkStridedDeviceAddressRegionKHR rgen = sbt.raygenRegion();
        VkStridedDeviceAddressRegionKHR rmiss = sbt.missRegion();
        VkStridedDeviceAddressRegionKHR rhit = sbt.hitRegion();
        VkStridedDeviceAddressRegionKHR rcall = sbt.callableRegion();
        fn.cmdTraceRays(cmd, &rgen, &rmiss, &rhit, &rcall,
                        extent.width, extent.height, 1);

        /* Storage image -> transfer source, swapchain -> transfer dest. */
        VkImageMemoryBarrier toSrc = toGeneral;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toSrc);

        VkImageMemoryBarrier toDst = toGeneral;
        toDst.image = swapchain->getImage(imageIndex);
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toDst);

        /* vkCmdCopyImage is a raw byte copy and does NOT convert formats.
           The storage image is R8G8B8A8_UNORM (the format guaranteed for
           storage use) while the swapchain is usually B8G8R8A8_UNORM, so a
           copy silently exchanges red and blue. vkCmdBlitImage reads and
           writes through the format, so it reorders correctly. */
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)extent.width, (int32_t)extent.height, 1 };
        blit.dstOffsets[1] = { (int32_t)extent.width, (int32_t)extent.height, 1 };
        vkCmdBlitImage(cmd, storageImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swapchain->getImage(imageIndex),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);

        VkImageMemoryBarrier toPresent = toDst;
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
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[imageIndex];
        vkQueueSubmit(queue, 1, &submit, inFlight);

        VkSwapchainKHR chain = swapchain->getSwapchain();
        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &chain;
        present.pImageIndices = &imageIndex;
        vkQueuePresentKHR(queue, &present);

        presented++;
        if (framesToRun > 0 && presented >= framesToRun) running = false;
    }

    vkDeviceWaitIdle(vk);
    for (VkSemaphore s : rendered) vkDestroySemaphore(vk, s, nullptr);
    vkDestroySemaphore(vk, acquired, nullptr);
    vkDestroyFence(vk, inFlight, nullptr);
    sbt.destroy(allocator);
    vkDestroyPipeline(vk, pipeline, nullptr);
    vkDestroyPipelineLayout(vk, pipelineLayout, nullptr);
    vkDestroyShaderModule(vk, rtModule, nullptr);
    vkDestroyDescriptorPool(vk, descPool, nullptr);
    vkDestroyDescriptorSetLayout(vk, setLayout, nullptr);
    vkDestroyImageView(vk, storageView, nullptr);
    vmaDestroyImage(allocator, storageImage, storageAlloc);
    tlas.destroy(fn, vk, allocator);
    blas.destroy(fn, vk, allocator);
    vertexBuffer.destroy(allocator);
    indexBuffer.destroy(allocator);
    vkDestroyCommandPool(vk, pool, nullptr);
    builder.shutdown();

    std::printf("  traced %d frames\n", presented);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
