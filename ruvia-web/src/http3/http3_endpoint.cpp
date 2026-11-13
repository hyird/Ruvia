#include "http3/http3_endpoint.h"

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/socket_base.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"

#include "http3/http3_session.h"
#include "http3/http3_udp_packet.h"
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

http3_endpoint::http3_endpoint(const http3_worker_services& services, http_server_session_config& session,
    asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections)
#if !defined(_WIN32)
    : activated_(services.worker_),
      services_(services),
#else
    : services_(services),
#endif
      session_(session),
      configured_endpoint_(endpoint),
      listener_index_(listener_index),
      tasks_(tasks),
      listener_connections_(listener_connections),
#if !defined(_WIN32)
      socket_(services.io_),
      receive_buffer_(),
#endif
      connections_() {
    if (!session_.http3_.has_value() || session_.http3_->secret_ == nullptr) {
        throw std::logic_error("HTTP/3 endpoint requires a validated HTTP/3 service");
    }
}

http3_endpoint::~http3_endpoint() {
    close();
    if (registered_) {
        services_.placement_.directory_->unregister_endpoint(services_.placement_.worker_index_, *this);
    }
}

void http3_endpoint::open([[maybe_unused]] inherited_listener_sockets* inherited) {
    initialize_quic_crypto();
#if defined(_WIN32)
    // The acceptor binds exactly the configured endpoint (its port is never 0).
    local_endpoint_ = configured_endpoint_;
#else
    receive_buffer_.resize(http3_max_udp_payload_bytes);
    services_.placement_.directory_->open_listener_socket(
        services_.placement_.worker_index_, listener_index_, configured_endpoint_, secret(), inherited, socket_, session_.ipv6_only_);
    local_endpoint_ = socket_.local_endpoint();
    enable_http3_packet_info(socket_);
#endif
    services_.placement_.directory_->register_endpoint(services_.placement_.worker_index_, *this,
        local_endpoint_, session_.http3_->secret_, listener_index_);
    registered_ = true;
    open_ = true;
#if !defined(_WIN32)
    tasks_.spawn(run_receive_loop());
#endif
}

void http3_endpoint::start() noexcept {
    admitting_ = true;
#if !defined(_WIN32)
    activated_.notify();
#endif
}

void http3_endpoint::retire() noexcept {
    admitting_ = false;
    retiring_ = true;
    if (sessions_ == 0) {
        release_after_last_connection();
    }
}

void http3_endpoint::release_after_last_connection() noexcept {
#if defined(_WIN32)
    // The other workers' connections may still need the acceptor's socket.
    if (std::exchange(open_, false)) {
        services_.placement_.acceptor_->release_http3_listener(listener_index_);
    }
#else
    close();
#endif
}

void http3_endpoint::close() noexcept {
    admitting_ = false;
    open_ = false;
#if !defined(_WIN32)
    activated_.notify();
    std::error_code ignored;
    socket_.close(ignored);
#endif
}

#if !defined(_WIN32)
native_socket http3_endpoint::duplicate_socket() {
    if (!socket_.is_open()) {
        throw std::logic_error("the HTTP/3 listener socket is closed");
    }
    return duplicate_native_socket(socket_.native_handle());
}
#endif

std::error_code http3_endpoint::send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
#if defined(_WIN32)
    const auto& placement = services_.placement_;
    if (!placement.acceptor_->transmit(placement.worker_index_, listener_index_, remote, bytes, local, interface_index)) {
        return asio::error::no_buffer_space;
    }
    return {};
#else
    return send_http3_packet(socket_, remote, bytes, local, interface_index);
#endif
}

