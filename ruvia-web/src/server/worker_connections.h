#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/WebWorker.h"

#include "server/HttpConnectionState.h"
#include "server/HttpServerListener.h"
#include "server/HttpServerOptions.h"
#include "server/HttpServerWorkerState.h"
#include "server/NativeAcceptedSocketTicket.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;
class AcceptedConnectionLease;
class ContextServices;
class RouteTable;
class WorkerCapabilities;

// Worker-local connection resources and session assembly. The surrounding Web
// runtime owns lifecycle phases and the TaskScope that joins all sessions.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const WorkerHandle& worker,
        WorkerMemory& memory, const RouteTable& routes, WorkerCapabilities& capabilities,
        HttpServerOptions& options, const StopToken& stop_token,
        HttpServerWorkerState& state, TaskScope& tasks,
        std::span<const HttpServerListenerDefinition> listeners);
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    void close_admission() noexcept {
        serving_.store(false, std::memory_order_release);
    }
    void stop() noexcept;
    void retire_tls() noexcept;
    [[nodiscard]] bool available() const noexcept;
    void accept(NativeAcceptedSocketTicket&& ticket) noexcept;
    [[nodiscard]] HttpServerStats stats() const noexcept;
    [[nodiscard]] ConnectionScanner& scanner() noexcept {
        return scanner_;
    }
    [[nodiscard]] HttpServerSessionConfig& listener(std::size_t index) noexcept {
        return *listeners_[index];
    }
    [[nodiscard]] std::atomic<std::size_t>& active_counter() noexcept {
        return active_;
    }
    [[nodiscard]] std::atomic<std::size_t>& refused_counter() noexcept {
        return refused_;
    }

private:
    void configure_tls(HttpServerSessionConfig& session);
    void accept_socket(std::size_t index, TcpSocket socket);
    Task<void> run_session(HttpServerSessionConfig& listener, AcceptedConnectionLease connection);
    template <typename Stream>
    Task<void> run_stream(HttpServerSessionConfig& listener, Stream& stream, TcpSocket& socket, ContextServices services);
    template <typename Stream>
    Task<void> run_http2(Stream& stream, TcpSocket& socket, ContextServices services, std::string_view initial = {});
    asio::io_context& io_;
    const WorkerHandle& worker_;
    WorkerMemory& memory_;
    const RouteTable& routes_;
    WorkerCapabilities& capabilities_;
    HttpServerOptions& options_;
    const StopToken& stop_token_;
    HttpServerWorkerState& state_;
    TaskScope& tasks_;
    std::pmr::vector<std::unique_ptr<HttpServerSessionConfig, PmrObjectDeleter<HttpServerSessionConfig>>> listeners_;
    ConnectionScanner scanner_;
    ConnectionWorkSetPool work_sets_;
    // Caller-thread statistics and ingress observations require atomic reads.
    std::atomic<std::size_t> active_{};
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
    std::atomic<bool> serving_{};
};

}  // namespace ruvia::detail
