#pragma once

#include <cstddef>

namespace ruvia {
struct http_client_tunnel_config final {
    std::size_t max_chunk_bytes_{64 * 1024};
    // Declares HTTP Datagram semantics (capsules) for an upgrade-protocol
    // tunnel. CONNECT-UDP enables this automatically.
    bool datagrams_{};
};
}  // namespace ruvia
