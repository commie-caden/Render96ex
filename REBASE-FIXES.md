# Corrections to the Phase 0 rebase

My hunk classifier dropped a family of RT64 changes as "upstream drift". They
were all small and looked like noise; every one was load-bearing.

## The pattern

RT64 is configured by `GFX_*` feature macros in
`src/pc/gfx/gfx_rendering_api_config.h`. Those macros change:

- the members of `struct GfxRenderingAPI`,
- the layout of the vertex buffer `gfx_pc.c` builds,
- whether `struct GraphNode` has a `uid`,
- whether textures are identified by name.

Every translation unit must agree. RT64 propagates the config by including it
from headers that everything else already includes — and those one-line
includes are exactly what the rebase discarded.

## What was missing

| File | Missing |
|---|---|
| `src/pc/gfx/gfx_rendering_api.h` | `#include "gfx_rendering_api_config.h"` |
| `src/pc/gfx/gfx_pc.h` | `#include "gfx_rendering_api_config.h"` |
| `src/engine/graph_node.h` | `#include "pc/gfx/gfx_pc.h"` |
| `include/types.h` | `#include "pc/gfx/gfx_rendering_api.h"` |
| `src/game/skybox.c` | `#include "pc/gfx/gfx_pc.h"` |
| `data/dynos_*.cpp` | `#include "pc/gfx/gfx_pc.h"` |
| `src/pc/gfx/gfx_pc.c` | `GFX_MAX_BUFFERED`, `GFX_DISABLE_LIGHTING`, `GFX_DISABLE_TEXTURE_GEN`, and the `GFX_OUTPUT_NORMALS_TO_VBO` write |
| `data/dynos_gfx_texture.cpp` | the `GFX_REQUIRE_TEXTURE_NAME` branch of `new_texture` |
| `data/dynos_gfx_init/update/misc.cpp` | five `gfx_register_layout_graph_node` calls |

## Symptoms these caused

- **`too many initializers for 'GfxRenderingAPI'`** — the struct had 26 members
  where the initializer supplied 30, because the config never reached it.
- **`Assertion ((buf_vbo_len * 4) % vertexStride) == 0 failed`** — `gfx_pc.c`
  was omitting three normal floats per vertex that RT64's stride assumes.
- **`basic_string: construction from null`** during precache — DynOS called
  `new_texture()` with no name, because its `GFX_REQUIRE_TEXTURE_NAME` branch
  was gone.

The last one is why disabling precache moved the crash: it changed which path
reached the missing code, not whether the code was missing.
