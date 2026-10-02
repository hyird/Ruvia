#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

#include <algorithm>
#include <array>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

namespace ruvia::detail {
namespace {
using error = http3_quic_socket_address_error;

bool is_unspecified(const std::array<std::uint8_t, 16>& bytes, std::size_t length) noexcept {
    return std::all_of(bytes.begin(), bytes.begin() + length,
        [](std::uint8_t byte) { return byte == 0; });
}

std::expected<http3_quic_datagram_address, error> to_quic_address(
    const asio::ip::udp::endpoint& endpoint, bool allow_unspecified) noexcept {
    if (endpoint.port() == 0) {
        return std::unexpected(error::zero_port);
    }

    http3_quic_datagram_address result;
    result.port = endpoint.port();
    const auto& address = endpoint.address();
    if (address.is_v4()) {
        result.address_family = http3_quic_datagram_address::family::ipv4;
        const auto bytes = address.to_v4().to_bytes();
        std::copy(bytes.begin(), bytes.end(), result.address.begin());
        if (!allow_unspecified && is_unspecified(result.address, bytes.size())) {
            return std::unexpected(error::unspecified_address);
        }
        return result;
    }
    if (!address.is_v6()) {
        return std::unexpected(error::unsupported_family);
    }

    const auto ipv6 = address.to_v6();
    if (ipv6.is_v4_mapped()) {
        return std::unexpected(error::ipv4_mapped_ipv6_not_supported);
    }
    if (ipv6.scope_id() != 0) {
        return std::unexpected(error::ipv6_scope_not_supported);
    }
    if (ipv6.is_link_local()) {
        return std::unexpected(error::ipv6_link_local_not_supported);
    }
    result.address_family = http3_quic_datagram_address::family::ipv6;
    const auto bytes = ipv6.to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.address.begin());
    if (!allow_unspecified && is_unspecified(result.address, bytes.size())) {
        return std::unexpected(error::unspecified_address);
    }
    return result;
}
}  // namespace

std::expected<http3_quic_datagram_address, http3_quic_socket_address_error>
to_http3_quic_datagram_address(const asio::ip::udp::endpoint& endpoint) noexcept {
    return to_quic_address(endpoint, false);
}

std::expected<http3_quic_datagram_address, http3_quic_socket_address_error>
to_http3_quic_bind_address(const asio::ip::udp::endpoint& endpoint) noexcept {
    return to_quic_address(endpoint, true);
}

std::expected<asio::ip::udp::endpoint, http3_quic_socket_address_error>
to_udp_endpoint(const http3_quic_datagram_address& address) noexcept {
    if (address.port == 0) {
        return std::unexpected(error::zero_port);
    }
    if (address.scope_id != 0) {
        return std::unexpected(error::ipv6_scope_not_supported);
    }

    if (address.address_family == http3_quic_datagram_address::family::ipv4) {
        if (is_unspecified(address.address, 4)) {
            return std::unexpected(error::unspecified_address);
        }
        asio::ip::address_v4::bytes_type bytes{};
        std::copy_n(address.address.begin(), bytes.size(), bytes.begin());
        return asio::ip::udp::endpoint(asio::ip::address_v4(bytes), address.port);
    }
    if (address.address_family != http3_quic_datagram_address::family::ipv6) {
        return std::unexpected(error::unsupported_family);
    }

    asio::ip::address_v6::bytes_type bytes{};
    std::copy(address.address.begin(), address.address.end(), bytes.begin());
    const asio::ip::address_v6 ipv6(bytes);
    if (ipv6.is_v4_mapped()) {
        return std::unexpected(error::ipv4_mapped_ipv6_not_supported);
    }
    if (ipv6.is_link_local()) {
        return std::unexpected(error::ipv6_link_local_not_supported);
    }
    if (ipv6.is_unspecified()) {
        return std::unexpected(error::unspecified_address);
    }
    return asio::ip::udp::endpoint(ipv6, address.port);
}

}  // namespace ruvia::detail
