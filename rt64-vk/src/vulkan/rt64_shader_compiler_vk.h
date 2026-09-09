/*
 * rt64_shader_compiler_vk — runtime HLSL to SPIR-V compilation.
 *
 * RT64 generates hit groups per material at runtime: rt64_shader.cpp builds
 * HLSL as a string for each colour combiner permutation and compiles it to a
 * lib_6_3 library. Those hit groups are what the ray tracing pipeline is
 * assembled from, so this cannot be done at build time.
 *
 * libdxcompiler.so is loaded lazily, so a build with no ray tracing still runs
 * without it.
 */
#ifndef RT64_SHADER_COMPILER_VK_H
#define RT64_SHADER_COMPILER_VK_H

#include <cstdint>
#include <string>
#include <vector>

namespace RT64 {

class ShaderCompilerVK {
public:
    ~ShaderCompilerVK();

    /* Locates and loads libdxcompiler.so. Searches, in order: an explicit
       path, $RT64_DXC_LIB, the directory beside the executable, then the
       normal loader path. */
    bool initialize(const std::string &explicitPath, std::string &error);
    bool isReady() const { return compiler != nullptr; }

    /* profile is e.g. "lib_6_3", "ps_6_3". entryPoint may be empty for
       libraries, which have no single entry. */
    bool compile(const std::string &source, const std::string &entryPoint,
                 const std::string &profile, std::vector<uint32_t> &spirv,
                 std::string &error);

    const std::string &getLibraryPath() const { return libraryPath; }

private:
    void *libraryHandle = nullptr;
    void *compiler = nullptr;       /* IDxcCompiler3 */
    void *utils = nullptr;          /* IDxcUtils */
    std::string libraryPath;
};

} /* namespace RT64 */

#endif /* RT64_SHADER_COMPILER_VK_H */
