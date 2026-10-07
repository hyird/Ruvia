#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerSubmissionView.h"
#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/quic_server.h"
#include "ruvia/web/detail/http3/http3_datagram_endpoint.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/NativeAcceptedSocketTicket.h"

namespace ruvia::detail {

class http3_datagram_channel;

// The ingress owner. TCP sockets are detached once and adopted by their worker;
// UDP packets are forwarded by CID without accessing worker connection state.
// Target objects must outlive this owner and its joined thread. Only endpoint
// values and the QUIC listener selection are retained from configuration.
class acceptor final {
public:
    struct worker_target final {
        WorkerSubmissionView submission{};
        void* object{};
        bool (*available)(void*) noexcept {};
        void (*accept)(void*, NativeAcceptedSocketTicket&&) noexcept {};
        // Stages immutable input before any worker launches. QUIC/TLS and HTTP/3
        // configuration, construction, I/O driving and retirement belong to it.
        void (*stage_quic)(void*, http3_datagram_channel&, asio::ip::udp::endpoint,
            ruvia::quic_cid_partition){};
    };
    using failure_callback = void (*)(void*) noexcept;

    acceptor(std::span<const HttpServerListenerDefinition> listeners,
        std::span<const worker_target> targets, void* failure_target = nullptr,
        failure_callback failure = nullptr);
    ~acceptor();
    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    void prepare();
    void launch();
    void wait_until_ready();
    void request_serve();
    [[nodiscard]] bool wait_until_serving();
    void stop() noexcept;
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint(std::size_t index) const;
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrow_failure() const;
    [[nodiscard]] RuntimeLifecycle::State state() const noexcept {
        return runtime_.state();
    }

private:
    struct listener final {
        listener(asio::io_context& context, asio::ip::tcp::endpoint configured, bool quic)
            : socket(context),
              endpoint(std::move(configured)),
              retry(context),
              quic(quic) {}
        asio::ip::tcp::acceptor socket;
        asio::ip::tcp::endpoint endpoint;
        asio::steady_timer retry;
        bool quic{};
    };
    using listener_ptr = std::unique_ptr<listener, PmrObjectDeleter<listener>>;
    using channel_ptr = std::unique_ptr<http3_datagram_channel,
        PmrObjectDeleter<http3_datagram_channel>>;

    void prepare_quic();
    Task<void> run_quic();
    void pump_quic() noexcept;
    [[nodiscard]] bool quic_retired() noexcept;
    static void datagram_ready(void*, http3_acceptor_datagram_endpoint::notification_kind) noexcept;
    void begin_accept(std::size_t index) noexcept;
    void accepted(std::size_t index, const asio::error_code&, asio::ip::tcp::socket) noexcept;
    void schedule_retry(std::size_t index) noexcept;
    void fail(std::exception_ptr) noexcept;
    void close_listeners() noexcept;
    void stop_on_owner() noexcept;

    worker_runtime runtime_;
    asio::io_context& io_context_;
    std::pmr::vector<listener_ptr> listeners_;
    std::pmr::vector<worker_target> targets_;
    std::optional<buffer_pool> quic_pool_;
    std::pmr::vector<channel_ptr> quic_channels_;
    std::unique_ptr<http3_acceptor_datagram_endpoint, PmrObjectDeleter<http3_acceptor_datagram_endpoint>> udp_;
    WorkerNotification quic_notification_;
    asio::steady_timer quic_retirement_;
    void* failure_target_{};
    failure_callback failure_callback_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::exception_ptr failure_;
    bool prepared_{};
    bool ready_{};
    bool serve_requested_{};
    bool serving_{};
    bool quic_running_{};
    bool quic_stopping_{};
    std::size_t next_target_{};
    std::size_t next_output_{};
};

}  // namespace ruvia::detail
