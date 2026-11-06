#pragma once

#include <ngtcp2/ngtcp2.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <system_error>
#include <unordered_map>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"

#include "http3/http3_connection_id.h"
#include "http3/http3_worker_directory.h"
#include "server/connection_count.h"
#include "server/connection_scanner.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"

namespace ruvia::detail {

class http3_session;
class route_table;
class worker_capabilities;

// The worker-owned services every HTTP/3 connection of a worker borrows.
// Each outlives the worker's connections, which its task scope joins.
struct http3_worker_services final {
    asio::io_context& io_;
    const worker_handle& worker_;
    worker_memory& memory_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    connection_scanner& scanner_;
    connection_count& connection_count_;
    task_scope& tasks_;
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
// first); an unsupported version gets Version Negotiation (RFC 9000 §6); any
// other datagram is dropped -- in particular no Stateless Reset is sent, so
// a peer that lost its connection here learns of it by its idle timeout.
class http3_endpoint final {
public:
    // `listener_index` names this listener's endpoint on every worker (and in
    // the Windows acceptor), for datagrams handed between them.
    http3_endpoint(const http3_worker_services& services, http_server_session_config& session,
        asio::ip::udp::endpoint endpoint, std::size_t listener_index);
    ~http3_endpoint();
    http3_endpoint(const http3_endpoint&) = delete;
    http3_endpoint& operator=(const http3_endpoint&) = delete;

    // POSIX binds the UDP socket (SO_REUSEADDR and SO_REUSEPORT); Windows,
    // whose acceptor binds it, only prepares the QUIC backend.
    void open();
    // Starts admitting connections, and on POSIX receiving.
    void start();
    // The worker stops: no connection is admitted any more, and the POSIX
    // socket closes. Its connections were already closed by the scanner.
    void close() noexcept;

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
    [[nodiscard]] std::error_code send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes) noexcept;
#if !defined(_WIN32)
    // Completes once the socket can send again, or with the error that ended
    // waiting (the socket closed).
    [[nodiscard]] task<std::error_code> wait_writable();
#endif
    // False when another connection already holds the ID.
    [[nodiscard]] bool register_connection_id(const http3_cid_key& key, http3_session& session);
    void unregister_connection_id(const http3_cid_key& key) noexcept;

private:
    using session_pointer = std::unique_ptr<http3_session, pmr_object_deleter<http3_session>>;

#if !defined(_WIN32)
    task<void> run_receive_loop();
#endif
    task<void> serve(session_pointer session);
    void receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, bool handed_off) noexcept;
    void accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote) noexcept;
    void send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote) noexcept;

    const http3_worker_services& services_;
    http_server_session_config& session_;
    asio::ip::udp::endpoint configured_endpoint_;
    asio::ip::udp::endpoint local_endpoint_;
    std::size_t listener_index_;
#if !defined(_WIN32)
    asio::ip::udp::socket socket_;
    // One received datagram at a time; a UDP payload never exceeds it.
    std::pmr::vector<std::byte> receive_buffer_;
#endif
    std::pmr::unordered_map<http3_cid_key, http3_session*, http3_cid_key_hash> connections_;
    // Between open() and close(): datagrams are processed.
    bool open_{false};
    bool admitting_{false};
};

}  // namespace ruvia::detail
