# rt64-vk — Vulkan port of the RT64 path tracer

Port of `DarioSamo/sm64rt-legacy-renderer` (MIT) from D3D12/DXR to Vulkan,
targeting Linux. Replaces the Visual Studio solution with CMake.

## Status

**Phase 1, step 1 complete**: the ABI stub builds, links and loads.

- `librt64.so` exports all 33 RT64 entry points.
- `ctest` dlopens it the way the game does and checks every symbol resolves.
- Clean build, zero warnings under `-Wall -Wextra`.
- No Windows or D3D dependencies remain in the link.

Every entry point currently fails cleanly, reporting through
`RT64_GetLastError`. The Vulkan backend fills in behind this same ABI, so the
game side does not change again.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

`-DRT64_BUILD_STUB=OFF` switches to the real backend once it exists.

## Layout

```
include/rt64/rt64.h    public C ABI — ported, unchanged in shape
src/vulkan/            the Vulkan backend (currently just the stub)
src/*.cpp              the D3D12 originals, awaiting port
src/shaders/           HLSL, compiles to SPIR-V unmodified
tests/abi_test.cpp     symbol-resolution and clean-failure test
```

## What changed in the public header

Deliberately minimal — only four Windows types appeared in the whole API, and
`RT64_CreateDevice` already took a `void *` window handle.

- `<Windows.h>` guarded; `<dlfcn.h>` on POSIX.
- `HMODULE handle` → `void *handle`.
- `UINT`/`WPARAM`/`LPARAM` in the inspector message hook → `RT64_MSG`,
  `RT64_WPARAM`, `RT64_LPARAM` on POSIX, native types on Windows.
- `LoadLibrary`/`GetProcAddress` → `dlopen`/`dlsym` behind an `RT64_SYM`
  macro, so all 33 lookup lines stayed as written.
- `FreeLibrary` → `dlclose`; `FormatMessageA` → `dlerror`.
- `RT64_LIBRARY` is now zeroed before use. On a failed load the original left
  the function pointers as stack garbage; the game checks `handle == 0` and
  aborts, so it was never dereferenced, but null is the safer contract.

**The runtime-loading design was kept**, contrary to what I suggested when
scoping. Linking directly would have meant editing `gfx_rt64.cpp` as well;
POSIX-ifying the loader instead leaves `RT64_LoadLibrary()` and all 45 call
sites in the game untouched. The .so name the loader looks for is
`librt64.so`, next to the executable or on the library path.

## Dropped

`rt64_dlss`, `rt64_xess`, `rt64_fsr`, `rt64_upscaler` and `rt64_optimus` —
843 lines. `RT64_GetViewUpscalerSupport` still exports and returns false, so
the game's upscaler menu degrades to native resolution rather than breaking.

7,033 lines carried forward.

## Shader pipeline

All 15 shaders compile to valid SPIR-V unmodified via DXC. See
`check_shaders.sh` in the Phase 0 tooling for the exact invocation; the two
flags that matter are `-HV 2018` and the `-fvk-*-shift` set.
