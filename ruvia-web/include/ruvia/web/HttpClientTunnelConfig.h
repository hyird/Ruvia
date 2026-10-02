#pragma once

#include <cstddef>

namespace ruvia {
struct HttpClientTunnelConfig final {
    std::size_t maxChunkBytes{64 * 1024};
    // Declares HTTP Datagram semantics for an Extended CONNECT protocol.
    // CONNECT-UDP enables this automatically.
    bool datagrams{};
};
}  // namespace ruvia
