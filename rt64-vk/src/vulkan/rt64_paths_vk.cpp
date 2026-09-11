#include "rt64_paths_vk.h"

#include <unistd.h>
#include <vector>

namespace RT64 {

std::string executableDirectory() {
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(),
                                    buffer.size() - 1);
    if (length <= 0) {
        return std::string();
    }
    buffer[(size_t)length] = '\0';
    std::string path(buffer.data());
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

std::string besideExecutable(const std::string &path) {
    if (!path.empty() && path[0] == '/') {
        return path;                       /* already absolute */
    }
    const std::string dir = executableDirectory();
    return dir.empty() ? path : (dir + "/" + path);
}

} /* namespace RT64 */
