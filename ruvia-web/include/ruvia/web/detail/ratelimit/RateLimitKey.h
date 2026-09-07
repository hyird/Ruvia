#pragma once

#include <cstddef>
#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

namespace ruvia::detail {

// "v6/" + the 8-byte /64 prefix rendered as 16 hex chars. Also enough for a
// dotted IPv4 address written when canonicalizing an IPv4-mapped IPv6 peer.
inline constexpr std::size_t kRateLimitKeyBufferBytes = 19;

// Rate-limit key derived from a peer address string. A genuine IPv6 address is
// grouped by its /64 network prefix: a single client typically controls an entire
// /64 (or larger) allocation, so keying on the full 128-bit address would let it
// rotate through billions of addresses to bypass the per-IP limit and fill a
// worker's fixed slot table -- turning the fail-closed limiter into an availability
// DoS. IPv4-mapped IPv6 is the same host as dotted IPv4 (a dual-stack listener
// presents both), so it is rewritten to dotted form; leaving the mapped spelling
// as a distinct key would let one client spend two slots. The /64 prefix is
// emitted into `buffer` as an allocation-free "v6/<16 hex>" token (distinct from
// any IPv4 dotted string). Pass-through IPv4 and scoped IPv6 return the input
// verbatim. Kept out of RateLimiter.h so that lightweight, widely-included header
// does not gain an asio dependency.
[[nodiscard]] inline std::string_view rateLimitKeyFor(
    std::string_view remoteAddress, char (&buffer)[kRateLimitKeyBufferBytes]) noexcept {
    if (!remoteAddress.contains(':')) {
        return remoteAddress;  // no ':' -> IPv4 or empty; already a per-host key
    }
    std::error_code ec;
    const auto address = asio::ip::make_address_v6(remoteAddress, ec);
    if (ec || address.scope_id() != 0) {
        return remoteAddress;  // unparseable or scoped -> a full host key
    }
    if (address.is_v4_mapped()) {
        const auto bytes = asio::ip::make_address_v4(asio::ip::v4_mapped, address).to_bytes();
        auto appendOctet = [](char* out, unsigned octet) noexcept {
            if (octet >= 100) {
                *out++ = static_cast<char>('0' + octet / 100);
                octet %= 100;
                *out++ = static_cast<char>('0' + octet / 10);
                *out++ = static_cast<char>('0' + octet % 10);
            } else if (octet >= 10) {
                *out++ = static_cast<char>('0' + octet / 10);
                *out++ = static_cast<char>('0' + octet % 10);
            } else {
                *out++ = static_cast<char>('0' + octet);
            }
            return out;
        };
        char* cursor = buffer;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i != 0) {
                *cursor++ = '.';
            }
            cursor = appendOctet(cursor, bytes[i]);
        }
        return std::string_view(buffer, static_cast<std::size_t>(cursor - buffer));
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
