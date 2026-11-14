#pragma once

#include <cstddef>
#include <string_view>

#include "url_encoding.h"

namespace ruvia::detail {

enum class route_segment_kind { literal,
    param,
    wildcard };

[[nodiscard]] constexpr route_segment_kind classify_route_segment(std::string_view segment) noexcept {
    if (segment == "*") {
        return route_segment_kind::wildcard;
    }
    return !segment.empty() && segment.front() == ':' ? route_segment_kind::param : route_segment_kind::literal;
}

[[nodiscard]] inline bool is_dot_segment(std::string_view segment) noexcept {
    // The longest spelling is "%2E%2E"; the size check keeps ordinary segments cheap.
    return !segment.empty() && segment.size() <= 6 && (uri_paths_equivalent(segment, ".") || uri_paths_equivalent(segment, ".."));
}

// Build, reverse routing and request matching share the same segments.
// Each remainder retains its leading slash, preserving empty segments without
// allowing them to match a parameter. A final slash yields a trailing empty
// segment, so "/a/" and "/a" are distinct paths (RFC 3986) for dynamic routes
// exactly as they are for byte-exact static routes.
[[nodiscard]] inline bool split_path_segment(std::string_view path, std::string_view& segment, std::string_view& rest) noexcept {
    if (path.empty()) {
        segment = {};
        rest = {};
        return false;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos) {
        segment = path;
        rest = {};
        return true;
    }

    segment = path.substr(0, slash);
    rest = path.substr(slash);  // keep the leading '/' so empty segments survive
    return true;
}

// Whether any segment of `path` is "." or "..", in any percent-encoded
// spelling (RFC 3986 6.2.2.2). Requests carrying one are rejected before
// routing, so a route pattern containing one could never be matched.
[[nodiscard]] inline bool has_dot_segment(std::string_view path) noexcept {
    std::size_t cursor = 0;
    for (;;) {
        const auto slash = path.find('/', cursor);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        if (is_dot_segment(path.substr(cursor, end - cursor))) {
            return true;
        }
        if (slash == std::string_view::npos) {
            return false;
        }
        cursor = slash + 1;
    }
}

// The one prefix-scoping rule, shared by fallback handler selection and by
// path-scoped middleware. `prefix` is normalized (see normalize_fallback_prefix)
// or empty for an unscoped registration, which scopes everything.
// A prefix matches on whole path segments only, so "/api" scopes "/api" and
// "/api/x" but never "/apix". Segments compare in the
// canonical form route matching and route parameters use (uri_paths_equivalent):
// split on raw '/' and percent-decoded, so "/%61pi/x" is under "/api" exactly
// when it routes like "/api/x", while an encoded "%2F" never acts as a segment
// boundary.
[[nodiscard]] inline bool path_is_under_prefix(std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty()) {
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
        if (!uri_paths_equivalent(path.substr(path_cursor, path_end - path_cursor), prefix.substr(prefix_cursor, prefix_end - prefix_cursor))) {
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

// Segment count of a normalized prefix. A scope nested inside another always
// has more segments, whatever either one's escaping.
[[nodiscard]] inline std::size_t fallback_prefix_depth(std::string_view prefix) noexcept {
    std::size_t depth = 0;
    for (const auto c : prefix) {
        depth += c == '/' ? 1U : 0U;
    }
    return depth;
}

}  // namespace ruvia::detail
