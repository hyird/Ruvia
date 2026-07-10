#pragma once

#include "ruvia/http/detail/HeaderAcceptUtils.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept;

[[nodiscard]] bool encodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::pmr::string& output,
    std::size_t maxOutputBytes);

}  // namespace ruvia::detail
