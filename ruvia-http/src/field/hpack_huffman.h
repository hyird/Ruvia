#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

// Appends decoded bytes using output's resource. Invalid input returns false and
// may leave a decoded prefix appended; the caller owns rollback and capacity policy.
[[nodiscard]] bool append_hpack_huffman(std::string_view encoded, std::pmr::string& output);

}  // namespace ruvia::detail
