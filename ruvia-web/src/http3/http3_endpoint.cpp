#include "http3/http3_endpoint.h"

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <array>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <sys/socket.h>

#include <cerrno>
#endif

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/socket_base.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"

#include "http3/http3_session.h"
#include "http3/http3_udp_receive.h"
#include "server/peer_address.h"
#include "tls/tls_random.h"
#if defined(_WIN32)
#include "server/windows_acceptor.h"
#endif

namespace ruvia::detail {
namespace {

#if !defined(_WIN32)
// Datagrams read after one completed wait before waiting again, so a busy
// socket neither costs a wait per datagram nor monopolizes its worker.
constexpr std::size_t receive_burst = 32;
#endif
// RFC 9000 §14.1: a client Initial is padded to 1200 bytes; smaller
// datagrams never get a Version Negotiation reply (§6.1), which keeps it no
// larger than what triggered it.
constexpr std::size_t min_initial_datagram_bytes = 1200;

// Optional per ngtcp2, but done before any QUIC TLS object exists.
void initialize_quic_crypto() {
    static const bool initialized = ngtcp2_crypto_ossl_init() == 0;
    if (!initialized) {
        throw std::runtime_error("failed to initialize the QUIC TLS backend");
    }
}

}  // namespace

http3_endpoint::http3_endpoint(
    const http3_worker_services& services, http_server_session_config& session, asio::ip::udp::endpoint endpoint,
    std::size_t listener_index)
    : services_(services),
      session_(session),
      configured_endpoint_(endpoint),
      listener_index_(listener_index),
#if !defined(_WIN32)
      socket_(services.io_),
      receive_buffer_(services.memory_.resource()),
#endif
      connections_(services.memory_.resource()) {
    if (!session_.http3_.has_value() || session_.http3_->secret_ == nullptr) {
        throw std::logic_error("HTTP/3 endpoint requires a validated HTTP/3 service");
    }
}

http3_endpoint::~http3_endpoint() {
    close();
}

void http3_endpoint::open() {
    initialize_quic_crypto();
#if defined(_WIN32)
    // The acceptor binds exactly the configured endpoint (its port is never 0).
    local_endpoint_ = configured_endpoint_;
#else
    receive_buffer_.resize(http3_max_udp_payload_bytes);
    socket_.open(configured_endpoint_.protocol());
    socket_.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
    // Every worker binds the port; the kernel spreads datagrams by 4-tuple
    // and connection IDs steer the rest (http3_connection_id.h).
    const int enabled = 1;
    if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEPORT");
    }
#else
    throw std::system_error(asio::error::operation_not_supported, "HTTP/3 worker listeners require SO_REUSEPORT");
#endif
    socket_.bind(configured_endpoint_);
    socket_.non_blocking(true);
    local_endpoint_ = socket_.local_endpoint();
#endif
    open_ = true;
}

void http3_endpoint::start() {
    admitting_ = true;
#if !defined(_WIN32)
    services_.tasks_.spawn(run_receive_loop());
#endif
}

void http3_endpoint::close() noexcept {
    admitting_ = false;
    open_ = false;
#if !defined(_WIN32)
    std::error_code ignored;
    socket_.close(ignored);
#endif
}

std::error_code http3_endpoint::send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes) noexcept {
#if defined(_WIN32)
    const auto& placement = services_.placement_;
    if (!placement.acceptor_->transmit(placement.worker_index_, listener_index_, remote, bytes)) {
        return asio::error::no_buffer_space;
    }
    return {};
#else
    std::error_code error;
    (void)socket_.send_to(asio::buffer(bytes.data(), bytes.size()), remote, 0, error);
    return error;
#endif
}

#if !defined(_WIN32)
task<std::error_code> http3_endpoint::wait_writable() {
    const auto completion = co_await ruvia::async_asio([this](auto handler) {
        socket_.async_wait(asio::socket_base::wait_write,
            asio::bind_allocator(services_.memory_.allocator(), std::move(handler)));
    });
    co_return completion.error_code();
}
#endif

bool http3_endpoint::register_connection_id(const http3_cid_key& key, http3_session& session) {
    return connections_.try_emplace(key, &session).second;
}

void http3_endpoint::unregister_connection_id(const http3_cid_key& key) noexcept {
    connections_.erase(key);
}

