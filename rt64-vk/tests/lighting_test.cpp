/*
 * lighting_test — a known scene for judging shading and lights.
 *
 * Driven entirely through the public C ABI, so it exercises the same path the
 * game does: RT64_CreateShader generates the hit groups, the five ray passes
 * run, ComposePS resolves.
 *
 * The scene is deliberately diagnostic:
 *
 *   floor         a large plane, so shadows and falloff have somewhere to land
 *   cube          flat-shaded by construction (per-face normals)
 *   smooth sphere normal = normalized position
 *   flat sphere   IDENTICAL geometry, but each triangle carries its own face
 *                 normal
 *
 * The two spheres are the control. A finely tessellated sphere can look smooth
 * even when normals are ignored, so a matched pair at low resolution is what
 * actually proves it: if they render identically, vertex normals are not
 * reaching the shading. Resolution defaults low for that reason and can be
 * raised with the third argument.
 *
 *   overhead   a wide-radius white light above the objects. RT64 has no area
 *              light type; a large pointRadius is its soft-area equivalent,
 *              which is what the game's rt64_sphere_lights option controls.
 *   key        a bright red point light between the camera and the objects
 *
 * What each part tells you:
 *   - a FACETED sphere means vertex normals are not being interpolated in the
 *     hit group, which is the "everything looks flat" symptom
 *   - a smooth sphere but a flat cube is correct: the cube has face normals
 *   - no red on the camera-facing sides means point lights are not reaching
 *     the surface at all
 *   - red on the sides but no falloff with distance means attenuation is wrong
 *   - no shadow under the cube means shadow rays are not being traced
 */
#include "rt64/rt64.h"

#include <cmath>
#include <cstdio>
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
    void RT64_SetViewDescription(RT64_VIEW *view, RT64_VIEW_DESC desc);
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

/* Combiner 0x01200200's layout: 44-byte stride, position at 0, normal at 16,
   colour at 28. Matching it exactly is what keeps the vertex buffer and the
   shader's computed stride in agreement. */
struct Vertex {
    float f[11];
    void set(float px, float py, float pz,
             float nx, float ny, float nz,
             float r, float g, float b, float a) {
        f[0] = px; f[1] = py; f[2] = pz; f[3] = 0.0f;
        f[4] = nx; f[5] = ny; f[6] = nz;
        f[7] = r;  f[8] = g;  f[9] = b;  f[10] = a;
    }
};

RT64_MATRIX4 identity() {
    RT64_MATRIX4 m = {};
    for (int i = 0; i < 4; i++) { m.m[i][i] = 1.0f; }
    return m;
}

/* Right-handed look-at in the row-vector form RT64's shaders expect. */
RT64_MATRIX4 lookAt(const float eye[3], const float target[3], const float up[3]) {
    auto sub = [](const float a[3], const float b[3], float o[3]) {
        o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
    };
    auto norm = [](float v[3]) {
        const float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    };
    auto cross = [](const float a[3], const float b[3], float o[3]) {
        o[0] = a[1]*b[2] - a[2]*b[1];
        o[1] = a[2]*b[0] - a[0]*b[2];
        o[2] = a[0]*b[1] - a[1]*b[0];
    };
    auto dot = [](const float a[3], const float b[3]) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };

    float f[3]; sub(eye, target, f); norm(f);      /* camera looks down -Z */
    float r[3]; cross(up, f, r);     norm(r);
    float u[3]; cross(f, r, u);

    RT64_MATRIX4 m = identity();
    m.m[0][0] = r[0]; m.m[0][1] = u[0]; m.m[0][2] = f[0];
    m.m[1][0] = r[1]; m.m[1][1] = u[1]; m.m[1][2] = f[1];
    m.m[2][0] = r[2]; m.m[2][1] = u[2]; m.m[2][2] = f[2];
    m.m[3][0] = -dot(r, eye);
    m.m[3][1] = -dot(u, eye);
    m.m[3][2] = -dot(f, eye);
    return m;
}

/* A quad in the y = 0 plane, normals straight up. */
void makeFloor(float extent, std::vector<Vertex> &verts,
               std::vector<unsigned int> &indices) {
    const float c[3] = { 0.75f, 0.75f, 0.78f };
    Vertex v;
    v.set(-extent, 0.0f, -extent, 0, 1, 0, c[0], c[1], c[2], 1.0f); verts.push_back(v);
    v.set( extent, 0.0f, -extent, 0, 1, 0, c[0], c[1], c[2], 1.0f); verts.push_back(v);
    v.set( extent, 0.0f,  extent, 0, 1, 0, c[0], c[1], c[2], 1.0f); verts.push_back(v);
    v.set(-extent, 0.0f,  extent, 0, 1, 0, c[0], c[1], c[2], 1.0f); verts.push_back(v);
    const unsigned int idx[6] = { 0, 2, 1, 0, 3, 2 };
    for (unsigned int i : idx) { indices.push_back(i); }
}

