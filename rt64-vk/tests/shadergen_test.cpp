/*
 * shadergen_test — RT64_CreateShader against real N64 colour combiner IDs.
 *
 * The generator itself is RT64's, carried over verbatim; what this checks is
 * that its output survives the trip through DXC to SPIR-V, for the material
 * variations a real scene produces. No GPU needed.
 */
#include "rt64/rt64.h"
#include "rt64_shader_compiler_vk.h"
#include "rt64_shader_vk.h"

#include <cstdio>
#include <string>
#include <vector>

/* Shader generation is entirely CPU-side — it only needs libdxcompiler, not a
   Vulkan device. Driving ShaderVK directly keeps this runnable anywhere,
   including machines with no GPU. RT64_CreateShader wraps exactly this. */

namespace {
int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}
} /* namespace */

int main(int argc, char **argv) {
    RT64::ShaderCompilerVK compiler;
    std::string error;
    if (!compiler.initialize(argc > 1 ? argv[1] : "", error)) {
        std::fprintf(stderr, "  compiler init failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  libdxcompiler: %s\n", compiler.getLibraryPath().c_str());

    const int flags = RT64_SHADER_RASTER_ENABLED | RT64_SHADER_RAYTRACE_ENABLED;

    /* The exact shader IDs the game precompiles, lifted from precomp_shaders[]
       in gfx_pc.c. Inventing combiner values is not a meaningful test: an ID
       the game can never produce may legitimately generate invalid HLSL, and
       an earlier version of this test failed on a fabricated 0x00000000 for
       exactly that reason. */
    struct Case { unsigned int id; const char *label; };
    const Case cases[] = {
        { 0x01200200, "precomp 0"  }, { 0x00000045, "precomp 1"  },
        { 0x00000200, "precomp 2"  }, { 0x01200a00, "precomp 3"  },
        { 0x00000a00, "precomp 4"  }, { 0x01a00045, "precomp 5"  },
        { 0x00000551, "precomp 6"  }, { 0x01045045, "precomp 7"  },
        { 0x05a00a00, "precomp 8"  }, { 0x01200045, "precomp 9"  },
        { 0x05045045, "precomp 10" }, { 0x01045a00, "precomp 11" },
        { 0x01a00a00, "precomp 12" }, { 0x0000038d, "precomp 13" },
        { 0x01081081, "precomp 14" }, { 0x0120038d, "precomp 15" },
        { 0x03200045, "precomp 16" }, { 0x03200a00, "precomp 17" },
        { 0x01a00a6f, "precomp 18" }, { 0x01141045, "precomp 19" },
        { 0x07a00a00, "precomp 20" }, { 0x05200200, "precomp 21" },
        { 0x03200200, "precomp 22" }, { 0x09200200, "precomp 23" },
        { 0x0920038d, "precomp 24" }, { 0x09200045, "precomp 25" },
    };

    int made = 0;
    size_t totalWords = 0;
    for (const Case &c : cases) {
        RT64::ShaderVK *s = new RT64::ShaderVK(&compiler, c.id,
            RT64::ShaderVK::Filter::Linear,
            RT64::ShaderVK::AddressingMode::Wrap,
            RT64::ShaderVK::AddressingMode::Wrap, flags);
        if (!s->isValid()) {
            std::printf("   \033[31mFAIL\033[0m %-18s %s\n", c.label,
                        s->getLastError().substr(0, 90).c_str());
            failures++;
            delete s;
            continue;
        }
        totalWords += s->getSurfaceHitGroup().spirv.size() +
                      s->getShadowHitGroup().spirv.size();
        if (made < 3) std::printf("   \033[32m ok \033[0m %-11s hit %zu words, "
                    "shadow %zu, VS %zu, PS %zu\n", c.label,
                    s->getSurfaceHitGroup().spirv.size(),
                    s->getShadowHitGroup().spirv.size(),
                    s->getRasterGroup().spirvVS.size(),
                    s->getRasterGroup().spirvPS.size());
        if (made == 0) {
            expect(s->hasHitGroups(), "  surface and shadow hit groups present");
            expect(s->hasRasterGroup(), "  raster group present");
            expect(!s->getSurfaceHitGroup().closestHitName.empty(),
                   "  closesthit entry point named");
            expect(!s->getRasterGroup().attributes.empty(),
                   "  vertex layout captured for the pipeline builder");
            std::printf("        vertex attributes: %zu, stride %u\n",
                        s->getRasterGroup().attributes.size(),
                        s->getRasterGroup().vertexStride);
        }
        made++;
        delete s;
    }
    std::printf("        (%d of %zu shown)\n", 3,
                sizeof(cases) / sizeof(*cases));
    expect(made == (int)(sizeof(cases) / sizeof(*cases)),
           "all 26 shader IDs the game precompiles generate and compile");
    std::printf("        %zu SPIR-V words of hit groups total\n", totalWords);

    /* Normal and specular map permutations must generate distinct shaders. */
    {
        RT64::ShaderVK a(&compiler, 0x00000045,
            RT64::ShaderVK::Filter::Linear,
            RT64::ShaderVK::AddressingMode::Wrap,
            RT64::ShaderVK::AddressingMode::Wrap, flags);
        RT64::ShaderVK b(&compiler, 0x00000045,
            RT64::ShaderVK::Filter::Linear,
            RT64::ShaderVK::AddressingMode::Wrap,
            RT64::ShaderVK::AddressingMode::Wrap,
            flags | RT64_SHADER_NORMAL_MAP_ENABLED |
                    RT64_SHADER_SPECULAR_MAP_ENABLED);
        expect(a.isValid() && b.isValid(),
               "normal/specular permutations compile");
        if (a.isValid() && b.isValid()) {
            expect(a.getSurfaceHitGroup().spirv.size() !=
                   b.getSurfaceHitGroup().spirv.size(),
                   "  and differ from the plain variant");
        }
    }
    std::printf("  %s\n", failures == 0 ? "all shader generation checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
