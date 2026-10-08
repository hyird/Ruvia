#include <string>

#include "http3/Http3QuicClientTlsContext.h"
#include "http3/Http3QuicClientTransport.h"
#include "http3/Http3QuicSocketAddress.h"
#include "test_harness.h"

namespace {

ruvia::detail::http3_quic_datagram_address clientAddress(std::uint16_t port) {
    using namespace ruvia::detail;
    return to_http3_quic_datagram_address(
        asio::ip::udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port))
        .value();
}

}  // namespace

RUVIA_TEST(http3QuicClientTransportOwnsCoreConnectionAndRetiresRepeatedly) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    for (int iteration = 0; iteration != 2; ++iteration) {
        auto local = clientAddress(static_cast<std::uint16_t>(40000 + iteration));
        auto peer = clientAddress(4433);
        ruvia::quic_connection_config config;
        config.local_address = to_quic_address(local);
        config.peer_address = to_quic_address(peer);
        {
            http3_quic_client_transport transport(tls, config, "example.test",
                std::chrono::steady_clock::now());
            RUVIA_CHECK(transport.connection().info().state ==
                        ruvia::quic_connection_state::connecting);
            const auto opened = transport.connection().open_stream(false);
            RUVIA_CHECK(opened.status == ruvia::quic_operation_status::would_block ||
                        opened.status == ruvia::quic_operation_status::need_input);
            transport.close();
        }
    }
}

RUVIA_TEST(http3QuicClientTransportRejectsInvalidPeerConfiguration) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    auto local = clientAddress(40000);
    auto peer = clientAddress(4433);
    peer.port = 0;
    ruvia::quic_connection_config config;
    config.local_address = to_quic_address(local);
    config.peer_address = to_quic_address(peer);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        http3_quic_client_transport transport(tls, config, "example.test",
            std::chrono::steady_clock::now());
    }));
}

RUVIA_TEST(http3QuicClientTransportRejectsInvalidTlsHostname) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(ClientTransportConfigView{});
    auto local = clientAddress(40000);
    auto peer = clientAddress(4433);
    ruvia::quic_connection_config config;
    config.local_address = to_quic_address(local);
    config.peer_address = to_quic_address(peer);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        http3_quic_client_transport transport(tls, config, "bad host",
            std::chrono::steady_clock::now());
    }));
}
