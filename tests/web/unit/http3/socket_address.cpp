#include <array>
#include <cstdint>

#include <asio/ip/address_v6.hpp>
#include <asio/ip/udp.hpp>

#include "http3/Http3QuicSocketAddress.h"
#include "test_harness.h"

namespace {
using Error = ruvia::detail::http3_quic_socket_address_error;
using QuicAddress = ruvia::detail::http3_quic_datagram_address;
using Udp = asio::ip::udp;

}  // namespace

RUVIA_TEST(http3QuicSocketAddressRoundTripsIpv4AndPreservesHostOrderPort) {
    for (const std::uint16_t port : std::array<std::uint16_t, 2>{443, 65535}) {
        const Udp::endpoint source(asio::ip::address_v4({192, 0, 2, 17}), port);
        const auto quic = ruvia::detail::to_http3_quic_datagram_address(source);
        RUVIA_CHECK(quic.has_value());
        if (!quic) {
            continue;
        }
        RUVIA_CHECK(quic->address_family == QuicAddress::family::ipv4);
        RUVIA_CHECK(quic->address[0] == 192);
        RUVIA_CHECK(quic->address[1] == 0);
        RUVIA_CHECK(quic->address[2] == 2);
        RUVIA_CHECK(quic->address[3] == 17);
        RUVIA_CHECK(quic->port == port);
        const auto roundTrip = ruvia::detail::to_udp_endpoint(*quic);
        RUVIA_CHECK(roundTrip.has_value());
        if (roundTrip) {
            RUVIA_CHECK(*roundTrip == source);
        }
    }
}

RUVIA_TEST(http3QuicSocketAddressRoundTripsGlobalIpv6BytesWithoutNarrowing) {
    const asio::ip::address_v6::bytes_type bytes{
        0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
        0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78};
    const Udp::endpoint source(asio::ip::address_v6(bytes), 443);
    const auto quic = ruvia::detail::to_http3_quic_datagram_address(source);
    RUVIA_CHECK(quic.has_value());
    if (!quic) {
        return;
    }
    RUVIA_CHECK(quic->address_family == QuicAddress::family::ipv6);
    RUVIA_CHECK(quic->address == bytes);
    RUVIA_CHECK(quic->scope_id == 0);
    const auto roundTrip = ruvia::detail::to_udp_endpoint(*quic);
    RUVIA_CHECK(roundTrip.has_value());
    if (roundTrip) {
        RUVIA_CHECK(*roundTrip == source);
    }
}

RUVIA_TEST(http3QuicSocketAddressAllowsWildcardOnlyForBindAddresses) {
    const auto ipv4 = ruvia::detail::to_http3_quic_bind_address(
        Udp::endpoint(asio::ip::address_v4::any(), 443));
    RUVIA_CHECK(ipv4.has_value());
    if (ipv4) {
        RUVIA_CHECK(ipv4->address_family == QuicAddress::family::ipv4);
        RUVIA_CHECK(ipv4->port == 443);
    }

    const auto ipv6 = ruvia::detail::to_http3_quic_bind_address(
        Udp::endpoint(asio::ip::address_v6::any(), 8443));
    RUVIA_CHECK(ipv6.has_value());
    if (ipv6) {
        RUVIA_CHECK(ipv6->address_family == QuicAddress::family::ipv6);
        RUVIA_CHECK(ipv6->port == 8443);
    }
}

