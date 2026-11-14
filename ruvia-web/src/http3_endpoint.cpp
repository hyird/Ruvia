#include "http3_endpoint.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/filter.h>
#endif

#include <asio/error.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>

#include "asio_bridge.h"
#include "connection_lifecycle.h"
#include "crypto.h"
#include "failure_report.h"
#include "http3_quic_driver.h"
#include "http3_session.h"
#include "ip_address_conversion.h"
#include "move_only_function.h"
#include "server_listener.h"
#include "worker_ref.h"
#if defined(_WIN32)
#include "windows_acceptor.h"
#endif

#if !defined(_WIN32)

namespace ruvia::detail {

inline void enable_http3_packet_info(asio::ip::udp::socket& socket) {
    const int enabled = 1;
    const bool v6 = socket.local_endpoint().protocol() == asio::ip::udp::v6();
    int result;
    if (v6) {
#if defined(IPV6_RECVPKTINFO)
        result = ::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_RECVPKTINFO, &enabled, sizeof(enabled));
#else
        result = ::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_PKTINFO, &enabled, sizeof(enabled));
#endif
    } else {
#if defined(IP_PKTINFO)
        result = ::setsockopt(socket.native_handle(), IPPROTO_IP, IP_PKTINFO, &enabled, sizeof(enabled));
#else
        result = ::setsockopt(socket.native_handle(), IPPROTO_IP, IP_RECVDSTADDR, &enabled, sizeof(enabled));
#endif
    }
    if (result != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable QUIC packet information");
    }
}

// Receives actual destination and ingress interface even for wildcard sockets.
// Socket is nonblocking; callers wait on the existing Asio reactor between bursts.
inline std::size_t receive_http3_packet(asio::ip::udp::socket& socket, std::span<std::byte> bytes, asio::ip::udp::endpoint& remote, asio::ip::udp::endpoint& local, std::uint32_t& interface_index, std::error_code& error) noexcept {
    alignas(cmsghdr) std::array<std::byte, 256> control{};
    iovec buffer{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_name = remote.data();
    message.msg_namelen = static_cast<socklen_t>(remote.capacity());
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    ssize_t received;
    do {
        received = ::recvmsg(socket.native_handle(), &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        error = {errno, asio::system_category()};
        return 0;
    }
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        error = asio::error::message_size;
        return 0;
    }
    if (message.msg_controllen > control.size()) {
        error = asio::error::fault;
        return 0;
    }
    remote.resize(message.msg_namelen);
    bool found = false;
    interface_index = 0;
    for (std::size_t offset = 0; message.msg_controllen - offset >= CMSG_LEN(0);) {
        auto* cmsg = reinterpret_cast<cmsghdr*>(control.data() + offset);
        const auto remaining = message.msg_controllen - offset;
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) {
            error = asio::error::fault;
            return 0;
        }
#if defined(IP_PKTINFO)
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
            in_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            asio::ip::address_v4::bytes_type address{};
            std::memcpy(address.data(), &info.ipi_addr, address.size());
            local.address(asio::ip::address_v4(address));
            if (info.ipi_ifindex < 0) {
                error = asio::error::fault;
                return 0;
            }
            interface_index = static_cast<std::uint32_t>(info.ipi_ifindex);
            found = true;
        }
#else
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVDSTADDR && cmsg->cmsg_len >= CMSG_LEN(sizeof(in_addr))) {
            asio::ip::address_v4::bytes_type address{};
            std::memcpy(address.data(), CMSG_DATA(cmsg), address.size());
            local.address(asio::ip::address_v4(address));
            found = true;
        }
#endif
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
            in6_pktinfo info{};
            std::memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
            asio::ip::address_v6::bytes_type address{};
            std::memcpy(address.data(), &info.ipi6_addr, address.size());
            auto v6 = asio::ip::address_v6(address);
            interface_index = info.ipi6_ifindex;
            if (v6.is_link_local() || v6.is_multicast_link_local()) {
                v6.scope_id(interface_index);
            }
            local.address(v6);
            found = true;
        }
        // CMSG_SPACE keeps the native alignment without CMSG_NXTHDR's
        // signed pointer-difference comparison on musl.
        const auto advance = CMSG_SPACE(cmsg->cmsg_len - CMSG_LEN(0));
        if (advance > remaining) {
            break;
        }
        offset += advance;
    }
    if (!found) {
        error = asio::error::fault;
        return 0;
    }
    error.clear();
    return static_cast<std::size_t>(received);
}

