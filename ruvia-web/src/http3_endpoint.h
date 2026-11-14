#pragma once

#if defined(RUVIA_ENABLE_HTTP3)

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "connection_lifecycle.h"
#include "connection_scanner.h"
#include "http3_quic_connection.h"
#include "http3_quic_driver.h"
#include "http_server.h"
#include "server_listener.h"
#include "server_options.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"
#include "worker_signal.h"

// Server-chosen QUIC connection IDs (RFC 9000 §5.1) that route themselves
// without exposing a linkable worker prefix on the wire (RFC 9000 §9.5).
// Every worker of a POSIX HTTP/3 listener binds the UDP port with
// SO_REUSEPORT (on Linux steered in the kernel by SO_ATTACH_REUSEPORT_CBPF),
// and on Windows the acceptor receives every datagram. Each connection ID
// this server issues names the worker that owns the connection, masked and
// authenticated by the listener's shared secret and the ID's random suffix so
// that sibling IDs of one connection share no fixed bytes and a client-chosen
// ID only passes the tag check by chance (one in 2^16):
//
//   bytes 0..1   (owner worker index) ^ mask, big endian
//   bytes 2..3   tag: keyed 16-bit authentication tag over worker index and random bytes
//   bytes 4..19  random (16 CSPRNG bytes)
//
// A datagram whose destination ID names another worker is steered to it (by
// the kernel cBPF filter on Linux, or handed through the worker directory).
// Any other unknown ID (a client's first Initial) is hashed by its connection
// ID and the listener secret.

