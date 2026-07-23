#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ruvia::edge {

// Runtime-independent listener value. Address accepts an IPv4 or IPv6 literal;
// port 0 asks the operating system to choose an ephemeral port.
struct EdgeEndpoint final {
    std::string address{"0.0.0.0"};
    std::uint16_t port{0};
};

// Where a front-facing Host is proxied. IPv6 literals retain their brackets.
struct OriginSettings final {
    std::string upstreamHost;
    std::uint16_t upstreamPort{80};
    bool https{false};
};

struct EdgeCacheLimits final {
    std::size_t maxBytes{64u * 1024u * 1024u};
    std::size_t maxEntries{4096};
};

struct OriginFetchLimits final {
    std::size_t maxResponseBytes{8u * 1024u * 1024u};
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds ioTimeout{30000};
    std::chrono::milliseconds idleTimeout{15000};
    std::size_t maxIdlePerHost{8};
    bool verifyOriginCertificate{true};
    // Circuit breaker, per upstream host:port. An origin that is down fails
    // every request only after connectTimeout, so without a breaker the wait
    // is paid again by each one and the requests pile up for as long as the
    // outage lasts. After this many consecutive transport failures the edge
    // stops dialing and answers immediately; 0 disables the breaker.
    std::size_t circuitFailureThreshold{5};
    // How long the breaker stays open before letting one request through to
    // test the origin. That probe closes the breaker if it succeeds, and
    // restarts this delay if it does not.
    std::chrono::milliseconds circuitResetTimeout{5000};
};

// PEM-encoded certificate chain and private key for terminating client TLS.
struct EdgeTlsConfig final {
    std::string certificateChainPem;
    std::string privateKeyPem;
};

}  // namespace ruvia::edge
