#pragma once

#include <string_view>

namespace ruvia::detail {

// Build, reverse routing and request matching share the same segments.
// Each remainder retains its leading slash, preserving empty segments without
// allowing them to match a parameter. A final slash yields a trailing empty
// segment, so "/a/" and "/a" are distinct paths (RFC 3986) for dynamic routes
// exactly as they are for byte-exact static routes.
[[nodiscard]] bool split_path_segment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept;

// Whether any segment of `path` is "." or "..", in any percent-encoded
// spelling (RFC 3986 6.2.2.2). Requests carrying one are rejected before
// routing, so a route pattern containing one could never be matched.
[[nodiscard]] bool has_dot_segment(std::string_view path) noexcept;

}  // namespace ruvia::detail
