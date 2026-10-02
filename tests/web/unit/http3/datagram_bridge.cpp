#include <array>
#include <chrono>
#include <cstddef>

#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicPacketIo.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

#include "test_harness.h"

namespace {

ruvia::detail::http3_quic_datagram_address address(std::uint16_t port) {
    using namespace ruvia::detail;
    return to_http3_quic_datagram_address(
        asio::ip::udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port))
        .value();
}

}  // namespace

RUVIA_TEST(http3QuicPacketIoWritesIntoCallerOwnedPacketStorage) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    const auto local = address(40000);
    const auto peer = address(4433);
    ruvia::quic_connection_config config;
    config.local_address = to_quic_address(local);
    config.peer_address = to_quic_address(peer);
    http3_quic_client_transport transport(tls, config, "example.test",
        std::chrono::steady_clock::now());

    std::array<std::byte, 1500> packet{};
    const auto result = http3_quic_packet_io::write(transport.connection(), packet,
        std::chrono::steady_clock::now());
    RUVIA_CHECK(result.size != 0);
    RUVIA_CHECK(result.size <= packet.size());
    const auto expected_peer = to_quic_address(peer);
    RUVIA_CHECK(result.peer.bytes == expected_peer.bytes);
    RUVIA_CHECK(result.peer.port == expected_peer.port);
    RUVIA_CHECK(result.peer.family == expected_peer.family);
}
