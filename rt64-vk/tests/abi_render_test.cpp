/*
 * abi_render_test — render through the public C ABI alone.
 *
 * Every call here is one the game makes: CreateDevice, CreateScene,
 * CreateShader, CreateMesh, SetMesh, CreateInstance, SetInstanceDescription,
 * SetSceneLights, CreateView, SetViewPerspective, DrawDevice. Nothing reaches
 * into the C++ classes.
 *
 * If this renders, gfx_rt64.cpp can drive the library unchanged.
 */
#include "rt64/rt64.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

extern "C" {
    const char *RT64_GetLastError(void);
    RT64_DEVICE *RT64_CreateDevice(void *hwnd);
    void RT64_DestroyDevice(RT64_DEVICE *device);
    void RT64_DrawDevice(RT64_DEVICE *device, int vsyncInterval, float deltaTimeMs);
    RT64_SCENE *RT64_CreateScene(RT64_DEVICE *device);
    void RT64_SetSceneLights(RT64_SCENE *scene, RT64_LIGHT *lights, int count);
    void RT64_DestroyScene(RT64_SCENE *scene);
    RT64_VIEW *RT64_CreateView(RT64_SCENE *scene);
    void RT64_SetViewPerspective(RT64_VIEW *view, RT64_MATRIX4 viewMatrix,
                                 float fovRadians, float nearDist, float farDist,
                                 bool canReproject);
    void RT64_DestroyView(RT64_VIEW *view);
    RT64_MESH *RT64_CreateMesh(RT64_DEVICE *device, int flags);
    void RT64_SetMesh(RT64_MESH *mesh, void *vertexArray, int vertexCount,
                      int vertexStride, unsigned int *indexArray, int indexCount);
    void RT64_DestroyMesh(RT64_MESH *mesh);
    RT64_SHADER *RT64_CreateShader(RT64_DEVICE *device, unsigned int shaderId,
                                   int filter, int hAddr, int vAddr,
                                   bool normalMapEnabled, bool specularMapEnabled);
    void RT64_DestroyShader(RT64_SHADER *shader);
    RT64_INSTANCE *RT64_CreateInstance(RT64_SCENE *scene);
    void RT64_SetInstanceDescription(RT64_INSTANCE *instance, RT64_INSTANCE_DESC desc);
    void RT64_DestroyInstance(RT64_INSTANCE *instance);
}

namespace {
struct Vertex { float f[11]; };   /* 44-byte stride, as combiner 0x01200200 wants */

RT64_MATRIX4 identity() {
    RT64_MATRIX4 m = {};
    for (int i = 0; i < 4; i++) { m.m[i][i] = 1.0f; }
    return m;
}
} /* namespace */

int main(int argc, char **argv) {
    if (argc > 1) { setenv("RT64_SHADER_DIR", argv[1], 1); }
    if (argc > 2) { setenv("RT64_DXC_LIB", argv[2], 1); }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "RT64 Vulkan — driven entirely through the C ABI",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    RT64_DEVICE *device = RT64_CreateDevice(window);
    if (device == nullptr) {
        std::fprintf(stderr, "RT64_CreateDevice: %s\n", RT64_GetLastError());
        return 1;
    }
    std::printf("  device created\n");

    RT64_SCENE *scene = RT64_CreateScene(device);
    RT64_SHADER *shader = RT64_CreateShader(device, 0x01200200, 1, 0, 0,
                                            false, false);
    if (scene == nullptr || shader == nullptr) {
        std::fprintf(stderr, "  setup: %s\n", RT64_GetLastError());
        return 1;
    }
    std::printf("  scene and shader created\n");

    /* Position at 0, normal at 16, colour at 28 — the layout combiner
       0x01200200 generates. */
    const float positions[3][3] = { {0,1,0}, {1,-1,0}, {-1,-1,0} };
    Vertex verts[3] = {};
    for (int v = 0; v < 3; v++) {
        verts[v].f[0] = positions[v][0];
        verts[v].f[1] = positions[v][1];
        verts[v].f[2] = positions[v][2];
        verts[v].f[6] = 1.0f;                       /* normal.z */
        for (int c = 7; c < 11; c++) { verts[v].f[c] = 1.0f; }   /* colour */
    }
    unsigned int indices[3] = { 0, 1, 2 };

    RT64_MESH *mesh = RT64_CreateMesh(device, RT64_MESH_RAYTRACE_ENABLED |
                                              RT64_MESH_RAYTRACE_FAST_TRACE);
    RT64_SetMesh(mesh, verts, 3, (int)sizeof(Vertex), indices, 3);

    RT64_INSTANCE *instance = RT64_CreateInstance(scene);
    RT64_INSTANCE_DESC desc = {};
    desc.mesh = mesh;
    desc.shader = shader;
    desc.transform = identity();
    desc.previousTransform = desc.transform;
    desc.material.solidAlphaMultiplier = 1.0f;
    desc.material.shadowAlphaMultiplier = 1.0f;
    desc.material.diffuseColorMix = { 1.0f, 1.0f, 1.0f, 1.0f };
    desc.material.selfLight = { 0.35f, 0.35f, 0.4f };
    desc.material.specularColor = { 1.0f, 1.0f, 1.0f };
    desc.material.specularExponent = 1.0f;
    desc.material.lightGroupMaskBits = 0xFFFF;
    desc.material.diffuseTexIndex = -1;
    desc.material.normalTexIndex = -1;
    desc.material.specularTexIndex = -1;
    RT64_SetInstanceDescription(instance, desc);

    RT64_LIGHT light = {};
    light.position = { 2.0f, 2.0f, 3.0f };
    light.diffuseColor = { 1.0f, 0.85f, 0.7f };
    light.specularColor = { 1.0f, 1.0f, 1.0f };
    light.attenuationRadius = 50.0f;
    light.pointRadius = 0.5f;
    light.shadowOffset = 0.0f;
    light.attenuationExponent = 1.0f;
    light.groupBits = 0xFFFF;
    RT64_SetSceneLights(scene, &light, 1);

    RT64_VIEW *view = RT64_CreateView(scene);
    if (view == nullptr) {
        std::fprintf(stderr, "  RT64_CreateView: %s\n", RT64_GetLastError());
        return 1;
    }
    RT64_MATRIX4 viewMatrix = identity();
    viewMatrix.m[3][2] = -3.0f;      /* camera at z = 3, looking down -Z */
    RT64_SetViewPerspective(view, viewMatrix, 1.047f, 0.1f, 100.0f, false);
    std::printf("  view created\n\n  Rendering through the C ABI. Escape to exit.\n\n");

    bool running = true;
    int frames = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = false; }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
        }
        if (!running) { break; }

        /* Rotate the instance so it is visibly a live render, not one frame
           held on screen. */
        const float angle = (float)frames * 0.01f;
        RT64_INSTANCE_DESC spin = desc;
        spin.previousTransform = spin.transform;
        spin.transform.m[0][0] = std::cos(angle);
        spin.transform.m[0][2] = std::sin(angle);
        spin.transform.m[2][0] = -std::sin(angle);
        spin.transform.m[2][2] = std::cos(angle);
        RT64_SetInstanceDescription(instance, spin);

        RT64_DrawDevice(device, 1, 16.6f);
        frames++;
    }

    std::printf("  presented %d frames\n", frames);
    RT64_DestroyView(view);
    RT64_DestroyInstance(instance);
    RT64_DestroyMesh(mesh);
    RT64_DestroyShader(shader);
    RT64_DestroyScene(scene);
    RT64_DestroyDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("  shut down cleanly\n");
    return 0;
}
