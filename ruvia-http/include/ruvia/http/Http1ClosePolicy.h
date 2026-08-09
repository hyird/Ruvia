#pragma once

#include <cstdint>

namespace ruvia {

// Whether a connection is reused after a completed HTTP/1 exchange. Both the
// client request writer and the server response planner express the same wire
// decision with this single shared policy: kAllowReuse keeps the connection
// poolable, kCloseAfterResponse emits Connection: close and closes it.
enum class Http1ClosePolicy : std::uint8_t {
    kAllowReuse,
    kCloseAfterResponse,
};

}  // namespace ruvia
