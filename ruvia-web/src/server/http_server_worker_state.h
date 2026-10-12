#pragma once

#include <cstdint>

namespace ruvia::detail {

// Owner-thread phase of one server worker:
// - running: admits connections and requests;
// - draining: admits nothing new, while in-flight requests finish and then
//   close their connections;
// - stopped: the core runtime stopped; every connection is closed now.
enum class http_server_worker_state : std::uint8_t {
    fresh,
    running,
    draining,
    stopped,
};

// True only while the worker admits new connections and requests.
[[nodiscard]] inline bool http_server_worker_running(http_server_worker_state state_value) noexcept {
    return state_value == http_server_worker_state::running;
}

}  // namespace ruvia::detail