/* Per-face normals: the cube should look faceted, and that is correct. */
void makeCube(float half, std::vector<Vertex> &verts,
              std::vector<unsigned int> &indices) {
    const float n[6][3] = {
        { 0, 0, 1}, { 0, 0,-1}, { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}
    };
    const float faces[6][4][3] = {
        {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}},
        {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
        {{ 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}},
        {{-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1}},
        {{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1}},
        {{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
    };
    for (int f = 0; f < 6; f++) {
        const unsigned int base = (unsigned int)verts.size();
        for (int k = 0; k < 4; k++) {
            Vertex v;
            v.set(faces[f][k][0] * half, faces[f][k][1] * half,
                  faces[f][k][2] * half, n[f][0], n[f][1], n[f][2],
                  0.85f, 0.82f, 0.80f, 1.0f);
            verts.push_back(v);
        }
        const unsigned int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (unsigned int o : order) { indices.push_back(base + o); }
    }
}

/* Face normals: every triangle gets three vertices carrying that triangle's
   own normal. This is the control against the smooth sphere. */
void makeFlatSphere(float radius, int rings, int segments,
                    std::vector<Vertex> &verts,
                    std::vector<unsigned int> &indices) {
    auto at = [&](int y, int x, float out[3]) {
        const float pitch = (float)M_PI * (float)y / (float)rings;
        const float yaw = 2.0f * (float)M_PI * (float)x / (float)segments;
        out[0] = std::sin(pitch) * std::cos(yaw);
        out[1] = std::cos(pitch);
        out[2] = std::sin(pitch) * std::sin(yaw);
    };
    auto emit = [&](const float a[3], const float b[3], const float c[3]) {
        const float e0[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float e1[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
        float n[3] = { e0[1]*e1[2] - e0[2]*e1[1],
                       e0[2]*e1[0] - e0[0]*e1[2],
                       e0[0]*e1[1] - e0[1]*e1[0] };
        const float l = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (l > 1e-6f) { n[0] /= l; n[1] /= l; n[2] /= l; }
        const float *tri[3] = { a, b, c };
        for (int k = 0; k < 3; k++) {
            Vertex v;
            v.set(tri[k][0] * radius, tri[k][1] * radius, tri[k][2] * radius,
                  n[0], n[1], n[2], 0.85f, 0.85f, 0.88f, 1.0f);
            indices.push_back((unsigned int)verts.size());
            verts.push_back(v);
        }
    };
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < segments; x++) {
            float a[3], b[3], c[3], d[3];
            at(y, x, a); at(y, x + 1, b); at(y + 1, x, c); at(y + 1, x + 1, d);
            emit(a, b, c);
            emit(c, b, d);
        }
    }
}

/* Normal = normalized position, so this is smooth-shaded by construction. If
   it renders faceted, the hit group is not interpolating vertex normals. */
void makeSphere(float radius, int rings, int segments,
                std::vector<Vertex> &verts, std::vector<unsigned int> &indices) {
    const unsigned int base = (unsigned int)verts.size();
    for (int y = 0; y <= rings; y++) {
        const float pitch = (float)M_PI * (float)y / (float)rings;
        for (int x = 0; x <= segments; x++) {
            const float yaw = 2.0f * (float)M_PI * (float)x / (float)segments;
            const float nx = std::sin(pitch) * std::cos(yaw);
            const float ny = std::cos(pitch);
            const float nz = std::sin(pitch) * std::sin(yaw);
            Vertex v;
            v.set(nx * radius, ny * radius, nz * radius, nx, ny, nz,
                  0.85f, 0.85f, 0.88f, 1.0f);
            verts.push_back(v);
        }
    }
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < segments; x++) {
            const unsigned int a = base + (unsigned int)(y * (segments + 1) + x);
            const unsigned int b = a + (unsigned int)segments + 1;
            indices.push_back(a);     indices.push_back(a + 1); indices.push_back(b);
            indices.push_back(b);     indices.push_back(a + 1); indices.push_back(b + 1);
        }
    }
}

