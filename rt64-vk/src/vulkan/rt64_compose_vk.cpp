#include "rt64_compose_vk.h"
#include "rt64_device_vk.h"
#include "rt64_shader_bindings.h"
#include "rt64_view_vk.h"

#include <cstring>
#include <fstream>

namespace RT64 {

namespace {

/* ComposePS names two inputs differently from the targets the ray passes
   write: its gDirectLight is the accumulation buffer gDirectLightAccum, and
   likewise for indirect. Everything else matches by name. */
const char *composeInputToTarget(const char *composeName) {
    if (std::strcmp(composeName, "gDirectLight") == 0) {
        return "gDirectLightAccum";
    }
    if (std::strcmp(composeName, "gIndirectLight") == 0) {
        return "gIndirectLightAccum";
    }
    return composeName;
}

const ShaderBindingGroup *composeGroup() {
    for (uint32_t i = 0; i < kShaderBindingGroupCount; i++) {
        if (std::strcmp(kShaderBindingGroups[i].name, "Compose") == 0) {
            return &kShaderBindingGroups[i];
        }
    }
    return nullptr;
}

bool loadModule(VkDevice device, const std::string &path,
                VkShaderModule &module, std::string &error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        error = path + " is not valid SPIR-V";
        return false;
    }
    std::vector<char> code((size_t)size);
    file.seekg(0);
    file.read(code.data(), size);

    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = (size_t)size;
    info.pCode = reinterpret_cast<const uint32_t *>(code.data());
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for " + path;
        return false;
    }
    return true;
}

} /* namespace */

bool ComposePass::create(DeviceVK *dev, VkDescriptorSetLayout setLayout,
                         VkFormat colorFormat, const std::string &shaderDir,
                         std::string &error) {
    device = dev;
    VkDevice vk = device->getDevice();

    if (!loadModule(vk, shaderDir + "/FullScreenVS.spv", vertexModule, error) ||
        !loadModule(vk, shaderDir + "/ComposePS.spv", pixelModule, error)) {
        return false;
    }

    VkSamplerCreateInfo sampInfo = {};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(vk, &sampInfo, nullptr, &sampler) != VK_SUCCESS) {
        error = "compose sampler creation failed";
        return false;
    }

    const ShaderBindingGroup *group = composeGroup();
    if (group == nullptr) {
        error = "no Compose binding group";
        return false;
    }
    std::vector<VkDescriptorPoolSize> sizes;
    for (uint32_t i = 0; i < group->count; i++) {
        const ShaderBinding &b = group->bindings[i];
        bool merged = false;
        for (VkDescriptorPoolSize &s : sizes) {
            if (s.type == b.type) { s.descriptorCount += b.count; merged = true; break; }
        }
        if (!merged) { sizes.push_back({ b.type, b.count }); }
    }
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = (uint32_t)sizes.size();
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(vk, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        error = "compose descriptor pool creation failed";
        return false;
    }
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(vk, &allocInfo, &descriptorSet) != VK_SUCCESS) {
        error = "compose descriptor set allocation failed";
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    if (vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        error = "compose pipeline layout creation failed";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "VSMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixelModule;
    stages[1].pName = "PSMain";

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
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

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
    if (vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        error = "compose pipeline creation failed";
        return false;
    }
    return true;
}

bool ComposePass::bindTargets(ViewVK &view, std::string &error) {
    const ShaderBindingGroup *group = composeGroup();
    if (group == nullptr) {
        error = "no Compose binding group";
        return false;
    }

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(group->count);

    for (uint32_t i = 0; i < group->count; i++) {
        const ShaderBinding &b = group->bindings[i];
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = b.binding;
        write.descriptorCount = 1;
        write.descriptorType = b.type;

        if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            infos.push_back({ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED });
        } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            const RenderTarget *t = view.findTarget(composeInputToTarget(b.name));
            if (t == nullptr || t->imageView == VK_NULL_HANDLE) {
                error = std::string("compose input ") + b.name +
                        " has no matching view target";
                return false;
            }
            /* GENERAL rather than SHADER_READ_ONLY_OPTIMAL: the ray passes
               keep these as storage images, and GENERAL is legal to sample
               from, so no transition is needed between the passes. */
            infos.push_back({ VK_NULL_HANDLE, t->imageView,
                              VK_IMAGE_LAYOUT_GENERAL });
        } else {
            continue;
        }
        write.pImageInfo = &infos.back();
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(device->getDevice(), (uint32_t)writes.size(),
                           writes.data(), 0, nullptr);
    return true;
}

void ComposePass::record(VkCommandBuffer cmd, VkImageView target,
                         VkExtent2D extent) {
    VkRenderingAttachmentInfo color = {};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderingInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);

    /* Negative-height viewport: D3D clip space has +Y up, Vulkan has +Y down,
       so a fullscreen pass authored for D3D12 samples the G-buffer vertically
       mirrored. Flipping the viewport makes Vulkan match D3D's convention
       exactly, which fixes every D3D-authored pass — compose, post-process,
       debug, im3d — without editing a single shader.

       Core since Vulkan 1.1 (was VK_KHR_maintenance1). The origin moves to the
       bottom edge and the height is negated. */
    VkViewport vp = {};
    vp.x = 0.0f;
    vp.y = (float)extent.height;
    vp.width = (float)extent.width;
    vp.height = -(float)extent.height;
    vp.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = extent;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void ComposePass::destroy() {
    if (device == nullptr) { return; }
    VkDevice vk = device->getDevice();
    if (pipeline != VK_NULL_HANDLE)       { vkDestroyPipeline(vk, pipeline, nullptr); pipeline = VK_NULL_HANDLE; }
    if (pipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(vk, pipelineLayout, nullptr); pipelineLayout = VK_NULL_HANDLE; }
    if (descriptorPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(vk, descriptorPool, nullptr); descriptorPool = VK_NULL_HANDLE; }
    if (sampler != VK_NULL_HANDLE)        { vkDestroySampler(vk, sampler, nullptr); sampler = VK_NULL_HANDLE; }
    if (vertexModule != VK_NULL_HANDLE)   { vkDestroyShaderModule(vk, vertexModule, nullptr); vertexModule = VK_NULL_HANDLE; }
    if (pixelModule != VK_NULL_HANDLE)    { vkDestroyShaderModule(vk, pixelModule, nullptr); pixelModule = VK_NULL_HANDLE; }
}

ComposePass::~ComposePass() { destroy(); }

} /* namespace RT64 */
