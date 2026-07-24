#pragma once

#include "ruvia/web/Error.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline bool isWindowsDrivePath(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Web static-file path policy: normalize a client path relative to the configured
// document root and reject every attempt to ascend above it.
[[nodiscard]] inline std::pmr::string normalizeStaticRelativePath(std::string_view input, std::pmr::polymorphic_allocator<char> allocator) {
    if (!input.empty() && (input.front() == '/' || input.front() == '\\' || isWindowsDrivePath(input))) {
        throw HttpError(ruvia::http_status::kForbidden, "forbidden", "invalid static file path");
    }

    std::pmr::string output(allocator);
    output.reserve(input.size());
    std::size_t cursor = 0;
    while (cursor <= input.size()) {
        const auto slash = input.find_first_of("/\\", cursor);
        const auto end = slash == std::string_view::npos ? input.size() : slash;
        const auto segment = input.substr(cursor, end - cursor);

        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (output.empty()) {
                    throw HttpError(ruvia::http_status::kForbidden, "forbidden", "invalid static file path");
                }
                const auto previousSlash = output.rfind('/');
                if (previousSlash == std::pmr::string::npos) {
                    output.clear();
                } else {
                    output.erase(previousSlash);
                }
            } else {
                if (!output.empty()) {
                    output.push_back('/');
                }
                output.append(segment.data(), segment.size());
            }
        }

        if (slash == std::string_view::npos) {
            break;
        }
        cursor = slash + 1;
    }

    return output;
}

}  // namespace ruvia::detail
