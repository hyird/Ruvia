#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class http1_client_initial_content_state : std::uint8_t {
    complete,
    pending,
    awaiting_continue,
};

}  // namespace ruvia::detail
