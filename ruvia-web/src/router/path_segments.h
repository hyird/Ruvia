#pragma once

#include <string_view>

#include "ruvia/http/url_encoding.h"

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
    return !segment.empty() && segment.size() <= 6 &&
           (uri_paths_equivalent(segment, ".") || uri_paths_equivalent(segment, ".."));
}

// Build, reverse routing and request matching share the same segments.
// Each remainder retains its leading slash, preserving empty segments without
// allowing them to match a parameter. A final slash yields a trailing empty
// segment, so "/a/" and "/a" are distinct paths (RFC 3986) for dynamic routes
// exactly as they are for byte-exact static routes.
[[nodiscard]] inline bool split_path_segment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept {
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

// Without parameter names, equality means the same trie slot: literal children
// compare canonically, parameters share one child, and wildcards share one slot.
// Other overlaps have a deterministic literal-before-parameter-before-wildcard
// winner and are not conflicts.
[[nodiscard]] inline bool route_patterns_equivalent(
    std::string_view left, std::string_view right, bool compare_param_names) noexcept {
    for (;;) {
        std::string_view left_segment;
        std::string_view left_rest;
        std::string_view right_segment;
        std::string_view right_rest;
        const auto has_left = split_path_segment(left, left_segment, left_rest);
        const auto has_right = split_path_segment(right, right_segment, right_rest);
        if (!has_left || !has_right) {
            return has_left == has_right;
        }
        const auto kind = classify_route_segment(left_segment);
        if (kind != classify_route_segment(right_segment) ||
            ((kind == route_segment_kind::literal || compare_param_names) &&
                !uri_paths_equivalent(left_segment, right_segment))) {
            return false;
        }
        left = left_rest;
        right = right_rest;
    }
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

}  // namespace ruvia::detail
