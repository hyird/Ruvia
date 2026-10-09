#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/http/quic_connection.h"
#include "ruvia/http/quic_server.h"

#include "http3/Http3QuicClientTlsContext.h"
#include "http3/Http3QuicClientTransport.h"
#include "http3/Http3QuicServerTransport.h"
#include "http3/Http3QuicSocketAddress.h"
#include "http3/Http3QuicTlsContext.h"

namespace ruvia::testing {

// Drives a real client/server HTTP QUIC pair over loopback UDP. It intentionally
// exposes the core connections so protocol tests observe the public status and
// result types rather than a transport's private stream ledger.
class http3_quic_udp_pair final {
public:
    using udp = asio::ip::udp;
    using address = detail::http3_quic_datagram_address;
    struct server_only_t final {};

    http3_quic_udp_pair(const detail::HttpServerListenerDefinition::Tls& server_config,
        std::string_view host = "localhost",
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        bool create_client = true)
        : http3_quic_udp_pair(
              std::make_unique<detail::http3_quic_tls_context>(server_config, resource),
              nullptr, host, resource, create_client) {}

    http3_quic_udp_pair(const detail::HttpServerListenerDefinition::Tls& server_config,
        server_only_t, std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : http3_quic_udp_pair(server_config, "localhost", resource, false) {}

    http3_quic_udp_pair(detail::http3_quic_tls_context& server_tls,
        server_only_t, std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : http3_quic_udp_pair(std::unique_ptr<detail::http3_quic_tls_context>{},
              &server_tls, "localhost", resource, false) {}

    http3_quic_udp_pair(const http3_quic_udp_pair&) = delete;
    http3_quic_udp_pair& operator=(const http3_quic_udp_pair&) = delete;

    ~http3_quic_udp_pair() {
        client_.reset();
        if (connection_) {
            server_.retire(*connection_);
        }
    }

    [[nodiscard]] ruvia::quic_connection& client() {
        if (!client_) {
            throw std::logic_error("QUIC test pair has no internal client");
        }
        return client_->connection();
    }
    [[nodiscard]] const udp::endpoint& server_endpoint() const noexcept {
        return server_endpoint_;
    }
    [[nodiscard]] ruvia::quic_connection& server() {
        if (!connection_) {
            throw std::logic_error("QUIC test server connection is not admitted");
        }
        return server_.server().connection(*connection_);
    }
    [[nodiscard]] ruvia::quic_connection_token connection_token() const {
        if (!connection_) {
            throw std::logic_error("QUIC test server connection is not admitted");
        }
        return *connection_;
    }
    [[nodiscard]] std::optional<ruvia::quic_connection_token> maybe_connection_token() const noexcept {
        return connection_;
    }

    [[nodiscard]] bool connected() const noexcept {
        return connection_ && server_.server().connection(*connection_).info().quic_handshake_complete &&
               (!client_ || client_->connection().info().quic_handshake_complete);
    }

    void pump(bool drop_server_packets = false, std::size_t max_turns = 32) {
        constexpr std::size_t maximum_turns = 32;
        for (std::size_t turn = 0; turn < std::min(max_turns, maximum_turns); ++turn) {
            const auto now = Clock::now();
            (void)server_.server().handle_expiry(now);
            if (client_) {
                (void)client_->handle_expiry(now);
            }
            bool progress = client_ && send_client_packets(now);
            progress = receive_server_packets(now) || progress;
            progress = send_server_packets(now, drop_server_packets) || progress;
            if (client_) {
                progress = receive_client_packets(now) || progress;
            }
            if (!progress) {
                return;
            }
        }
    }

    template <typename Predicate>
    [[nodiscard]] bool run_until(Predicate&& predicate,
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(8)) {
        const auto deadline = Clock::now() + timeout;
        while (!predicate() && Clock::now() < deadline) {
            pump();
            if (!predicate()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return predicate();
    }

private:
    http3_quic_udp_pair(std::unique_ptr<detail::http3_quic_tls_context> owned_server_tls,
        detail::http3_quic_tls_context* shared_server_tls, std::string_view host,
        std::pmr::memory_resource* resource, bool create_client)
        : server_socket_(io_),
          client_socket_(io_),
          owned_server_tls_(std::move(owned_server_tls)),
          server_tls_(shared_server_tls != nullptr ? shared_server_tls : owned_server_tls_.get()),
          client_tls_(detail::ClientTransportConfigView{
              .tlsPeerVerification = TlsPeerVerificationPolicy::kSkipVerification}),
          server_(*server_tls_, server_config_for_migration(), resource),
          resource_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
        server_socket_.open(udp::v4());
        server_socket_.bind({asio::ip::address_v4::loopback(), 0});
        server_socket_.non_blocking(true);
        server_endpoint_ = server_socket_.local_endpoint();
        if (!create_client) {
            return;
        }
        client_socket_.open(udp::v4());
        client_socket_.bind({asio::ip::address_v4::loopback(), 0});
        client_socket_.non_blocking(true);
        client_endpoint_ = client_socket_.local_endpoint();
        const auto local = detail::to_http3_quic_datagram_address(client_endpoint_);
        const auto peer = detail::to_http3_quic_datagram_address(server_endpoint_);
        if ((local.index() != 0) || (peer.index() != 0)) {
            throw std::runtime_error("invalid loopback QUIC test socket address");
        }
        ruvia::quic_connection_config config;
        config.local_address = detail::to_quic_address(std::get<0>(local));
        config.peer_address = detail::to_quic_address(std::get<0>(peer));
        client_ = std::make_unique<detail::http3_quic_client_transport>(
            client_tls_, config, host, Clock::now(), resource_);
    }

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] static ruvia::quic_server_config server_config_for_migration() {
        ruvia::quic_server_config config;
        config.local_transport_parameters.disable_active_migration = false;
        return config;
    }

    [[nodiscard]] bool send_client_packets(Clock::time_point now) {
        bool progress = false;
        for (std::size_t count = 0; count < 32; ++count) {
            const auto packet = client_->write_packet(packet_buffer_, now);
            if (packet.size == 0) {
                break;
            }
            asio::error_code error;
            const auto peer = detail::to_udp_endpoint(detail::from_quic_address(packet.peer));
            if ((peer.index() != 0)) {
                throw std::runtime_error("invalid QUIC client output address");
            }
            const auto sent = client_socket_.send_to(
                asio::buffer(packet_buffer_.data(), packet.size), std::get<0>(peer), 0, error);
            if (error || sent != packet.size) {
                throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                    "send loopback QUIC client packet");
            }
            progress = true;
        }
        return progress;
    }

    [[nodiscard]] bool send_server_packets(Clock::time_point now, bool drop_packets) {
        if (!connection_) {
            return false;
        }
        bool progress = false;
        for (std::size_t count = 0; count < 32; ++count) {
            const auto packet = server().write_packet(packet_buffer_, now);
            if (packet.size == 0) {
                break;
            }
            if (drop_packets) {
                progress = true;
                continue;
            }
            asio::error_code error;
            const auto peer = detail::to_udp_endpoint(detail::from_quic_address(packet.peer));
            if ((peer.index() != 0)) {
                throw std::runtime_error("invalid QUIC server output address");
            }
            const auto sent = server_socket_.send_to(
                asio::buffer(packet_buffer_.data(), packet.size), std::get<0>(peer), 0, error);
            if (error || sent != packet.size) {
                throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                    "send loopback QUIC server packet");
            }
            progress = true;
        }
        return progress;
    }

