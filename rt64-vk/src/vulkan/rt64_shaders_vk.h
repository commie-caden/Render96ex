/*
 * rt64_shaders_vk — load the build-time SPIR-V and create shader modules.
 */
#ifndef RT64_SHADERS_VK_H
#define RT64_SHADERS_VK_H

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace RT64 {

enum class ShaderKind { Vertex, Pixel, Geometry, Compute, RayTracingLib };

struct ShaderInfo {
    const char *name;       /* file stem, matching <name>.spv */
    ShaderKind  kind;
};

/* The full set the backend needs, in no particular order. */
const std::vector<ShaderInfo> &allShaders();

class ShaderLibraryVK {
public:
    ~ShaderLibraryVK();

    /* directory holds the .spv files produced by the build. */
    bool load(VkDevice device, const std::string &directory,
              std::string &error);

    VkShaderModule get(const std::string &name) const;
    size_t count() const { return modules.size(); }

private:
    VkDevice device = VK_NULL_HANDLE;
    std::vector<std::pair<std::string, VkShaderModule>> modules;
};

} /* namespace RT64 */

#endif /* RT64_SHADERS_VK_H */
