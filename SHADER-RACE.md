# The vertex layout mismatch was a data race

## Symptom

    RT64 vertex layout mismatch:
      buf_vbo_len   = 180 floats (720 bytes)
      vertexStride  = 52 bytes
      useTexture=1 numInputs=1 useAlpha=1 shaderId=0x5200A00
      implied floats/vertex = 15.000

RT64 expected 13 floats per vertex; gfx_pc had written 15.

    RT64:    4 position + 3 normal + 2 UV + 1 input x 4 (alpha)  = 13
    gfx_pc:  4 position + 3 normal + 2 UV + 2 inputs x 3         = 15

Decoding 0x5200A00 by hand agrees with RT64: TEXEL0 in slot c[0][3],
INPUT_1 in c[1][3], SHADER_OPT_ALPHA set. So RT64's decode was correct — but
it was not the shader gfx_pc was drawing with.

## Cause

`gfx_rt64_render_thread_preload_shader` called
`gfx_rt64_rapi_create_and_load_new_shader`, whose last act is

    RT64.shaderProgram = new_prg;

That runs on the **render thread**. The main thread selects its shader, then
the preloader rebinds `RT64.shaderProgram` to whatever it is compiling, and the
main thread's draw computes its stride from the wrong combiner.

The console output shows the overlap directly: preload lines interleave with
texture loading and gameplay, so preloading is still running when the first
frames are drawn. On Windows the preload evidently finished first; runtime DXC
compilation on Linux is slow enough to lose the race reliably.

## Fix

Creation and binding are now separate. `gfx_rt64_create_shader_program`
registers a program without touching `RT64.shaderProgram`; only the rendering
API entry point binds, because gfx_pc calls it while selecting the shader for
the draw it is about to issue.
