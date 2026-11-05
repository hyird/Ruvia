#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"

namespace ruvia::detail {

// The one compressibility policy shared by dynamic response compression and
// static precompression. It is an allowlist of text-like media types
// (Pingora's compressible() is likewise an allowlist): an unknown or missing
// Content-Type stays identity, and any type naming a zip container is already
// compressed.
[[nodiscard]] inline bool http_media_type_compressible(std::string_view content_type_value) noexcept {
    const auto media_type = http_media_type_only(content_type_value);
    if (media_type.empty()) {
        return false;
    }
    const auto starts_with = [&](std::string_view prefix) noexcept {
        return media_type.size() >= prefix.size() &&
               http_ascii_equals_ignore_case(media_type.substr(0, prefix.size()), prefix);
    };
    const auto ends_with = [&](std::string_view suffix) noexcept {
        return media_type.size() >= suffix.size() &&
               http_ascii_equals_ignore_case(media_type.substr(media_type.size() - suffix.size()), suffix);
    };
    for (std::size_t offset = 0; offset + 3 <= media_type.size(); ++offset) {
        if (http_ascii_equals_ignore_case(media_type.substr(offset, 3), "zip")) {
            return false;
        }
    }
    if (starts_with("text/") || ends_with("+json") || ends_with("+xml")) {
        return true;
    }
    static constexpr std::array<std::string_view, 14> compressible_types{
        "application/json",
        "application/javascript",
        "application/x-javascript",
        "application/ecmascript",
        "application/wasm",
        "application/xml",
        "application/x-ndjson",
        "application/vnd.ms-fontobject",
        "font/ttf",
        "font/otf",
        "application/x-font-ttf",
        "application/x-font-otf",
        "image/x-icon",
        "image/vnd.microsoft.icon",
    };
    for (const auto type : compressible_types) {
        if (http_ascii_equals_ignore_case(media_type, type)) {
            return true;
        }
    }
    return false;
}

}  // namespace ruvia::detail