inline std::error_code send_http3_packet(asio::ip::udp::socket& socket, const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    alignas(cmsghdr) std::array<std::byte, 256> control{};
    iovec buffer{const_cast<std::byte*>(bytes.data()), bytes.size()};
    msghdr message{};
    message.msg_name = const_cast<asio::ip::udp::endpoint::data_type*>(remote.data());
    message.msg_namelen = static_cast<socklen_t>(remote.size());
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    auto* cmsg = reinterpret_cast<cmsghdr*>(control.data());
    if (local.address().is_v4()) {
        if (!std::in_range<int>(interface_index)) {
            return asio::error::invalid_argument;
        }
        const auto address = local.address().to_v4().to_bytes();
        cmsg->cmsg_level = IPPROTO_IP;
#if defined(IP_PKTINFO)
        cmsg->cmsg_type = IP_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
        in_pktinfo info{};
        info.ipi_ifindex = static_cast<int>(interface_index);
        std::memcpy(&info.ipi_spec_dst, address.data(), address.size());
        std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
#else
        cmsg->cmsg_type = IP_SENDSRCADDR;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in_addr));
        message.msg_controllen = CMSG_SPACE(sizeof(in_addr));
        std::memcpy(CMSG_DATA(cmsg), address.data(), address.size());
#endif
    } else {
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
        message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
        in6_pktinfo info{};
        const auto address = local.address().to_v6().to_bytes();
        info.ipi6_ifindex = interface_index;
        std::memcpy(&info.ipi6_addr, address.data(), address.size());
        std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
    }
    ssize_t sent;
    do {
        sent = ::sendmsg(socket.native_handle(), &message, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        return {errno, asio::system_category()};
    }
    if (static_cast<std::size_t>(sent) != bytes.size()) {
        return asio::error::message_size;
    }
    return {};
}

}  // namespace ruvia::detail
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

void initialize_server_quic_crypto() {
    if (!try_initialize_quic_crypto()) {
        throw std::runtime_error("failed to initialize the QUIC TLS backend");
    }
}

}  // namespace

http3_endpoint::http3_endpoint(const http3_worker_services& services, http_server_session_config& session, asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections)
#if !defined(_WIN32)
    :
    activated_(services.worker_),
    services_(services),
#else
    :
    services_(services),
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
    initialize_server_quic_crypto();
#if defined(_WIN32)
    // The acceptor binds exactly the configured endpoint (its port is never 0).
    local_endpoint_ = configured_endpoint_;
#else
    receive_buffer_.resize(http3_max_udp_payload_bytes);
    services_.placement_.directory_->open_listener_socket(services_.placement_.worker_index_, listener_index_, configured_endpoint_, secret(), inherited, socket_, session_.ipv6_only_);
    local_endpoint_ = socket_.local_endpoint();
    enable_http3_packet_info(socket_);
#endif
    services_.placement_.directory_->register_endpoint(services_.placement_.worker_index_, *this, local_endpoint_, session_.http3_->secret_, listener_index_);
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

std::error_code http3_endpoint::send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
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
        socket_.async_wait(asio::socket_base::wait_write, std::move(handler));
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
                socket_.async_wait(asio::socket_base::wait_read, std::move(handler));
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
    receive(std::span<const std::byte>(datagram.bytes_.data(), datagram.bytes_.size()), datagram.remote_, datagram.local_, datagram.interface_index_);
}

void http3_endpoint::receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
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
                const connection_event event(services_.connection_ids_.next(), {to_ip_address(local.address()), local.port()}, {to_ip_address(remote.address()), remote.port()}, true);
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

