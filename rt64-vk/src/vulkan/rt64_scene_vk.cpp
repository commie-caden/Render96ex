#include "rt64_scene_vk.h"
#include "rt64_device_vk.h"
#include "rt64_mesh_vk.h"

#include <algorithm>
#include <cstring>

namespace RT64 {

VkTransformMatrixKHR toVkTransform(const RT64_MATRIX4 &m) {
    VkTransformMatrixKHR out = {};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            /* Transpose: RT64 keeps translation in m[3][0..2], Vulkan wants it
               in matrix[0..2][3]. */
            out.matrix[row][col] = m.m[col][row];
        }
    }
    return out;
}

/* ---------------------------------------------------------------- instance */

InstanceVK::InstanceVK(SceneVK *s) : scene(s) {
    if (scene != nullptr) {
        scene->addInstance(this);
    }
}

InstanceVK::~InstanceVK() {
    if (scene != nullptr) {
        scene->removeInstance(this);
    }
}

void InstanceVK::setDescription(const RT64_INSTANCE_DESC &desc) {
    description = desc;
}

/* ------------------------------------------------------------------- scene */

SceneVK::SceneVK(DeviceVK *dev, AccelerationStructureBuilder *bld)
    : device(dev), builder(bld) {}

SceneVK::~SceneVK() {
    /* Instances unregister themselves on destruction; clear first so they do
       not walk a half-destroyed vector. */
    for (InstanceVK *instance : instances) {
        (void)instance;
    }
    instances.clear();
    tlas.destroy(builder->functions(), device->getDevice(),
                 device->getAllocator());
    lightBuffer.destroy(device->getAllocator());
    transformBuffer.destroy(device->getAllocator());
    materialBuffer.destroy(device->getAllocator());
}

void SceneVK::addInstance(InstanceVK *instance) {
    instances.push_back(instance);
}

void SceneVK::removeInstance(InstanceVK *instance) {
    instances.erase(std::remove(instances.begin(), instances.end(), instance),
                    instances.end());
}

bool SceneVK::setLights(const RT64_LIGHT *lights, int count,
                        std::string &error) {
    if (count < 0) {
        error = "negative light count";
        return false;
    }
    lightCount = count;
    if (count == 0) {
        return true;
    }
    if (lights == nullptr) {
        error = "null light array with a non-zero count";
        return false;
    }

    /* Grow only: light counts churn per frame, and reallocating each time
       would be wasteful. */
    if (count > lightCapacity) {
        lightBuffer.destroy(device->getAllocator());
        const VkDeviceSize bytes = sizeof(RT64_LIGHT) * (VkDeviceSize)count;
        if (!createBuffer(device->getAllocator(), device->getDevice(), bytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          true, lightBuffer, error)) {
            return false;
        }
        lightCapacity = count;
    }
    std::memcpy(lightBuffer.mapped, lights, sizeof(RT64_LIGHT) * (size_t)count);
    return true;
}

bool SceneVK::updateInstanceBuffers(std::string &error) {
    /* Must match InstanceTransforms in Instances.hlsli: three 4x4 matrices. */
    struct InstanceTransforms {
        float objectToWorld[16];
        float objectToWorldNormal[16];
        float objectToWorldPrevious[16];
    };

    std::vector<InstanceTransforms> transforms;
    std::vector<RT64_MATERIAL> materials;
    transforms.reserve(instances.size());
    materials.reserve(instances.size());

    /* Order must match the order updateTopLevel assigns instance indices in,
       since the shaders index both by InstanceIndex(). */
    for (InstanceVK *instance : instances) {
        const RT64_INSTANCE_DESC &desc = instance->getDescription();
        if (desc.mesh == nullptr) {
            continue;
        }
        MeshVK *mesh = (MeshVK *)desc.mesh;
        if (mesh->accelerationStructure().handle == VK_NULL_HANDLE) {
            continue;
        }

        InstanceTransforms t = {};
        std::memcpy(t.objectToWorld, desc.transform.m, sizeof(t.objectToWorld));
        /* The original passes the same matrix for the normal transform; it is
           correct for the rigid transforms SM64 uses. */
        std::memcpy(t.objectToWorldNormal, desc.transform.m,
                    sizeof(t.objectToWorldNormal));
        std::memcpy(t.objectToWorldPrevious, desc.previousTransform.m,
                    sizeof(t.objectToWorldPrevious));
        transforms.push_back(t);
        materials.push_back(desc.material);
    }

    packedInstances = (uint32_t)transforms.size();
    if (packedInstances == 0) {
        return true;
    }

    if (packedInstances > transformCapacity) {
        transformBuffer.destroy(device->getAllocator());
        materialBuffer.destroy(device->getAllocator());
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (!createBuffer(device->getAllocator(), device->getDevice(),
                          sizeof(InstanceTransforms) * packedInstances, usage,
                          true, transformBuffer, error) ||
            !createBuffer(device->getAllocator(), device->getDevice(),
                          sizeof(RT64_MATERIAL) * packedInstances, usage,
                          true, materialBuffer, error)) {
            return false;
        }
        transformCapacity = packedInstances;
    }

    std::memcpy(transformBuffer.mapped, transforms.data(),
                sizeof(InstanceTransforms) * packedInstances);
    std::memcpy(materialBuffer.mapped, materials.data(),
                sizeof(RT64_MATERIAL) * packedInstances);
    return true;
}

bool SceneVK::updateTopLevel(std::string &error) {
    std::vector<VkAccelerationStructureInstanceKHR> vkInstances;
    vkInstances.reserve(instances.size());

    uint32_t index = 0;
    for (InstanceVK *instance : instances) {
        const RT64_INSTANCE_DESC &desc = instance->getDescription();
        if (desc.mesh == nullptr) {
            continue;
        }
        MeshVK *mesh = (MeshVK *)desc.mesh;
        const AccelerationStructureVK &blas = mesh->accelerationStructure();
        if (blas.handle == VK_NULL_HANDLE) {
            continue;   /* raster-only instance */
        }

        VkAccelerationStructureInstanceKHR vkInstance = {};
        vkInstance.transform = toVkTransform(desc.transform);
        vkInstance.instanceCustomIndex = index;
        vkInstance.mask = 0xFF;
        /* Two hit group records per instance, matching the original's
           2 * i shader binding table offset. */
        vkInstance.instanceShaderBindingTableRecordOffset = 2 * index;
        vkInstance.flags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vkInstance.accelerationStructureReference = blas.address;
        vkInstances.push_back(vkInstance);
        index++;
    }

    raytracedCount = (uint32_t)vkInstances.size();
    if (vkInstances.empty()) {
        /* Nothing to trace against. Not an error: a scene may legitimately be
           raster-only, or empty during startup. */
        return true;
    }

    /* A TLAS is cheap to rebuild and its contents change every frame as
       objects move, so this rebuilds rather than refitting. */
    tlas.destroy(builder->functions(), device->getDevice(),
                 device->getAllocator());
    tlas = AccelerationStructureVK();
    return builder->buildTopLevel(vkInstances, tlas, error);
}

} /* namespace RT64 */
