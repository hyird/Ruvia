#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] std::uint32_t classifyResponseHeaderName(std::string_view name) noexcept;

}  // namespace ruvia::detail
