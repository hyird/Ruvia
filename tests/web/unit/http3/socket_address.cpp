#include <array>
#include <cstdint>

#include <asio/ip/address_v6.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

#include "test_harness.h"

namespace {
using Error = ruvia::detail::Http3QuicSocketAddressError;
using QuicAddress = ruvia::detail::Http3QuicDatagramAddress;
using Udp = asio::ip::udp;

}  // namespace

RUVIA_TEST(http3QuicSocketAddressRoundTripsIpv4AndPreservesHostOrderPort) {
    for (const std::uint16_t port : {443, 65535}) {
        const Udp::endpoint source(asio::ip::address_v4({192, 0, 2, 17}), port);
        const auto quic = ruvia::detail::toHttp3QuicDatagramAddress(source);
        RUVIA_CHECK(quic.has_value());
        if (!quic) {
            continue;
        }
        RUVIA_CHECK(quic->family == QuicAddress::Family::kIPv4);
        RUVIA_CHECK(quic->address[0] == 192);
        RUVIA_CHECK(quic->address[1] == 0);
        RUVIA_CHECK(quic->address[2] == 2);
        RUVIA_CHECK(quic->address[3] == 17);
        RUVIA_CHECK(quic->port == port);
        const auto roundTrip = ruvia::detail::toHttp3UdpEndpoint(*quic);
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
    const auto quic = ruvia::detail::toHttp3QuicDatagramAddress(source);
    RUVIA_CHECK(quic.has_value());
    if (!quic) {
        return;
    }
    RUVIA_CHECK(quic->family == QuicAddress::Family::kIPv6);
    RUVIA_CHECK(quic->address == bytes);
    RUVIA_CHECK(quic->scopeId == 0);
    const auto roundTrip = ruvia::detail::toHttp3UdpEndpoint(*quic);
    RUVIA_CHECK(roundTrip.has_value());
    if (roundTrip) {
        RUVIA_CHECK(*roundTrip == source);
    }
}

RUVIA_TEST(http3QuicSocketAddressAllowsWildcardOnlyForBindAddresses) {
    const auto ipv4 = ruvia::detail::toHttp3QuicBindAddress(
        Udp::endpoint(asio::ip::address_v4::any(), 443));
    RUVIA_CHECK(ipv4.has_value());
    if (ipv4) {
        RUVIA_CHECK(ipv4->family == QuicAddress::Family::kIPv4);
        RUVIA_CHECK(ipv4->port == 443);
    }

    const auto ipv6 = ruvia::detail::toHttp3QuicBindAddress(
        Udp::endpoint(asio::ip::address_v6::any(), 8443));
    RUVIA_CHECK(ipv6.has_value());
    if (ipv6) {
        RUVIA_CHECK(ipv6->family == QuicAddress::Family::kIPv6);
        RUVIA_CHECK(ipv6->port == 8443);
    }
}

RUVIA_TEST(http3QuicSocketAddressRejectsWildcardZeroPortScopeAndLinkLocal) {
    const auto wildcard = ruvia::detail::toHttp3QuicDatagramAddress(
        Udp::endpoint(asio::ip::address_v4::any(), 443));
    RUVIA_CHECK(!wildcard);
    if (!wildcard) {
        RUVIA_CHECK(wildcard.error() == Error::kUnspecifiedAddress);
    }

    const auto zeroPort = ruvia::detail::toHttp3QuicDatagramAddress(
        Udp::endpoint(asio::ip::address_v4({192, 0, 2, 1}), 0));
    RUVIA_CHECK(!zeroPort);
    if (!zeroPort) {
        RUVIA_CHECK(zeroPort.error() == Error::kZeroPort);
    }

    const asio::ip::address_v6::bytes_type globalBytes{
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto scoped = ruvia::detail::toHttp3QuicDatagramAddress(
        Udp::endpoint(asio::ip::address_v6(globalBytes, 7), 443));
    RUVIA_CHECK(!scoped);
    if (!scoped) {
        RUVIA_CHECK(scoped.error() == Error::kIPv6ScopeNotSupported);
    }

    const asio::ip::address_v6::bytes_type linkLocalBytes{
        0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto linkLocal = ruvia::detail::toHttp3QuicDatagramAddress(
        Udp::endpoint(asio::ip::address_v6(linkLocalBytes), 443));
    RUVIA_CHECK(!linkLocal);
    if (!linkLocal) {
        RUVIA_CHECK(linkLocal.error() == Error::kIPv6LinkLocalNotSupported);
    }

    const asio::ip::address_v6::bytes_type mappedBytes{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1};
    const auto mapped = ruvia::detail::toHttp3QuicDatagramAddress(
        Udp::endpoint(asio::ip::address_v6(mappedBytes), 443));
    RUVIA_CHECK(!mapped);
    if (!mapped) {
        RUVIA_CHECK(mapped.error() == Error::kIPv4MappedIPv6NotSupported);
    }

    QuicAddress invalidMapped;
    invalidMapped.family = QuicAddress::Family::kIPv6;
    invalidMapped.address = mappedBytes;
    invalidMapped.port = 443;
    const auto rejectedMapped = ruvia::detail::toHttp3UdpEndpoint(invalidMapped);
    RUVIA_CHECK(!rejectedMapped);
    if (!rejectedMapped) {
        RUVIA_CHECK(rejectedMapped.error() == Error::kIPv4MappedIPv6NotSupported);
    }

    QuicAddress unspecified;
    unspecified.port = 443;
    const auto invalidWildcard = ruvia::detail::toHttp3UdpEndpoint(unspecified);
    RUVIA_CHECK(!invalidWildcard);
    if (!invalidWildcard) {
        RUVIA_CHECK(invalidWildcard.error() == Error::kUnspecifiedAddress);
    }

    QuicAddress invalidPort;
    invalidPort.family = QuicAddress::Family::kIPv4;
    invalidPort.address[0] = 192;
    invalidPort.address[2] = 2;
    invalidPort.address[3] = 1;
    const auto rejectedPort = ruvia::detail::toHttp3UdpEndpoint(invalidPort);
    RUVIA_CHECK(!rejectedPort);
    if (!rejectedPort) {
        RUVIA_CHECK(rejectedPort.error() == Error::kZeroPort);
    }

    QuicAddress invalidScope;
    invalidScope.family = QuicAddress::Family::kIPv6;
    invalidScope.address = globalBytes;
    invalidScope.port = 443;
    invalidScope.scopeId = 7;
    const auto rejectedScope = ruvia::detail::toHttp3UdpEndpoint(invalidScope);
    RUVIA_CHECK(!rejectedScope);
    if (!rejectedScope) {
        RUVIA_CHECK(rejectedScope.error() == Error::kIPv6ScopeNotSupported);
    }

    QuicAddress invalidLinkLocal;
    invalidLinkLocal.family = QuicAddress::Family::kIPv6;
    invalidLinkLocal.address = linkLocalBytes;
    invalidLinkLocal.port = 443;
    const auto rejectedLinkLocal = ruvia::detail::toHttp3UdpEndpoint(invalidLinkLocal);
    RUVIA_CHECK(!rejectedLinkLocal);
    if (!rejectedLinkLocal) {
        RUVIA_CHECK(rejectedLinkLocal.error() == Error::kIPv6LinkLocalNotSupported);
    }
}
