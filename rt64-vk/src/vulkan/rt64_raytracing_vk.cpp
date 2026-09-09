#include "rt64_raytracing_vk.h"
#include "rt64_device_vk.h"

#include <cstring>

namespace RT64 {

/* ------------------------------------------------------------------ loader */

bool RayTracingFunctions::load(VkDevice device, std::string &error) {
    struct Entry { const char *name; void **slot; };
    const Entry entries[] = {
        { "vkGetAccelerationStructureBuildSizesKHR",        (void **)&getBuildSizes },
        { "vkCreateAccelerationStructureKHR",               (void **)&createAccelerationStructure },
        { "vkDestroyAccelerationStructureKHR",              (void **)&destroyAccelerationStructure },
        { "vkCmdBuildAccelerationStructuresKHR",            (void **)&cmdBuildAccelerationStructures },
        { "vkGetAccelerationStructureDeviceAddressKHR",     (void **)&getAccelerationStructureAddress },
        { "vkCreateRayTracingPipelinesKHR",                 (void **)&createRayTracingPipelines },
        { "vkGetRayTracingShaderGroupHandlesKHR",           (void **)&getShaderGroupHandles },
        { "vkCmdTraceRaysKHR",                              (void **)&cmdTraceRays },
    };
    for (const Entry &e : entries) {
        *e.slot = (void *)vkGetDeviceProcAddr(device, e.name);
        if (*e.slot == nullptr) {
            error = std::string("could not load ") + e.name +
                    " — was VK_KHR_ray_tracing_pipeline enabled on the device?";
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ buffers */

void BufferVK::destroy(VmaAllocator allocator) {
    if (buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        address = 0;
        mapped = nullptr;
    }
}

bool createBuffer(VmaAllocator allocator, VkDevice device, VkDeviceSize size,
                  VkBufferUsageFlags usage, bool hostVisible, BufferVK &out,
                  std::string &error) {
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc = {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocationInfo allocInfo = {};
    VkResult res = vmaCreateBuffer(allocator, &info, &alloc, &out.buffer,
                                   &out.allocation, &allocInfo);
    if (res != VK_SUCCESS) {
        error = "vmaCreateBuffer failed (" + std::to_string((int)res) + ")";
        return false;
    }
    out.mapped = allocInfo.pMappedData;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo addressInfo = {};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = out.buffer;
        out.address = vkGetBufferDeviceAddress(device, &addressInfo);
    }
    return true;
}

/* ------------------------------------------------- acceleration structures */

void AccelerationStructureVK::destroy(const RayTracingFunctions &fn,
                                      VkDevice device, VmaAllocator allocator) {
    if (handle != VK_NULL_HANDLE) {
        fn.destroyAccelerationStructure(device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    storage.destroy(allocator);
    scratch.destroy(allocator);
}

bool AccelerationStructureBuilder::initialize(DeviceVK *dev, std::string &error) {
    device = dev;
    if (!fn.load(device->getDevice(), error)) {
        return false;
    }

    /* Scratch buffers have their own alignment requirement, separate from the
       usual buffer alignment. */
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps =
        device->getAccelerationStructureProperties();
    scratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
    if (scratchAlignment == 0) {
        scratchAlignment = 256;
    }

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device->getGraphicsFamily();
    VkResult res = vkCreateCommandPool(device->getDevice(), &poolInfo, nullptr,
                                       &pool);
    if (res != VK_SUCCESS) {
        error = "vkCreateCommandPool failed (" + std::to_string((int)res) + ")";
        return false;
    }
    return true;
}

void AccelerationStructureBuilder::shutdown() {
    if (pool != VK_NULL_HANDLE && device != nullptr) {
        vkDestroyCommandPool(device->getDevice(), pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
}

bool AccelerationStructureBuilder::submitBuild(
    const VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
    const VkAccelerationStructureBuildRangeInfoKHR *range, std::string &error) {

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device->getDevice(), &allocInfo, &cmd) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    fn.cmdBuildAccelerationStructures(cmd, 1, &buildInfo, &range);

    /* A TLAS build reads the BLAS it references, and tracing reads the TLAS,
       so the build must be ordered against subsequent acceleration structure
       reads. */
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(device->getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->getGraphicsQueue());
    vkFreeCommandBuffers(device->getDevice(), pool, 1, &cmd);
    return true;
}

bool AccelerationStructureBuilder::buildBottomLevel(
    const std::vector<TriangleGeometry> &geometries,
    VkBuildAccelerationStructureFlagsKHR flags,
    AccelerationStructureVK &out, std::string &error) {

    if (geometries.empty()) {
        error = "buildBottomLevel called with no geometry";
        return false;
    }

    std::vector<VkAccelerationStructureGeometryKHR> geoms(geometries.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(geometries.size());
    std::vector<uint32_t> primitiveCounts(geometries.size());

    for (size_t i = 0; i < geometries.size(); i++) {
        const TriangleGeometry &g = geometries[i];
        VkAccelerationStructureGeometryTrianglesDataKHR tris = {};
        tris.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tris.vertexFormat = g.vertexFormat;
        tris.vertexData.deviceAddress = g.vertexAddress;
        tris.vertexStride = g.vertexStride;
        tris.maxVertex = g.vertexCount - 1;
        tris.indexType = VK_INDEX_TYPE_UINT32;
        tris.indexData.deviceAddress = g.indexAddress;

        geoms[i] = {};
        geoms[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geoms[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geoms[i].geometry.triangles = tris;
        geoms[i].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

        primitiveCounts[i] = g.indexCount / 3;
        ranges[i] = {};
        ranges[i].primitiveCount = primitiveCounts[i];
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = flags;
    /* Refit an existing updatable structure instead of rebuilding it. */
    const bool refit = out.built && out.updatable();
    buildInfo.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = (uint32_t)geoms.size();
    buildInfo.pGeometries = geoms.data();

    VkAccelerationStructureBuildSizesInfoKHR sizes = {};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    fn.getBuildSizes(device->getDevice(),
                     VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                     primitiveCounts.data(), &sizes);

    if (!refit) {
        if (!createBuffer(device->getAllocator(), device->getDevice(),
                          sizes.accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, out.storage, error)) {
            return false;
        }

        VkAccelerationStructureCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = out.storage.buffer;
        createInfo.size = sizes.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkResult res = fn.createAccelerationStructure(device->getDevice(),
                                                      &createInfo, nullptr,
                                                      &out.handle);
        if (res != VK_SUCCESS) {
            error = "vkCreateAccelerationStructureKHR failed (" +
                    std::to_string((int)res) + ")";
            return false;
        }
    }

    /* An update needs updateScratchSize, which may differ from (and is never
       larger than) the initial build scratch. Keep the buffer for updatable
       structures so a refit does no allocation. */
    const VkDeviceSize scratchNeeded =
        refit ? sizes.updateScratchSize : sizes.buildScratchSize;
    if (out.scratch.buffer == VK_NULL_HANDLE) {
        if (!createBuffer(device->getAllocator(), device->getDevice(),
                          sizes.buildScratchSize + scratchAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, out.scratch, error)) {
            return false;
        }
    }
    (void)scratchNeeded;

    buildInfo.srcAccelerationStructure =
        refit ? out.handle : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = out.handle;
    buildInfo.scratchData.deviceAddress =
        alignUp(out.scratch.address, scratchAlignment);

    const VkAccelerationStructureBuildRangeInfoKHR *rangePtr = ranges.data();
    bool ok = submitBuild(buildInfo, rangePtr, error);
    if (!ok) {
        return false;
    }
    out.buildFlags = flags;
    out.built = true;

    /* A non-updatable structure will never refit, so its scratch is dead. */
    if (!out.updatable()) {
        out.scratch.destroy(device->getAllocator());
    }

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = out.handle;
    out.address = fn.getAccelerationStructureAddress(device->getDevice(),
                                                     &addressInfo);
    return true;
}

bool AccelerationStructureBuilder::buildTopLevel(
    const std::vector<VkAccelerationStructureInstanceKHR> &instances,
    AccelerationStructureVK &out, std::string &error) {

    if (instances.empty()) {
        error = "buildTopLevel called with no instances";
        return false;
    }

    const VkDeviceSize instanceBytes =
        sizeof(VkAccelerationStructureInstanceKHR) * instances.size();
    BufferVK instanceBuffer;
    if (!createBuffer(device->getAllocator(), device->getDevice(), instanceBytes,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      true, instanceBuffer, error)) {
        return false;
    }
    std::memcpy(instanceBuffer.mapped, instances.data(), (size_t)instanceBytes);

    VkAccelerationStructureGeometryInstancesDataKHR instancesData = {};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = instanceBuffer.address;

    VkAccelerationStructureGeometryKHR geometry = {};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    const uint32_t instanceCount = (uint32_t)instances.size();
    VkAccelerationStructureBuildSizesInfoKHR sizes = {};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    fn.getBuildSizes(device->getDevice(),
                     VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                     &instanceCount, &sizes);

    if (!createBuffer(device->getAllocator(), device->getDevice(),
                      sizes.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      false, out.storage, error)) {
        instanceBuffer.destroy(device->getAllocator());
        return false;
    }

    VkAccelerationStructureCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = out.storage.buffer;
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkResult res = fn.createAccelerationStructure(device->getDevice(),
                                                  &createInfo, nullptr,
                                                  &out.handle);
    if (res != VK_SUCCESS) {
        error = "vkCreateAccelerationStructureKHR (TLAS) failed (" +
                std::to_string((int)res) + ")";
        instanceBuffer.destroy(device->getAllocator());
        return false;
    }

    BufferVK scratch;
    if (!createBuffer(device->getAllocator(), device->getDevice(),
                      sizes.buildScratchSize + scratchAlignment,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      false, scratch, error)) {
        instanceBuffer.destroy(device->getAllocator());
        return false;
    }

    buildInfo.dstAccelerationStructure = out.handle;
    buildInfo.scratchData.deviceAddress = alignUp(scratch.address, scratchAlignment);

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR *rangePtr = &range;

    bool ok = submitBuild(buildInfo, rangePtr, error);
    scratch.destroy(device->getAllocator());
    instanceBuffer.destroy(device->getAllocator());
    if (!ok) {
        return false;
    }

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = out.handle;
    out.address = fn.getAccelerationStructureAddress(device->getDevice(),
                                                     &addressInfo);
    return true;
}

/* ---------------------------------------------------- shader binding table */

bool ShaderBindingTable::build(DeviceVK *device, const RayTracingFunctions &fn,
                               VkPipeline pipeline, uint32_t raygenCount,
                               uint32_t missCount, uint32_t hitCount,
                               std::string &error) {
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &props =
        device->getRayTracingProperties();
    handleSize = props.shaderGroupHandleSize;
    handleAlignment = props.shaderGroupHandleAlignment;
    baseAlignment = props.shaderGroupBaseAlignment;

    const uint32_t groupCount = raygenCount + missCount + hitCount;
    const VkDeviceSize handleStride = alignUp(handleSize, handleAlignment);

    std::vector<uint8_t> handles(groupCount * handleSize);
    VkResult res = fn.getShaderGroupHandles(device->getDevice(), pipeline, 0,
                                            groupCount,
                                            handles.size(), handles.data());
    if (res != VK_SUCCESS) {
        error = "vkGetRayTracingShaderGroupHandlesKHR failed (" +
                std::to_string((int)res) + ")";
        return false;
    }

    /* Each region's base address must be baseAlignment-aligned, while records
       within a region step by handleStride. The raygen region is special: the
       spec requires its size to equal its stride. */
    const VkDeviceSize raygenSize = alignUp(raygenCount * handleStride, baseAlignment);
    const VkDeviceSize missSize   = alignUp(missCount   * handleStride, baseAlignment);
    const VkDeviceSize hitSize    = alignUp(hitCount    * handleStride, baseAlignment);
    const VkDeviceSize total = raygenSize + missSize + hitSize;

    if (!createBuffer(device->getAllocator(), device->getDevice(), total,
                      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      true, buffer, error)) {
        return false;
    }

    uint8_t *dst = (uint8_t *)buffer.mapped;
    std::memset(dst, 0, (size_t)total);
    const uint8_t *src = handles.data();

    for (uint32_t i = 0; i < raygenCount; i++) {
        std::memcpy(dst + i * handleStride, src, handleSize);
        src += handleSize;
    }
    uint8_t *missBase = dst + raygenSize;
    for (uint32_t i = 0; i < missCount; i++) {
        std::memcpy(missBase + i * handleStride, src, handleSize);
        src += handleSize;
    }
    uint8_t *hitBase = dst + raygenSize + missSize;
    for (uint32_t i = 0; i < hitCount; i++) {
        std::memcpy(hitBase + i * handleStride, src, handleSize);
        src += handleSize;
    }

    this->raygenCount = raygenCount;
    raygen.deviceAddress = buffer.address;
    raygen.stride = handleStride;
    raygen.size = handleStride;          /* must equal stride for raygen */

    miss.deviceAddress = buffer.address + raygenSize;
    miss.stride = handleStride;
    miss.size = missCount * handleStride;

    hit.deviceAddress = buffer.address + raygenSize + missSize;
    hit.stride = handleStride;
    hit.size = hitCount * handleStride;

    callable = {};
    return true;
}

void ShaderBindingTable::destroy(VmaAllocator allocator) {
    buffer.destroy(allocator);
}

} /* namespace RT64 */
