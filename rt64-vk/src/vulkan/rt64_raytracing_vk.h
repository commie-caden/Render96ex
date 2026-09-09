/*
 * rt64_raytracing_vk — acceleration structures and the shader binding table.
 *
 * Replaces nv_helpers_dx12, which the original used for BLAS/TLAS building and
 * SBT generation. Three things differ from the D3D12 original in ways that
 * matter:
 *
 *  - The KHR ray tracing entry points are not exported by the Vulkan loader.
 *    They must come from vkGetDeviceProcAddr, hence the dispatch table.
 *
 *  - Geometry and instance data are referenced by device address, not by
 *    resource binding, so every buffer involved needs
 *    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
 *
 *  - SBT strides follow shaderGroupHandleAlignment while region base addresses
 *    follow shaderGroupBaseAlignment, and the two differ (16 and 32 on RADV).
 *    D3D12's fixed 32-byte record / 64-byte table rules do not carry over, so
 *    everything here is computed from the queried device properties.
 */
#ifndef RT64_RAYTRACING_VK_H
#define RT64_RAYTRACING_VK_H

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

namespace RT64 {

class DeviceVK;

/* ------------------------------------------------------------------ loader */

struct RayTracingFunctions {
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureAddress = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRayTracingPipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR getShaderGroupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR cmdTraceRays = nullptr;

    bool load(VkDevice device, std::string &error);
};

/* ------------------------------------------------------------------ buffers */

struct BufferVK {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void *mapped = nullptr;

    void destroy(VmaAllocator allocator);
};

/* Creates a buffer, taking its device address when the usage asks for it. */
bool createBuffer(VmaAllocator allocator, VkDevice device, VkDeviceSize size,
                  VkBufferUsageFlags usage, bool hostVisible, BufferVK &out,
                  std::string &error);

/* ------------------------------------------------- acceleration structures */

struct AccelerationStructureVK {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    BufferVK storage;
    VkDeviceAddress address = 0;
    /* Kept alive for updatable structures: a refit needs scratch too, and
       reallocating it every frame would defeat the point of refitting. */
    BufferVK scratch;
    VkBuildAccelerationStructureFlagsKHR buildFlags = 0;
    bool built = false;

    bool updatable() const {
        return (buildFlags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0;
    }
    void destroy(const RayTracingFunctions &fn, VkDevice device,
                 VmaAllocator allocator);
};

struct TriangleGeometry {
    VkDeviceAddress vertexAddress = 0;
    VkDeviceAddress indexAddress = 0;
    uint32_t vertexCount = 0;
    uint32_t vertexStride = 0;
    uint32_t indexCount = 0;
    VkFormat vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
};

class AccelerationStructureBuilder {
public:
    bool initialize(DeviceVK *device, std::string &error);
    void shutdown();

    /* Builds one BLAS from a set of triangle geometries. Submits and waits;
       batching is a later concern.

       If out is already built and was created with ALLOW_UPDATE, this refits
       in place rather than rebuilding — which is the whole point of
       RT64_MESH_RAYTRACE_UPDATABLE. A refit requires the topology to be
       unchanged; only vertex positions may move. */
    bool buildBottomLevel(const std::vector<TriangleGeometry> &geometries,
                          VkBuildAccelerationStructureFlagsKHR flags,
                          AccelerationStructureVK &out, std::string &error);

    /* Convenience overload keeping the previous default flags. */
    bool buildBottomLevel(const std::vector<TriangleGeometry> &geometries,
                          AccelerationStructureVK &out, std::string &error) {
        return buildBottomLevel(geometries,
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            out, error);
    }

    /* Builds a TLAS over already-built bottom level structures. */
    bool buildTopLevel(const std::vector<VkAccelerationStructureInstanceKHR> &instances,
                       AccelerationStructureVK &out, std::string &error);

    const RayTracingFunctions &functions() const { return fn; }

private:
    bool submitBuild(const VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
                     const VkAccelerationStructureBuildRangeInfoKHR *range,
                     std::string &error);

    DeviceVK *device = nullptr;
    RayTracingFunctions fn;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t scratchAlignment = 256;
};

/* ------------------------------------------------- shader binding table */

/* One hit record per instance per ray type, not per material.
   D3D12 supplied vertexBuffer and indexBuffer through a local root signature,
   so the record carried that instance's mesh addresses alongside the handle of
   its material's hit group. Vulkan's shader record buffer works the same way,
   which means records are indexed by instance — matching the
   instanceShaderBindingTableRecordOffset of 2 * instanceIndex. */
struct HitRecord {
    uint32_t groupIndex = 0;        /* which material hit group to invoke */
    uint64_t vertexAddress = 0;     /* this instance's mesh */
    uint64_t indexAddress = 0;
};

class ShaderBindingTable {
public:
    /* raygen and miss records are plain handles; hit records carry the mesh
       addresses the shader record buffer exposes. */
    bool build(DeviceVK *device, const RayTracingFunctions &fn,
               VkPipeline pipeline, uint32_t raygenCount, uint32_t missCount,
               uint32_t groupCount, const std::vector<HitRecord> &hitRecords,
               std::string &error);
    void destroy(VmaAllocator allocator);

    /* The spec requires the raygen region's size to equal its stride, so a
       dispatch can only ever see one raygen record. Selecting among several
       means pointing the region at a different record, not enlarging it. */
    VkStridedDeviceAddressRegionKHR raygenRegion(uint32_t index = 0) const {
        VkStridedDeviceAddressRegionKHR region = raygen;
        region.deviceAddress = raygen.deviceAddress + index * raygen.stride;
        return region;
    }
    uint32_t getRaygenCount() const { return raygenCount; }
    const VkStridedDeviceAddressRegionKHR &missRegion()   const { return miss; }
    const VkStridedDeviceAddressRegionKHR &hitRegion()    const { return hit; }
    const VkStridedDeviceAddressRegionKHR &callableRegion() const { return callable; }

    /* For reporting: what the device asked of us. */
    uint32_t handleSize = 0;
    uint32_t handleAlignment = 0;
    uint32_t baseAlignment = 0;

private:
    BufferVK buffer;
    uint32_t raygenCount = 0;
    VkStridedDeviceAddressRegionKHR raygen = {};
    VkStridedDeviceAddressRegionKHR miss = {};
    VkStridedDeviceAddressRegionKHR hit = {};
    VkStridedDeviceAddressRegionKHR callable = {};
};

/* Rounds value up to the next multiple of alignment. */
inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} /* namespace RT64 */

#endif /* RT64_RAYTRACING_VK_H */
