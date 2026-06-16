#pragma once

#include <string_view>

namespace ruvia::detail {

[[nodiscard]] std::string_view cachedDateHeader() noexcept;

}  // namespace ruvia::detail