#if !defined(_WIN32)
task<std::error_code> http3_endpoint::wait_writable() {
    const auto completion = co_await ruvia::async_asio([this](auto handler) {
        socket_.async_wait(asio::socket_base::wait_write,
            std::move(handler));
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
    co_await activated_.wait();
    try {
        while (socket_.is_open()) {
            const auto completion = co_await ruvia::async_asio([this](auto handler) {
                socket_.async_wait(asio::socket_base::wait_read,
                    std::move(handler));
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
            for (std::size_t burst = 0; burst < receive_burst && socket_.is_open(); ++burst) {
                asio::ip::udp::endpoint remote;
                auto local = local_endpoint_;
                std::uint32_t interface_index{};
                std::error_code error;
                const auto size = receive_http3_packet(socket_, receive_buffer_, remote, local, interface_index, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    break;
                }
                if (error) {
                    if (http3_ignorable_receive_error(error)) {
                        continue;
                    }
                    throw std::system_error(error, "HTTP/3 UDP receive failed");
                }
                receive(std::span<const std::byte>(receive_buffer_.data(), size), remote, local, interface_index);
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
    receive(std::span<const std::byte>(datagram.bytes_.data(), datagram.bytes_.size()), datagram.remote_,
        datagram.local_, datagram.interface_index_);
}

void http3_endpoint::receive(
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(&header, data, bytes.size(), http3_connection_id_length);
    if ((decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) || header.dcidlen > NGTCP2_MAX_CIDLEN) {
        return;
    }
    const auto destination = std::span<const std::uint8_t>(header.dcid, header.dcidlen);
    const auto key = http3_cid_key::from(destination);
    const auto& placement = services_.placement_;
    const http3_endpoint_registration* authenticated = nullptr;
    http3_endpoint* admitting = nullptr;
    std::optional<std::size_t> owner;
    // SO_REUSEPORT may choose any socket, including a removed listener's.
    // Search the one worker-local directory before interpreting an unknown
    // CID with the receiving socket's (possibly obsolete) secret.
    for (const auto& registration : placement.directory_->endpoints(placement.worker_index_)) {
        if (registration.local_ != local_endpoint_) {
            continue;
        }
        auto* endpoint = registration.endpoint_;
        if (endpoint != nullptr && endpoint->open_) {
            if (const auto found = endpoint->connections_.find(key); found != endpoint->connections_.end()) {
                found->second->receive(bytes, remote, local, interface_index);
                return;
            }
            if (endpoint->admitting_) {
                admitting = endpoint;
            }
        }
    }
    for (const auto& registration : placement.directory_->endpoints(placement.worker_index_)) {
        if (registration.local_ != local_endpoint_) {
            continue;
        }
        if (const auto candidate = http3_connection_id_owner(*registration.secret_, destination, placement.directory_->worker_count())) {
            authenticated = &registration;
            owner = candidate;
        }
    }
    if (owner.has_value() && *owner != placement.worker_index_) {
        (void)placement.directory_->hand_off(*owner, authenticated->listener_index_, bytes, remote, local, interface_index);
        return;
    }
    auto* endpoint = authenticated != nullptr ? authenticated->endpoint_ : admitting;
    if (endpoint == nullptr || !endpoint->open_) {
        return;
    }
    if (!owner.has_value() && header.version != 0) {
        owner = http3_initial_worker(endpoint->secret(), destination, placement.directory_->worker_count());
        if (*owner != placement.worker_index_) {
            (void)placement.directory_->hand_off(*owner, endpoint->listener_index_, bytes, remote, local, interface_index);
            return;
        }
    }
    if (decoded == NGTCP2_ERR_VERSION_NEGOTIATION) {
        if (bytes.size() >= min_initial_datagram_bytes && endpoint->admitting_) {
            if (const auto admission = services_.options_.connection_admission_) {
                const connection_event event(services_.connection_ids_.next(), {to_ip_address(local.address()), local.port()},
                    {to_ip_address(remote.address()), remote.port()}, true);
                if (!admission(event)) {
                    services_.refused_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            endpoint->send_version_negotiation(header, remote, local, interface_index);
        }
    } else if (header.version != 0) {
        endpoint->accept(bytes, remote, local, interface_index);
    } else if (endpoint->admitting_ && owner == placement.worker_index_) {
        endpoint->send_stateless_reset(destination, bytes.size(), remote, local, interface_index);
    }
}

void http3_endpoint::accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    if (!admitting_ || !http_server_worker_running(services_.state_)) {
        return;
    }
    ngtcp2_pkt_hd header{};
    if (ngtcp2_accept(&header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != 0) {
        return;
    }
    const auto connection_id = services_.connection_ids_.next();
    connection_event event(connection_id, {to_ip_address(local.address()), local.port()},
        {to_ip_address(remote.address()), remote.port()}, true);
    if (services_.options_.connection_admission_ &&
        !services_.options_.connection_admission_(event)) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ngtcp2_cid original_dcid = header.dcid;
    std::optional<ngtcp2_cid> retry_scid;
    std::span<const std::uint8_t> retry_token;
    const bool require_retry = session_.http3_->retry_;
    if (require_retry || header.tokenlen != 0) {
        if (header.tokenlen == 0) {
            const auto max_bytes = bytes.size() > (std::numeric_limits<std::size_t>::max)() / 3
                                       ? (std::numeric_limits<std::size_t>::max)()
                                       : bytes.size() * 3;
            send_retry(header, remote, max_bytes, local, interface_index);
            return;
        }
        if (header.token[0] != NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2) {
            if (require_retry || header.dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN) {
                send_stateless_connection_close(header, remote, NGTCP2_INVALID_TOKEN, local, interface_index);
                return;
            }
        } else {
            const auto& server = services_.options_.server_;
            const auto timeout_ms = server.header_completion_timeout_.has_value()
                                        ? server.header_completion_timeout_
                                        : server.request_header_timeout_;
            constexpr auto max_milliseconds = static_cast<std::int64_t>(UINT64_MAX / NGTCP2_MILLISECONDS);
            const auto milliseconds =
                timeout_ms.has_value() ? (std::max)(timeout_ms->value().count(), std::int64_t{1}) : std::int64_t{0};
            const ngtcp2_duration token_lifetime =
                timeout_ms.has_value()
                    ? (milliseconds >= max_milliseconds
                              ? UINT64_MAX
                              : static_cast<ngtcp2_duration>(milliseconds) * NGTCP2_MILLISECONDS)
                    : 10 * NGTCP2_SECONDS;
            const auto timestamp = static_cast<ngtcp2_tstamp>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            const int verified = ngtcp2_crypto_verify_retry_token2(&original_dcid,
                header.token, header.tokenlen, secret().data(), secret().size(),
                header.version, reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()),
                static_cast<ngtcp2_socklen>(remote.size()), &header.dcid,
                token_lifetime, timestamp);
            if (verified != 0) {
                if (verified == NGTCP2_CRYPTO_ERR_VERIFY_TOKEN || require_retry ||
                    header.dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN) {
                    send_stateless_connection_close(header, remote, NGTCP2_INVALID_TOKEN, local, interface_index);
                    return;
                }
            } else {
                retry_scid = header.dcid;
                retry_token = std::span<const std::uint8_t>(header.token, header.tokenlen);
            }
        }
    }
    const auto& max_connections = services_.options_.server_.max_connections_per_worker_;
    // At the cap the connection idle longest gives up its slot, as for TCP.
    if (max_connections.has_value() && services_.connection_count_.active() >= *max_connections &&
        !services_.scanner_.evict_oldest_reusable()) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        connection_lifecycle lifecycle(services_.registry_, &session_, std::move(event),
            services_.options_.connection_open_, services_.options_.connection_close_, interface_index);
        auto session = std::make_unique<http3_session>(*this, connection_slot(listener_connections_), header, remote, original_dcid, retry_scid, retry_token, std::move(lifecycle));
        session->receive(bytes, remote, local, interface_index);
        tasks_.spawn(serve(std::move(session)));
    } catch (...) {
        auto failure = std::current_exception();
        std::string address;
        try {
            assign_peer_address(address, remote.address());
        } catch (...) {
            address.clear();
        }
        services_.options_.connection_failure_.invoke(address, std::move(failure));
    }
}

task<void> http3_endpoint::serve(session_pointer session) {
    // Spawning runs this task until its first suspension, so the count
    // includes every session spawned.
    ++sessions_;
    try {
        co_await session->run();
    } catch (...) {
        // run() already ended the connection; its reason goes to the
        // connection-failure sink rather than the worker's task scope.
        services_.options_.connection_failure_.invoke(session->remote_address(), std::current_exception());
    }
    // The session object itself is destroyed with this frame, which the
    // listener's scope joins before the endpoint goes away.
    if (--sessions_ == 0 && retiring_) {
        release_after_last_connection();
    }
}

void http3_endpoint::send_version_negotiation(
    const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
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
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))),
            local, interface_index);
    }
}

void http3_endpoint::send_retry(
    const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::size_t max_packet_bytes,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    ngtcp2_cid retry_scid{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!make_http3_connection_id(secret(), services_.placement_.worker_index_, retry_scid)) {
            return;
        }
        if (ngtcp2_cid_eq(&retry_scid, &header.dcid) == 0) {
            break;
        }
    }
    if (ngtcp2_cid_eq(&retry_scid, &header.dcid) != 0) {
        return;
    }
    std::array<std::uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token{};
    const auto timestamp = static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const auto token_len = ngtcp2_crypto_generate_retry_token2(token.data(),
        secret().data(), secret().size(), header.version,
        reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()),
        static_cast<ngtcp2_socklen>(remote.size()), &retry_scid,
        &header.dcid, timestamp);
    if (token_len < 0) {
        return;
    }
    std::array<std::uint8_t, http3_max_datagram_bytes> packet{};
    const auto capacity = (std::min)(packet.size(), max_packet_bytes);
    const auto written = ngtcp2_crypto_write_retry(packet.data(), capacity,
        header.version, &header.scid, &retry_scid, &header.dcid,
        token.data(), static_cast<std::size_t>(token_len));
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

void http3_endpoint::send_stateless_connection_close(
    const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::uint64_t error_code,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    std::array<std::uint8_t, http3_max_datagram_bytes> packet{};
    const auto written = ngtcp2_crypto_write_connection_close(packet.data(),
        packet.size(), header.version, &header.scid, &header.dcid,
        error_code, nullptr, 0);
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

bool http3_endpoint::consume_stateless_reset_budget() noexcept {
    constexpr std::size_t max_burst = 32;
    constexpr auto refill_interval = std::chrono::milliseconds(25);
    const auto current = std::chrono::steady_clock::now();
    if (stateless_reset_refill_at_ == std::chrono::steady_clock::time_point{}) {
        stateless_reset_refill_at_ = current;
    } else if (current > stateless_reset_refill_at_) {
        const auto steps = static_cast<std::size_t>((current - stateless_reset_refill_at_) / refill_interval);
        if (steps != 0) {
            stateless_reset_tokens_ =
                steps >= max_burst - stateless_reset_tokens_ ? max_burst : stateless_reset_tokens_ + steps;
            stateless_reset_refill_at_ = current - ((current - stateless_reset_refill_at_) % refill_interval);
        }
    }
    if (stateless_reset_tokens_ == 0) {
        return false;
    }
    --stateless_reset_tokens_;
    return true;
}

void http3_endpoint::send_stateless_reset(std::span<const std::uint8_t> destination_cid,
    std::size_t trigger_bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    constexpr std::size_t min_stateless_reset_bytes =
        1 + NGTCP2_MIN_STATELESS_RESET_RANDLEN + NGTCP2_STATELESS_RESET_TOKENLEN;
    if (trigger_bytes <= min_stateless_reset_bytes || !consume_stateless_reset_budget()) {
        return;
    }
    ngtcp2_cid cid{};
    ngtcp2_cid_init(&cid, destination_cid.data(), destination_cid.size());
    http3_quic_stateless_reset_token token{};
    std::array<unsigned char, http3_connection_id_length + 22 - NGTCP2_STATELESS_RESET_TOKENLEN> random_bytes{};
    if (!make_http3_stateless_reset_token(secret(), cid, token) || !fill_random_bytes(random_bytes)) {
        return;
    }
    std::array<std::byte, http3_connection_id_length + 23> packet{};
    const auto written = write_http3_quic_stateless_reset(
        token, random_bytes, packet, trigger_bytes - 1);
    if (written != 0) {
        (void)send(remote, std::span<const std::byte>(packet.data(), written), local, interface_index);
    }
}

}  // namespace ruvia::detail
