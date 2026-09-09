#include "rt64_rt_pipeline_vk.h"
#include "rt64_device_vk.h"
#include "rt64_shader_vk.h"

namespace RT64 {

bool RayTracingPipeline::setRaygen(RayPass pass,
                                   const std::vector<uint32_t> &spirv,
                                   const std::string &entryPoint,
                                   std::string &error) {
    if (spirv.empty()) {
        error = "raygen SPIR-V is empty for " + entryPoint;
        return false;
    }
    const size_t index = (size_t)pass;
    raygenStages[index] = { spirv, entryPoint, VK_SHADER_STAGE_RAYGEN_BIT_KHR };
    raygenSet[index] = true;
    return true;
}

bool RayTracingPipeline::setMissShaders(const std::vector<uint32_t> &spirv,
                                        const std::string &surfaceMiss,
                                        const std::string &shadowMiss,
                                        std::string &error) {
    if (spirv.empty()) {
        error = "miss SPIR-V is empty";
        return false;
    }
    /* Both miss shaders live inside PrimaryRayGen, so one module supplies two
       stages. Order matters: TraceRay's miss index selects between them. */
    missStages.clear();
    missStages.push_back({ spirv, surfaceMiss, VK_SHADER_STAGE_MISS_BIT_KHR });
    missStages.push_back({ spirv, shadowMiss, VK_SHADER_STAGE_MISS_BIT_KHR });
    return true;
}

bool RayTracingPipeline::addMaterial(const ShaderVK &shader,
                                     uint32_t &firstHitGroupIndex,
                                     std::string &error) {
    const ShaderVK::HitGroup &surface =
        const_cast<ShaderVK &>(shader).getSurfaceHitGroup();
    const ShaderVK::HitGroup &shadow =
        const_cast<ShaderVK &>(shader).getShadowHitGroup();
    if (surface.spirv.empty() || shadow.spirv.empty()) {
        error = "material has no hit group SPIR-V";
        return false;
    }

    firstHitGroupIndex = (uint32_t)hitGroups.size();

    /* Surface group: closesthit plus anyhit for alpha testing. */
    const uint32_t surfaceClosest = (uint32_t)hitStages.size();
    hitStages.push_back({ surface.spirv, surface.closestHitName,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR });
    const uint32_t surfaceAny = (uint32_t)hitStages.size();
    hitStages.push_back({ surface.spirv, surface.anyHitName,
                          VK_SHADER_STAGE_ANY_HIT_BIT_KHR });
    hitGroups.push_back({ surfaceClosest, surfaceAny });

    const uint32_t shadowClosest = (uint32_t)hitStages.size();
    hitStages.push_back({ shadow.spirv, shadow.closestHitName,
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR });
    const uint32_t shadowAny = (uint32_t)hitStages.size();
    hitStages.push_back({ shadow.spirv, shadow.anyHitName,
                          VK_SHADER_STAGE_ANY_HIT_BIT_KHR });
    hitGroups.push_back({ shadowClosest, shadowAny });

    materialCount++;
    return true;
}

bool RayTracingPipeline::createModule(DeviceVK *device,
                                      const std::vector<uint32_t> &spirv,
                                      VkShaderModule &module,
                                      std::string &error) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkResult res = vkCreateShaderModule(device->getDevice(), &info, nullptr,
                                        &module);
    if (res != VK_SUCCESS) {
        error = "vkCreateShaderModule failed (" + std::to_string((int)res) + ")";
        return false;
    }
    modules.push_back(module);
    return true;
}

