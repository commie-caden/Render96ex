/*
 * compiler_test — runtime HLSL to SPIR-V compilation.
 *
 * This is the mechanism RT64 uses for hit groups: one library per material,
 * generated as a string and compiled while the game runs. No GPU needed.
 *
 * The binding check matters most. Runtime hit groups share a descriptor set
 * with build-time shaders, so if the runtime compiler used different register
 * shifts they would bind to the wrong slots — with no error, just wrong
 * rendering.
 */
#include "rt64_shader_compiler_vk.h"
#include "rt64_shader_hlsli.h"

#include <sstream>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

/* Shaped like what rt64_shader.cpp emits: a hit group library with a
   closesthit and an anyhit, referencing the shared descriptor bindings. */
std::string makeHitGroup(unsigned int shaderId) {
    std::string s;
    s += "struct Attributes { float2 barycentrics; };\n";
    s += "struct HitInfo { float4 colorAndDistance; };\n";
    s += "Texture2D<float4> gTextures[512] : register(t8);\n";
    s += "SamplerState gSampler : register(s0);\n";
    s += "cbuffer gParams : register(b0) { float4 resolution; };\n";
    s += "[shader(\"closesthit\")]\n";
    s += "void Shader" + std::to_string(shaderId) + "ClosestHit("
         "inout HitInfo payload, Attributes attrib) {\n";
    s += "    float4 t = gTextures[" + std::to_string(shaderId % 512) +
         "].SampleLevel(gSampler, attrib.barycentrics, 0);\n";
    s += "    payload.colorAndDistance = t * resolution.x;\n";
    s += "}\n";
    s += "[shader(\"anyhit\")]\n";
    s += "void Shader" + std::to_string(shaderId) + "AnyHit("
         "inout HitInfo payload, Attributes attrib) {\n";
    s += "    if (attrib.barycentrics.x < 0.0f) { IgnoreHit(); }\n";
    s += "}\n";
    return s;
}

/* Minimal SPIR-V walk for OpDecorate ... Binding N, so this test does not
   depend on spirv-dis being installed. */
std::vector<std::pair<uint32_t, uint32_t>> decoratedBindings(
    const std::vector<uint32_t> &spirv) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (spirv.size() < 5) return out;
    size_t i = 5;                       /* skip the header */
    while (i < spirv.size()) {
        const uint32_t opcode = spirv[i] & 0xFFFFu;
        const uint32_t length = spirv[i] >> 16;
        if (length == 0) break;
        /* 71 = OpDecorate; operands: target id, decoration, literal */
        if (opcode == 71 && length >= 4 && spirv[i + 2] == 33 /* Binding */) {
            out.push_back({ spirv[i + 1], spirv[i + 3] });
        }
        i += length;
    }
    return out;
}

} /* namespace */

