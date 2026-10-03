#pragma once

#include <string_view>

namespace ruvia {

// Validates a WHATWG serialized origin, not a general URI or origin list.
[[nodiscard]] bool is_valid_http_serialized_origin(std::string_view value) noexcept;

}  // namespace ruvia
