#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_submission_view.h"

#include "server/http_server_listener.h"
#include "server/native_accepted_socket_ticket.h"

namespace ruvia::detail {

// The ingress owner detaches TCP sockets once for adoption by their worker.
// Target objects must outlive this owner and its joined thread. Core owns the
// lifecycle; serving starts acceptance and stopping closes every listener.
// close_admission() closes the listeners earlier, while the runtime still runs.
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
        listener(asio::io_context& context_value, asio::ip::tcp::endpoint configured)
            : socket_(context_value),
              endpoint_(std::move(configured)),
              retry_(context_value) {}
        asio::ip::tcp::acceptor socket_;
        asio::ip::tcp::endpoint endpoint_;
        asio::steady_timer retry_;
    };
    using listener_ptr = std::unique_ptr<listener, pmr_object_deleter<listener>>;
    void begin_accept(std::size_t index) noexcept;
    void accepted(std::size_t index, const asio::error_code&, asio::ip::tcp::socket) noexcept;
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
