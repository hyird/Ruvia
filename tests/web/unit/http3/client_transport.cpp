#include <string>
#include <variant>

#include "http3/http3_quic_client_tls_context.h"
#include "http3/http3_quic_client_transport.h"
#include "http3/http3_quic_socket_address.h"
#include "test_harness.h"

namespace {

ruvia::detail::http3_quic_datagram_address client_address(std::uint16_t port) {
    using namespace ruvia::detail;
    return std::get<0>(to_http3_quic_datagram_address(
        asio::ip::udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port)));
}

}  // namespace

RUVIA_TEST(http3_quic_client_transport_owns_core_connection_and_retires_repeatedly) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(client_transport_config_view{});
    for (int iteration = 0; iteration != 2; ++iteration) {
        auto local = client_address(static_cast<std::uint16_t>(40000 + iteration));
        auto peer = client_address(4433);
        ruvia::quic_connection_config config;
        config.local_address_ = to_quic_address(local);
        config.peer_address_ = to_quic_address(peer);
        {
            http3_quic_client_transport transport(tls, config, "example.test",
                std::chrono::steady_clock::now());
            RUVIA_CHECK(transport.connection().info().state_ ==
                        ruvia::quic_connection_state::connecting);
            const auto opened = transport.connection().open_stream(false);
            RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::would_block ||
                        opened.status_ == ruvia::quic_operation_status::need_input);
            transport.close();
        }
    }
}

RUVIA_TEST(http3_quic_client_transport_rejects_invalid_peer_configuration) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(client_transport_config_view{});
    auto local = client_address(40000);
    auto peer = client_address(4433);
    peer.port_ = 0;
    ruvia::quic_connection_config config;
    config.local_address_ = to_quic_address(local);
    config.peer_address_ = to_quic_address(peer);
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        http3_quic_client_transport transport(tls, config, "example.test",
            std::chrono::steady_clock::now());
    }));
}

RUVIA_TEST(http3_quic_client_transport_rejects_invalid_tls_hostname) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(client_transport_config_view{});
    auto local = client_address(40000);
    auto peer = client_address(4433);
    ruvia::quic_connection_config config;
    config.local_address_ = to_quic_address(local);
    config.peer_address_ = to_quic_address(peer);
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        http3_quic_client_transport transport(tls, config, "bad host",
            std::chrono::steady_clock::now());
    }));
}
