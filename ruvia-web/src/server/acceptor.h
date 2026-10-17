#pragma once

#if !defined(_WIN32)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_submission_view.h"

#include "server/http_server_listener.h"
#include "server/native_accepted_socket_ticket.h"

namespace ruvia::detail {

// POSIX ingress hands native TCP sockets off once for worker adoption. Linux
// accepts before the new socket's first reactor registration; other POSIX
// backends detach Asio's accepted socket. All use the same bounded worker
// hand-off and capacity gate. QUIC/UDP remains worker-local; prepare() only
// resolves the UDP listener's endpoint.
// Target objects must outlive this owner and its joined thread. Only endpoint
// values are retained from configuration. Core owns the lifecycle: serving
// starts accepting, and stopping closes the listeners before this runtime
// finalizes itself.
class acceptor final {
public:
    struct worker_target final {
        worker_submission_view submission_{};
        void* object_{};
        bool (*available_)(void*) noexcept {};
        void (*accept_)(void*, native_accepted_socket_ticket&&) noexcept {};
    };
    using failure_callback = void (*)(void*) noexcept;

    acceptor(std::span<const http_server_listener_definition> listeners, std::span<const worker_target> targets,
        void* failure_target, failure_callback failure);
    ~acceptor();
    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    // Binds every TCP listener. Cold, once, before the runtime launches.
    void prepare();
    // The HTTP/3 listener's UDP endpoint (its TCP listener's address and bound
    // port), known after prepare(); nullopt without an HTTP/3 listener.
    [[nodiscard]] std::optional<asio::ip::udp::endpoint> quic_endpoint() const noexcept;
    [[nodiscard]] worker_runtime& runtime() & noexcept {
        return runtime_;
    }
    worker_runtime& runtime() && = delete;
    // Any thread: closes every listener on the owner thread without stopping
    // the runtime. A stop closes them anyway.
    void close_admission() noexcept;
    // TCP connections closed at ingress because no worker could take them or
    // the chosen worker's bounded queue rejected the hand-off. Any thread.
    [[nodiscard]] std::size_t refused_connections() const noexcept {
        return refused_connections_.load(std::memory_order_relaxed);
    }

private:
    struct listener final {
        listener(asio::io_context& context_value, asio::ip::tcp::endpoint configured, bool quic)
            : socket_(context_value),
              endpoint_(std::move(configured)),
              retry_(context_value),
              quic_(quic) {}
        asio::ip::tcp::acceptor socket_;
        asio::ip::tcp::endpoint endpoint_;
        asio::steady_timer retry_;
        bool quic_;
    };
    using listener_ptr = std::unique_ptr<listener, pmr_object_deleter<listener>>;

    void begin_accept(std::size_t index) noexcept;
    void accept_failed(std::size_t index, const asio::error_code& error) noexcept;
    [[nodiscard]] bool hand_off(std::size_t index, native_accepted_socket_ticket ticket) noexcept;
    void schedule_retry(std::size_t index) noexcept;
    void close_listeners() noexcept;
    void stop_on_owner() noexcept;
    // Owner thread: listeners still accept.
    [[nodiscard]] bool admitting() const noexcept;

    worker_runtime runtime_;
    asio::io_context& io_context_;
    std::pmr::vector<listener_ptr> listeners_;
    std::pmr::vector<worker_target> targets_;
    void* failure_target_{};
    failure_callback failure_callback_{};
    std::size_t next_target_{};
    // Owner thread only.
    bool admitting_{true};
    std::atomic<std::size_t> refused_connections_{};
};

}  // namespace ruvia::detail

#endif  // !defined(_WIN32)
