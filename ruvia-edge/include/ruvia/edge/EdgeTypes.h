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
};

// PEM-encoded certificate chain and private key for terminating client TLS.
struct EdgeTlsConfig final {
    std::string certificateChainPem;
    std::string privateKeyPem;
};

}  // namespace ruvia::edge