namespace ruvia::detail {

inline constexpr std::size_t http3_connection_id_length = 20;
using http3_secret = std::array<std::uint8_t, 32>;

// 32-bit mixing primes shared with the Linux SO_ATTACH_REUSEPORT_CBPF program.
inline constexpr std::uint32_t http3_cid_mix_prime_1 = 0x85ebca6bu;
inline constexpr std::uint32_t http3_cid_mix_prime_2 = 0xc2b2ae35u;

[[nodiscard]] constexpr std::uint16_t read_http3_be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) | static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] constexpr std::uint32_t read_http3_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) | (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

// A connection ID as a map key: up to NGTCP2_MAX_CIDLEN bytes.
struct http3_cid_key final {
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> bytes_{};
    std::uint8_t size_{0};

    [[nodiscard]] static http3_cid_key from(std::span<const std::uint8_t> value) noexcept {
        http3_cid_key key;
        key.size_ = static_cast<std::uint8_t>(value.size() < key.bytes_.size() ? value.size() : key.bytes_.size());
        std::memcpy(key.bytes_.data(), value.data(), key.size_);
        return key;
    }

    [[nodiscard]] static http3_cid_key from(const ngtcp2_cid& cid) noexcept {
        return from(std::span<const std::uint8_t>(cid.data, cid.datalen));
    }

    friend bool operator==(const http3_cid_key& left, const http3_cid_key& right) noexcept {
        return left.size_ == right.size_ && std::memcmp(left.bytes_.data(), right.bytes_.data(), left.size_) == 0;
    }
};

struct http3_cid_key_hash final {
    [[nodiscard]] std::size_t operator()(const http3_cid_key& key) const noexcept;
};

// Fills `cid` with a fresh ID owned by `worker_index`. False when the CSPRNG
// failed.
[[nodiscard]] bool make_http3_connection_id(const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept;

// The worker a destination connection ID names, or nullopt when it is not an
// ID of this listener (wrong length, bad tag) or names no running worker.
[[nodiscard]] std::optional<std::size_t> http3_connection_id_owner(const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;
// The worker that opens the connection whose client Initial carries the
// client-chosen destination ID `cid`: a hash of it keyed by the listener
// secret. Retransmissions of that Initial carry the same ID, so they reach
// the same worker without per-connection acceptor state, and a client that
// does not know the secret cannot choose its worker.
[[nodiscard]] std::size_t http3_initial_worker(const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;

// The stateless reset token of `cid` (RFC 9000 §10.3.2), derived from the
// listener secret so every worker computes the same token.
[[nodiscard]] bool make_http3_stateless_reset_token(const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept;

class http3_endpoint;

#if defined(_WIN32)
class windows_acceptor;
#endif

// One datagram handed to a worker that did not receive it. It owns its bytes
// because it crosses threads.
struct http3_handoff_datagram final {
    std::vector<std::byte> bytes_;
    asio::ip::udp::endpoint remote_;
    std::uint32_t ecn_{0};
    asio::ip::udp::endpoint local_;
    std::uint32_t interface_index_{};
};

// Slot identities outlive their worker-local endpoint. Another worker can
// still own a connection issued under that slot's secret after this one has
// released its last connection and socket.
struct http3_endpoint_registration final {
    http3_endpoint* endpoint_;
    asio::ip::udp::endpoint local_;
    std::shared_ptr<const http3_secret> secret_;
    std::size_t listener_index_;
};

// Datagrams in flight from one producer to one worker, product limit. A
// datagram past it is dropped, as UDP may drop it anyway; QUIC loss recovery
// retransmits what matters.
inline constexpr std::size_t http3_max_datagrams_in_flight = 256;

// One handed-off datagram's place in its in-flight bound: reserved before the
// datagram is posted, released when the posted job ran or was abandoned, on
// whichever thread that is.
class http3_in_flight_token final {
public:
    // Empty when the bound is full.
    [[nodiscard]] static http3_in_flight_token reserve(std::atomic<std::size_t>& count) noexcept {
        if (count.fetch_add(1, std::memory_order_relaxed) >= http3_max_datagrams_in_flight) {
            count.fetch_sub(1, std::memory_order_relaxed);
            return http3_in_flight_token(nullptr);
        }
        return http3_in_flight_token(&count);
    }
    http3_in_flight_token(http3_in_flight_token&& other) noexcept :
        count_(std::exchange(other.count_, nullptr)) {}
    http3_in_flight_token(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(http3_in_flight_token&&) = delete;
    ~http3_in_flight_token() {
        if (count_ != nullptr) {
            count_->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return count_ != nullptr;
    }

private:
    explicit http3_in_flight_token(std::atomic<std::size_t>* count) noexcept :
        count_(count) {}

    std::atomic<std::size_t>* count_;
};

// The HTTP/3 rendezvous of every worker of one application run. A datagram
// for a connection a worker owns (see http3_connection_id_owner()) is handed to
// that worker here when another thread received it: on POSIX another worker
// whose SO_REUSEPORT socket the kernel chose, on Windows the acceptor that
// receives every datagram. It travels on the owner's bounded worker queue,
// at most http3_max_datagrams_in_flight per owner at a time, and is dropped
// when either bound is full or the owner no longer serves.
class http3_worker_directory final {
public:
    using receive_type = void (*)(void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;

    explicit http3_worker_directory(std::size_t worker_count);
    http3_worker_directory(const http3_worker_directory&) = delete;
    ~http3_worker_directory();
    http3_worker_directory& operator=(const http3_worker_directory&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return slots_.size();
    }

    // Construction thread, before any worker serves -- for a directory
    // application::add_listener() creates, before any worker uses it.
    void bind_worker(std::size_t index, const worker_ref& worker_value);
    // The owning worker's thread: opens and closes its receiving side.
    void attach(std::size_t index, void* target, receive_type receive) noexcept;
    void detach(std::size_t index) noexcept;

    // Worker-local endpoint registrations share packet routing across removed
    // and re-added listeners bound to the same UDP endpoint.
    void register_endpoint(std::size_t worker_index, http3_endpoint& endpoint, const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret, std::size_t listener_index);
    void unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept;
    [[nodiscard]] std::span<const http3_endpoint_registration> endpoints(std::size_t worker_index) const noexcept {
        return slots_[worker_index]->endpoints_;
    }

#if !defined(_WIN32)
    // The first call for `listener_index` binds every worker's SO_REUSEPORT
    // socket in worker index order 0..N-1 -- or takes `inherited`'s UDP
    // sockets, in that order already, when it holds them -- and attaches
    // SO_ATTACH_REUSEPORT_CBPF on Linux; each call assigns `worker_index`'s
    // socket to `socket`.
    void open_listener_socket(std::size_t worker_index, std::size_t listener_index, const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret, inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only);
    // Closes the sockets of `listener_index` no worker claimed, for a listener
    // whose addition was rolled back.
    void close_unclaimed_listener_sockets(std::size_t listener_index) noexcept;
#endif
    // Any thread. False when the datagram was dropped.
    [[nodiscard]] bool hand_off(std::size_t owner, std::size_t listener_index, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index, std::uint32_t ecn = 0) noexcept;

private:
    struct slot final {
        worker_ref worker_;
        // Read and written only on the owning worker's thread.
        void* target_{nullptr};
        receive_type receive_{nullptr};
        std::atomic<std::size_t> in_flight_{0};
        std::vector<http3_endpoint_registration> endpoints_{};
    };

    std::vector<std::unique_ptr<slot>> slots_;
    std::mutex bind_mutex_;
    std::vector<std::vector<int>> listener_sockets_;
};

// Where one worker's HTTP/3 endpoints sit among the application's workers:
// the directory (null without an HTTP/3 listener) and this worker's index in
// it, which the connection IDs it issues carry.
struct http3_worker_placement final {
    http3_worker_directory* directory_;
    std::size_t worker_index_;
#if defined(_WIN32)
    // Sends this worker's datagrams from the listener's one UDP socket.
    windows_acceptor* acceptor_;
#endif
};

class http3_session;
class route_table;
class worker_capabilities;

// The worker-owned services every HTTP/3 connection of a worker borrows.
// Each outlives the worker's connections, which their listeners' task scopes
// join.
struct http3_worker_services final {
    asio::io_context& io_;
    const worker_ref& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    connection_scanner& scanner_;
    connection_count& connection_count_;
    connection_id_sequence& connection_ids_;
    connection_registry& registry_;
    const stop_token& stop_token_;
    const http_server_worker_state& state_;
    std::atomic<std::size_t>& refused_;
    // Where this worker sits among the application's workers. Its directory
    // (and on Windows its acceptor) is set whenever an endpoint exists.
    http3_worker_placement placement_;
};

// One worker's QUIC endpoint of one HTTP/3 listener: the table from
// connection ID to this worker's connections, connection admission, packet
// steering and the datagram path.
//
// On POSIX the endpoint owns a UDP socket bound with SO_REUSEPORT. On Windows
// the application's acceptor owns the listener's only socket: every datagram
// arrives here handed over through the directory, and every datagram this
// worker sends leaves through the acceptor's bounded queue, so every worker
// serves HTTP/3 alike.
//
// A datagram is delivered to the connection its destination ID names in the
// table. Otherwise an ID that names another worker (http3_connection_id_owner())
// is handed to it through the directory; an unknown ID with a valid client
// Initial opens a connection on this worker, bounded like TCP by
// max_connections_per_worker_ (the connection idle longest is evicted
// first); an unsupported version gets Version Negotiation (RFC 9000 §6); a
// short header naming a retired ID of this worker gets a rate-limited
// Stateless Reset (RFC 9000 §10.3); any other datagram is dropped.
class http3_endpoint final {
public:
    // `listener_index` names this listener's endpoint on every worker (and in
    // the Windows acceptor), for datagrams handed between them. Its receive
    // loop and connections run in `tasks`, its listener's scope.
    http3_endpoint(const http3_worker_services& services, http_server_session_config& session, asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections);
    ~http3_endpoint();
    http3_endpoint(const http3_endpoint&) = delete;
    http3_endpoint& operator=(const http3_endpoint&) = delete;

    // POSIX binds the UDP socket (SO_REUSEADDR and SO_REUSEPORT), or claims
    // it from `inherited` when that holds the service's sockets; Windows,
    // whose acceptor binds it, only prepares the QUIC backend.
    void open(inherited_listener_sockets* inherited);
    // Starts admitting connections, and on POSIX receiving.
    void start() noexcept;
    // The listener was removed: no connection is admitted any more, while
    // established ones are served. After the last one ends the POSIX socket
    // closes; on Windows the acceptor learns that this worker no longer
    // needs the listener's socket.
    void retire() noexcept;
    // The worker stops: no connection is admitted any more, and the POSIX
    // socket closes. Its connections were already closed by the scanner.
    void close() noexcept;
#if !defined(_WIN32)
    // A dup() of the open socket, for application::duplicate_listener_sockets().
    [[nodiscard]] native_socket duplicate_socket();
#endif

    // A datagram another thread received for this worker.
    void receive_handoff(http3_handoff_datagram& datagram) noexcept;

    // Session interface.
    [[nodiscard]] const http3_worker_services& services() const noexcept {
        return services_;
    }
    [[nodiscard]] http_server_session_config& session_config() const noexcept {
        return session_;
    }
    [[nodiscard]] const http3_secret& secret() const noexcept {
        return *session_.http3_->secret_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& local_endpoint() const noexcept {
        return local_endpoint_;
    }
    // Sends one datagram without blocking. On POSIX would_block means the
    // socket's send buffer is full (then wait with wait_writable()); on
    // Windows the datagram is queued to the acceptor, or lost when that
    // bounded queue is full (no_buffer_space).
    [[nodiscard]] std::error_code send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
#if !defined(_WIN32)
    // Completes once the socket can send again, or with the error that ended
    // waiting (the socket closed).
    [[nodiscard]] task<std::error_code> wait_writable();
#endif
    // False when another connection already holds the ID.
    [[nodiscard]] bool register_connection_id(const http3_cid_key& key, http3_session& session);
    void unregister_connection_id(const http3_cid_key& key) noexcept;

private:
    using session_pointer = std::unique_ptr<http3_session>;

#if !defined(_WIN32)
    task<void> run_receive_loop();
    worker_signal activated_;
#endif
    task<void> serve(session_pointer session);
    void receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_retry(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::size_t max_packet_bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_connection_close(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote, std::uint64_t error_code, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_reset(std::span<const std::uint8_t> destination_cid, std::size_t trigger_bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] bool consume_stateless_reset_budget() noexcept;
    // A retiring endpoint whose last connection ended.
    void release_after_last_connection() noexcept;

    const http3_worker_services& services_;
    http_server_session_config& session_;
    asio::ip::udp::endpoint configured_endpoint_;
    asio::ip::udp::endpoint local_endpoint_;
    std::size_t listener_index_;
    task_scope& tasks_;
    connection_count& listener_connections_;
#if !defined(_WIN32)
    asio::ip::udp::socket socket_;
    // One received datagram at a time; a UDP payload never exceeds it.
    std::vector<std::byte> receive_buffer_;
#endif
    std::unordered_map<http3_cid_key, http3_session*, http3_cid_key_hash> connections_;
    std::chrono::steady_clock::time_point stateless_reset_refill_at_{};
    std::size_t stateless_reset_tokens_{32};
    // Sessions serve() is running.
    std::size_t sessions_{0};
    // Between open() and close(): datagrams are processed.
    bool open_{false};
    bool admitting_{false};
    bool registered_{false};
    bool retiring_{false};
};

}  // namespace ruvia::detail

#endif
