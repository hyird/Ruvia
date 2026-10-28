#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <span>
#include <string_view>
#include <variant>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

#include "ruvia/core/socket.h"
#include "ruvia/http/http_hex.h"

namespace ruvia::detail {

// "v6/" + the 8-byte /64 prefix rendered as 16 hex chars. Also enough for a
// dotted IPv4 address written when canonicalizing an IPv4-mapped IPv6 peer.
inline constexpr std::size_t rate_limit_key_buffer_bytes = 19;

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
// verbatim. Kept out of rate_limiter.h so that lightweight, widely-included
// header does not gain an asio dependency.
[[nodiscard]] inline std::string_view rate_limit_key_for(
    std::string_view remote_address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    if (!(remote_address.find(':') != std::string_view::npos)) {
        return remote_address;  // no ':' -> IPv4 or empty; already a per-host key
    }
    const auto parsed_value = ruvia::parse_ip_address(remote_address);
    if (!parsed_value || !parsed_value->is_v6()) {
        return remote_address;
    }
    const auto address = parsed_value->to_v6();
    if (address.scope_id() != 0) {
        return remote_address;  // unparseable or scoped -> a full host key
    }
    if (address.is_v4_mapped()) {
        const auto bytes_value = asio::ip::make_address_v4(asio::ip::v4_mapped, address).to_bytes();
        char* cursor_value = buffer;
        for (std::size_t i = 0; i < bytes_value.size(); ++i) {
            if (i != 0) {
                *cursor_value++ = '.';
            }
            cursor_value = std::to_chars(
                cursor_value, buffer + rate_limit_key_buffer_bytes, bytes_value[i])
                               .ptr;
        }
        return std::string_view(buffer, static_cast<std::size_t>(cursor_value - buffer));
    }
    const auto bytes_value = address.to_bytes();
    buffer[0] = 'v';
    buffer[1] = '6';
    buffer[2] = '/';
    // The first 8 bytes are the /64 network prefix.
    http_encode_lower_hex(buffer + 3, std::span<const unsigned char>(bytes_value.data(), 8));
    return std::string_view(buffer, rate_limit_key_buffer_bytes);
}

}  // namespace ruvia::detail
