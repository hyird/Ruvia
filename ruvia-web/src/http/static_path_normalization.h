#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/error.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_windows_drive_path(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Web static-file path policy: normalize a client path relative to the configured
// document root and reject every attempt to ascend above it.
[[nodiscard]] inline std::pmr::string normalize_static_relative_path(
    std::string_view input, std::pmr::polymorphic_allocator<char> allocator) {
    if (!input.empty() &&
        (input.front() == '/' || input.front() == '\\' || is_windows_drive_path(input))) {
        throw http_error({.status_ = ruvia::http_status::forbidden,
            .code_ = "forbidden",
            .message_ = "invalid static file path"});
    }

    std::pmr::string output(allocator);
    output.reserve(input.size());
    std::size_t cursor_value = 0;
    while (cursor_value <= input.size()) {
        const auto slash = input.find_first_of("/\\", cursor_value);
        const auto end = slash == std::string_view::npos ? input.size() : slash;
        const auto segment = input.substr(cursor_value, end - cursor_value);

        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (output.empty()) {
                    throw http_error({.status_ = ruvia::http_status::forbidden,
                        .code_ = "forbidden",
                        .message_ = "invalid static file path"});
                }
                const auto previous_slash = output.rfind('/');
                if (previous_slash == std::pmr::string::npos) {
                    output.clear();
                } else {
                    output.erase(previous_slash);
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
        cursor_value = slash + 1;
    }

    return output;
}

}  // namespace ruvia::detail