void http3_endpoint::accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    if (!admitting_ || !http_server_worker_running(services_.state_)) {
        return;
    }
    ngtcp2_pkt_hd header{};
    if (ngtcp2_accept(&header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()) != 0) {
        return;
    }
    const auto connection_id = services_.connection_ids_.next();
    connection_event event(connection_id, {to_ip_address(local.address()), local.port()}, {to_ip_address(remote.address()), remote.port()}, true);
    if (services_.options_.connection_admission_ && !services_.options_.connection_admission_(event)) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ngtcp2_cid original_dcid = header.dcid;
    std::optional<ngtcp2_cid> retry_scid;
    std::span<const std::uint8_t> retry_token;
    const bool require_retry = session_.http3_->retry_;
    if (require_retry || header.tokenlen != 0) {
        if (header.tokenlen == 0) {
            const auto max_bytes = bytes.size() > (std::numeric_limits<std::size_t>::max)() / 3 ? (std::numeric_limits<std::size_t>::max)() : bytes.size() * 3;
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
            const ngtcp2_duration token_lifetime = (std::max)(http3_quic_duration(server.header_completion_timeout_), ngtcp2_duration{NGTCP2_MILLISECONDS});
            const auto timestamp = http3_quic_now();
            const int verified = ngtcp2_crypto_verify_retry_token2(&original_dcid, header.token, header.tokenlen, secret().data(), secret().size(), header.version, reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()), static_cast<ngtcp2_socklen>(remote.size()), &header.dcid, token_lifetime, timestamp);
            if (verified != 0) {
                if (verified == NGTCP2_CRYPTO_ERR_VERIFY_TOKEN || require_retry || header.dcid.datalen < NGTCP2_MIN_INITIAL_DCIDLEN) {
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
    if (max_connections.has_value() && services_.connection_count_.active() >= *max_connections && !services_.scanner_.evict_oldest_reusable()) {
        services_.refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        connection_lifecycle lifecycle(services_.registry_, &session_, std::move(event), services_.options_.connection_open_, services_.options_.connection_close_, interface_index);
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

void http3_endpoint::send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    // RFC 9000 §17.2.1: the client's IDs echoed back, swapped, with the
    // versions this server accepts.
    static constexpr std::array<std::uint32_t, 2> versions{NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    std::array<std::uint8_t, 1 + 4 + 1 + 255 + 1 + 255 + sizeof(versions)> packet{};
    unsigned char unused = 0;
    if (!fill_random_bytes(std::span<unsigned char>(&unused, 1))) {
        return;
    }
    const auto written = ngtcp2_pkt_write_version_negotiation(packet.data(), packet.size(), unused, header.scid, header.scidlen, header.dcid, header.dcidlen, versions.data(), versions.size());
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

void http3_endpoint::send_retry(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::size_t max_packet_bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
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
    const auto timestamp = http3_quic_now();
    const auto token_len = ngtcp2_crypto_generate_retry_token2(token.data(), secret().data(), secret().size(), header.version, reinterpret_cast<const ngtcp2_sockaddr*>(remote.data()), static_cast<ngtcp2_socklen>(remote.size()), &retry_scid, &header.dcid, timestamp);
    if (token_len < 0) {
        return;
    }
    std::array<std::uint8_t, http3_max_datagram_bytes> packet{};
    const auto capacity = (std::min)(packet.size(), max_packet_bytes);
    const auto written = ngtcp2_crypto_write_retry(packet.data(), capacity, header.version, &header.scid, &retry_scid, &header.dcid, token.data(), static_cast<std::size_t>(token_len));
    if (written > 0) {
        (void)send(remote, std::as_bytes(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(written))), local, interface_index);
    }
}

void http3_endpoint::send_stateless_connection_close(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::uint64_t error_code, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    std::array<std::uint8_t, http3_max_datagram_bytes> packet{};
    const auto written = ngtcp2_crypto_write_connection_close(packet.data(), packet.size(), header.version, &header.scid, &header.dcid, error_code, nullptr, 0);
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
            stateless_reset_tokens_ = steps >= max_burst - stateless_reset_tokens_ ? max_burst : stateless_reset_tokens_ + steps;
            stateless_reset_refill_at_ = current - ((current - stateless_reset_refill_at_) % refill_interval);
        }
    }
    if (stateless_reset_tokens_ == 0) {
        return false;
    }
    --stateless_reset_tokens_;
    return true;
}

void http3_endpoint::send_stateless_reset(std::span<const std::uint8_t> destination_cid, std::size_t trigger_bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    constexpr std::size_t min_stateless_reset_bytes = 1 + NGTCP2_MIN_STATELESS_RESET_RANDLEN + NGTCP2_STATELESS_RESET_TOKENLEN;
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
    const auto written = write_http3_quic_stateless_reset(token, random_bytes, packet, trigger_bytes - 1);
    if (written != 0) {
        (void)send(remote, std::span<const std::byte>(packet.data(), written), local, interface_index);
    }
}

namespace {

// Computes the 32-bit keyed digest of the 16 random bytes `cid[4..19]`. The
// high 16 bits mask the owner worker index in `cid[0..1]`; the full digest
// and the unmasked worker index feed `connection_id_tag`. Executed identically
// by the Linux SO_ATTACH_REUSEPORT_CBPF program.
[[nodiscard]] std::uint32_t connection_id_digest(const http3_secret& secret, const std::uint8_t* cid) noexcept {
    const auto k0 = read_http3_be32(secret.data());
    const auto k1 = read_http3_be32(secret.data() + 4);
    const auto k2 = read_http3_be32(secret.data() + 8);
    const auto k3 = read_http3_be32(secret.data() + 12);
    const auto w0 = read_http3_be32(cid + 4);
    const auto w1 = read_http3_be32(cid + 8);
    const auto w2 = read_http3_be32(cid + 12);
    const auto w3 = read_http3_be32(cid + 16);
    std::uint32_t acc = (w0 ^ k0) * http3_cid_mix_prime_1;
    acc = (acc ^ w1 ^ k1) * http3_cid_mix_prime_2;
    acc = (acc ^ w2 ^ k2) * http3_cid_mix_prime_1;
    acc = (acc ^ w3 ^ k3) * http3_cid_mix_prime_2;
    return acc ^ (acc >> 16);
}

[[nodiscard]] std::uint16_t connection_id_tag(const http3_secret& secret, std::uint32_t digest, std::uint16_t worker_index) noexcept {
    const auto k4 = read_http3_be32(secret.data() + 16);
    const std::uint32_t mixed = (digest ^ k4 ^ static_cast<std::uint32_t>(worker_index)) * http3_cid_mix_prime_1;
    return static_cast<std::uint16_t>(mixed ^ (mixed >> 16));
}

}  // namespace

std::size_t http3_cid_key_hash::operator()(const http3_cid_key& key) const noexcept {
    return static_cast<std::size_t>(fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(key.bytes_.data()), key.size_)));
}