#if !defined(_WIN32)
task<void> http3_endpoint::run_receive_loop() {
    try {
        while (socket_.is_open()) {
            asio::ip::udp::endpoint remote;
            const auto completion = co_await ruvia::async_asio<std::size_t>([this, &remote](auto handler) {
                socket_.async_receive_from(asio::buffer(receive_buffer_.data(), receive_buffer_.size()), remote,
                    asio::bind_allocator(services_.memory_.allocator(), std::move(handler)));
            });
            if (!socket_.is_open()) {
                co_return;
            }
            if (const auto error = completion.error_code()) {
                if (http3_ignorable_receive_error(error)) {
                    continue;
                }
                throw std::system_error(error, "HTTP/3 UDP receive failed");
            }
            receive(std::span<const std::byte>(receive_buffer_.data(), completion.result()), remote, false);
            for (std::size_t burst = 1; burst < receive_burst && socket_.is_open(); ++burst) {
                std::error_code error;
                const auto size = socket_.receive_from(
                    asio::buffer(receive_buffer_.data(), receive_buffer_.size()), remote, 0, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    break;
                }
                if (error) {
                    if (http3_ignorable_receive_error(error)) {
                        continue;
                    }
                    throw std::system_error(error, "HTTP/3 UDP receive failed");
                }
                receive(std::span<const std::byte>(receive_buffer_.data(), size), remote, false);
            }
        }
    } catch (...) {
        // The listener cannot receive any more: its connections time out and
        // the failure is reported once.
        admitting_ = false;
        ruvia::report_unhandled_failure("HTTP/3 receive", std::current_exception());
    }
}
#endif

void http3_endpoint::receive_handoff(http3_handoff_datagram& datagram) noexcept {
    if (!open_) {
        return;
    }
    receive(std::span<const std::byte>(datagram.bytes_.data(), datagram.bytes_.size()), datagram.remote_, true);
}

void http3_endpoint::receive(
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, bool handed_off) noexcept {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(&header, data, bytes.size(), http3_connection_id_length);
    if (decoded == NGTCP2_ERR_VERSION_NEGOTIATION) {
        if (bytes.size() >= min_initial_datagram_bytes && admitting_) {
            send_version_negotiation(header, remote);
        }
        return;
    }
    if (decoded != 0 || header.dcidlen > NGTCP2_MAX_CIDLEN) {
        return;
    }
    const auto destination = std::span<const std::uint8_t>(header.dcid, header.dcidlen);
    if (const auto found = connections_.find(http3_cid_key::from(destination)); found != connections_.end()) {
        found->second->receive(bytes, remote);
        return;
    }
    // A datagram another thread received was already steered to this worker.
    if (!handed_off) {
        const auto& placement = services_.placement_;
        const auto owner = http3_connection_id_owner(secret(), destination, placement.directory_->worker_count());
        if (owner.has_value() && *owner != placement.worker_index_) {
            try {
                http3_handoff_datagram datagram{
                    .bytes_ = std::vector<std::byte>(bytes.begin(), bytes.end()), .remote_ = remote, .ecn_ = 0};
                (void)placement.directory_->hand_off(*owner, listener_index_, std::move(datagram));
            } catch (...) {
                // Dropped, as the network may drop it.
            }
            return;
        }
    }
    // Only a client Initial opens a connection; a short header naming no
    // connection of this worker is dropped.
    if (header.version != 0) {
        accept(bytes, remote);
    }
}

void http3_endpoint::accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote) noexcept {
    if (!admitting_ || !http_server_worker_running(services_.state_)) {
        return;
    }
    ngtcp2_pkt_hd header{};
    if (ngtcp2_accept(&header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != 0) {
        return;
    }
    const auto& max_connections = services_.options_.server_.max_connections_per_worker_;
    // At the cap the connection idle longest gives up its slot, as for TCP.
    if (max_connections.has_value() && services_.connection_count_.active() >= *max_connections &&
        !services_.scanner_.evict_oldest_reusable()) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        auto session = make_pmr_object<http3_session>(
            services_.memory_.resource(), *this, connection_slot(services_.connection_count_), header, remote);
        session->receive(bytes, remote);
        services_.tasks_.spawn(serve(std::move(session)));
    } catch (...) {
        auto failure = std::current_exception();
        std::pmr::string address(services_.memory_.resource());
        try {
            assign_peer_address(address, remote.address());
        } catch (...) {
            address.clear();
        }
        services_.options_.connection_failure_.invoke(address, std::move(failure));
    }
}

task<void> http3_endpoint::serve(session_pointer session) {
    try {
        co_await session->run();
    } catch (...) {
        // run() already ended the connection; its reason goes to the
        // connection-failure sink rather than the worker's task scope.
        services_.options_.connection_failure_.invoke({}, std::current_exception());
    }
}

void http3_endpoint::send_version_negotiation(
    const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote) noexcept {
    // RFC 9000 §17.2.1: the client's IDs echoed back, swapped, with the
    // versions this server accepts.
    static constexpr std::array<std::uint32_t, 2> versions{NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    std::array<std::uint8_t, 1 + 4 + 1 + 255 + 1 + 255 + sizeof(versions)> packet{};
    unsigned char unused = 0;
    if (!fill_random_bytes(std::span<unsigned char>(&unused, 1))) {
        return;
    }
    const auto written = ngtcp2_pkt_write_version_negotiation(packet.data(), packet.size(), unused, header.scid,
        header.scidlen, header.dcid, header.dcidlen, versions.data(), versions.size());
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))));
    }
}

}  // namespace ruvia::detail
