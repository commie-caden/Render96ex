/*
 * rt64_paths_vk — resolve files against the executable, not the process's
 * working directory.
 *
 * Windows LoadLibrary searches the application directory, so the D3D12 build
 * found rt64lib.dll and its shaders regardless of where the game was launched
 * from. Every dlopen and every data file this port loads needs the same
 * treatment, or "./build/us_pc/sm64.us.f3dex2e" from a project root silently
 * fails while "cd build/us_pc && ./sm64.us.f3dex2e" works.
 */
#ifndef RT64_PATHS_VK_H
#define RT64_PATHS_VK_H

#include <string>

namespace RT64 {

/* Directory containing the running executable, or "" if it cannot be found. */
std::string executableDirectory();

/* path, resolved against the executable directory when it is relative. */
std::string besideExecutable(const std::string &path);

} /* namespace RT64 */

#endif /* RT64_PATHS_VK_H */