RUVIA_TEST(http3QuicSocketAddressRejectsWildcardZeroPortScopeAndLinkLocal) {
    const auto wildcard = ruvia::detail::to_http3_quic_datagram_address(
        Udp::endpoint(asio::ip::address_v4::any(), 443));
    RUVIA_CHECK(!wildcard);
    if (!wildcard) {
        RUVIA_CHECK(wildcard.error() == Error::unspecified_address);
    }

    const auto zeroPort = ruvia::detail::to_http3_quic_datagram_address(
        Udp::endpoint(asio::ip::address_v4({192, 0, 2, 1}), 0));
    RUVIA_CHECK(!zeroPort);
    if (!zeroPort) {
        RUVIA_CHECK(zeroPort.error() == Error::zero_port);
    }

    const asio::ip::address_v6::bytes_type globalBytes{
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto scoped = ruvia::detail::to_http3_quic_datagram_address(
        Udp::endpoint(asio::ip::address_v6(globalBytes, 7), 443));
    RUVIA_CHECK(!scoped);
    if (!scoped) {
        RUVIA_CHECK(scoped.error() == Error::ipv6_scope_not_supported);
    }

    const asio::ip::address_v6::bytes_type linkLocalBytes{
        0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto linkLocal = ruvia::detail::to_http3_quic_datagram_address(
        Udp::endpoint(asio::ip::address_v6(linkLocalBytes), 443));
    RUVIA_CHECK(!linkLocal);
    if (!linkLocal) {
        RUVIA_CHECK(linkLocal.error() == Error::ipv6_link_local_not_supported);
    }

    const asio::ip::address_v6::bytes_type mappedBytes{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1};
    const auto mapped = ruvia::detail::to_http3_quic_datagram_address(
        Udp::endpoint(asio::ip::address_v6(mappedBytes), 443));
    RUVIA_CHECK(!mapped);
    if (!mapped) {
        RUVIA_CHECK(mapped.error() == Error::ipv4_mapped_ipv6_not_supported);
    }

    QuicAddress invalidMapped;
    invalidMapped.address_family = QuicAddress::family::ipv6;
    invalidMapped.address = mappedBytes;
    invalidMapped.port = 443;
    const auto rejectedMapped = ruvia::detail::to_udp_endpoint(invalidMapped);
    RUVIA_CHECK(!rejectedMapped);
    if (!rejectedMapped) {
        RUVIA_CHECK(rejectedMapped.error() == Error::ipv4_mapped_ipv6_not_supported);
    }

    QuicAddress unspecified;
    unspecified.port = 443;
    const auto invalidWildcard = ruvia::detail::to_udp_endpoint(unspecified);
    RUVIA_CHECK(!invalidWildcard);
    if (!invalidWildcard) {
        RUVIA_CHECK(invalidWildcard.error() == Error::unspecified_address);
    }

    QuicAddress invalidPort;
    invalidPort.address_family = QuicAddress::family::ipv4;
    invalidPort.address[0] = 192;
    invalidPort.address[2] = 2;
    invalidPort.address[3] = 1;
    const auto rejectedPort = ruvia::detail::to_udp_endpoint(invalidPort);
    RUVIA_CHECK(!rejectedPort);
    if (!rejectedPort) {
        RUVIA_CHECK(rejectedPort.error() == Error::zero_port);
    }

    QuicAddress invalidScope;
    invalidScope.address_family = QuicAddress::family::ipv6;
    invalidScope.address = globalBytes;
    invalidScope.port = 443;
    invalidScope.scope_id = 7;
    const auto rejectedScope = ruvia::detail::to_udp_endpoint(invalidScope);
    RUVIA_CHECK(!rejectedScope);
    if (!rejectedScope) {
        RUVIA_CHECK(rejectedScope.error() == Error::ipv6_scope_not_supported);
    }

    QuicAddress invalidLinkLocal;
    invalidLinkLocal.address_family = QuicAddress::family::ipv6;
    invalidLinkLocal.address = linkLocalBytes;
    invalidLinkLocal.port = 443;
    const auto rejectedLinkLocal = ruvia::detail::to_udp_endpoint(invalidLinkLocal);
    RUVIA_CHECK(!rejectedLinkLocal);
    if (!rejectedLinkLocal) {
        RUVIA_CHECK(rejectedLinkLocal.error() == Error::ipv6_link_local_not_supported);
    }
}
