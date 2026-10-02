#pragma once

#include <chrono>

namespace ruvia {

struct HttpTunnelRouteConfig final {
    // Once the handler returns, bound flushing the local FIN and draining peer
    // input. A handler may half-close and continue reading without this deadline.
    std::chrono::milliseconds peerTransportFinTimeout{5000};
    // Declares HTTP Datagram semantics for an Extended CONNECT protocol.
    // CONNECT-UDP enables this automatically.
    bool datagrams{};
};

}  // namespace ruvia
