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

[[nodiscard]] inline bool is_valid_encoded_request_path(std::string_view path) noexcept {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '%') {
            continue;
        }
        const auto decoded = http_try_decode_percent_byte(path, i);
        if (!decoded || *decoded == '/' || *decoded == '\\') {
            return false;
        }
        i += 2;
    }
    return true;
}

// Percent-decodes an encoded request path, or a wildcard tail of one, exactly
// once into the file path it names. Routing and path-scoped middleware split
// request paths on raw '/' only, so an escape revealing '/' or '\\' would
// forge a segment boundary they never saw; that, like a malformed escape,
// yields nullopt.
[[nodiscard]] inline std::optional<std::pmr::string> decode_request_path(
    std::string_view path, std::pmr::memory_resource* resource) {
    if (!is_valid_encoded_request_path(path)) {
        return std::nullopt;
    }
    return decode_url_component(path, {.mode_ = url_decode_mode::percent, .resource_ = resource});
}

// Web static-file path policy: validate a decoded file path relative to the
// configured document root. '/' and '\\' both separate segments; a "." or ".."
// segment, an empty interior segment, a NUL, or an absolute or drive path is
// rejected (403) rather than resolved. Requests carrying a dot segment never
// reach a route, and a decoded parameter could otherwise hide one behind an
// encoded separator ("sub%2F..%2Fsecret") that no route or path scope saw, so
// every caller fails closed instead of resolving it. A final separator names
// the directory itself and is dropped. The input is borrowed when it is
// already canonical; only a '\\' separator rebuilds it in `storage`.
[[nodiscard]] inline std::optional<std::string_view> try_normalize_static_relative_path(
    std::string_view input, std::pmr::string& storage) {
    if (input.empty()) {
        return input;
    }
    if (input.front() == '/' || input.front() == '\\' || is_windows_drive_path(input)) {
        return std::nullopt;
    }

    constexpr std::string_view stop_bytes("/\\\0", 3);
    bool backslash = false;
    std::size_t cursor_value = 0;
    for (;;) {
        const auto stop = input.find_first_of(stop_bytes, cursor_value);
        const auto end = stop == std::string_view::npos ? input.size() : stop;
        const auto segment = input.substr(cursor_value, end - cursor_value);
        if (segment == "." || segment == "..") {
            return std::nullopt;
        }
        if (stop == std::string_view::npos) {
            break;
        }
        if (input[stop] == '\0' || segment.empty()) {
            return std::nullopt;
        }
        backslash = backslash || input[stop] == '\\';
        cursor_value = stop + 1;
    }

    auto canonical = input;
    if (canonical.back() == '/' || canonical.back() == '\\') {
        canonical.remove_suffix(1);
    }
    if (!backslash) {
        return canonical;
    }
    storage.assign(canonical.data(), canonical.size());
    std::ranges::replace(storage, '\\', '/');
    return storage;
}

[[nodiscard]] inline std::string_view normalize_static_relative_path(
    std::string_view input, std::pmr::string& storage) {
    const auto normalized = try_normalize_static_relative_path(input, storage);
    if (!normalized.has_value()) {
        throw_invalid_static_file_path();
    }
    return *normalized;
}

}  // namespace ruvia::detail
