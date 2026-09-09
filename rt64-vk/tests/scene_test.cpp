/*
 * scene_test — scenes, instances and TLAS building through the C ABI.
 *
 * The transform conversion gets its own checks because it fails silently:
 * a transposed matrix puts geometry somewhere plausible but wrong, with no
 * validation error and no crash.
 */
#include "rt64/rt64.h"
#include "rt64_mesh_vk.h"
#include "rt64_scene_vk.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
    const char *RT64_GetLastError(void);
    RT64_DEVICE *RT64_CreateDevice(void *hwnd);
    void RT64_DestroyDevice(RT64_DEVICE *device);
    RT64_MESH *RT64_CreateMesh(RT64_DEVICE *device, int flags);
    void RT64_SetMesh(RT64_MESH *mesh, void *vertexArray, int vertexCount,
                      int vertexStride, unsigned int *indexArray, int indexCount);
    void RT64_DestroyMesh(RT64_MESH *mesh);
    RT64_SCENE *RT64_CreateScene(RT64_DEVICE *device);
    void RT64_SetSceneLights(RT64_SCENE *scene, RT64_LIGHT *lights, int count);
    void RT64_DestroyScene(RT64_SCENE *scene);
    RT64_INSTANCE *RT64_CreateInstance(RT64_SCENE *scene);
    void RT64_SetInstanceDescription(RT64_INSTANCE *instance, RT64_INSTANCE_DESC desc);
    void RT64_DestroyInstance(RT64_INSTANCE *instance);
}

namespace {

int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

struct Vertex { float x, y, z; };

RT64_MATRIX4 identity() {
    RT64_MATRIX4 m = {};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
    return m;
}

} /* namespace */

int main() {
    /* --- transform conversion, no GPU needed --------------------------- */
    {
        RT64_MATRIX4 m = identity();
        m.m[3][0] = 10.0f;   /* RT64 keeps translation in row 3 */
        m.m[3][1] = 20.0f;
        m.m[3][2] = 30.0f;
        VkTransformMatrixKHR vk = RT64::toVkTransform(m);
        /* Vulkan wants translation in column 3. */
        expect(vk.matrix[0][3] == 10.0f && vk.matrix[1][3] == 20.0f &&
               vk.matrix[2][3] == 30.0f,
               "translation moves from row 3 to column 3");
        expect(vk.matrix[0][0] == 1.0f && vk.matrix[1][1] == 1.0f &&
               vk.matrix[2][2] == 1.0f, "identity basis preserved");
    }
    {
        /* An asymmetric basis catches a missing transpose, which an identity
           matrix cannot. */
        RT64_MATRIX4 m = identity();
        m.m[0][1] = 5.0f;    /* row 0, column 1 */
        VkTransformMatrixKHR vk = RT64::toVkTransform(m);
        expect(vk.matrix[1][0] == 5.0f && vk.matrix[0][1] == 0.0f,
               "basis is transposed, not copied");
    }

    RT64_DEVICE *device = RT64_CreateDevice(nullptr);
    if (device == nullptr) {
        std::fprintf(stderr, "RT64_CreateDevice failed: %s\n", RT64_GetLastError());
        return 1;
    }
    std::printf("  device created\n");

    RT64_SCENE *scene = RT64_CreateScene(device);
    expect(scene != nullptr, "scene created");
    if (scene == nullptr) return 1;
    RT64::SceneVK *s = (RT64::SceneVK *)scene;

    /* --- lights --------------------------------------------------------- */
    {
        std::vector<RT64_LIGHT> lights(4);
        for (size_t i = 0; i < lights.size(); i++) {
            lights[i].position = { (float)i, 0.0f, 0.0f };
            lights[i].diffuseColor = { 1.0f, 1.0f, 1.0f };
            lights[i].attenuationRadius = 100.0f;
            lights[i].groupBits = 0xFFFF;
        }
        RT64_SetSceneLights(scene, lights.data(), (int)lights.size());
        expect(s->getLightCount() == 4, "four lights uploaded");
        expect(s->getLightBuffer() != VK_NULL_HANDLE, "light buffer allocated");

        RT64_SetSceneLights(scene, lights.data(), 2);
        expect(s->getLightCount() == 2, "light count shrinks without realloc");
        RT64_SetSceneLights(scene, nullptr, 0);
        expect(s->getLightCount() == 0, "zero lights accepted");
    }

    /* --- instances and TLAS --------------------------------------------- */
    const Vertex verts[3] = { {0,1,0}, {1,-1,0}, {-1,-1,0} };
    const unsigned int idx[3] = { 0, 1, 2 };

    RT64_MESH *mesh = RT64_CreateMesh(device, RT64_MESH_RAYTRACE_ENABLED);
    RT64_SetMesh(mesh, (void *)verts, 3, (int)sizeof(Vertex),
                 (unsigned int *)idx, 3);
    expect(mesh != nullptr, "mesh created");

    std::vector<RT64_INSTANCE *> created;
    for (int i = 0; i < 3; i++) {
        RT64_INSTANCE *inst = RT64_CreateInstance(scene);
        RT64_INSTANCE_DESC desc = {};
        desc.mesh = mesh;
        desc.transform = identity();
        desc.transform.m[3][0] = (float)i * 3.0f;   /* spread them out */
        RT64_SetInstanceDescription(inst, desc);
        created.push_back(inst);
    }
    expect(s->getInstances().size() == 3, "three instances registered");

    std::string error;
    expect(s->updateTopLevel(error), "TLAS built");
    expect(s->getRaytracedInstanceCount() == 3, "three raytraced instances");
    expect(s->getTopLevel().handle != VK_NULL_HANDLE, "TLAS handle valid");

    /* An instance with no mesh must be skipped, not fatal. */
    {
        RT64_INSTANCE *empty = RT64_CreateInstance(scene);
        RT64_INSTANCE_DESC desc = {};
        desc.transform = identity();
        RT64_SetInstanceDescription(empty, desc);
        expect(s->updateTopLevel(error), "TLAS rebuild survives a meshless instance");
        expect(s->getRaytracedInstanceCount() == 3, "meshless instance skipped");
        RT64_DestroyInstance(empty);
    }

    /* Destroying an instance must unregister it from the scene. Three were
       created and the meshless one has already been destroyed, so removing
       one more must leave two. */
    const size_t before = s->getInstances().size();
    RT64_DestroyInstance(created.back());
    created.pop_back();
    const size_t after = s->getInstances().size();
    if (after != before - 1) {
        std::printf("   \033[31mFAIL\033[0m destroyed instance unregistered "
                    "(%zu -> %zu, expected %zu)\n", before, after, before - 1);
        failures++;
    } else {
        std::printf("   \033[32m ok \033[0m destroyed instance unregistered "
                    "(%zu -> %zu)\n", before, after);
    }
    expect(s->updateTopLevel(error), "TLAS rebuilt after removal");
    expect(s->getRaytracedInstanceCount() == 2, "two instances remain");

    for (RT64_INSTANCE *inst : created) RT64_DestroyInstance(inst);
    RT64_DestroyScene(scene);
    RT64_DestroyMesh(mesh);
    RT64_DestroyDevice(device);

    std::printf("  %s\n", failures == 0 ? "all scene checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
