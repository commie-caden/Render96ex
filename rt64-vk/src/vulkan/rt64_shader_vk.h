/*
 * rt64_shader_vk — per-material shader generation.
 *
 * The generator itself is RT64's, carried over essentially verbatim: ~600
 * lines of N64 colour combiner decoding and HLSL string emission that has
 * nothing platform-specific about it. What changed is everything around it —
 * D3D12 root signatures are gone (Vulkan uses the reflected descriptor set
 * layouts), IDxcBlob became std::vector<uint32_t>, and compilation goes
 * through ShaderCompilerVK.
 *
 * Entry point names stay UTF-8: Vulkan takes const char*, so the UTF-16
 * conversion the D3D12 path needed is dropped.
 */
#ifndef RT64_SHADER_VK_H
#define RT64_SHADER_VK_H

#include <cstdint>
#include <string>
#include <vector>

#include "rt64/rt64.h"

namespace RT64 {

class ShaderCompilerVK;

class ShaderVK {
public:
    enum class Filter : int { Point, Linear };
    enum class AddressingMode : int { Wrap, Mirror, Clamp };

    /* The raster path's vertex layout, captured portably. Offsets come from
       the colour combiner, so they cannot be reconstructed at pipeline time. */
    enum class VertexAttribute { Position, Normal, TexCoord, Color };
    struct VertexAttributeDesc {
        VertexAttribute attribute;
        uint32_t offset;
        uint32_t componentCount;
    };

    struct RasterGroup {
        std::vector<VertexAttributeDesc> attributes;
        uint32_t vertexStride = 0;
        bool alphaBlend = false;
        std::vector<uint32_t> spirvVS;
        std::vector<uint32_t> spirvPS;
        std::string vertexShaderName;
        std::string pixelShaderName;
    };

    struct HitGroup {
        std::vector<uint32_t> spirv;
        std::string hitGroupName;
        std::string closestHitName;
        std::string anyHitName;
    };

    ShaderVK(ShaderCompilerVK *compiler, unsigned int shaderId, Filter filter,
             AddressingMode hAddr, AddressingMode vAddr, int flags);
    ~ShaderVK();

    const RasterGroup &getRasterGroup() const;
    HitGroup &getSurfaceHitGroup();
    HitGroup &getShadowHitGroup();
    bool hasRasterGroup() const;
    bool hasHitGroups() const;

    /* False when any permutation failed to compile; lastError names why. */
    bool isValid() const { return valid; }
    const std::string &getLastError() const { return lastError; }

private:
    unsigned int uniqueSamplerRegisterIndex(Filter filter, AddressingMode hAddr,
                                            AddressingMode vAddr);
    void generateRasterGroup(unsigned int shaderId, Filter filter,
                             AddressingMode hAddr, AddressingMode vAddr,
                             const std::string &vertexShaderName,
                             const std::string &pixelShaderName);
    void generateSurfaceHitGroup(unsigned int shaderId, Filter filter,
                                 AddressingMode hAddr, AddressingMode vAddr,
                                 bool normalMapEnabled, bool specularMapEnabled,
                                 const std::string &hitGroupName,
                                 const std::string &closestHitName,
                                 const std::string &anyHitName);
    void generateShadowHitGroup(unsigned int shaderId, Filter filter,
                                AddressingMode hAddr, AddressingMode vAddr,
                                const std::string &hitGroupName,
                                const std::string &closestHitName,
                                const std::string &anyHitName);
    void compileShaderCode(const std::string &shaderCode,
                           const std::string &entryName,
                           const std::string &profile,
                           std::vector<uint32_t> &spirv);

    ShaderCompilerVK *compiler = nullptr;
    RasterGroup rasterGroup;
    HitGroup surfaceHitGroup;
    HitGroup shadowHitGroup;
    bool valid = false;
    std::string lastError;
};

ShaderVK::Filter convertFilter(unsigned int filter);
ShaderVK::AddressingMode convertAddressingMode(unsigned int mode);

} /* namespace RT64 */

#endif /* RT64_SHADER_VK_H */
