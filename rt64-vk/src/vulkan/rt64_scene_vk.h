/*
 * rt64_scene_vk — scenes and instances.
 *
 * A scene owns its instances and the top level acceleration structure built
 * from them, plus the light buffer the ray passes read.
 */
#ifndef RT64_SCENE_VK_H
#define RT64_SCENE_VK_H

#include <string>
#include <vector>

#include "rt64/rt64.h"
#include "rt64_raytracing_vk.h"

namespace RT64 {

class DeviceVK;
class SceneVK;

/* RT64_MATRIX4 is m[4][4] row-major with translation in row 3, matching
   DirectXMath's XMMATRIX. Vulkan wants a 3x4 affine with translation in
   column 3, so the conversion is a transpose of the upper 3x4 — exactly what
   nv_helpers_dx12 did before handing it to D3D12.

   Getting this wrong puts geometry in the wrong place with no error, so it is
   a named function with its own test rather than an inline memcpy. */
VkTransformMatrixKHR toVkTransform(const RT64_MATRIX4 &m);

class InstanceVK {
public:
    InstanceVK(SceneVK *scene);
    ~InstanceVK();

    void setDescription(const RT64_INSTANCE_DESC &desc);
    const RT64_INSTANCE_DESC &getDescription() const { return description; }

private:
    SceneVK *scene = nullptr;
    RT64_INSTANCE_DESC description = {};
};

class SceneVK {
public:
    SceneVK(DeviceVK *device, AccelerationStructureBuilder *builder);
    ~SceneVK();

    void addInstance(InstanceVK *instance);
    void removeInstance(InstanceVK *instance);
    const std::vector<InstanceVK *> &getInstances() const { return instances; }

    void setDescription(const RT64_SCENE_DESC &desc) { description = desc; }
    const RT64_SCENE_DESC &getDescription() const { return description; }

    bool setLights(const RT64_LIGHT *lights, int lightCount,
                   std::string &error);
    int getLightCount() const { return lightCount; }
    VkBuffer getLightBuffer() const { return lightBuffer.buffer; }

    /* Packs per-instance transforms and materials for the shaders.
       Transforms are copied verbatim, exactly as the D3D12 original did — the
       shader-side matrix convention is DXC's, and it is the same compiler
       here, so anything other than a straight copy would change behaviour. */
    bool updateInstanceBuffers(std::string &error);
    VkBuffer getTransformBuffer() const { return transformBuffer.buffer; }
    VkBuffer getMaterialBuffer() const { return materialBuffer.buffer; }
    uint32_t getPackedInstanceCount() const { return packedInstances; }

    /* Rebuilds the TLAS from every instance whose mesh has a bottom level
       structure. Instances without ray tracing are skipped, not an error —
       the raster passes still draw them. */
    bool updateTopLevel(std::string &error);
    const AccelerationStructureVK &getTopLevel() const { return tlas; }
    uint32_t getRaytracedInstanceCount() const { return raytracedCount; }

private:
    DeviceVK *device = nullptr;
    AccelerationStructureBuilder *builder = nullptr;
    std::vector<InstanceVK *> instances;
    RT64_SCENE_DESC description = {};

    BufferVK lightBuffer;
    int lightCount = 0;
    int lightCapacity = 0;

    BufferVK transformBuffer;
    BufferVK materialBuffer;
    uint32_t packedInstances = 0;
    uint32_t transformCapacity = 0;

    AccelerationStructureVK tlas;
    uint32_t raytracedCount = 0;
};

} /* namespace RT64 */

#endif /* RT64_SCENE_VK_H */
