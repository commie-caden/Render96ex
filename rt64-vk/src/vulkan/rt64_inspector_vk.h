/*
 * rt64_inspector_vk — Dear ImGui inspector.
 *
 * The D3D12 original used the Win32 ImGui backend and drove input from the
 * game's window procedure. This port uses the SDL2 backend instead, and the
 * game forwards events through RT64_HandleMessageInspector.
 *
 * The inspector draws into the swapchain image after the compose pass, using
 * dynamic rendering like everything else here, so it needs no VkRenderPass.
 */
#ifndef RT64_INSPECTOR_VK_H
#define RT64_INSPECTOR_VK_H

#include <string>
#include <vector>

#include "rt64/rt64.h"
#include "rt64_raytracing_vk.h"

struct SDL_Window;

namespace RT64 {

class DeviceVK;
class SceneVK;

class InspectorVK {
public:
    explicit InspectorVK(DeviceVK *device);
    ~InspectorVK();

    bool initialize(SDL_Window *window, VkFormat colorFormat,
                    uint32_t imageCount, std::string &error);

    /* Records the UI into the given swapchain image view. */
    void render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);

    /* SDL_Event, type-erased so the public header stays SDL-free. */
    bool handleEvent(const void *sdlEvent);

    void setScene(SceneVK *scene) { inspectedScene = scene; }
    void setLights(RT64_LIGHT *lights, int count, int maxCount);
    void getLights(RT64_LIGHT *lights, int *count) const;
    void setMaterial(RT64_MATERIAL *material, const std::string &name);

    void printMessage(const std::string &message);
    void printClear() { messages.clear(); }

    bool isReady() const { return ready; }

private:
    void drawLightsWindow();
    void drawMaterialWindow();
    void drawMessagesWindow();

    DeviceVK *device = nullptr;
    SDL_Window *window = nullptr;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    bool ready = false;

    SceneVK *inspectedScene = nullptr;

    /* Pointers the game hands over so edits write straight back into its own
       structures, which is how the original behaved. */
    RT64_LIGHT *lightArray = nullptr;
    int lightCount = 0;
    int lightMaxCount = 0;
    RT64_MATERIAL *materialTarget = nullptr;
    std::string materialName;

    std::vector<std::string> messages;
};

} /* namespace RT64 */

#endif /* RT64_INSPECTOR_VK_H */
