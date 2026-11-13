#pragma once

#include <ngtcp2/ngtcp2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>

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

#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/error.h"
#include "ruvia/web/web_worker.h"

#include "http/context_access.h"
#include "http/operation_interrupt.h"
#include "server/connection_lifecycle.h"
#include "server/multiplexed_stream_signal.h"
#include "server/route_table.h"

namespace ruvia::detail {

inline constexpr std::size_t http3_connection_id_length = 20;
using http3_secret = std::array<std::uint8_t, 32>;

// 32-bit mixing primes shared with the Linux SO_ATTACH_REUSEPORT_CBPF program.
inline constexpr std::uint32_t http3_cid_mix_prime_1 = 0x85ebca6bu;
inline constexpr std::uint32_t http3_cid_mix_prime_2 = 0xc2b2ae35u;

[[nodiscard]] constexpr std::uint16_t read_http3_be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] constexpr std::uint32_t read_http3_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
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
[[nodiscard]] bool make_http3_connection_id(
    const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept;

// The worker a destination connection ID names, or nullopt when it is not an
// ID of this listener (wrong length, bad tag) or names no running worker.
[[nodiscard]] std::optional<std::size_t> http3_connection_id_owner(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;
// The worker that opens the connection whose client Initial carries the
// client-chosen destination ID `cid`: a hash of it keyed by the listener
// secret. Retransmissions of that Initial carry the same ID, so they reach
// the same worker without per-connection acceptor state, and a client that
// does not know the secret cannot choose its worker.
[[nodiscard]] std::size_t http3_initial_worker(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;

// The stateless reset token of `cid` (RFC 9000 §10.3.2), derived from the
// listener secret so every worker computes the same token.
[[nodiscard]] bool make_http3_stateless_reset_token(
    const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

// The largest UDP payload (65535 minus the UDP header): a receive buffer of
// this size truncates no datagram.
inline constexpr std::size_t http3_max_udp_payload_bytes = 65527;

// A receive error that concerns a single datagram, not the socket: ICMP
// errors of earlier sends (Windows reports them on later receives) and
// truncation. The socket keeps receiving.
[[nodiscard]] inline bool http3_ignorable_receive_error(const std::error_code& error) noexcept {
    return error == asio::error::connection_refused || error == asio::error::connection_reset ||
           error == asio::error::message_size || error == asio::error::network_unreachable ||
           error == asio::error::host_unreachable;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

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
    http3_in_flight_token(http3_in_flight_token&& other) noexcept
        : count_(std::exchange(other.count_, nullptr)) {}
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
    explicit http3_in_flight_token(std::atomic<std::size_t>* count) noexcept
        : count_(count) {}

    std::atomic<std::size_t>* count_;
};

// The HTTP/3 rendezvous of every worker of one application run. A datagram
// for a connection a worker owns (see http3_connection_id.h) is handed to
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
    void register_endpoint(std::size_t worker_index, http3_endpoint& endpoint,
        const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret,
        std::size_t listener_index);
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
    void open_listener_socket(std::size_t worker_index, std::size_t listener_index,
        const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret,
        inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only);
    // Closes the sockets of `listener_index` no worker claimed, for a listener
    // whose addition was rolled back.
    void close_unclaimed_listener_sockets(std::size_t listener_index) noexcept;
#endif
    // Any thread. False when the datagram was dropped.
    [[nodiscard]] bool hand_off(std::size_t owner, std::size_t listener_index,
        std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index,
        std::uint32_t ecn = 0) noexcept;

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

}  // namespace ruvia::detail

namespace ruvia::detail {

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
// table. Otherwise an ID that names another worker (http3_connection_id.h)
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
    http3_endpoint(const http3_worker_services& services, http_server_session_config& session,
        asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections);
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
    [[nodiscard]] std::error_code send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
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
    void receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_retry(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote,
        std::size_t max_packet_bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_connection_close(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote,
        std::uint64_t error_code, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_reset(std::span<const std::uint8_t> destination_cid, std::size_t trigger_bytes,
        const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
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

namespace ruvia::detail {

// Resumes the coroutine on a later turn of its worker, letting other
// connections run between bursts of one connection's output.
struct worker_turn final {
    explicit worker_turn(const worker_ref& worker_value) noexcept
        : worker_(worker_value) {}
    worker_turn(worker_ref&&) = delete;
    const worker_turn* operator&() const = delete;
    worker_turn* operator&() = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}

    const worker_ref& worker_;
};

struct free_tls final {
    void operator()(SSL* tls) const noexcept {
        SSL_free(tls);
    }
};

struct free_crypto final {
    void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
        ngtcp2_crypto_ossl_ctx_del(crypto);
    }
};

inline void fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) noexcept {
    // ngtcp2 cannot fail this callback; packet protection without randomness
    // must not continue.
    if (!fill_random_bytes(std::span<unsigned char>(destination, size))) {
        std::terminate();
    }
}

// The ngtcp2_crypto_ossl adapter callbacks both roles share; the client adds
// client_initial_ and recv_retry_, the server recv_client_initial_.
[[nodiscard]] inline http3_quic_crypto_callbacks make_http3_quic_crypto_callbacks() noexcept {
    return {
        .recv_crypto_data_ = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt_ = ngtcp2_crypto_encrypt_cb,
        .decrypt_ = ngtcp2_crypto_decrypt_cb,
        .hp_mask_ = ngtcp2_crypto_hp_mask_cb,
        .update_key_ = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx_ = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx_ = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data_ = ngtcp2_crypto_get_path_challenge_data2_cb,
        .version_negotiation_ = ngtcp2_crypto_version_negotiation_cb,
        .rand_ = fill_random,
    };
}

[[nodiscard]] inline ngtcp2_tstamp http3_quic_now() noexcept {
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(timeout_duration value) noexcept {
    const auto milliseconds = static_cast<std::uint64_t>((std::max)(value.value().count(), std::int64_t{0}));
    return milliseconds > UINT64_MAX / NGTCP2_MILLISECONDS ? UINT64_MAX : milliseconds * NGTCP2_MILLISECONDS;
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(const std::optional<timeout_duration>& value) noexcept {
    return value.has_value() ? http3_quic_duration(*value) : 0;
}

[[nodiscard]] inline std::chrono::steady_clock::time_point http3_quic_steady_time(ngtcp2_tstamp timestamp) noexcept {
    using nanoseconds = std::chrono::nanoseconds;
    if (timestamp >= static_cast<ngtcp2_tstamp>((std::numeric_limits<nanoseconds::rep>::max)())) {
        return (std::chrono::steady_clock::time_point::max)();
    }
    return std::chrono::steady_clock::time_point(
        std::chrono::ceil<std::chrono::steady_clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(timestamp))));
}

[[nodiscard]] inline std::chrono::nanoseconds http3_quic_nanoseconds(ngtcp2_duration duration) noexcept {
    return std::chrono::nanoseconds(
        static_cast<std::int64_t>((std::min)(duration, static_cast<ngtcp2_duration>(INT64_MAX))));
}

[[nodiscard]] inline ngtcp2_path http3_quic_path(
    asio::ip::udp::endpoint& local, asio::ip::udp::endpoint& remote) noexcept {
    return {
        .local = {local.data(), static_cast<ngtcp2_socklen>(local.size())},
        .remote = {remote.data(), static_cast<ngtcp2_socklen>(remote.size())},
        .user_data = nullptr,
    };
}

[[nodiscard]] inline std::optional<asio::ip::udp::endpoint> http3_udp_endpoint(
    const ngtcp2_addr& address) noexcept {
    asio::ip::udp::endpoint endpoint;
    if (address.addr == nullptr || address.addrlen == 0 || address.addrlen > endpoint.capacity()) {
        return std::nullopt;
    }
    std::memcpy(endpoint.data(), address.addr, address.addrlen);
    endpoint.resize(address.addrlen);
    return endpoint;
}
}  // namespace ruvia::detail

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; this runtime owns what a route needs: the
// validated head, the request body as the route consumes it (buffered or
// streamed, with flow-control credit returned only as bytes are consumed), the
// trailers, and the wake/stop signal a dispatched handler waits on.

namespace ruvia::detail {

enum class http3_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

// Tunnel bytes taken together with the receive credit they hold; the reader
// drops the credit once it consumed the bytes.
struct http3_tunnel_chunk final {
    std::string bytes_;
    std::optional<http3_received_data_credit> credit_;
};

class http3_stream final {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode,
        inbound_buffer_account* inbound = nullptr) noexcept
        : head_(std::move(head)),
          resolution_(std::move(resolution)),
          body_mode_(body_mode),
          body_reservation_(inbound),
          active_reservation_(inbound) {}

    http3_stream(const http3_stream&) = delete;
    http3_stream& operator=(const http3_stream&) = delete;

    [[nodiscard]] std::int64_t id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;
    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_;
    }
    const std::optional<http_priority>& priority() const&& = delete;
    void set_priority(std::optional<http_priority> priority) noexcept {
        priority_ = priority;
    }
    void defer_dispatch() noexcept {
        dispatch_pending_ = true;
    }
    [[nodiscard]] bool dispatch_pending() const noexcept {
        return dispatch_pending_ && !dispatched();
    }
    void defer_continue() noexcept {
        continue_pending_ = true;
    }
    [[nodiscard]] bool take_continue_pending() noexcept {
        return std::exchange(continue_pending_, false);
    }

    void store_datagram(std::string_view payload, std::size_t byte_limit) {
        if ((datagrams_ && datagrams_->size() >= 64) || datagram_bytes_ > byte_limit ||
            payload.size() > byte_limit - datagram_bytes_) {
            return;  // RFC 9221 permits loss; never backpressure reliable DATA.
        }
        try {
            if (!datagrams_) {
                datagrams_.emplace();
            }
            datagrams_->emplace_back(payload);
            datagram_bytes_ += payload.size();
        } catch (const inbound_buffer_limit_error&) {
            return;
        }
        if (signal_) {
            signal_->wake();
        }
    }
    [[nodiscard]] std::optional<std::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        return payload;
    }
    [[nodiscard]] http3_tunnel_chunk take_tunnel_bytes() {
        http3_tunnel_chunk result{std::string{}, std::exchange(body_credit_, std::nullopt)};
        result.bytes_.swap(body_);
        body_reservation_.clear();
        return result;
    }

    // Buffered body: the whole content, borrowed by the request at dispatch;
    // its credit returns as it is stored. Streaming body and tunnel: the bytes
    // queued for the reader, holding their credit until it takes them.
    [[nodiscard]] http3_body_store_status store(std::string_view data, http3_received_data_credit&& credit,
        protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http3_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream &&
            (body_.size() > backlog_limit || data.size() > backlog_limit - body_.size())) {
            return http3_body_store_status::backlog_overflow;
        }
        try {
            inbound_append(body_, body_reservation_, data);
        } catch (const inbound_buffer_limit_error&) {
            return http3_body_store_status::buffer_limit;
        }
        received_bytes_ += data.size();
        if (body_mode_ == request_body_mode::stream && credit.valid()) {
            if (body_credit_) {
                body_credit_->merge(std::move(credit));
            } else {
                body_credit_.emplace(std::move(credit));
            }
        }
        return http3_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view buffered_body() const&& = delete;

    // Streaming reader: hands out every queued byte as one chunk, valid until
    // the next pop(). The previous chunk's flow-control credit returns now;
    // true when it held any, so the caller can let the writer send it.
    [[nodiscard]] std::pair<std::string_view, bool> pop() & RUVIA_LIFETIMEBOUND {
        const bool returned = active_credit_.has_value();
        active_credit_.reset();
        if (body_credit_) {
            active_credit_.emplace(std::move(*body_credit_));
            body_credit_.reset();
        }
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        active_chunk_.swap(body_);
        active_reservation_.swap(body_reservation_);
        return {active_chunk_, returned};
    }
    std::pair<std::string_view, bool> pop() && = delete;

    // The handler finished: the content's storage goes back to the
    // connection's inbound budget, and credit for content received but never
    // read returns so a stopped reader cannot strand connection-level credit.
    // True when any credit returned.
    bool discard_body() noexcept {
        const bool returned = body_credit_.has_value() || active_credit_.has_value();
        body_credit_.reset();
        active_credit_.reset();
        inbound_discard(body_, body_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        datagrams_.reset();
        datagram_bytes_ = 0;
        return returned;
    }

    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;

    // The request content ended (the peer's FIN, or no content at all).
    void mark_request_ended() noexcept {
        request_ended_ = true;
        request_body_deadline_.close();
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return request_ended_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }
    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
    }

    // The peer reset the stream, or this side did: no response byte may follow.
    void mark_aborted() noexcept {
        aborted_ = true;
        if (signal_) {
            signal_->stop();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

    // The QUIC stream closed in both directions.
    void mark_closed() noexcept {
        closed_ = true;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (signal_ || handler_done_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        if (aborted_) {
            signal_->stop();
        }
        return &*signal_;
    }
    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }
    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;
    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    void mark_handler_done() noexcept {
        handler_done_ = true;
    }
    [[nodiscard]] bool handler_done() const noexcept {
        return handler_done_;
    }

private:
    http3_request_head head_;
    route_resolution resolution_;
    request_body_mode body_mode_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_{};
    // Received request content: the whole body (buffered), or the queue the
    // reader drains (streaming). active_chunk_ backs the last popped view.
    std::string body_;
    inbound_buffer_reservation body_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    // Receive credit of body_ and active_chunk_ (streaming body or tunnel).
    std::optional<http3_received_data_credit> body_credit_;
    std::optional<http3_received_data_credit> active_credit_;
    std::optional<std::deque<std::string>> datagrams_;
    std::size_t datagram_bytes_{0};
    std::size_t received_bytes_{0};
    std::optional<http_request_body_failure> body_failure_{};
    std::optional<multiplexed_stream_signal> signal_;
    multiplexed_request_body_deadline request_body_deadline_;
    bool request_ended_{false};
    bool tunnel_{false};
    bool response_committed_{false};
    bool aborted_{false};
    bool closed_{false};
    bool handler_done_{false};
    bool dispatch_pending_{false};
    bool continue_pending_{false};
};

}  // namespace ruvia::detail

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {

class http3_session;
class http3_stream;
enum class http3_data_write_status : std::uint8_t {
    completed,
    // The stream was reset (by either side) or stopped.
    stream_aborted,
    // The connection ended.
    terminated,
};

// Submits all of `bytes` as DATA of a stream, at most the stream's output
// budget at a time (http3_connection::submit_data): when the budget is
// spent it waits for the peer's acknowledgements, timed by the session
// against write_timeout_. A Content-Length overrun throws std::length_error.
[[nodiscard]] task<http3_data_write_status> write_http3_stream_data(
    http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes);

// Writes a buffered or file response on one request stream: the head with its
// write plan's Content-Length, then the content within the stream's output
// budget. A file body is opened and validated before the head without being
// read; each later chunk is read only once output budget is free, at most that
// much, while holding one of multiplexed_file_read_slots through its submission. A
// stream it cannot complete is reset through the session.
class http3_buffered_response_writer final {
public:
    http3_buffered_response_writer(http3_session& session, response_file_io file_io) noexcept;

    http3_buffered_response_writer(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer& operator=(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer(http3_buffered_response_writer&&) = delete;
    http3_buffered_response_writer& operator=(http3_buffered_response_writer&&) = delete;

    [[nodiscard]] task<multiplexed_buffered_response_write_result> write(std::int64_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<multiplexed_buffered_response_write_result> write_file_body(
        std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source,
        http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail

struct ngtcp2_crypto_ossl_ctx;

namespace ruvia::detail {

class http3_endpoint;
struct http3_worker_services;

// Per-stream DATA a connection holds between submission and the peer's
// acknowledgement (http3_connection_options::max_stream_output_bytes_).
inline constexpr std::size_t http3_stream_output_bytes = 256 * 1024;
// Largest UDP payload this server sends: fits a 1500-byte Ethernet MTU behind
// IPv6, UDP and common tunnel headers, without path MTU discovery.
inline constexpr std::size_t http3_max_datagram_bytes = 1350;

// One HTTP/3 connection on the worker that accepted it: its TLS session, its
// QUIC/HTTP/3 state (http3_quic_connection), its one QUIC timer, its writer
// and the dispatch of its request streams through the shared request spine
// (admission, routes, response tail), as the HTTP/2 session engine does.
//
// The endpoint feeds datagrams in with receive(); run() is the connection's
// task: it writes every datagram the connection produces, re-arms the timer to
// expiry(), and returns once the connection has ended and every handler has
// finished. All of it runs on the owning worker.
//
// Lifecycle: open -> (graceful drain on the request cap or a connection
// timeout: shutdown-notice GOAWAY, a bounded round trip, final GOAWAY,
// CONNECTION_CLOSE(H3_NO_ERROR) once no stream is left) -> closing period
// (RFC 9000 §10.2, three probe timeouts absorbing late packets) -> ended. A
// worker stop, eviction or a timeout past its grace ends it at once with
// CONNECTION_CLOSE(H3_NO_ERROR).
class http3_session final {
public:
    // Accepts the connection the client Initial `initial` opens (RFC 9000
    // §7.2): a server connection ID owned by this worker, the client's chosen
    // ID as original_destination_connection_id.
    http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial,
        const asio::ip::udp::endpoint& remote, const ngtcp2_cid& original_dcid,
        std::optional<ngtcp2_cid> retry_scid, std::span<const std::uint8_t> retry_token,
        connection_lifecycle lifecycle);
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) = delete;
    http3_session& operator=(http3_session&&) = delete;

    // One datagram for this connection, from `remote`.
    void receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] task<void> run();
    // Ends the connection now: CONNECTION_CLOSE(H3_NO_ERROR), streams
    // cancelled, no closing period. The scanner's close action.
    void close_now() noexcept;

    // Stream I/O for the response writers, the streaming sink and the request
    // body reader of this connection's handlers.
    [[nodiscard]] http3_connection& codec() noexcept {
        return connection_->http3();
    }
    [[nodiscard]] http3_stream* find_stream(std::int64_t stream_id) noexcept;
    // Queues the stream's output for the writer.
    void wake_writer() noexcept;
    // Abandons both directions of a request stream (RFC 9114 §4.1.1) and
    // stops every wait of its handler.
    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] std::string_view remote_address() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_address_;
    }
    std::string_view remote_address() const&& = delete;
    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload);

private:
    using stream_pointer = std::unique_ptr<http3_stream>;

    enum class lifecycle_type : std::uint8_t {
        open,
        // The QUIC connection closed (either side, or idle): late packets
        // are absorbed until the closing period elapses.
        closing,
        ended,
    };

    static ngtcp2_conn* native_connection(ngtcp2_crypto_conn_ref* reference) noexcept;
    static int new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
        std::size_t size, void* user_data) noexcept;
    static int remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept;

    [[nodiscard]] static ngtcp2_tstamp now() noexcept;
    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& remote) noexcept;
    void register_connection_id(const ngtcp2_cid& cid);
    void unregister_connection_ids() noexcept;

    // Connection events.
    void process_events() noexcept;
    void drain_events();
    void start_application();
    void on_request_head(http3_request_head_event& event);
    void on_data(http3_data_event& event);
    void on_datagram(const http3_datagram_event& event);
    void on_message_end(const http3_message_end_event& event);
    void on_stream_end(std::int64_t stream_id);
    void on_stream_abort(std::int64_t stream_id) noexcept;
    void on_stream_close(const http3_stream_close_event& event) noexcept;
    void wake_output_waiters() noexcept;

    // Requests.
    [[nodiscard]] bool admit_stream(std::int64_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::int64_t stream_id);
    void finish_stream_handler(std::int64_t stream_id) noexcept;
    void remove_stream_if_done(std::int64_t stream_id) noexcept;
    void set_inactivity_phase() noexcept;
    void expire_stream_deadlines(std::int64_t now_ms) noexcept;

    // Output and timers.
    [[nodiscard]] task<void> flush();
    [[nodiscard]] task<void> send_datagram();
    void arm_timer() noexcept;
    void timer_fired(worker_timer_outcome outcome) noexcept;
    void enter_closing() noexcept;
    void terminate(std::error_code error) noexcept;

    // Graceful drain (RFC 9114 §5.2).
    void request_drain() noexcept;
    void begin_drain() noexcept;
    void arm_drain_timer() noexcept;
    void drain_timer_expired() noexcept;
    void finish_drain_if_ready() noexcept;

    http3_endpoint& endpoint_;
    const http3_worker_services& services_;
    connection_slot slot_;
    connection_lifecycle connection_lifecycle_;
    // conn_info::connection_id() of this connection.
    std::uint64_t connection_id_;
    asio::ip::udp::endpoint local_;
    // The actual destination address of the Initial, including wildcard-bound
    // listeners. TLS identity selection borrows this stable address.
    asio::ip::address tls_local_address_;
    asio::ip::udp::endpoint remote_;
    // The sender of the datagram being read, for its ngtcp2 path.
    asio::ip::udp::endpoint path_remote_;
    asio::ip::udp::endpoint path_local_;
    std::uint32_t path_interface_index_;
    std::string remote_address_;
    // Present only with an on_tls_client_hello observer; tls_ borrows it, so
    // it is declared before tls_.
    std::string local_address_;
    std::optional<tls_client_hello_observation> client_hello_;
    // Keys this connection holds in the endpoint's table.
    std::vector<http3_cid_key> connection_ids_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::array<unsigned char, 32> tls_resumption_context_{};
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref connection_reference_{};
    // Destroyed first: ngtcp2 still references the TLS objects above.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram datagram_;
    std::string client_certificate_;
    std::optional<tls_served_certificate> served_certificate_;
    inbound_buffer_account inbound_buffers_;
    std::optional<context_services> base_services_;
    http_request_admission_policy admission_policy_;
    multiplexed_session_termination termination_;
    worker_signal write_signal_;
    worker_signal handler_finished_;
    // Declared after the inbound buffers their bodies borrow.
    std::unordered_map<std::int64_t, stream_pointer> streams_;
    http3_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    std::size_t tunnel_streams_{0};
    lifecycle_type lifecycle_{lifecycle_type::open};
    bool drain_begun_{false};
    // The deadline the expiry timer is armed for, in ngtcp2 time.
    ngtcp2_tstamp armed_expiry_{UINT64_MAX};
    connection_scanner::entry scanner_entry_;
    std::optional<connection_scanner::guard> scanner_guard_;
    connection_scanner::periodic_check_registration scanner_checks_;
    // Declared last: unregistered before the state their completions touch.
    multiplexed_drain_timer drain_timer_;
    worker_timer_registration timer_;
};

}  // namespace ruvia::detail
