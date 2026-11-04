#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <variant>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

#include "ruvia/core/socket.h"
#include "ruvia/http/http_hex.h"

namespace ruvia::detail {

// "v6s/" + 32 hex address digits + '/' + a decimal numeric scope. This also
// fits the unscoped /64 token and canonical dotted IPv4 within a 64-byte slot.
inline constexpr std::size_t rate_limit_key_buffer_bytes =
    37 + std::numeric_limits<asio::ip::scope_id_type>::digits10 + 1;
static_assert(rate_limit_key_buffer_bytes <= 64);
[[nodiscard]] inline std::string_view format_ipv4_address(
    const asio::ip::address_v4& address, std::span<char, 15> buffer) noexcept {
    const auto bytes_value = address.to_bytes();
    char* cursor_value = buffer.data();
    char* const end = buffer.data() + buffer.size();
    for (std::size_t i = 0; i < bytes_value.size(); ++i) {
        if (i != 0) {
            *cursor_value++ = '.';
        }
        cursor_value = std::to_chars(cursor_value, end, bytes_value[i]).ptr;
    }
    return std::string_view(buffer.data(), static_cast<std::size_t>(cursor_value - buffer.data()));
}

// Rate-limit key derived from a peer address string. A genuine IPv6 address is
// grouped by its /64 network prefix: a single client typically controls an entire
// /64 (or larger) allocation, so keying on the full 128-bit address would let it
// rotate through billions of addresses to bypass the per-IP limit and fill a
// worker's fixed slot table -- turning the fail-closed limiter into an availability
// DoS. IPv4-mapped IPv6 is the same host as dotted IPv4 (a dual-stack listener
// presents both), so it is rewritten to dotted form; leaving the mapped spelling
// as a distinct key would let one client spend two slots. The /64 prefix is
// emitted into `buffer` as an allocation-free "v6/<16 hex>" token (distinct from
// any IPv4 dotted string). Scoped IPv6 uses its full address and numeric scope
// in a separate "v6s/" namespace; equivalent spellings share a host key, while
// different interfaces remain isolated. IPv4 passes through unchanged. Kept out
// of rate_limiter.h so that lightweight header does not gain an asio dependency.
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
        const auto bytes_value = address.to_bytes();
        buffer[0] = 'v';
        buffer[1] = '6';
        buffer[2] = 's';
        buffer[3] = '/';
        http_encode_lower_hex(buffer + 4, std::span<const unsigned char>(bytes_value));
        buffer[36] = '/';
        const auto end = std::to_chars(buffer + 37, buffer + rate_limit_key_buffer_bytes, address.scope_id()).ptr;
        return std::string_view(buffer, static_cast<std::size_t>(end - buffer));
    }
    if (address.is_v4_mapped()) {
        return format_ipv4_address(asio::ip::make_address_v4(asio::ip::v4_mapped, address),
            std::span<char, 15>(buffer, 15));
    }
    const auto bytes_value = address.to_bytes();
    buffer[0] = 'v';
    buffer[1] = '6';
    buffer[2] = '/';
    // The first 8 bytes are the /64 network prefix.
    http_encode_lower_hex(buffer + 3, std::span<const unsigned char>(bytes_value.data(), 8));
    return std::string_view(buffer, 19);
}

}  // namespace ruvia::detail
