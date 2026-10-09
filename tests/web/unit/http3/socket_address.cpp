#include <array>
#include <cstdint>
#include <variant>

#include <asio/ip/address_v6.hpp>
#include <asio/ip/udp.hpp>

#include "http3/http3_quic_socket_address.h"
#include "test_harness.h"

namespace {
using error_type = ruvia::detail::http3_quic_socket_address_error;
using quic_address_type = ruvia::detail::http3_quic_datagram_address;
using udp_type = asio::ip::udp;

}  // namespace

RUVIA_TEST(http3_quic_socket_address_round_trips_ipv4_and_preserves_host_order_port) {
    for (const std::uint16_t port : std::array<std::uint16_t, 2>{443, 65535}) {
        const udp_type::endpoint source_value(asio::ip::address_v4({192, 0, 2, 17}), port);
        const auto quic = ruvia::detail::to_http3_quic_datagram_address(source_value);
        RUVIA_CHECK((quic.index() == 0));
        if ((quic.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(quic).address_family_ == quic_address_type::family::ipv4);
        RUVIA_CHECK(std::get<0>(quic).address_[0] == 192);
        RUVIA_CHECK(std::get<0>(quic).address_[1] == 0);
        RUVIA_CHECK(std::get<0>(quic).address_[2] == 2);
        RUVIA_CHECK(std::get<0>(quic).address_[3] == 17);
        RUVIA_CHECK(std::get<0>(quic).port_ == port);
        const auto round_trip = ruvia::detail::to_udp_endpoint(std::get<0>(quic));
        RUVIA_CHECK((round_trip.index() == 0));
        if ((round_trip.index() == 0)) {
            RUVIA_CHECK(std::get<0>(round_trip) == source_value);
        }
    }
}

RUVIA_TEST(http3_quic_socket_address_round_trips_global_ipv6_bytes_without_narrowing) {
    const asio::ip::address_v6::bytes_type bytes_value{
        0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
        0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78};
    const udp_type::endpoint source_value(asio::ip::address_v6(bytes_value), 443);
    const auto quic = ruvia::detail::to_http3_quic_datagram_address(source_value);
    RUVIA_CHECK((quic.index() == 0));
    if ((quic.index() != 0)) {
        return;
    }
    RUVIA_CHECK(std::get<0>(quic).address_family_ == quic_address_type::family::ipv6);
    RUVIA_CHECK(std::get<0>(quic).address_ == bytes_value);
    RUVIA_CHECK(std::get<0>(quic).scope_id_ == 0);
    const auto round_trip = ruvia::detail::to_udp_endpoint(std::get<0>(quic));
    RUVIA_CHECK((round_trip.index() == 0));
    if ((round_trip.index() == 0)) {
        RUVIA_CHECK(std::get<0>(round_trip) == source_value);
    }
}

RUVIA_TEST(http3_quic_socket_address_allows_wildcard_only_for_bind_addresses) {
    const auto ipv4 = ruvia::detail::to_http3_quic_bind_address(
        udp_type::endpoint(asio::ip::address_v4::any(), 443));
    RUVIA_CHECK((ipv4.index() == 0));
    if ((ipv4.index() == 0)) {
        RUVIA_CHECK(std::get<0>(ipv4).address_family_ == quic_address_type::family::ipv4);
        RUVIA_CHECK(std::get<0>(ipv4).port_ == 443);
    }

    const auto ipv6 = ruvia::detail::to_http3_quic_bind_address(
        udp_type::endpoint(asio::ip::address_v6::any(), 8443));
    RUVIA_CHECK((ipv6.index() == 0));
    if ((ipv6.index() == 0)) {
        RUVIA_CHECK(std::get<0>(ipv6).address_family_ == quic_address_type::family::ipv6);
        RUVIA_CHECK(std::get<0>(ipv6).port_ == 8443);
    }
}

RUVIA_TEST(http3_quic_socket_address_rejects_wildcard_zero_port_scope_and_link_local) {
    const auto wildcard = ruvia::detail::to_http3_quic_datagram_address(
        udp_type::endpoint(asio::ip::address_v4::any(), 443));
    RUVIA_CHECK((wildcard.index() != 0));
    if ((wildcard.index() != 0)) {
        RUVIA_CHECK(std::get<1>(wildcard) == error_type::unspecified_address);
    }

    const auto zero_port = ruvia::detail::to_http3_quic_datagram_address(
        udp_type::endpoint(asio::ip::address_v4({192, 0, 2, 1}), 0));
    RUVIA_CHECK((zero_port.index() != 0));
    if ((zero_port.index() != 0)) {
        RUVIA_CHECK(std::get<1>(zero_port) == error_type::zero_port);
    }

    const asio::ip::address_v6::bytes_type global_bytes{
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto scoped = ruvia::detail::to_http3_quic_datagram_address(
        udp_type::endpoint(asio::ip::address_v6(global_bytes, 7), 443));
    RUVIA_CHECK((scoped.index() != 0));
    if ((scoped.index() != 0)) {
        RUVIA_CHECK(std::get<1>(scoped) == error_type::ipv6_scope_not_supported);
    }

    const asio::ip::address_v6::bytes_type link_local_bytes{
        0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto link_local = ruvia::detail::to_http3_quic_datagram_address(
        udp_type::endpoint(asio::ip::address_v6(link_local_bytes), 443));
    RUVIA_CHECK((link_local.index() != 0));
    if ((link_local.index() != 0)) {
        RUVIA_CHECK(std::get<1>(link_local) == error_type::ipv6_link_local_not_supported);
    }

    const asio::ip::address_v6::bytes_type mapped_bytes{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1};
    const auto mapped = ruvia::detail::to_http3_quic_datagram_address(
        udp_type::endpoint(asio::ip::address_v6(mapped_bytes), 443));
    RUVIA_CHECK((mapped.index() != 0));
    if ((mapped.index() != 0)) {
        RUVIA_CHECK(std::get<1>(mapped) == error_type::ipv4_mapped_ipv6_not_supported);
    }

    quic_address_type invalid_mapped;
    invalid_mapped.address_family_ = quic_address_type::family::ipv6;
    invalid_mapped.address_ = mapped_bytes;
    invalid_mapped.port_ = 443;
    const auto rejected_mapped = ruvia::detail::to_udp_endpoint(invalid_mapped);
    RUVIA_CHECK((rejected_mapped.index() != 0));
    if ((rejected_mapped.index() != 0)) {
        RUVIA_CHECK(std::get<1>(rejected_mapped) == error_type::ipv4_mapped_ipv6_not_supported);
    }

    quic_address_type unspecified;
    unspecified.port_ = 443;
    const auto invalid_wildcard = ruvia::detail::to_udp_endpoint(unspecified);
    RUVIA_CHECK((invalid_wildcard.index() != 0));
    if ((invalid_wildcard.index() != 0)) {
        RUVIA_CHECK(std::get<1>(invalid_wildcard) == error_type::unspecified_address);
    }

    quic_address_type invalid_port;
    invalid_port.address_family_ = quic_address_type::family::ipv4;
    invalid_port.address_[0] = 192;
    invalid_port.address_[2] = 2;
    invalid_port.address_[3] = 1;
    const auto rejected_port = ruvia::detail::to_udp_endpoint(invalid_port);
    RUVIA_CHECK((rejected_port.index() != 0));
    if ((rejected_port.index() != 0)) {
        RUVIA_CHECK(std::get<1>(rejected_port) == error_type::zero_port);
    }

    quic_address_type invalid_scope;
    invalid_scope.address_family_ = quic_address_type::family::ipv6;
    invalid_scope.address_ = global_bytes;
    invalid_scope.port_ = 443;
    invalid_scope.scope_id_ = 7;
    const auto rejected_scope = ruvia::detail::to_udp_endpoint(invalid_scope);
    RUVIA_CHECK((rejected_scope.index() != 0));
    if ((rejected_scope.index() != 0)) {
        RUVIA_CHECK(std::get<1>(rejected_scope) == error_type::ipv6_scope_not_supported);
    }

    quic_address_type invalid_link_local;
    invalid_link_local.address_family_ = quic_address_type::family::ipv6;
    invalid_link_local.address_ = link_local_bytes;
    invalid_link_local.port_ = 443;
    const auto rejected_link_local = ruvia::detail::to_udp_endpoint(invalid_link_local);
    RUVIA_CHECK((rejected_link_local.index() != 0));
    if ((rejected_link_local.index() != 0)) {
        RUVIA_CHECK(std::get<1>(rejected_link_local) == error_type::ipv6_link_local_not_supported);
    }
}
