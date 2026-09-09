/*
 * rt64_rt_pipeline_vk — the ray tracing pipeline.
 *
 * RT64 dispatches five separate passes (primary, direct, indirect, reflection,
 * refraction) against one pipeline, exactly as the D3D12 original used a
 * single state object. Selecting a pass means pointing the shader binding
 * table's raygen region at a different record, since the spec fixes that
 * region's size to a single record.
 *
 * Hit groups are per material and come from ShaderVK, so the pipeline is
 * rebuilt whenever the set of materials changes. Each material contributes two
 * groups — surface and shadow — which is why instances carry a shader binding
 * table offset of 2 * index.
 */
#ifndef RT64_RT_PIPELINE_VK_H
#define RT64_RT_PIPELINE_VK_H

#include <string>
#include <vector>

#include "rt64_raytracing_vk.h"

namespace RT64 {

class DeviceVK;
class ShaderVK;

/* Which raygen pass to dispatch. Order defines the SBT record order. */
enum class RayPass : uint32_t {
    Primary = 0,
    Direct = 1,
    Indirect = 2,
    Reflection = 3,
    Refraction = 4,
    Count = 5,
};

class RayTracingPipeline {
public:
    ~RayTracingPipeline();

    /* spirvByName supplies the five raygen libraries and, inside
       PrimaryRayGen, the two miss shaders. */
    bool setRaygen(RayPass pass, const std::vector<uint32_t> &spirv,
                   const std::string &entryPoint, std::string &error);
    bool setMissShaders(const std::vector<uint32_t> &spirv,
                        const std::string &surfaceMiss,
                        const std::string &shadowMiss, std::string &error);

    /* Adds a material's surface and shadow hit groups. Returns the index of
       the first of the two, which is what an instance's SBT offset refers
       to. */
    bool addMaterial(const ShaderVK &shader, uint32_t &firstHitGroupIndex,
                     std::string &error);

    bool build(DeviceVK *device, const RayTracingFunctions &fn,
               VkDescriptorSetLayout setLayout, std::string &error);

    /* Separate from build() because hit records are per instance, not per
       material: the pipeline outlives a frame, the record table does not. Call
       again whenever instances or their meshes change. */
    bool buildShaderBindingTable(DeviceVK *device, const RayTracingFunctions &fn,
                                 const std::vector<HitRecord> &hitRecords,
                                 std::string &error);

    /* Group index of a material's surface hit group; +1 is its shadow group.
       This is what a HitRecord names. */
    uint32_t getMaterialGroupIndex(uint32_t materialIndex) const {
        return firstHitGroup + materialIndex * 2;
    }

    VkPipeline getPipeline() const { return pipeline; }
    VkPipelineLayout getLayout() const { return pipelineLayout; }
    const ShaderBindingTable &getSBT() const { return sbt; }
    uint32_t getMaterialCount() const { return materialCount; }
    uint32_t getGroupCount() const { return (uint32_t)groups.size(); }

    void destroy(DeviceVK *device);

private:
    bool createModule(DeviceVK *device, const std::vector<uint32_t> &spirv,
                      VkShaderModule &module, std::string &error);

    struct PendingStage {
        std::vector<uint32_t> spirv;
        std::string entryPoint;
        VkShaderStageFlagBits stage;
    };
    /* Raygen stages are held by pass so the SBT record order is deterministic
       regardless of the order they were supplied in. */
    PendingStage raygenStages[(size_t)RayPass::Count];
    bool raygenSet[(size_t)RayPass::Count] = { false };
    std::vector<PendingStage> missStages;
    std::vector<PendingStage> hitStages;
    struct HitGroupRef { uint32_t closestHit; uint32_t anyHit; };
    std::vector<HitGroupRef> hitGroups;
    uint32_t materialCount = 0;
    uint32_t firstHitGroup = 0;   /* raygen and miss groups come first */

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    std::vector<VkShaderModule> modules;
    std::vector<std::string> entryNameStorage;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    ShaderBindingTable sbt;
};

} /* namespace RT64 */

#endif /* RT64_RT_PIPELINE_VK_H */
