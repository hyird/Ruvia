#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory_resource>
#include <string>

#include <asio/detail/socket_ops.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>

#include "ruvia/http/ip_address.h"

#include "ratelimit/rate_limit_key.h"

namespace ruvia::detail {

[[nodiscard]] inline ip_address to_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id()));
}

[[nodiscard]] inline asio::ip::address to_asio_address(const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        asio::ip::address_v4::bytes_type bytes{};
        std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
        return asio::ip::address_v4(bytes);
    }
    asio::ip::address_v6::bytes_type bytes{};
    std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
    return asio::ip::address_v6(bytes, address.scope_id());
}

// The peer address a connection's services name, shared by TCP and QUIC
// sessions: dotted IPv4, also for an IPv4-mapped IPv6 peer, otherwise the
// RFC 5952 IPv6 text.
inline void assign_peer_address(std::pmr::string& output, const asio::ip::address& address) {
    asio::ip::address_v4 v4;
    if (address.is_v4()) {
        v4 = address.to_v4();
    } else {
        const auto v6 = address.to_v6();
        if (!v6.is_v4_mapped()) {
            const auto bytes = v6.to_bytes();
            std::array<char, 64> ipv6_buffer;
            asio::error_code ec;
            const char* formatted = asio::detail::socket_ops::inet_ntop(
                ASIO_OS_DEF(AF_INET6), bytes.data(), ipv6_buffer.data(),
                ipv6_buffer.size(), v6.scope_id(), ec);
            if (formatted == nullptr) {
                asio::detail::throw_error(ec);
            }
            output.assign(formatted, std::strlen(formatted));
            return;
        }
        v4 = asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
    }
    const auto v4_bytes = v4.to_bytes();
    std::array<char, 15> buffer;
    const auto formatted = format_ipv4_octets(v4_bytes.data(), buffer);
    output.assign(formatted.data(), formatted.size());
}

}  // namespace ruvia::detail
