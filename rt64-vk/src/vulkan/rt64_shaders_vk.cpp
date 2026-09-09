/*
 * rt64_shaders_vk — SPIR-V loading.
 *
 * Shaders are compiled by the build rather than at runtime (see
 * cmake/CompileShaders.cmake). The runtime HLSL generator for the N64 colour
 * combiner permutations is separate and still needs libdxcompiler; these are
 * the fixed shaders only.
 */
#include "rt64_shaders_vk.h"

#include <cstdio>
#include <fstream>

namespace RT64 {

const std::vector<ShaderInfo> &allShaders() {
    static const std::vector<ShaderInfo> shaders = {
        { "FullScreenVS",           ShaderKind::Vertex },
        { "ComposePS",              ShaderKind::Pixel },
        { "PostProcessPS",          ShaderKind::Pixel },
        { "DebugPS",                ShaderKind::Pixel },
        { "Im3DVS",                 ShaderKind::Vertex },
        { "Im3DPS",                 ShaderKind::Pixel },
        { "Im3DGSLines",            ShaderKind::Geometry },
        { "Im3DGSPoints",           ShaderKind::Geometry },
        { "GaussianFilterRGB3x3CS", ShaderKind::Compute },
        { "GenerateMipsCS",         ShaderKind::Compute },
        { "PrimaryRayGen",          ShaderKind::RayTracingLib },
        { "DirectRayGen",           ShaderKind::RayTracingLib },
        { "IndirectRayGen",         ShaderKind::RayTracingLib },
        { "ReflectionRayGen",       ShaderKind::RayTracingLib },
        { "RefractionRayGen",       ShaderKind::RayTracingLib },
    };
    return shaders;
}

bool ShaderLibraryVK::load(VkDevice dev, const std::string &directory,
                           std::string &error) {
    device = dev;
    for (const ShaderInfo &info : allShaders()) {
        const std::string path = directory + "/" + info.name + ".spv";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            error = "cannot open " + path +
                    " (did the shader build run?)";
            return false;
        }
        const std::streamsize size = file.tellg();
        /* SPIR-V is a stream of 32-bit words; a size that is not a multiple of
           four means a truncated or non-SPIR-V file. */
        if (size <= 0 || (size % 4) != 0) {
            error = path + " is not a valid SPIR-V module (size " +
                    std::to_string((long)size) + ")";
            return false;
        }
        std::vector<char> code((size_t)size);
        file.seekg(0);
        file.read(code.data(), size);

        VkShaderModuleCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = (size_t)size;
        createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

        VkShaderModule module = VK_NULL_HANDLE;
        VkResult res = vkCreateShaderModule(device, &createInfo, nullptr,
                                            &module);
        if (res != VK_SUCCESS) {
            error = "vkCreateShaderModule failed for " + std::string(info.name) +
                    " (" + std::to_string((int)res) + ")";
            return false;
        }
        modules.emplace_back(info.name, module);
    }
    return true;
}

VkShaderModule ShaderLibraryVK::get(const std::string &name) const {
    for (const auto &entry : modules) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return VK_NULL_HANDLE;
}

ShaderLibraryVK::~ShaderLibraryVK() {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    for (auto &entry : modules) {
        vkDestroyShaderModule(device, entry.second, nullptr);
    }
}

} /* namespace RT64 */
