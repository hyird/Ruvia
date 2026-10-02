#pragma once

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
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/http/quic_connection.h"
#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"
#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"

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
        : server_socket_(io_),
          client_socket_(io_),
          server_tls_(server_config, resource),
          client_tls_(detail::ClientTransportConfigView{
              .tlsPeerVerification = TlsPeerVerificationPolicy::kSkipVerification}),
          server_(server_tls_, {}, resource),
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
        if (!local || !peer) {
            throw std::runtime_error("invalid loopback QUIC test socket address");
        }
        ruvia::quic_connection_config config;
        config.local_address = detail::to_quic_address(*local);
        config.peer_address = detail::to_quic_address(*peer);
        client_ = std::make_unique<detail::http3_quic_client_transport>(
            client_tls_, config, host, Clock::now(), resource_);
    }

    http3_quic_udp_pair(const detail::HttpServerListenerDefinition::Tls& server_config,
        server_only_t, std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : http3_quic_udp_pair(server_config, "localhost", resource, false) {}

    ~http3_quic_udp_pair() {
        client_.reset();
        if (connection_) {
            server_.retire(*connection_);
        }
    }

    http3_quic_udp_pair(const http3_quic_udp_pair&) = delete;
    http3_quic_udp_pair& operator=(const http3_quic_udp_pair&) = delete;

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

    void pump() {
        constexpr std::size_t max_turns = 32;
        for (std::size_t turn = 0; turn < max_turns; ++turn) {
            const auto now = Clock::now();
            (void)server_.server().handle_expiry(now);
            if (client_) {
                (void)client_->handle_expiry(now);
            }
            bool progress = client_ && send_client_packets(now);
            progress = receive_server_packets(now) || progress;
            progress = send_server_packets(now) || progress;
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
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] bool send_client_packets(Clock::time_point now) {
        bool progress = false;
        for (std::size_t count = 0; count < 32; ++count) {
            const auto packet = client_->write_packet(packet_buffer_, now);
            if (packet.size == 0) {
                break;
            }
            asio::error_code error;
            const auto peer = detail::to_udp_endpoint(detail::from_quic_address(packet.peer));
            if (!peer) {
                throw std::runtime_error("invalid QUIC client output address");
            }
            const auto sent = client_socket_.send_to(
                asio::buffer(packet_buffer_.data(), packet.size), *peer, 0, error);
            if (error || sent != packet.size) {
                throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                    "send loopback QUIC client packet");
            }
            progress = true;
        }
        return progress;
    }

    [[nodiscard]] bool send_server_packets(Clock::time_point now) {
        if (!connection_) {
            return false;
        }
        bool progress = false;
        for (std::size_t count = 0; count < 32; ++count) {
            const auto packet = server().write_packet(packet_buffer_, now);
            if (packet.size == 0) {
                break;
            }
            asio::error_code error;
            const auto peer = detail::to_udp_endpoint(detail::from_quic_address(packet.peer));
            if (!peer) {
                throw std::runtime_error("invalid QUIC server output address");
            }
            const auto sent = server_socket_.send_to(
                asio::buffer(packet_buffer_.data(), packet.size), *peer, 0, error);
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
                throw std::system_error(error, "receive loopback QUIC server packet");
            }
            const auto local = detail::to_http3_quic_datagram_address(server_endpoint_);
            const auto remote = detail::to_http3_quic_datagram_address(peer);
            if (!local || !remote) {
                throw std::runtime_error("invalid received QUIC server address");
            }
            const auto routed = server_.route_datagram(
                std::span<const std::byte>(packet_buffer_).first(size), *local, *remote);
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
                    detail::to_quic_address(*local), detail::to_quic_address(*remote)};
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
            if (!local || !remote) {
                throw std::runtime_error("invalid received QUIC client address");
            }
            const ruvia::quic_datagram_view datagram{
                std::span<const std::byte>(packet_buffer_).first(size),
                detail::to_quic_address(*local), detail::to_quic_address(*remote)};
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
    detail::http3_quic_tls_context server_tls_;
    detail::http3_quic_client_tls_context client_tls_;
    detail::http3_quic_server_transport server_;
    std::pmr::memory_resource* resource_;
    std::unique_ptr<detail::http3_quic_client_transport> client_;
    std::optional<ruvia::quic_connection_token> connection_;
    std::array<std::byte, 65536> packet_buffer_{};
};

}  // namespace ruvia::testing
