#pragma once

#if defined(_WIN32)
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>
#endif

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"

#include "server/http_server_listener.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

class connection_count;
#if defined(RUVIA_ENABLE_HTTP3)
class http3_worker_directory;
#endif

// An accepted TCP SOCKET that no I/O completion port owns yet: the acceptor
// creates it for AcceptEx and hands it to a business worker, whose io_context
// binds it on adoption. Closes it unless released.
class windows_accepted_socket final {
public:
    explicit windows_accepted_socket(SOCKET handle) noexcept
        : handle_(handle) {}
    windows_accepted_socket(windows_accepted_socket&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_SOCKET)) {}
    windows_accepted_socket(const windows_accepted_socket&) = delete;
    windows_accepted_socket& operator=(const windows_accepted_socket&) = delete;
    windows_accepted_socket& operator=(windows_accepted_socket&&) = delete;
    ~windows_accepted_socket();

    [[nodiscard]] SOCKET get() const noexcept {
        return handle_;
    }
    // The caller now owns the handle.
    SOCKET release() noexcept {
        return std::exchange(handle_, INVALID_SOCKET);
    }

private:
    SOCKET handle_;
};

// The application's dedicated Windows acceptor (AGENTS.md §5): a lightweight
// worker with its own core runtime and thread that owns every TCP listener,
// and every HTTP/3 UDP socket, exclusively (SO_EXCLUSIVEADDRUSE), so the
// business workers stay alike.
//
// TCP: AcceptEx accepts a raw SOCKET that no completion port owns and hands
// it to the business worker holding the fewest connections (ties rotate),
// through that worker's bounded queue: admission is reserved first, and a
// socket the queue refuses is closed and counted as refused. The worker binds
// the socket to its own io_context and serves it like any connection.
//
// HTTP/3: the acceptor receives every datagram and hands it, through the
// HTTP/3 worker directory, to the worker its destination connection ID names,
// or for a long header naming none (a new connection's Initial) to the worker
// http3_initial_worker picks. Workers send through transmit(): the datagram
// is queued, bounded per worker, and sent from the listener's socket.
//
// Lifecycle (driven by the application run):
//   start()          binds every listener; the two-phase start's first phase
//   serve()          after every business worker serves: accepting begins
//   stop_admission() pending accepts and receives are cancelled; the UDP
//                    sockets stay open for the business workers' last
//                    datagrams (CONNECTION_CLOSE)
//   request_stop()   after the business workers joined: every socket closes
//   join()
class windows_acceptor final {
public:
    // Business worker thread: adopts one accepted socket.
    using adopt_type = void (*)(void* target, std::size_t listener_index, windows_accepted_socket accepted);

    // One business worker as a hand-over target.
    struct business_worker final {
        worker_handle worker_;
        const connection_count* load_{nullptr};
        void* target_{nullptr};
        adopt_type adopt_{nullptr};
    };

    windows_acceptor(std::span<const http_server_listener_definition> listeners, std::size_t worker_count,
#if defined(RUVIA_ENABLE_HTTP3)
        http3_worker_directory* directory,
#endif
        worker_failure_sink failure, std::pmr::memory_resource* resource);
    ~windows_acceptor();

    windows_acceptor(const windows_acceptor&) = delete;
    windows_acceptor& operator=(const windows_acceptor&) = delete;

    // Construction thread, once per business worker in worker order.
    void bind_worker(std::size_t index, business_worker target);
    // Lifecycle thread. False when a stop came first; rethrows the
    // acceptor's failure (a listener that cannot be bound).
    [[nodiscard]] bool start();
    [[nodiscard]] bool serve();
    // Any thread.
    void stop_admission() noexcept;
    void request_stop() noexcept;
    // Lifecycle thread, after request_stop().
    void join();
    // After join(): the acceptor's failure, taking ownership of it.
    [[nodiscard]] std::exception_ptr failure() noexcept;
    // Any thread: sockets the chosen worker's queue refused, and accept
    // failures other than peer disconnects.
    [[nodiscard]] std::size_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t accept_failures() const noexcept {
        return accept_failures_.load(std::memory_order_relaxed);
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // Business worker `worker_index`'s thread: queues one datagram for the
    // socket of HTTP/3 listener `http3_listener_index`. False when it was
    // dropped (the worker's bound is full or the acceptor stopped).
    [[nodiscard]] bool transmit(std::size_t worker_index, std::size_t http3_listener_index,
        const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes) noexcept;
#endif

private:
    struct tcp_listener;
    struct worker_slot;
#if defined(RUVIA_ENABLE_HTTP3)
    struct udp_listener;
#endif

    task<void> run();
    void open_listeners();
    void serve_on_context();
    void close_admission() noexcept;
    task<void> run_accepts(tcp_listener& listener);
    void report_accept_error(const std::error_code& error);
    void dispatch(std::size_t listener_index, windows_accepted_socket accepted);
    [[nodiscard]] std::size_t select_worker() noexcept;
#if defined(RUVIA_ENABLE_HTTP3)
    task<void> run_receives(udp_listener& listener);
    void steer(const udp_listener& listener, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& remote) noexcept;
    void send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote,
        std::span<const std::byte> bytes) noexcept;
#endif

    // Declared first: every socket below belongs to its io_context.
    worker_runtime runtime_;
    asio::io_context& io_;
    std::pmr::memory_resource* resource_;
    worker_failure_sink failure_sink_;
    worker_signal finalize_signal_;
    task_scope tasks_;
    std::pmr::vector<std::unique_ptr<worker_slot, pmr_object_deleter<worker_slot>>> workers_;
    std::pmr::vector<std::unique_ptr<tcp_listener, pmr_object_deleter<tcp_listener>>> tcp_listeners_;
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_directory* directory_;
    std::pmr::vector<std::unique_ptr<udp_listener, pmr_object_deleter<udp_listener>>> udp_listeners_;
#endif
    // Owner thread.
    std::size_t next_worker_{0};
    bool admitting_{false};
    bool stopping_{false};
    std::atomic<std::size_t> refused_{0};
    std::atomic<std::size_t> accept_failures_{0};
};

}  // namespace ruvia::detail
#endif
