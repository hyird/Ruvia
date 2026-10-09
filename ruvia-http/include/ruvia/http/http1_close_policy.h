#pragma once

#include <cstdint>

namespace ruvia {

// Whether a connection is reused after a completed HTTP/1 exchange. Both the
// client request writer and the server response planner express the same wire
// decision with this single shared policy: allow_reuse keeps the connection
// poolable, close_after_response emits Connection: close and closes it.
enum class http1_close_policy : std::uint8_t {
    allow_reuse,
    close_after_response,
};

}  // namespace ruvia