int main(int argc, char **argv) {
    RT64::ShaderCompilerVK compiler;
    std::string error;
    const std::string explicitPath = (argc > 1) ? argv[1] : "";
    if (!compiler.initialize(explicitPath, error)) {
        std::fprintf(stderr, "  compiler init failed: %s\n", error.c_str());
        std::fprintf(stderr, "  hint: RT64_DXC_LIB=third_party/dxc/lib/libdxcompiler.so\n");
        return 1;
    }
    std::printf("  libdxcompiler: %s\n", compiler.getLibraryPath().c_str());

    std::vector<uint32_t> spirv;
    expect(compiler.compile(makeHitGroup(0), "", "lib_6_3", spirv, error),
           "hit group library compiles");
    if (!spirv.empty()) {
        expect(spirv[0] == 0x07230203, "output is SPIR-V (magic number)");
        std::printf("        %zu words (%zu bytes)\n", spirv.size(),
                    spirv.size() * 4);
    } else if (!error.empty()) {
        std::printf("        %s\n", error.c_str());
    }

    /* Several permutations, as a material-heavy scene would produce. */
    int compiled = 0;
    for (unsigned int id = 1; id <= 8; id++) {
        std::vector<uint32_t> out;
        std::string e;
        if (compiler.compile(makeHitGroup(id), "", "lib_6_3", out, e)) {
            compiled++;
        } else {
            std::printf("        permutation %u: %s\n", id, e.c_str());
        }
    }
    expect(compiled == 8, "eight permutations compile");

    /* The whole point of sharing shift values with the build: t8 must land on
       108, s0 on 300 and b0 on 200, exactly as the build-time shaders do. A
       mismatch here would bind runtime hit groups to the wrong slots with no
       error at all. */
    {
        std::vector<uint32_t> out;
        std::string e;
        if (compiler.compile(makeHitGroup(3), "", "lib_6_3", out, e)) {
            std::vector<uint32_t> found;
            for (const auto &b : decoratedBindings(out)) {
                found.push_back(b.second);
            }
            auto has = [&](uint32_t n) {
                for (uint32_t f : found) { if (f == n) return true; }
                return false;
            };
            std::printf("        bindings emitted:");
            for (uint32_t f : found) { std::printf(" %u", f); }
            std::printf("\n");
            expect(has(108), "gTextures (t8) shifted to 108");
            expect(has(300), "gSampler (s0) shifted to 300");
            expect(has(200), "gParams (b0) shifted to 200");
        } else {
            expect(false, "binding-shift permutation compiles");
        }
    }

    /* The real thing: RT64's own .hlsli includes, assembled the way
       rt64_shader.cpp assembles them, compiled at runtime. This exercises the
       raw-string embedding and the prologue stripping as well as the
       compiler. */
    {
        std::stringstream ss;
        ss << INCLUDE_HLSLI(MaterialsHLSLI) << std::endl;
        ss << INCLUDE_HLSLI(InstancesHLSLI) << std::endl;
        ss << INCLUDE_HLSLI(GlobalHitBuffersHLSLI) << std::endl;
        ss << "struct HitInfo { float4 colorAndDistance; };" << std::endl;
        ss << "struct Attributes { float2 barycentrics; };" << std::endl;
        ss << "[shader(\"closesthit\")]" << std::endl;
        ss << "void RealClosestHit(inout HitInfo payload, in Attributes attrib) {"
           << std::endl;
        ss << "  uint idx = getHitBufferIndex(0, uint2(1,1), uint2(8,8));"
           << std::endl;
        ss << "  gHitColor[idx] = float4(1,0,0,1);" << std::endl;
        ss << "  payload.colorAndDistance = float4(attrib.barycentrics, 0, 1);"
           << std::endl;
        ss << "}" << std::endl;

        std::vector<uint32_t> out;
        std::string e;
        const bool ok = compiler.compile(ss.str(), "", "lib_6_3", out, e);
        expect(ok, "RT64's real HLSLI includes compile at runtime");
        if (!ok) {
            std::string firstLine = e.substr(0, e.find('\n'));
            std::printf("        %s\n", firstLine.c_str());
        } else {
            std::printf("        %zu words, using getHitBufferIndex and "
                        "gHitColor\n", out.size());
        }
    }

    /* The embedded strings must not begin with a leftover conditional. The
       prologue skip is the only thing standing between the raw-string trick
       and a corrupt shader source. */
    {
        struct Embedded { const char *name; const char *text; };
        const Embedded all[] = {
            { "GlobalHitBuffers", INCLUDE_HLSLI(GlobalHitBuffersHLSLI) },
            { "Instances",        INCLUDE_HLSLI(InstancesHLSLI) },
            { "Materials",        INCLUDE_HLSLI(MaterialsHLSLI) },
            { "Ray",              INCLUDE_HLSLI(RayHLSLI) },
            { "Textures",         INCLUDE_HLSLI(TexturesHLSLI) },
        };
        bool clean = true;
        for (const Embedded &e : all) {
            const char *p = e.text;
            while (*p == '\n' || *p == '\r') { p++; }
            if (std::strncmp(p, "#else", 5) == 0 ||
                std::strncmp(p, "#endif", 6) == 0) {
                std::printf("        %s still starts with a conditional\n",
                            e.name);
                clean = false;
            }
        }
        expect(clean, "no leftover preprocessor conditionals in embedded HLSLI");
    }

    /* A compile error must be reported, not swallowed. */
    {
        std::vector<uint32_t> out;
        std::string e;
        const bool ok = compiler.compile("this is not valid HLSL", "", "lib_6_3",
                                         out, e);
        expect(!ok, "invalid HLSL is rejected");
        expect(!e.empty(), "  with a diagnostic from dxc");
        if (!e.empty()) {
            std::string firstLine = e.substr(0, e.find('\n'));
            std::printf("        %s\n", firstLine.c_str());
        }
    }

    std::printf("  %s\n", failures == 0 ? "all compiler checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