RT64_MATERIAL diagnosticMaterial() {
    RT64_MATERIAL m = {};
    m.solidAlphaMultiplier = 1.0f;
    m.shadowAlphaMultiplier = 1.0f;
    m.diffuseColorMix = { 0.0f, 0.0f, 0.0f, 0.0f };  /* keep the vertex colour */
    m.selfLight = { 0.02f, 0.02f, 0.03f };           /* barely any ambient, so
                                                        the lights do the work */
    m.specularColor = { 1.0f, 1.0f, 1.0f };
    m.specularExponent = 24.0f;
    m.lightGroupMaskBits = 0xFFFF;   /* load-bearing: zero here means the raygen
                                        never stores the hit */
    m.diffuseTexIndex = -1;
    m.normalTexIndex = -1;
    m.specularTexIndex = -1;
    m.ignoreNormalFactor = 0.0f;
    m.uvDetailScale = 1.0f;
    return m;
}

/* Semi-glossy: a partial mirror with a Fresnel falloff, so it is dull looking
   straight down and brighter at grazing angles. This also puts the reflection
   ray pass to work, which the matte materials never exercise. */
RT64_MATERIAL glossyFloorMaterial() {
    RT64_MATERIAL m = diagnosticMaterial();
    m.reflectionFactor = 0.35f;
    m.reflectionFresnelFactor = 1.4f;
    m.reflectionShineFactor = 0.6f;
    m.specularColor = { 1.0f, 1.0f, 1.0f };
    m.specularExponent = 64.0f;
    return m;
}

RT64_INSTANCE *addInstance(RT64_SCENE *scene, RT64_MESH *mesh,
                           RT64_SHADER *shader, const RT64_MATRIX4 &transform,
                           const RT64_MATERIAL &material) {
    RT64_INSTANCE *instance = RT64_CreateInstance(scene);
    RT64_INSTANCE_DESC desc = {};
    desc.mesh = mesh;
    desc.shader = shader;
    desc.transform = transform;
    desc.previousTransform = transform;
    desc.material = material;
    RT64_SetInstanceDescription(instance, desc);
    return instance;
}

} /* namespace */

