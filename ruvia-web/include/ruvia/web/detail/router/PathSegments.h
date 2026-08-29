#pragma once

#include <string_view>

// Splitting a path into its next segment and the rest, in the two ways routing
// needs. Both are pure functions of the string: the route table applies them,
// it does not own them.

namespace ruvia::detail {

// Build-time split: a leading '/' is dropped and an empty path ends the walk, so
// "/a/b" and "a/b" register the same route.
[[nodiscard]] bool splitRoutePathSegment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept;

// Request-time split. Unlike the build-time one it preserves empty segments and
// a trailing slash so dynamic matching is byte-exact like static matching:
// "/users/42/" and "/a//b" no longer collapse to "/users/42" / "/a/b". `path` is
// expected to start with '/' at each level; each returned `rest` keeps its
// leading '/'. Returns false only at true end-of-path (empty `path`); a lone "/"
// yields an empty segment that fails to match a param child.
[[nodiscard]] bool splitRequestPathSegment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept;

}  // namespace ruvia::detail