bool make_http3_connection_id(const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept {
    if (!std::in_range<std::uint16_t>(worker_index)) {
        return false;
    }
    std::array<std::uint8_t, http3_connection_id_length> bytes{};
    if (!fill_random_bytes(std::span<unsigned char>(bytes.data() + 4, bytes.size() - 4))) {
        return false;
    }
    const auto worker = static_cast<std::uint16_t>(worker_index);
    const auto digest = connection_id_digest(secret, bytes.data());
    const auto masked_worker = static_cast<std::uint16_t>(worker ^ static_cast<std::uint16_t>(digest >> 16));
    const auto tag = connection_id_tag(secret, digest, worker);
    bytes[0] = static_cast<std::uint8_t>(masked_worker >> 8);
    bytes[1] = static_cast<std::uint8_t>(masked_worker);
    bytes[2] = static_cast<std::uint8_t>(tag >> 8);
    bytes[3] = static_cast<std::uint8_t>(tag);
    ngtcp2_cid_init(&cid, bytes.data(), bytes.size());
    return true;
}

std::optional<std::size_t> http3_connection_id_owner(const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    if (cid.size() != http3_connection_id_length) {
        return std::nullopt;
    }
    const auto digest = connection_id_digest(secret, cid.data());
    const auto worker = static_cast<std::uint16_t>(read_http3_be16(cid.data()) ^ static_cast<std::uint16_t>(digest >> 16));
    if (static_cast<std::size_t>(worker) >= worker_count) {
        return std::nullopt;
    }
    if (read_http3_be16(cid.data() + 2) != connection_id_tag(secret, digest, worker)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(worker);
}

std::size_t http3_initial_worker(const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    // Match the optional Linux cBPF selector, so software dispatch keeps the
    // same owner when SO_REUSEPORT members change or no selector is available.
    if (cid.size() == http3_connection_id_length) {
        return connection_id_digest(secret, cid.data()) % worker_count;
    }
    if (cid.size() >= NGTCP2_MIN_INITIAL_DCIDLEN) {
        const auto first = read_http3_be32(cid.data()) ^ read_http3_be32(secret.data());
        const auto second = read_http3_be32(cid.data() + 4) ^ read_http3_be32(secret.data() + 4);
        const auto mixed = ((first * http3_cid_mix_prime_1) ^ second) * http3_cid_mix_prime_2;
        return (mixed >> 16) % worker_count;
    }
    auto hash = fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()));
    hash = fnv1a_accumulate(hash, std::string_view(reinterpret_cast<const char*>(cid.data()), cid.size()));
    return static_cast<std::size_t>(hash % worker_count);
}