    [[nodiscard]] bool receive_server_packets(Clock::time_point now) {
        bool progress = false;
        for (std::size_t count = 0; count < 32; ++count) {
            udp::endpoint peer;
            asio::error_code error;
            const auto size = server_socket_.receive_from(asio::buffer(packet_buffer_), peer, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                break;
            }
            if (error) {
                // A client may close before the server sends its final packets.
                // Winsock reports that peer's ICMP port-unreachable on receive;
                // it does not retire the shared server socket.
                if (error == asio::error::connection_reset) {
                    continue;
                }
                throw std::system_error(error, "receive loopback QUIC server packet");
            }
            const auto local = detail::to_http3_quic_datagram_address(server_endpoint_);
            const auto remote = detail::to_http3_quic_datagram_address(peer);
            if ((local.index() != 0) || (remote.index() != 0)) {
                throw std::runtime_error("invalid received QUIC server address");
            }
            const auto routed = server_.route_datagram(
                std::span<const std::byte>(packet_buffer_).first(size), std::get<0>(local), std::get<0>(remote));
            if (routed.kind == ruvia::quic_server_route_kind::initial_offer) {
                if (!connection_) {
                    const auto admitted = server_.admit_initial(routed.offer, now);
                    if (admitted.status == ruvia::quic_operation_status::accepted) {
                        connection_ = admitted.connection;
                    }
                }
            } else if (routed.kind == ruvia::quic_server_route_kind::existing_connection) {
                const ruvia::quic_datagram_view datagram{
                    std::span<const std::byte>(packet_buffer_).first(size),
                    detail::to_quic_address(std::get<0>(local)), detail::to_quic_address(std::get<0>(remote))};
                (void)server_.server().receive(routed.connection, datagram, now);
            }
            progress = true;
        }
        return progress;
    }

    [[nodiscard]] bool receive_client_packets(Clock::time_point now) {
        bool progress = false;
        for (std::size_t count = 0; count < 32; ++count) {
            udp::endpoint peer;
            asio::error_code error;
            const auto size = client_socket_.receive_from(asio::buffer(packet_buffer_), peer, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                break;
            }
            if (error) {
                throw std::system_error(error, "receive loopback QUIC client packet");
            }
            const auto local = detail::to_http3_quic_datagram_address(client_endpoint_);
            const auto remote = detail::to_http3_quic_datagram_address(peer);
            if ((local.index() != 0) || (remote.index() != 0)) {
                throw std::runtime_error("invalid received QUIC client address");
            }
            const ruvia::quic_datagram_view datagram{
                std::span<const std::byte>(packet_buffer_).first(size),
                detail::to_quic_address(std::get<0>(local)), detail::to_quic_address(std::get<0>(remote))};
            (void)client_->receive(datagram, now);
            progress = true;
        }
        return progress;
    }

    asio::io_context io_;
    udp::socket server_socket_;
    udp::socket client_socket_;
    udp::endpoint server_endpoint_;
    udp::endpoint client_endpoint_;
    std::unique_ptr<detail::http3_quic_tls_context> owned_server_tls_;
    detail::http3_quic_tls_context* server_tls_{};
    detail::http3_quic_client_tls_context client_tls_;
    detail::http3_quic_server_transport server_;
    std::pmr::memory_resource* resource_;
    std::unique_ptr<detail::http3_quic_client_transport> client_;
    std::optional<ruvia::quic_connection_token> connection_;
    std::array<std::byte, 65536> packet_buffer_{};
};

}  // namespace ruvia::testing
