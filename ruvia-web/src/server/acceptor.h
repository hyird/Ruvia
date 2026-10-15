#pragma once

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

#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_submission_view.h"
#include "ruvia/http/quic_server.h"

#include "http3/http3_capacity.h"
#include "http3/http3_datagram_endpoint.h"
#include "server/http_server_listener.h"
#include "server/native_accepted_socket_ticket.h"

namespace ruvia::detail {

class http3_datagram_channel;

// The ingress owner. TCP sockets are detached once and adopted by their worker;
// UDP packets are forwarded by CID without accessing worker connection state.
// Target objects must outlive this owner and its joined thread. Only endpoint
// values and the QUIC listener selection are retained from configuration.
// Core owns the lifecycle: startup stages QUIC channels into the (unlaunched)
// workers, serving starts accepting, and stopping retires QUIC before this
// runtime finalizes itself.
class acceptor final {
public:
    struct worker_target final {
        worker_submission_view submission_{};
        void* object_{};
        bool (*available_)(void*) noexcept {};
        void (*accept_)(void*, native_accepted_socket_ticket&&) noexcept {};
        // Stages immutable input before any worker launches. QUIC/TLS and HTTP/3
        // configuration, construction, I/O driving and retirement belong to it.
        void (*stage_quic_)(void*, http3_datagram_channel&, asio::ip::udp::endpoint,
            ruvia::quic_cid_partition){};
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
        listener(asio::io_context& context_value, asio::ip::tcp::endpoint configured, std::optional<http3_capacity> quic)
            : socket_(context_value),
              endpoint_(std::move(configured)),
              retry_(context_value),
              quic_(quic) {}
        asio::ip::tcp::acceptor socket_;
        asio::ip::tcp::endpoint endpoint_;
        asio::steady_timer retry_;
        std::optional<http3_capacity> quic_;
    };
    using listener_ptr = std::unique_ptr<listener, pmr_object_deleter<listener>>;
    using channel_ptr = std::unique_ptr<http3_datagram_channel,
        pmr_object_deleter<http3_datagram_channel>>;

    void prepare_quic();
    task<void> run_quic();
    void pump_quic() noexcept;
    [[nodiscard]] bool quic_retired() noexcept;
    static void datagram_ready(void*, http3_acceptor_datagram_endpoint::notification_kind) noexcept;
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
    std::optional<buffer_pool> quic_pool_;
    std::pmr::vector<channel_ptr> quic_channels_;
    std::unique_ptr<http3_acceptor_datagram_endpoint, pmr_object_deleter<http3_acceptor_datagram_endpoint>> udp_;
    worker_notification quic_notification_;
    // Channels with Acceptor work since the last wake, and this wake's visits.
    std::optional<http3_channel_ready_set> quic_ready_;
    std::pmr::vector<std::uint32_t> quic_visits_;
    asio::steady_timer quic_retirement_;
    void* failure_target_{};
    failure_callback failure_callback_{};
    bool quic_running_{};
    bool quic_stopping_{};
    std::size_t next_target_{};
    // Owner thread only.
    bool admitting_{true};
    std::size_t next_output_{};
    std::atomic<std::size_t> refused_connections_{};
};

}  // namespace ruvia::detail
