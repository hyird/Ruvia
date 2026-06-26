#pragma once

#include <atomic>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/steady_timer.hpp>
#include <condition_variable>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/app/App.h"
#include "ruvia/app/Task.h"
#include "ruvia/memory/MemoryPool.h"
#include "ConnectionScanner.h"
#include "HttpConnectionState.h"
#include "RateLimiter.h"
#include "../../db/DbInternal.h"
#include "../../redis/RedisInternal.h"
#include "../../http/client/HttpClientInternal.h"
#include "RateLimitDecision.h"

namespace ruvia::detail {

class RouteTable;

using SniContextStore = std::pmr::vector<asio::ssl::context>;
using SniContextLookup = std::pmr::vector<std::pair<std::pmr::string, asio::ssl::context*>>;

class HttpServer final {
public:
    HttpServer(
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases = {},
        HttpServerOptions options = {});
    HttpServer(
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases,
        std::span<const RedisDefinition> redis,
        HttpServerOptions options = {});
    HttpServer(
        asio::ip::tcp::endpoint endpoint,
        const RouteTable& routes,
        std::span<const DbDefinition> databases,
        std::span<const RedisDefinition> redis,
        std::span<const HttpClientDefinition> httpClients,
        HttpServerOptions options = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();
    void stop();
    void join();
    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint() const;

private:
    void configureAcceptor();
    void configureTlsContext();
    void stopOnContext() noexcept;
    void forceCloseAll() noexcept;
    void resetStartupState();
    void completeStartup(std::exception_ptr exception = nullptr) noexcept;
    void waitForStartupReady();
    void runIoContext() noexcept;
    Task<void> runWorker();
    Task<void> acceptLoop();
    Task<void> handleSession(asio::ip::tcp::socket socket);
    template <typename Stream>
    Task<void> handleStreamSession(Stream& stream, asio::ip::tcp::socket& socket, std::string_view clientCertificate = {});
    template <typename Stream>
    Task<void> handleHttp2Session(Stream& stream, asio::ip::tcp::socket& socket, std::string_view initialBytes = {}, std::string_view clientCertificate = {});
    [[nodiscard]] std::optional<HttpResponse> tryDocumentRootResponse(
        const HttpRequest& request,
        RequestMemory& memory) const;

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer drainTimer_;
    std::optional<asio::ssl::context> tlsContext_;
    asio::ip::tcp::endpoint endpoint_;
    const RouteTable& routes_;
    WorkerMemory memory_;
    // Per-host SNI contexts (RFC 6066), owned here so they outlive connections;
    // sniLookup_ maps a lowercased host to its context for the SNI callback.
    SniContextStore sniContexts_;
    SniContextLookup sniLookup_;
    HttpServerOptions options_;
    DbRegistry databases_;
    RedisRegistry redis_;
    HttpClientRegistry httpClients_;
    RateLimiter rateLimiter_;
    ConnectionScanner connectionScanner_;
    ConnectionWorkSetPool workSetPool_;
    std::size_t activeConnectionCount_{0};

    std::atomic_bool started_{false};
    std::jthread workerThread_;

    std::mutex startupMutex_;
    std::condition_variable startupCv_;
    std::exception_ptr startupException_;
    bool startupReady_{false};
};

}  // namespace ruvia::detail
