#pragma once

#include <chrono>

namespace ruvia {

struct http_tunnel_route_config final {
    // Once the handler returns, bound flushing the local FIN and draining peer
    // input. A handler may half-close and continue reading without this deadline.
    std::chrono::milliseconds peer_transport_fin_timeout_{5000};
    // Declares HTTP Datagram semantics for an Extended CONNECT protocol.
    // CONNECT-UDP enables this automatically.
    bool datagrams_{};
};

}  // namespace ruvia
