/*
 * rt64_stub — the complete RT64 C ABI, failing cleanly.
 *
 * Phase 1 milestone. Building this proves the ported public header, the
 * dlopen loader path and the .so layout all line up with what the game
 * expects, before a single line of Vulkan exists. The real backend replaces
 * this file behind the identical ABI, so the game side never changes again.
 *
 * Every creator returns null and records a message retrievable through
 * RT64_GetLastError, which is exactly how the original signals failure.
 */
#include "rt64/rt64.h"
#include "rt64_device_vk.h"

#include <string>

#if defined(_WIN32) || defined(_WIN64)
#   define DLLEXPORT __declspec(dllexport)
#else
#   define DLLEXPORT __attribute__((visibility("default")))
#endif

namespace {
    /* The original keeps a global last-error string; match that behaviour. */
    std::string g_lastError;
}

extern "C" {

DLLEXPORT const char *RT64_GetLastError(void) {
    return g_lastError.c_str();
}

/* -------------------------------------------------------------- device */
DLLEXPORT RT64_DEVICE *RT64_CreateDevice(void *hwnd) {
    RT64::DeviceVK *device = new RT64::DeviceVK();
    std::string error;
    if (!device->initialize(hwnd, error)) {
        g_lastError = "Failed to create RT64 device: " + error;
        delete device;
        return nullptr;
    }
    g_lastError.clear();
    return (RT64_DEVICE *)device;
}
DLLEXPORT void RT64_DestroyDevice(RT64_DEVICE *device) {
    delete (RT64::DeviceVK *)device;
}
DLLEXPORT void RT64_DrawDevice(RT64_DEVICE *device, int vsyncInterval,
                               float deltaTimeMs) {
    (void)device; (void)vsyncInterval; (void)deltaTimeMs;
}

/* ------------------------------------------------------------ inspector */
DLLEXPORT RT64_INSPECTOR *RT64_CreateInspector(RT64_DEVICE *device) {
    (void)device;
    return nullptr;
}
DLLEXPORT bool RT64_HandleMessageInspector(RT64_INSPECTOR *inspector,
                                           unsigned int msg,
                                           unsigned long long wParam,
                                           long long lParam) {
    (void)inspector; (void)msg; (void)wParam; (void)lParam;
    return false;
}
DLLEXPORT void RT64_SetSceneInspector(RT64_INSPECTOR *inspector,
                                      RT64_SCENE_DESC *sceneDesc) {
    (void)inspector; (void)sceneDesc;
}
DLLEXPORT void RT64_SetMaterialInspector(RT64_INSPECTOR *inspector,
                                         RT64_MATERIAL *material,
                                         const char *materialName) {
    (void)inspector; (void)material; (void)materialName;
}
DLLEXPORT void RT64_SetLightsInspector(RT64_INSPECTOR *inspector,
                                       RT64_LIGHT *lights, int *lightCount,
                                       int maxLightCount) {
    (void)inspector; (void)lights; (void)lightCount; (void)maxLightCount;
}
DLLEXPORT void RT64_PrintClearInspector(RT64_INSPECTOR *inspector) {
    (void)inspector;
}
DLLEXPORT void RT64_PrintMessageInspector(RT64_INSPECTOR *inspector,
                                          const char *message) {
    (void)inspector; (void)message;
}
DLLEXPORT void RT64_DestroyInspector(RT64_INSPECTOR *inspector) {
    (void)inspector;
}

/* ------------------------------------------------------------- instance */
DLLEXPORT RT64_INSTANCE *RT64_CreateInstance(RT64_SCENE *scene) {
    (void)scene;
    return nullptr;
}
DLLEXPORT void RT64_SetInstanceDescription(RT64_INSTANCE *instance,
                                           RT64_INSTANCE_DESC desc) {
    (void)instance; (void)desc;
}
DLLEXPORT void RT64_DestroyInstance(RT64_INSTANCE *instance) { (void)instance; }

/* ----------------------------------------------------------------- mesh */
DLLEXPORT RT64_MESH *RT64_CreateMesh(RT64_DEVICE *device, int flags) {
    (void)device; (void)flags;
    return nullptr;
}
DLLEXPORT void RT64_SetMesh(RT64_MESH *mesh, void *vertexArray,
                            int vertexCount, int vertexStride,
                            unsigned int *indexArray, int indexCount) {
    (void)mesh; (void)vertexArray; (void)vertexCount; (void)vertexStride;
    (void)indexArray; (void)indexCount;
}
DLLEXPORT void RT64_DestroyMesh(RT64_MESH *mesh) { (void)mesh; }

/* ---------------------------------------------------------------- scene */
DLLEXPORT RT64_SCENE *RT64_CreateScene(RT64_DEVICE *device) {
    (void)device;
    return nullptr;
}
DLLEXPORT void RT64_SetSceneDescription(RT64_SCENE *scene,
                                        RT64_SCENE_DESC sceneDesc) {
    (void)scene; (void)sceneDesc;
}
DLLEXPORT void RT64_SetSceneLights(RT64_SCENE *scene, RT64_LIGHT *lightArray,
                                   int lightCount) {
    (void)scene; (void)lightArray; (void)lightCount;
}
DLLEXPORT void RT64_DestroyScene(RT64_SCENE *scene) { (void)scene; }

/* --------------------------------------------------------------- shader */
DLLEXPORT RT64_SHADER *RT64_CreateShader(RT64_DEVICE *device,
                                         unsigned int shaderId, int filter,
                                         int hAddr, int vAddr,
                                         bool normalMapEnabled,
                                         bool specularMapEnabled) {
    (void)device; (void)shaderId; (void)filter; (void)hAddr; (void)vAddr;
    (void)normalMapEnabled; (void)specularMapEnabled;
    return nullptr;
}
DLLEXPORT void RT64_DestroyShader(RT64_SHADER *shader) { (void)shader; }

/* -------------------------------------------------------------- texture */
DLLEXPORT RT64_TEXTURE *RT64_CreateTexture(RT64_DEVICE *device,
                                           RT64_TEXTURE_DESC desc) {
    (void)device; (void)desc;
    return nullptr;
}
DLLEXPORT void RT64_DestroyTexture(RT64_TEXTURE *texture) { (void)texture; }

/* ----------------------------------------------------------------- view */
DLLEXPORT RT64_VIEW *RT64_CreateView(RT64_SCENE *scene) {
    (void)scene;
    return nullptr;
}
DLLEXPORT void RT64_SetViewPerspective(RT64_VIEW *view, RT64_MATRIX4 viewMatrix,
                                       float fovRadians, float nearDist,
                                       float farDist, bool canReproject) {
    (void)view; (void)viewMatrix; (void)fovRadians; (void)nearDist;
    (void)farDist; (void)canReproject;
}
DLLEXPORT void RT64_SetViewDescription(RT64_VIEW *view, RT64_VIEW_DESC desc) {
    (void)view; (void)desc;
}
DLLEXPORT void RT64_SetViewSkyPlane(RT64_VIEW *view, RT64_TEXTURE *texture) {
    (void)view; (void)texture;
}
DLLEXPORT RT64_INSTANCE *RT64_GetViewRaytracedInstanceAt(RT64_VIEW *view,
                                                         int x, int y) {
    (void)view; (void)x; (void)y;
    return nullptr;
}
DLLEXPORT bool RT64_GetViewUpscalerSupport(RT64_VIEW *view, int upscaler) {
    /* Upscalers were dropped from this port: native resolution only. */
    (void)view; (void)upscaler;
    return false;
}
DLLEXPORT void RT64_DestroyView(RT64_VIEW *view) { (void)view; }

} /* extern "C" */
