/*
 * rt64_mesh_vk — Vulkan replacement for the D3D12 Mesh.
 *
 * Owns vertex and index buffers plus the bottom level acceleration structure
 * built from them, and maps the RT64_MESH_RAYTRACE_* flags onto Vulkan build
 * flags.
 */
#ifndef RT64_MESH_VK_H
#define RT64_MESH_VK_H

#include <string>

#include "rt64_raytracing_vk.h"

namespace RT64 {

class DeviceVK;

class MeshVK {
public:
    MeshVK(DeviceVK *device, AccelerationStructureBuilder *builder, int flags);
    ~MeshVK();

    /* Mirrors RT64_SetMesh. Rebuilds the acceleration structure, or refits it
       when the mesh was created updatable and the topology is unchanged. */
    bool setMesh(const void *vertexArray, int vertexCount, int vertexStride,
                 const unsigned int *indexArray, int indexCount,
                 std::string &error);

    bool raytraceEnabled() const;
    const AccelerationStructureVK &accelerationStructure() const { return blas; }
    int getVertexCount() const { return vertexCount; }
    int getIndexCount() const { return indexCount; }
    /* True when the last setMesh refitted rather than rebuilt. */
    bool lastBuildWasRefit() const { return refitted; }

private:
    VkBuildAccelerationStructureFlagsKHR vulkanBuildFlags() const;

    DeviceVK *device = nullptr;
    AccelerationStructureBuilder *builder = nullptr;
    int flags = 0;

    BufferVK vertexBuffer;
    BufferVK indexBuffer;
    int vertexCount = 0;
    int vertexStride = 0;
    int indexCount = 0;
    bool refitted = false;

    AccelerationStructureVK blas;
};

} /* namespace RT64 */

#endif /* RT64_MESH_VK_H */
