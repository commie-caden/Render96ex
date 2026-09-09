/*
 * mesh_test — exercise the real RT64 mesh entry points through the C ABI.
 *
 * Everything here goes through RT64_CreateDevice / RT64_CreateMesh /
 * RT64_SetMesh exactly as the game would, rather than through the C++ classes,
 * so it tests the ABI as shipped.
 *
 * Covers the three cases that behave differently:
 *   - a static mesh, which builds a BLAS once
 *   - an updatable mesh whose vertices move, which must refit
 *   - an updatable mesh whose topology changes, which must fall back to a
 *     full rebuild because a refit cannot handle it
 */
#include "rt64/rt64.h"
#include "rt64_mesh_vk.h"

/* rt64.h declares only the function-pointer typedefs and the loader struct —
   the game reaches these through dlsym. This test links the library directly
   so it can inspect MeshVK state (refit vs rebuild), which the opaque C ABI
   deliberately does not expose, so it declares the entry points itself. */
extern "C" {
    const char *RT64_GetLastError(void);
    RT64_DEVICE *RT64_CreateDevice(void *hwnd);
    void RT64_DestroyDevice(RT64_DEVICE *device);
    RT64_MESH *RT64_CreateMesh(RT64_DEVICE *device, int flags);
    void RT64_SetMesh(RT64_MESH *mesh, void *vertexArray, int vertexCount,
                      int vertexStride, unsigned int *indexArray,
                      int indexCount);
    void RT64_DestroyMesh(RT64_MESH *mesh);
}

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Vertex { float x, y, z; float pad[5]; };   /* 32-byte stride, position first */

std::vector<Vertex> makeGrid(int quads, float scale) {
    std::vector<Vertex> v;
    for (int i = 0; i <= quads; i++) {
        for (int j = 0; j <= quads; j++) {
            Vertex vert = {};
            vert.x = (float)j / quads * scale - scale * 0.5f;
            vert.y = (float)i / quads * scale - scale * 0.5f;
            vert.z = 0.0f;
            v.push_back(vert);
        }
    }
    return v;
}

std::vector<unsigned int> makeIndices(int quads) {
    std::vector<unsigned int> idx;
    for (int i = 0; i < quads; i++) {
        for (int j = 0; j < quads; j++) {
            unsigned int a = i * (quads + 1) + j;
            unsigned int b = a + 1;
            unsigned int c = a + (quads + 1);
            unsigned int d = c + 1;
            idx.insert(idx.end(), { a, b, c, b, d, c });
        }
    }
    return idx;
}

int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

} /* namespace */

int main() {
    RT64_DEVICE *device = RT64_CreateDevice(nullptr);   /* headless */
    if (device == nullptr) {
        std::fprintf(stderr, "RT64_CreateDevice failed: %s\n", RT64_GetLastError());
        return 1;
    }
    std::printf("  device created\n");

    const int quads = 16;
    std::vector<Vertex> verts = makeGrid(quads, 2.0f);
    std::vector<unsigned int> indices = makeIndices(quads);
    std::printf("  grid: %zu vertices, %zu indices (%zu triangles)\n",
                verts.size(), indices.size(), indices.size() / 3);

    /* --- static mesh ---------------------------------------------------- */
    RT64_MESH *staticMesh = RT64_CreateMesh(
        device, RT64_MESH_RAYTRACE_ENABLED | RT64_MESH_RAYTRACE_FAST_TRACE);
    expect(staticMesh != nullptr, "static mesh created");
    if (staticMesh) {
        RT64_SetMesh(staticMesh, verts.data(), (int)verts.size(),
                     (int)sizeof(Vertex), indices.data(), (int)indices.size());
        RT64::MeshVK *m = (RT64::MeshVK *)staticMesh;
        expect(m->accelerationStructure().handle != VK_NULL_HANDLE,
               "static mesh built a BLAS");
        expect(!m->accelerationStructure().updatable(),
               "static BLAS is not marked updatable");
        expect(!m->lastBuildWasRefit(), "first build is a full build");
    }

    /* --- updatable mesh, vertices move ---------------------------------- */
    RT64_MESH *dynamicMesh = RT64_CreateMesh(
        device, RT64_MESH_RAYTRACE_ENABLED | RT64_MESH_RAYTRACE_UPDATABLE);
    expect(dynamicMesh != nullptr, "updatable mesh created");
    if (dynamicMesh) {
        RT64::MeshVK *m = (RT64::MeshVK *)dynamicMesh;
        RT64_SetMesh(dynamicMesh, verts.data(), (int)verts.size(),
                     (int)sizeof(Vertex), indices.data(), (int)indices.size());
        expect(m->accelerationStructure().updatable(),
               "updatable BLAS carries ALLOW_UPDATE");
        expect(!m->lastBuildWasRefit(), "first build is not a refit");

        for (Vertex &v : verts) { v.z += 0.25f; }
        RT64_SetMesh(dynamicMesh, verts.data(), (int)verts.size(),
                     (int)sizeof(Vertex), indices.data(), (int)indices.size());
        expect(m->lastBuildWasRefit(),
               "moving vertices refits instead of rebuilding");
        expect(m->accelerationStructure().handle != VK_NULL_HANDLE,
               "BLAS still valid after refit");
    }

    /* --- updatable mesh, topology changes ------------------------------- */
    if (dynamicMesh) {
        RT64::MeshVK *m = (RT64::MeshVK *)dynamicMesh;
        std::vector<Vertex> smaller = makeGrid(4, 2.0f);
        std::vector<unsigned int> smallerIdx = makeIndices(4);
        RT64_SetMesh(dynamicMesh, smaller.data(), (int)smaller.size(),
                     (int)sizeof(Vertex), smallerIdx.data(),
                     (int)smallerIdx.size());
        expect(!m->lastBuildWasRefit(),
               "changing topology falls back to a full rebuild");
        expect(m->getIndexCount() == (int)smallerIdx.size(),
               "mesh reports the new index count");
    }

    /* --- rejection paths ------------------------------------------------ */
    if (staticMesh) {
        RT64_SetMesh(staticMesh, verts.data(), (int)verts.size(),
                     (int)sizeof(Vertex), indices.data(), 5);   /* not /3 */
        expect(std::string(RT64_GetLastError()).find("multiple of 3") != std::string::npos,
               "non-triangular index count is rejected");
    }

    RT64_DestroyMesh(dynamicMesh);
    RT64_DestroyMesh(staticMesh);
    RT64_DestroyDevice(device);
    std::printf("  %s\n", failures == 0 ? "all mesh checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
