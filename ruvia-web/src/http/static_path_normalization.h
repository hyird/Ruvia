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

[[noreturn]] inline void throw_invalid_static_file_path() {
    throw http_error({.status_ = ruvia::http_status::forbidden,
        .code_ = "forbidden",
        .message_ = "invalid static file path"});
}

// A client path normalized against a document root. `path_` borrows the input
// when it is already canonical ('/'-separated, no empty or dot segment) -- the
// common case -- and otherwise points into the caller's storage.
struct static_relative_path final {
    std::string_view path_;
    // '/' and '\\' bytes the input carried; a separator that percent-decoding
    // revealed changes this count against the raw request path.
    std::size_t separator_count_;
};

// Web static-file path policy: normalize a client path relative to the
// configured document root and reject every attempt to ascend above it. One
// forward scan: the canonical prefix is borrowed and only the bytes from the
// first rewrite on ('\\', an empty segment, "." or "..") are rebuilt in
// `storage`. A NUL cannot occur in a filename and is rejected.
[[nodiscard]] inline static_relative_path normalize_static_relative_path(
    std::string_view input, std::pmr::string& storage) {
    if (input.empty()) {
        return {input, 0};
    }
    if (input.front() == '/' || input.front() == '\\' || is_windows_drive_path(input)) {
        throw_invalid_static_file_path();
    }

    constexpr std::string_view stop_bytes("/\\\0", 3);
    std::size_t separator_count = 0;
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
            return {input, separator_count};
        }
        ++separator_count;
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
        ++separator_count;
        cursor_value = stop + 1;
    }

    return {storage, separator_count};
}

}  // namespace ruvia::detail