int main(int argc, char **argv) {
    if (argc > 1) { setenv("RT64_SHADER_DIR", argv[1], 1); }
    if (argc > 2) { setenv("RT64_DXC_LIB", argv[2], 1); }
    /* Low by default: the whole point is that the facets should be obvious on
       the flat sphere and absent on the smooth one. */
    int rings = (argc > 3) ? atoi(argv[3]) : 8;
    int segments = (argc > 4) ? atoi(argv[4]) : 12;
    if (rings < 3) { rings = 3; }
    if (segments < 3) { segments = 3; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "RT64 lighting test — floor, cube, smooth sphere, two lights",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);

    RT64_DEVICE *device = RT64_CreateDevice(window);
    if (device == nullptr) {
        std::fprintf(stderr, "RT64_CreateDevice: %s\n", RT64_GetLastError());
        return 1;
    }
    RT64_SCENE *scene = RT64_CreateScene(device);
    RT64_SHADER *shader = RT64_CreateShader(device, 0x01200200, 1, 0, 0, false, false);
    if (scene == nullptr || shader == nullptr) {
        std::fprintf(stderr, "setup: %s\n", RT64_GetLastError());
        return 1;
    }

    /* --- geometry ------------------------------------------------------ */
    std::vector<Vertex> fv, cv, sv, pv;
    std::vector<unsigned int> fi, ci, si, pi;
    makeFloor(12.0f, fv, fi);
    makeCube(1.0f, cv, ci);
    makeSphere(1.0f, rings, segments, sv, si);
    makeFlatSphere(1.0f, rings, segments, pv, pi);
    std::printf("  sphere resolution: %d rings x %d segments\n", rings, segments);
    std::printf("  floor        %4zu verts %4zu tris\n", fv.size(), fi.size() / 3);
    std::printf("  cube         %4zu verts %4zu tris (flat)\n", cv.size(), ci.size() / 3);
    std::printf("  sphere LEFT  %4zu verts %4zu tris (SMOOTH normals)\n",
                sv.size(), si.size() / 3);
    std::printf("  sphere RIGHT %4zu verts %4zu tris (FACE normals)\n",
                pv.size(), pi.size() / 3);

    const int meshFlags = RT64_MESH_RAYTRACE_ENABLED;
    RT64_MESH *floorMesh = RT64_CreateMesh(device, meshFlags);
    RT64_MESH *cubeMesh  = RT64_CreateMesh(device, meshFlags);
    RT64_MESH *sphereMesh = RT64_CreateMesh(device, meshFlags);
    RT64_MESH *flatSphereMesh = RT64_CreateMesh(device, meshFlags);
    RT64_SetMesh(floorMesh, fv.data(), (int)fv.size(), (int)sizeof(Vertex),
                 fi.data(), (int)fi.size());
    RT64_SetMesh(cubeMesh, cv.data(), (int)cv.size(), (int)sizeof(Vertex),
                 ci.data(), (int)ci.size());
    RT64_SetMesh(sphereMesh, sv.data(), (int)sv.size(), (int)sizeof(Vertex),
                 si.data(), (int)si.size());
    RT64_SetMesh(flatSphereMesh, pv.data(), (int)pv.size(), (int)sizeof(Vertex),
                 pi.data(), (int)pi.size());

    RT64_MATRIX4 floorAt = identity();
    RT64_MATRIX4 cubeAt = identity();
    cubeAt.m[3][0] = -4.0f; cubeAt.m[3][1] = 1.0f;   /* resting on the floor */
    RT64_MATRIX4 sphereAt = identity();
    sphereAt.m[3][0] = -0.9f; sphereAt.m[3][1] = 1.0f;
    RT64_MATRIX4 flatAt = identity();
    flatAt.m[3][0] = 2.2f; flatAt.m[3][1] = 1.0f;

    addInstance(scene, floorMesh,      shader, floorAt,  glossyFloorMaterial());
    addInstance(scene, cubeMesh,       shader, cubeAt,   diagnosticMaterial());
    addInstance(scene, sphereMesh,     shader, sphereAt, diagnosticMaterial());
    addInstance(scene, flatSphereMesh, shader, flatAt,   diagnosticMaterial());

    /* --- lights -------------------------------------------------------- */
    RT64_LIGHT lights[2] = {};

    /* Overhead soft light. A large pointRadius is RT64's area-light stand-in. */
    lights[0].position = { 0.0f, 6.0f, 0.0f };
    lights[0].diffuseColor = { 1.0f, 0.98f, 0.95f };
    lights[0].specularColor = { 1.0f, 1.0f, 1.0f };
    lights[0].attenuationRadius = 40.0f;
    lights[0].attenuationExponent = 1.0f;
    lights[0].pointRadius = 2.5f;
    lights[0].shadowOffset = 0.0f;
    lights[0].groupBits = 0xFFFF;

    /* Red key light between the camera and the objects. */
    lights[1].position = { 0.0f, 1.6f, 4.0f };
    lights[1].diffuseColor = { 1.0f, 0.12f, 0.10f };
    lights[1].specularColor = { 1.0f, 0.4f, 0.4f };
    lights[1].attenuationRadius = 18.0f;
    lights[1].attenuationExponent = 1.0f;
    lights[1].pointRadius = 0.15f;
    lights[1].shadowOffset = 0.0f;
    lights[1].groupBits = 0xFFFF;

    RT64_SetSceneLights(scene, lights, 2);
    std::printf("  overhead light at (0, 6, 0) radius 2.5 (soft)\n");
    std::printf("  red key light at (0, 1.6, 4) radius 0.15\n");

    /* --- view ---------------------------------------------------------- */
    RT64_VIEW *view = RT64_CreateView(scene);
    if (view == nullptr) {
        std::fprintf(stderr, "RT64_CreateView: %s\n", RT64_GetLastError());
        return 1;
    }
    RT64_VIEW_DESC viewDesc = {};
    viewDesc.resolutionScale = 1.0f;
    viewDesc.diSamples = 4;
    viewDesc.giSamples = 0;
    viewDesc.maxLights = 6;
    RT64_SetViewDescription(view, viewDesc);

    const float eye[3] = { -0.6f, 3.4f, 10.5f };
    const float target[3] = { -0.9f, 0.9f, 0.0f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    RT64_SetViewPerspective(view, lookAt(eye, target, up), 0.9f, 0.1f, 200.0f, false);

    std::printf("\n  Left to right: cube, SMOOTH sphere, FACE-NORMAL sphere.\n");
    std::printf("  The two spheres are identical geometry — only their normals\n");
    std::printf("  differ, so they are the control for each other.\n\n");
    std::printf("  What to look for:\n");
    std::printf("    spheres look DIFFERENT -> vertex normals are working\n");
    std::printf("    spheres look THE SAME  -> normals ignored; shading is\n");
    std::printf("                              falling back to face normals\n");
    std::printf("    no red on fronts       -> point lights never reach surfaces\n");
    std::printf("    red everywhere         -> attenuation is not applied\n");
    std::printf("    no shadows             -> shadow rays are not traced\n");
    std::printf("    no sheen on the floor  -> the reflection pass is not running\n\n");
    std::printf("  Escape to exit.\n");

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
        RT64_DrawDevice(device, 1, 16.6f);
        frames++;
    }

    std::printf("  presented %d frames\n", frames);
    RT64_DestroyView(view);
    RT64_DestroyScene(scene);
    RT64_DestroyMesh(flatSphereMesh);
    RT64_DestroyMesh(sphereMesh);
    RT64_DestroyMesh(cubeMesh);
    RT64_DestroyMesh(floorMesh);
    RT64_DestroyShader(shader);
    RT64_DestroyDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
