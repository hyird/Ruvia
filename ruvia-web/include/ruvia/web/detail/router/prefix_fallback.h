#pragma once

#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"

namespace ruvia::detail {

// One canonical spelling is shared by application, test_app and route_table. router_impl
// receives the already-normalized registrations from the public configuration
// facades and keeps the final table's validation as its defensive boundary.
// A trailing slash does not create a different path scope; accepting both
// spellings at one layer and not another makes duplicate fallback handlers
// order-dependent and lets tests exercise a different route graph.
[[nodiscard]] inline std::string_view normalize_fallback_prefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    if ((prefix.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(prefix)) {
        throw std::invalid_argument("fallback prefix must be an origin-form path without query");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    return prefix;
}

// The one prefix-scoping rule, shared by fallback handler selection and by
// path-scoped middleware. `prefix` is normalized (see normalize_fallback_prefix).
// A prefix matches on whole path segments only, so "/api" scopes "/api" and
// "/api/x" but never "/apix"; "/" scopes everything. Segments are split on raw
// '/' and compared percent-decoded, so "/%61pi/x" is under "/api" just as its
// decoded route parameters read, while an encoded "%2F" never acts as a
// segment boundary.
[[nodiscard]] inline bool path_is_under_prefix(
    std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty() || prefix == "/") {
        return true;
    }
    if (path.empty() || path.front() != '/') {
        return false;
    }
    std::size_t path_cursor = 1;
    std::size_t prefix_cursor = 1;
    for (;;) {
        const auto prefix_slash = prefix.find('/', prefix_cursor);
        const auto prefix_end = prefix_slash == std::string_view::npos ? prefix.size() : prefix_slash;
        const auto path_slash = path.find('/', path_cursor);
        const auto path_end = path_slash == std::string_view::npos ? path.size() : path_slash;
        if (!url_components_equivalent(path.substr(path_cursor, path_end - path_cursor),
                prefix.substr(prefix_cursor, prefix_end - prefix_cursor), url_decode_mode::percent)) {
            return false;
        }
        if (prefix_end == prefix.size()) {
            // path_end is a '/' or the end of the path: a segment boundary.
            return true;
        }
        if (path_end == path.size()) {
            return false;
        }
        prefix_cursor = prefix_end + 1;
        path_cursor = path_end + 1;
    }
}

// Two normalized prefixes that scope the same request paths, such as "/api"
// and "/%61pi", are one registration.
[[nodiscard]] inline bool fallback_prefixes_equivalent(std::string_view left, std::string_view right) noexcept {
    return path_is_under_prefix(left, right) && path_is_under_prefix(right, left);
}

// Segment count of a normalized prefix ("/" has none). A scope nested inside
// another always has more segments, whatever either one's escaping.
[[nodiscard]] inline std::size_t fallback_prefix_depth(std::string_view prefix) noexcept {
    if (prefix == "/") {
        return 0;
    }
    std::size_t depth = 0;
    for (const auto c : prefix) {
        depth += c == '/' ? 1U : 0U;
    }
    return depth;
}

}  // namespace ruvia::detail
