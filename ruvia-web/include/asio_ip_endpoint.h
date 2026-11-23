#pragma once

#include <algorithm>
#include <cstdint>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include "ip_address.h"

namespace ruvia {

[[nodiscard]] inline ip_address to_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id()));
}

[[nodiscard]] inline asio::ip::address to_asio_address(const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        asio::ip::address_v4::bytes_type octets{};
        std::copy(address.bytes().begin(), address.bytes().end(), octets.begin());
        return asio::ip::address_v4(octets);
    }
    asio::ip::address_v6::bytes_type octets{};
    std::copy(address.bytes().begin(), address.bytes().end(), octets.begin());
    return asio::ip::address_v6(octets, address.scope_id());
}

[[nodiscard]] inline ip_endpoint to_ip_endpoint(const asio::ip::tcp::endpoint& endpoint) noexcept {
    return ip_endpoint{to_ip_address(endpoint.address()), port_number{endpoint.port()}};
}

[[nodiscard]] inline ip_endpoint to_ip_endpoint(const asio::ip::udp::endpoint& endpoint) noexcept {
    return ip_endpoint{to_ip_address(endpoint.address()), port_number{endpoint.port()}};
}

[[nodiscard]] inline asio::ip::tcp::endpoint to_asio_tcp_endpoint(const ip_endpoint& endpoint) noexcept {
    return asio::ip::tcp::endpoint{to_asio_address(endpoint.address()), endpoint.port().value()};
}

[[nodiscard]] inline asio::ip::udp::endpoint to_asio_udp_endpoint(const ip_endpoint& endpoint) noexcept {
    return asio::ip::udp::endpoint{to_asio_address(endpoint.address()), endpoint.port().value()};
}

}  // namespace ruvia