bool make_http3_stateless_reset_token(const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept {
    return ngtcp2_crypto_generate_stateless_reset_token(token.data(), secret.data(), secret.size(), &cid) == 0;
}

#if !defined(_WIN32)
namespace {

class unique_udp_fd final {
public:
    unique_udp_fd() noexcept = default;
    explicit unique_udp_fd(int fd) noexcept :
        fd_(fd) {}
    unique_udp_fd(unique_udp_fd&& other) noexcept :
        fd_(std::exchange(other.fd_, -1)) {}
    unique_udp_fd& operator=(unique_udp_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    unique_udp_fd(const unique_udp_fd&) = delete;
    unique_udp_fd& operator=(const unique_udp_fd&) = delete;
    ~unique_udp_fd() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }
    void reset() noexcept {
        if (fd_ >= 0) {
            (void)::close(std::exchange(fd_, -1));
        }
    }

private:
    int fd_{-1};
};

unique_udp_fd open_reuseport_udp_fd(const asio::ip::udp& protocol, const sockaddr* address, socklen_t address_length, [[maybe_unused]] std::optional<bool> ipv6_only) {
#if !defined(SO_REUSEPORT)
    (void)protocol;
    (void)address;
    (void)address_length;
    throw std::system_error(asio::error::operation_not_supported, "HTTP/3 worker listeners require SO_REUSEPORT");
#else
    int type = SOCK_DGRAM;
#if defined(SOCK_CLOEXEC)
    type |= SOCK_CLOEXEC;
#endif
    unique_udp_fd fd(::socket(protocol.family(), type, protocol.protocol()));
    if (!fd) {
        throw std::system_error(errno, asio::system_category(), "failed to open HTTP/3 UDP listener socket");
    }
#if !defined(SOCK_CLOEXEC) && defined(FD_CLOEXEC)
    const int flags = ::fcntl(fd.get(), F_GETFD);
    if (flags == -1 || ::fcntl(fd.get(), F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, asio::system_category(), "failed to configure HTTP/3 UDP listener FD_CLOEXEC");
    }
#endif
    const int enabled = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEADDR");
    }
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEPORT");
    }
    if (ipv6_only) {
        const int value = *ipv6_only ? 1 : 0;
        if (::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &value, sizeof(value)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to configure HTTP/3 IPV6_V6ONLY");
        }
    }
    if (::bind(fd.get(), address, address_length) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to bind HTTP/3 UDP listener socket");
    }
    return fd;
#endif
}

