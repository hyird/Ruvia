#include "ruvia/web/detail/app/DotenvInternal.h"

#include <stdexcept>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace ruvia::detail {

std::filesystem::path dotenvExecutableDirectory() {
#if defined(__APPLE__)
    uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    std::pmr::vector<char> buffer(size, appResource());
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("failed to resolve executable path");
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.data())).parent_path();
#else
    std::pmr::vector<char> buffer(1024, appResource());
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string_view(buffer.data(), static_cast<std::size_t>(length))).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

}  // namespace ruvia::detail
