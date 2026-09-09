#include "rt64_mesh_vk.h"
#include "rt64_device_vk.h"
#include "rt64/rt64.h"

#include <cstring>

namespace RT64 {

MeshVK::MeshVK(DeviceVK *dev, AccelerationStructureBuilder *bld, int f)
    : device(dev), builder(bld), flags(f) {}

MeshVK::~MeshVK() {
    VmaAllocator allocator = device->getAllocator();
    blas.destroy(builder->functions(), device->getDevice(), allocator);
    vertexBuffer.destroy(allocator);
    indexBuffer.destroy(allocator);
}

bool MeshVK::raytraceEnabled() const {
    return (flags & RT64_MESH_RAYTRACE_ENABLED) != 0;
}

VkBuildAccelerationStructureFlagsKHR MeshVK::vulkanBuildFlags() const {
    VkBuildAccelerationStructureFlagsKHR out = 0;
    if (flags & RT64_MESH_RAYTRACE_UPDATABLE) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (flags & RT64_MESH_RAYTRACE_FAST_TRACE) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if (flags & RT64_MESH_RAYTRACE_COMPACT) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }
    /* Updatable geometry is rebuilt often, so bias toward build speed unless
       fast trace was asked for explicitly. */
    if (out == 0) {
        out = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    return out;
}

bool MeshVK::setMesh(const void *vertexArray, int newVertexCount,
                     int newVertexStride, const unsigned int *indexArray,
                     int newIndexCount, std::string &error) {
    if (vertexArray == nullptr || indexArray == nullptr) {
        error = "setMesh called with null geometry";
        return false;
    }
    if (newVertexCount <= 0 || newIndexCount <= 0 || newVertexStride <= 0) {
        error = "setMesh called with empty geometry";
        return false;
    }
    if ((newIndexCount % 3) != 0) {
        error = "index count is not a multiple of 3";
        return false;
    }

    VmaAllocator allocator = device->getAllocator();
    const VkDeviceSize vertexBytes =
        (VkDeviceSize)newVertexCount * (VkDeviceSize)newVertexStride;
    const VkDeviceSize indexBytes =
        (VkDeviceSize)newIndexCount * sizeof(unsigned int);

    /* A refit requires identical topology. Any change in counts or stride
       forces a full rebuild, so the buffers are reallocated too. */
    const bool topologyChanged = (newVertexCount != vertexCount) ||
                                 (newIndexCount != indexCount) ||
                                 (newVertexStride != vertexStride);

    if (topologyChanged) {
        vertexBuffer.destroy(allocator);
        indexBuffer.destroy(allocator);
        blas.destroy(builder->functions(), device->getDevice(), allocator);
        blas = AccelerationStructureVK();

        const VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        /* Host visible so updates are a memcpy. Static meshes would be better
           in device-local memory with a staging copy; that is an optimisation
           for later, and on ReBAR hardware the difference is small. */
        if (!createBuffer(allocator, device->getDevice(), vertexBytes, usage,
                          true, vertexBuffer, error) ||
            !createBuffer(allocator, device->getDevice(), indexBytes, usage,
                          true, indexBuffer, error)) {
            return false;
        }
    }

    std::memcpy(vertexBuffer.mapped, vertexArray, (size_t)vertexBytes);
    std::memcpy(indexBuffer.mapped, indexArray, (size_t)indexBytes);

    vertexCount = newVertexCount;
    vertexStride = newVertexStride;
    indexCount = newIndexCount;

    if (!raytraceEnabled()) {
        refitted = false;
        return true;
    }

    TriangleGeometry geo = {};
    geo.vertexAddress = vertexBuffer.address;
    geo.indexAddress = indexBuffer.address;
    geo.vertexCount = (uint32_t)vertexCount;
    geo.vertexStride = (uint32_t)vertexStride;
    geo.indexCount = (uint32_t)indexCount;

    /* buildBottomLevel refits when the structure is already built and was
       created updatable, so this single call covers both paths. */
    refitted = blas.built && blas.updatable();
    return builder->buildBottomLevel({ geo }, vulkanBuildFlags(), blas, error);
}

} /* namespace RT64 */