#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
// Attaches an unprivileged cBPF socket selector that verifies server-issued
// 20-byte QUIC connection IDs (both Short Header 1-RTT packets, including
// post-migration / NAT-rebound paths, and Long Header packets carrying a
// 20-byte server DCID) using the listener secret inlined as immediate words,
// returning the unmasked owner worker index directly in the kernel. Client
// Initial packets and forged IDs fall back to a keyed hash of the packet's
// connection ID bytes so retransmissions stick to one worker while unkeyed
// floods spread across all workers.
void attach_reuseport_cbpf(int fd, const http3_secret& secret, std::size_t worker_count) {
    if (worker_count <= 1 || !std::in_range<std::uint16_t>(worker_count)) {
        return;
    }
    const auto count = static_cast<std::uint32_t>(worker_count);
    const auto k0 = read_http3_be32(secret.data());
    const auto k1 = read_http3_be32(secret.data() + 4);
    const auto k2 = read_http3_be32(secret.data() + 8);
    const auto k3 = read_http3_be32(secret.data() + 12);
    const auto k4 = read_http3_be32(secret.data() + 16);
    constexpr auto p1 = http3_cid_mix_prime_1;
    constexpr auto p2 = http3_cid_mix_prime_2;
    constexpr auto random_offset = static_cast<std::uint32_t>(SKF_AD_OFF + SKF_AD_RANDOM);

    std::array<sock_filter, 79> filter{{
        // 0..10: locate 20-byte server DCID at offset X = 1 (Short Header, len >= 21)
        // or X = 6 (Long Header, len >= 26 and DCID length == 20).
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 21, 0, 60),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x80, 2, 0),
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 1),
        BPF_STMT(BPF_JMP | BPF_JA, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 26, 0, 54),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 5),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, http3_connection_id_length, 0, 52),
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 6),

        // 11..21: load masked worker, tag, and random words w0..w3 from DCID[0..19].
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 0),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 2),
        BPF_STMT(BPF_ST, 1),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 8),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 12),
        BPF_STMT(BPF_ST, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 16),
        BPF_STMT(BPF_ST, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 4),

        // 22..40: compute connection_id_digest(secret, dcid) into M[2].
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k1),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_LDX | BPF_MEM, 3),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k2),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 4),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k3),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ST, 2),

        // 41..45: unmask worker_index = (digest >> 16) ^ masked_worker and bounds-check.
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, count, 14, 0),
        BPF_STMT(BPF_ST, 0),

        // 46..58: verify connection_id_tag(secret, digest, worker_index).
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k4),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_ST, 3),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 3),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xffffu),
        BPF_STMT(BPF_LDX | BPF_MEM, 1),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_X, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_MEM, 0),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 59..61: unauthenticated 20-byte CID: route by keyed digest % worker_count.
        BPF_STMT(BPF_LD | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 62..75: client Initial with 8..19-byte DCID: keyed hash of DCID[0..7].
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 14, 0, 12),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 10),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k1),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 76..78: sub-minimum datagram fallback.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, random_offset),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),
    }};
    const sock_fprog program{
        .len = static_cast<unsigned short>(filter.size()),
        .filter = filter.data(),
    };
    if (::setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &program, sizeof(program)) != 0) {
        if (errno != ENOPROTOOPT && errno != EOPNOTSUPP && errno != EPERM && errno != EACCES) {
            throw std::system_error(errno, asio::system_category(), "failed to attach HTTP/3 SO_ATTACH_REUSEPORT_CBPF");
        }
    }
}
#endif

}  // namespace
#endif

http3_worker_directory::http3_worker_directory(std::size_t worker_count) :
    slots_(),
    listener_sockets_() {
    slots_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        slots_.push_back(std::make_unique<slot>());
    }
}

http3_worker_directory::~http3_worker_directory() {
#if !defined(_WIN32)
    for (auto& group : listener_sockets_) {
        for (int& fd : group) {
            if (fd >= 0) {
                (void)::close(std::exchange(fd, -1));
            }
        }
    }
#endif
}

void http3_worker_directory::bind_worker(std::size_t index, const worker_ref& worker_value) {
    if (index >= slots_.size() || !worker_value.valid()) {
        throw std::invalid_argument("invalid HTTP/3 worker directory slot");
    }
    slots_[index]->worker_ = worker_value;
}

void http3_worker_directory::attach(std::size_t index, void* target, receive_type receive) noexcept {
    auto& value = *slots_[index];
    value.target_ = target;
    value.receive_ = receive;
}

void http3_worker_directory::detach(std::size_t index) noexcept {
    auto& value = *slots_[index];
    value.target_ = nullptr;
    value.receive_ = nullptr;
}

void http3_worker_directory::register_endpoint(std::size_t worker_index, http3_endpoint& endpoint, const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret, std::size_t listener_index) {
    slots_[worker_index]->endpoints_.push_back({&endpoint, local, std::move(secret), listener_index});
}

void http3_worker_directory::unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept {
    for (auto& registration : slots_[worker_index]->endpoints_) {
        if (registration.endpoint_ == &endpoint) {
            registration.endpoint_ = nullptr;
            return;
        }
    }
}

