#pragma once

#include <string_view>

namespace ruvia::detail {

// Build, reverse routing and request matching share the same segments.
// Each remainder retains its leading slash, preserving interior empty segments
// without allowing them to match a parameter. A single final slash ends the
// walk, so parameterized routes keep their documented trailing-slash alias.
[[nodiscard]] bool split_path_segment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept;

}  // namespace ruvia::detail
