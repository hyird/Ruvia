#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <openssl/types.h>

#include "ruvia/http/http_target_origin.h"

namespace ruvia::detail {

// The certificate one server TLS connection presented, answering RFC 9110
// §7.4's question whether it is valid for a request's target host. RFC 9525
// §6.3 matching: subjectAltName DNS-ID or IP-ID only (the subject CN is never
// consulted), a wildcard only as a whole left-most label. Matching against the
// certificate, not the SNI name, keeps RFC 9113 §9.1.1 connection reuse
// working for every name the certificate covers.
//
// Lives on the connection's frame after the handshake and is used only on its
// thread. Verdicts are cached for the last few distinct hosts, so a
// connection pays OpenSSL's certificate scan once per host, not per request.
class tls_served_certificate final {
public:
    // `certificate` is the leaf the handshake selected (SSL_get_certificate
    // after any SNI context switch), borrowed for the connection's lifetime;
    // null fails every check closed.
    explicit tls_served_certificate(X509* certificate) noexcept
        : certificate_(certificate) {}

    tls_served_certificate(const tls_served_certificate&) = delete;
    tls_served_certificate& operator=(const tls_served_certificate&) = delete;

    // `origin` must ask for verify_certificate.
    [[nodiscard]] bool covers(const http_target_origin& origin) noexcept;

private:
    // DNS names are at most 253 octets without the trailing dot; a longer
    // host is checked without caching.
    static constexpr std::size_t cached_key_capacity = 253;
    static constexpr std::size_t cache_size = 4;

    struct cached_verdict final {
        std::array<char, cached_key_capacity> key_{};
        std::uint8_t key_size_{0};
        http_target_host_kind kind_{http_target_host_kind::dns_name};
        bool covered_{false};
    };

    [[nodiscard]] bool check(const http_target_origin& origin) const noexcept;

    X509* certificate_;
    std::array<cached_verdict, cache_size> cache_{};
    std::size_t cached_{0};
    std::size_t next_{0};
};

}  // namespace ruvia::detail