#if !defined(_WIN32)
void http3_worker_directory::open_listener_socket(std::size_t worker_index, std::size_t listener_index, const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret, inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only) {
    if (worker_index >= slots_.size()) {
        throw std::invalid_argument("invalid HTTP/3 worker index");
    }
    unique_udp_fd assigned;
    {
        std::lock_guard<std::mutex> lock(bind_mutex_);
        while (listener_sockets_.size() <= listener_index) {
            listener_sockets_.emplace_back();
        }
        auto& group = listener_sockets_[listener_index];
        if (group.empty()) {
            std::vector<unique_udp_fd> opened;
            opened.reserve(slots_.size());
            if (inherited != nullptr && !inherited->udp().empty()) {
                // Validated as bound to this endpoint, one per worker in
                // worker order: the group's members as the kernel numbers them.
                for (const auto fd : inherited->claim_udp()) {
                    opened.emplace_back(fd);
                }
            } else {
                asio::ip::udp::endpoint bound_endpoint = configured_endpoint;
                for (std::size_t index = 0; index < slots_.size(); ++index) {
                    auto fd = open_reuseport_udp_fd(configured_endpoint.protocol(), bound_endpoint.data(), static_cast<socklen_t>(bound_endpoint.size()), ipv6_only);
                    if (index == 0) {
                        auto length = static_cast<socklen_t>(bound_endpoint.capacity());
                        if (::getsockname(fd.get(), bound_endpoint.data(), &length) != 0) {
                            throw std::system_error(errno, asio::system_category(), "failed to query HTTP/3 UDP listener endpoint");
                        }
                        bound_endpoint.resize(length);
                    }
                    opened.push_back(std::move(fd));
                }
            }
            if (opened.size() != slots_.size()) {
                throw std::invalid_argument("HTTP/3 listener sockets must number one per worker");
            }
#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
            attach_reuseport_cbpf(opened.front().get(), secret, slots_.size());
#else
            (void)secret;
#endif
            group.resize(slots_.size(), -1);
            for (std::size_t index = 0; index < opened.size(); ++index) {
                group[index] = opened[index].release();
            }
        }
        if (group[worker_index] < 0) {
            throw std::logic_error("HTTP/3 listener socket already claimed by worker");
        }
        assigned = unique_udp_fd(std::exchange(group[worker_index], -1));
    }
    socket.assign(configured_endpoint.protocol(), assigned.get());
    (void)assigned.release();
    socket.non_blocking(true);
}

void http3_worker_directory::close_unclaimed_listener_sockets(std::size_t listener_index) noexcept {
    const std::lock_guard<std::mutex> lock(bind_mutex_);
    if (listener_index < listener_sockets_.size()) {
        for (int& fd : listener_sockets_[listener_index]) {
            if (fd >= 0) {
                (void)::close(std::exchange(fd, -1));
            }
        }
    }
}
#endif

bool http3_worker_directory::hand_off(std::size_t owner, std::size_t listener_index, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index, std::uint32_t ecn) noexcept {
    if (owner >= slots_.size()) {
        return false;
    }
    auto& target = *slots_[owner];
    auto token = http3_in_flight_token::reserve(target.in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
            std::vector<std::byte> copied(bytes.begin(), bytes.end());
            http3_handoff_datagram datagram{
                .bytes_ = std::move(copied),
                .remote_ = remote,
                .ecn_ = ecn,
                .local_ = local,
                .interface_index_ = interface_index,
            };
            return [&target, listener_index, owned = std::move(datagram), token = std::move(token)]() mutable noexcept {
                // The target pointer is confined to this, the owner's, thread.
                if (target.receive_ == nullptr) {
                    return;
                }
                for (const auto& registration : target.endpoints_) {
                    if (registration.listener_index_ != listener_index) {
                        continue;
                    }
                    if (registration.endpoint_ != nullptr) {
                        target.receive_(target.target_, listener_index, owned);
                        return;
                    }
                    // This worker may already have released the old endpoint
                    // while another worker still owns one of its connections.
                    for (const auto& fallback : target.endpoints_) {
                        if (fallback.endpoint_ != nullptr && fallback.local_ == registration.local_) {
                            target.receive_(target.target_, fallback.listener_index_, owned);
                            return;
                        }
                    }
                    return;
                }
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

}  // namespace ruvia::detail
