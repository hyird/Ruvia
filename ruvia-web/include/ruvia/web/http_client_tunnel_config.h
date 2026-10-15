#pragma once

#include <cstddef>

namespace ruvia {
// open_tunnel() and open_udp_tunnel() select the protocol; this policy bounds
// the size of one transport write, independently of Capsule payload limits.
struct http_client_tunnel_config final {
    std::size_t max_chunk_bytes_{64 * 1024};
};
}  // namespace ruvia
