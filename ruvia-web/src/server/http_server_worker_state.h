#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class http_server_worker_state : std::uint8_t {
    fresh,
    running,
    stopped,
};

[[nodiscard]] inline bool http_server_worker_running(http_server_worker_state state_value) noexcept {
    return state_value == http_server_worker_state::running;
}

}  // namespace ruvia::detail