bool RayTracingPipeline::build(DeviceVK *device, const RayTracingFunctions &fn,
                               VkDescriptorSetLayout setLayout,
                               std::string &error) {
    for (size_t i = 0; i < (size_t)RayPass::Count; i++) {
        if (!raygenSet[i]) {
            error = "raygen pass " + std::to_string(i) + " was never supplied";
            return false;
        }
    }
    if (missStages.size() != 2) {
        error = "expected two miss shaders, have " +
                std::to_string(missStages.size());
        return false;
    }

    stages.clear();
    groups.clear();
    /* Entry point names must outlive pipeline creation, and pStages points at
       these strings rather than copying them. */
    entryNameStorage.clear();
    entryNameStorage.reserve((size_t)RayPass::Count + missStages.size() +
                             hitStages.size());

    auto addStage = [&](const PendingStage &pending) -> bool {
        VkShaderModule module = VK_NULL_HANDLE;
        if (!createModule(device, pending.spirv, module, error)) {
            return false;
        }
        entryNameStorage.push_back(pending.entryPoint);
        VkPipelineShaderStageCreateInfo stage = {};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = pending.stage;
        stage.module = module;
        stage.pName = entryNameStorage.back().c_str();
        stages.push_back(stage);
        return true;
    };

    auto generalGroup = [&](uint32_t stageIndex) {
        VkRayTracingShaderGroupCreateInfoKHR group = {};
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        group.generalShader = stageIndex;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
        groups.push_back(group);
    };

    /* Order is load-bearing: raygen records first, then miss, then hit. The
       SBT regions are laid out in that order and the dispatch indexes them. */
    for (size_t i = 0; i < (size_t)RayPass::Count; i++) {
        if (!addStage(raygenStages[i])) { return false; }
        generalGroup((uint32_t)stages.size() - 1);
    }
    for (const PendingStage &m : missStages) {
        if (!addStage(m)) { return false; }
        generalGroup((uint32_t)stages.size() - 1);
    }

    const uint32_t hitStageBase = (uint32_t)stages.size();
    for (const PendingStage &h : hitStages) {
        if (!addStage(h)) { return false; }
    }
    for (const HitGroupRef &ref : hitGroups) {
        VkRayTracingShaderGroupCreateInfoKHR group = {};
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = hitStageBase + ref.closestHit;
        group.anyHitShader = hitStageBase + ref.anyHit;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
        groups.push_back(group);
    }

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    VkResult res = vkCreatePipelineLayout(device->getDevice(), &layoutInfo,
                                          nullptr, &pipelineLayout);
    if (res != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed (" + std::to_string((int)res) + ")";
        return false;
    }

    VkRayTracingPipelineCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    info.stageCount = (uint32_t)stages.size();
    info.pStages = stages.data();
    info.groupCount = (uint32_t)groups.size();
    info.pGroups = groups.data();
    /* RT64 traces shadow rays from within a hit shader, so one level of
       recursion is required. The device reports 31 available. */
    info.maxPipelineRayRecursionDepth = 2;
    info.layout = pipelineLayout;

    const uint32_t deviceMax =
        device->getRayTracingProperties().maxRayRecursionDepth;
    if (info.maxPipelineRayRecursionDepth > deviceMax) {
        error = "device allows a recursion depth of only " +
                std::to_string(deviceMax);
        return false;
    }

    res = fn.createRayTracingPipelines(device->getDevice(), VK_NULL_HANDLE,
                                       VK_NULL_HANDLE, 1, &info, nullptr,
                                       &pipeline);
    if (res != VK_SUCCESS) {
        error = "vkCreateRayTracingPipelinesKHR failed (" +
                std::to_string((int)res) + ")";
        return false;
    }

    /* Hit groups follow the raygen and miss groups. */
    firstHitGroup = (uint32_t)RayPass::Count + (uint32_t)missStages.size();
    return true;
}

bool RayTracingPipeline::buildShaderBindingTable(
    DeviceVK *device, const RayTracingFunctions &fn,
    const std::vector<HitRecord> &hitRecords, std::string &error) {
    if (pipeline == VK_NULL_HANDLE) {
        error = "shader binding table requested before the pipeline was built";
        return false;
    }
    sbt.destroy(device->getAllocator());
    return sbt.build(device, fn, pipeline, (uint32_t)RayPass::Count,
                     (uint32_t)missStages.size(), (uint32_t)groups.size(),
                     hitRecords, error);
}

void RayTracingPipeline::destroy(DeviceVK *device) {
    VkDevice vk = device->getDevice();
    sbt.destroy(device->getAllocator());
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
    for (VkShaderModule m : modules) {
        vkDestroyShaderModule(vk, m, nullptr);
    }
    modules.clear();
}

RayTracingPipeline::~RayTracingPipeline() {
    /* destroy() needs the device, so callers must invoke it explicitly. */
}

} /* namespace RT64 */
