#include "rt64_inspector_vk.h"

#include "rt64_device_vk.h"
#include "rt64_scene_vk.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

#include <SDL2/SDL.h>

namespace RT64 {

InspectorVK::InspectorVK(DeviceVK *device) : device(device) {}

InspectorVK::~InspectorVK() {
    if (ready) {
        vkDeviceWaitIdle(device->getDevice());
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device->getDevice(), descriptorPool, nullptr);
    }
}

bool InspectorVK::initialize(SDL_Window *sdlWindow, VkFormat colorFormat,
                             uint32_t imageCount, std::string &error) {
    window = sdlWindow;
    if (window == nullptr) {
        error = "the inspector needs the SDL window";
        return false;
    }

    /* ImGui allocates a combined image sampler per texture it draws; the pool
       only has to cover the font atlas plus a little slack. */
    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device->getDevice(), &poolInfo, nullptr,
                               &descriptorPool) != VK_SUCCESS) {
        error = "the inspector could not create its descriptor pool";
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo init = {};
    init.Instance = device->getInstance();
    init.PhysicalDevice = device->getPhysicalDevice();
    init.Device = device->getDevice();
    init.QueueFamily = device->getGraphicsFamily();
    init.Queue = device->getGraphicsQueue();
    init.DescriptorPool = descriptorPool;
    init.MinImageCount = (imageCount < 2) ? 2 : imageCount;
    init.ImageCount = (imageCount < 2) ? 2 : imageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    /* Dynamic rendering rather than a VkRenderPass, matching the rest of the
       port. ImGui needs to be told the colour format up front for this. */
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo = {};
    init.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;

    if (!ImGui_ImplVulkan_Init(&init)) {
        error = "ImGui_ImplVulkan_Init failed";
        return false;
    }
    ready = true;
    return true;
}

bool InspectorVK::handleEvent(const void *sdlEvent) {
    if (!ready || (sdlEvent == nullptr)) {
        return false;
    }
    ImGui_ImplSDL2_ProcessEvent((const SDL_Event *)sdlEvent);
    /* Report whether the UI swallowed the input, so the game can skip its own
       handling and the camera does not move while you drag a slider. */
    const ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void InspectorVK::setLights(RT64_LIGHT *lights, int count, int maxCount) {
    lightArray = lights;
    lightCount = count;
    lightMaxCount = maxCount;
}

void InspectorVK::getLights(RT64_LIGHT *lights, int *count) const {
    if ((lights != nullptr) && (lightArray != nullptr) && (count != nullptr)) {
        for (int i = 0; i < lightCount; i++) { lights[i] = lightArray[i]; }
        *count = lightCount;
    }
}

void InspectorVK::setMaterial(RT64_MATERIAL *material, const std::string &name) {
    materialTarget = material;
    materialName = name;
}

void InspectorVK::printMessage(const std::string &message) {
    messages.push_back(message);
    if (messages.size() > 64) { messages.erase(messages.begin()); }
}

void InspectorVK::drawLightsWindow() {
    if (lightArray == nullptr) { return; }
    if (!ImGui::Begin("Lights")) { ImGui::End(); return; }

    ImGui::Text("%d of %d", lightCount, lightMaxCount);
    ImGui::Separator();
    for (int i = 0; i < lightCount; i++) {
        ImGui::PushID(i);
        char label[32];
        std::snprintf(label, sizeof(label), "Light %d", i);
        if (ImGui::CollapsingHeader(label)) {
            RT64_LIGHT &l = lightArray[i];
            ImGui::DragFloat3("Position", &l.position.x, 1.0f);
            ImGui::ColorEdit3("Diffuse", &l.diffuseColor.x);
            ImGui::ColorEdit3("Specular", &l.specularColor.x);
            ImGui::DragFloat("Attenuation radius", &l.attenuationRadius, 1.0f,
                             0.0f, 100000.0f);
            ImGui::DragFloat("Attenuation exponent", &l.attenuationExponent,
                             0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Point radius", &l.pointRadius, 0.1f, 0.0f, 1000.0f);
            ImGui::DragFloat("Shadow offset", &l.shadowOffset, 1.0f);
            ImGui::DragFloat("Flicker", &l.flickerIntensity, 0.01f, 0.0f, 1.0f);
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void InspectorVK::drawMaterialWindow() {
    if (materialTarget == nullptr) { return; }
    if (!ImGui::Begin("Material")) { ImGui::End(); return; }

    ImGui::TextUnformatted(materialName.c_str());
    ImGui::Separator();
    RT64_MATERIAL &m = *materialTarget;
    ImGui::ColorEdit3("Self light", &m.selfLight.x);
    ImGui::ColorEdit3("Specular", &m.specularColor.x);
    ImGui::DragFloat("Specular exponent", &m.specularExponent, 0.1f, 0.0f, 512.0f);
    ImGui::DragFloat("Reflection", &m.reflectionFactor, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Reflection fresnel", &m.reflectionFresnelFactor, 0.01f,
                     0.0f, 10.0f);
    ImGui::DragFloat("Reflection shine", &m.reflectionShineFactor, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Refraction", &m.refractionFactor, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Solid alpha", &m.solidAlphaMultiplier, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Shadow alpha", &m.shadowAlphaMultiplier, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Depth bias", &m.depthBias, 0.01f);
    ImGui::DragFloat("Shadow ray bias", &m.shadowRayBias, 0.01f);
    ImGui::DragFloat("Ignore normal", &m.ignoreNormalFactor, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("UV detail scale", &m.uvDetailScale, 0.01f, 0.0f, 64.0f);
    ImGui::End();
}

void InspectorVK::drawMessagesWindow() {
    if (messages.empty()) { return; }
    if (!ImGui::Begin("Messages")) { ImGui::End(); return; }
    for (const std::string &m : messages) {
        ImGui::TextUnformatted(m.c_str());
    }
    ImGui::End();
}

void InspectorVK::render(VkCommandBuffer cmd, VkImageView target,
                         VkExtent2D extent) {
    if (!ready) { return; }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    drawLightsWindow();
    drawMaterialWindow();
    drawMessagesWindow();

    ImGui::Render();

    /* LOAD, not CLEAR: the composed frame is already in this image and the UI
       goes on top of it. */
    VkRenderingAttachmentInfo color = {};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = { {0, 0}, extent };
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

} /* namespace RT64 */
