#pragma once

#include "ruvia/http/Error.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

// True if `path` begins with a Windows drive specifier (e.g. "C:..."), which
// would make an otherwise-relative static path absolute on Windows.
[[nodiscard]] inline bool isWindowsDrivePath(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Normalizes a client-supplied static file path against the document root: it
// drops empty and "." segments and applies ".." within the root, returning a
// root-relative, '/'-joined path (possibly empty, meaning the root itself).
//
// This is the static-file-serving path-traversal defense, so it is deliberately
// strict: an absolute path (leading '/' or '\\', or a Windows drive letter) and
// any ".." that would ascend above the root are rejected with 403. Both '/' and
// '\\' are treated as segment separators so backslash traversal cannot slip a
// ".." past the check on any platform.
[[nodiscard]] inline std::pmr::string normalizeStaticRelativePath(
    std::string_view input,
    std::pmr::polymorphic_allocator<char> allocator) {
    if (!input.empty() && (input.front() == '/' || input.front() == '\\' || isWindowsDrivePath(input))) {
        throw HttpError(403, "forbidden", "invalid static file path");
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
                    throw HttpError(403, "forbidden", "invalid static file path");
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
