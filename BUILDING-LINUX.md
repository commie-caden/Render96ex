# Building Render96ex with RT64 on Linux

    ./extract_assets.py us
    make TEXTURE_FIX=1 DISCORDRPC=0 RENDER_API=RT64 -j$(nproc)
    ./build/us_pc/sm64.us.f3dex2e

That is the whole thing. The renderer is built as part of the game build.

## What the build does

`make` with `RENDER_API=RT64` runs two sequenced sub-makes: the first fetches
DXC if needed, configures and builds `rt64-vk`, and stages `librt64.so`,
`libdxcompiler.so` and the compiled SPIR-V into `build/us_pc`; the second
builds the game. They are sequenced rather than made prerequisites of one
target because with `-j` the prerequisites of a single target run in parallel,
and the renderer has to finish first.

`make clean` removes `rt64-vk/build` as well.

Everything is resolved against the **executable's** directory, not the working
directory, so running from the project root works:

    ./build/us_pc/sm64.us.f3dex2e

`RT64_LIB`, `RT64_DXC_EXECUTABLE` and `RT64_SHADER_DIR` override the library,
compiler and shader locations.

## Two traps

**`make` does not rebuild when `RENDER_API` changes.** Objects from a previous
build are reused, so the RT64 sources are never compiled and you get a GL
binary that looks like it succeeded. `rm -rf build` when switching.

**`levels/ending/leveldata.c` must not exist.** It is not part of Render96ex;
the Makefile globs `levels/*/leveldata.c` and that file includes a `cake.inc.c`
nothing generates. It comes from upstream sm64ex. Delete it.

## Still missing

- Skybox and 2D/HUD elements: the ortho and `GFX_SEPARATE_SKYBOX` paths are not
  wired into `RT64_DrawDevice`.
- Textures are unbound, so geometry renders untextured.
- DDS texture packs unsupported; PNG only.
