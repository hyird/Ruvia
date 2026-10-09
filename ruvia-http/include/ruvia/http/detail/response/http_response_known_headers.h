#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] std::uint32_t classify_response_header_name(std::string_view name) noexcept;

}  // namespace ruvia::detail
