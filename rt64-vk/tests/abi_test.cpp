/*
 * abi_test — load the built librt64.so exactly the way the game does and
 * confirm every entry point in RT64_LIBRARY resolved.
 *
 * A missing symbol here is a null function pointer that the game would call
 * at runtime, so catching it at build time is worth the few lines.
 */
#include "rt64/rt64.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "./librt64.so";

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "FAIL dlopen(%s): %s\n", path, dlerror());
        return 1;
    }

    /* Same names the loader in rt64.h looks up. */
    static const char *symbols[] = {
        "RT64_GetLastError",
        "RT64_CreateDevice", "RT64_DestroyDevice", "RT64_DrawDevice",
        "RT64_CreateView", "RT64_SetViewPerspective", "RT64_SetViewDescription",
        "RT64_SetViewSkyPlane", "RT64_GetViewRaytracedInstanceAt",
        "RT64_GetViewUpscalerSupport", "RT64_DestroyView",
        "RT64_CreateScene", "RT64_SetSceneDescription", "RT64_SetSceneLights",
        "RT64_DestroyScene",
        "RT64_CreateMesh", "RT64_SetMesh", "RT64_DestroyMesh",
        "RT64_CreateShader", "RT64_DestroyShader",
        "RT64_CreateTexture", "RT64_DestroyTexture",
        "RT64_CreateInstance", "RT64_SetInstanceDescription",
        "RT64_DestroyInstance",
        "RT64_CreateInspector", "RT64_HandleMessageInspector",
        "RT64_SetSceneInspector", "RT64_SetMaterialInspector",
        "RT64_SetLightsInspector", "RT64_PrintClearInspector",
        "RT64_PrintMessageInspector", "RT64_DestroyInspector",
    };
    const int count = (int)(sizeof(symbols) / sizeof(*symbols));

    int missing = 0;
    for (int i = 0; i < count; i++) {
        dlerror();
        void *sym = dlsym(h, symbols[i]);
        const char *err = dlerror();
        if (!sym || err) {
            std::fprintf(stderr, "  MISSING %s\n", symbols[i]);
            missing++;
        }
    }

    if (missing) {
        std::fprintf(stderr, "FAIL %d/%d symbols missing\n", missing, count);
        dlclose(h);
        return 1;
    }
    std::printf("  all %d ABI symbols resolved\n", count);

    /* Headless device creation. Both outcomes are valid and neither may
       crash: with a working driver we get a device, and on a machine with no
       usable ICD (CI, a container without GPU nodes) we must get null plus a
       non-empty error string. Asserting only one of these would fail on the
       other kind of machine. */
    typedef const char *(*GetLastErrorFn)(void);
    typedef RT64_DEVICE *(*CreateDeviceFn)(void *);
    typedef void (*DestroyDeviceFn)(RT64_DEVICE *);
    GetLastErrorFn getLastError = (GetLastErrorFn)dlsym(h, "RT64_GetLastError");
    CreateDeviceFn createDevice = (CreateDeviceFn)dlsym(h, "RT64_CreateDevice");
    DestroyDeviceFn destroyDevice =
        (DestroyDeviceFn)dlsym(h, "RT64_DestroyDevice");

    RT64_DEVICE *dev = createDevice(nullptr);
    if (dev != nullptr) {
        std::printf("  device created headless\n");
        destroyDevice(dev);
        std::printf("  device destroyed cleanly\n");
    } else {
        const char *msg = getLastError();
        if (!msg || !*msg) {
            std::fprintf(stderr,
                "FAIL CreateDevice returned null with no error message\n");
            dlclose(h);
            return 1;
        }
        std::printf("  no device available, reported cleanly:\n    %s\n", msg);
    }

    dlclose(h);
    std::printf("  ABI OK\n");
    return 0;
}
