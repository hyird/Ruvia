#pragma once

#include <asio/ip/address.hpp>
#include <asio/ip/address_v6.hpp>

#include <cstddef>
#include <string_view>
#include <system_error>

namespace ruvia::detail {

// "v6/" + the 8-byte /64 prefix rendered as 16 hex chars.
inline constexpr std::size_t kRateLimitKeyBufferBytes = 19;

// Rate-limit key derived from a peer address string. A genuine IPv6 address is
// grouped by its /64 network prefix: a single client typically controls an entire
// /64 (or larger) allocation, so keying on the full 128-bit address would let it
// rotate through billions of addresses to bypass the per-IP limit and fill a
// worker's fixed slot table -- turning the fail-closed limiter into an availability
// DoS. IPv4, and IPv4-mapped IPv6, pass through unchanged since each host is
// already a distinct key. The /64 prefix is emitted into `buffer` as an
// allocation-free "v6/<16 hex>" token (distinct from any IPv4 dotted string) and
// returned; pass-through cases return the input verbatim. Kept out of RateLimiter.h
// so that lightweight, widely-included header does not gain an asio dependency.
[[nodiscard]] inline std::string_view rateLimitKeyFor(
    std::string_view remoteAddress, char (&buffer)[kRateLimitKeyBufferBytes]) noexcept {
    if (!remoteAddress.contains(':')) {
        return remoteAddress;  // no ':' -> IPv4 or empty; already a per-host key
    }
    std::error_code ec;
    const auto address = asio::ip::make_address_v6(remoteAddress, ec);
    if (ec || address.is_v4_mapped()) {
        return remoteAddress;  // unparseable, or IPv4-mapped -> a full host key
    }
    const auto bytes = address.to_bytes();
    static constexpr char kHex[] = "0123456789abcdef";
    buffer[0] = 'v';
    buffer[1] = '6';
    buffer[2] = '/';
    for (std::size_t i = 0; i < 8; ++i) {  // first 8 bytes = the /64 network prefix
        buffer[3 + i * 2] = kHex[(bytes[i] >> 4) & 0x0F];
        buffer[3 + i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    return std::string_view(buffer, kRateLimitKeyBufferBytes);
}

}  // namespace ruvia::detail
