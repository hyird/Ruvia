#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/url_encoding.h"
#include "ruvia/web/error.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_windows_drive_path(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[noreturn]] inline void throw_invalid_static_file_path() {
    throw http_error({.status_ = ruvia::http_status::forbidden,
        .code_ = "forbidden",
        .message_ = "invalid static file path"});
}

[[nodiscard]] inline std::size_t count_path_separators(std::string_view path) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(path, [](char c) noexcept { return c == '/' || c == '\\'; }));
}

// Percent-decodes an encoded request path, or a wildcard tail of one, exactly
// once into the file path it names. Routing and path-scoped middleware split
// request paths on raw '/' only, so an escape revealing '/' or '\\' would
// forge a segment boundary they never saw; that, like a malformed escape,
// yields nullopt.
[[nodiscard]] inline std::optional<std::pmr::string> decode_request_path(
    std::string_view path, std::pmr::memory_resource* resource) {
    auto decoded = decode_url_component(path, {.mode_ = url_decode_mode::percent, .resource_ = resource});
    if (decoded && count_path_separators(*decoded) != count_path_separators(path)) {
        decoded.reset();
    }
    return decoded;
}

// Web static-file path policy: normalize a decoded file path relative to the
// configured document root and reject every attempt to ascend above it. One
// forward scan: the canonical prefix ('/'-separated, no empty or dot segment)
// is borrowed and only the bytes from the first rewrite on ('\\', an empty
// segment, "." or "..") are rebuilt in `storage`. A NUL cannot occur in a
// filename and is rejected.
[[nodiscard]] inline std::string_view normalize_static_relative_path(
    std::string_view input, std::pmr::string& storage) {
    if (input.empty()) {
        return input;
    }
    if (input.front() == '/' || input.front() == '\\' || is_windows_drive_path(input)) {
        throw_invalid_static_file_path();
    }

    constexpr std::string_view stop_bytes("/\\\0", 3);
    std::size_t cursor_value = 0;
    for (;;) {
        const auto stop = input.find_first_of(stop_bytes, cursor_value);
        const auto end = stop == std::string_view::npos ? input.size() : stop;
        if (stop != std::string_view::npos && input[stop] == '\0') {
            throw_invalid_static_file_path();
        }
        const auto segment = input.substr(cursor_value, end - cursor_value);
        if (segment.empty() || segment == "." || segment == ".." ||
            (stop != std::string_view::npos && input[stop] == '\\')) {
            break;
        }
        if (stop == std::string_view::npos) {
            return input;
        }
        cursor_value = stop + 1;
    }

    // input[0, cursor_value) is canonical and, when non-empty, ends with '/'.
    storage.clear();
    storage.reserve(input.size());
    storage.append(input.data(), cursor_value == 0 ? 0 : cursor_value - 1);
    while (cursor_value <= input.size()) {
        const auto stop = input.find_first_of(stop_bytes, cursor_value);
        const auto end = stop == std::string_view::npos ? input.size() : stop;
        if (stop != std::string_view::npos && input[stop] == '\0') {
            throw_invalid_static_file_path();
        }
        const auto segment = input.substr(cursor_value, end - cursor_value);

        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (storage.empty()) {
                    throw_invalid_static_file_path();
                }
                const auto previous_slash = storage.rfind('/');
                if (previous_slash == std::pmr::string::npos) {
                    storage.clear();
                } else {
                    storage.erase(previous_slash);
                }
            } else {
                if (!storage.empty()) {
                    storage.push_back('/');
                }
                storage.append(segment.data(), segment.size());
            }
        }

        if (stop == std::string_view::npos) {
            break;
        }
        cursor_value = stop + 1;
    }

    return storage;
}

}  // namespace ruvia::detail
