#pragma once

#include <atomic>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

#include "ruvia/app/App.h"
#include "ruvia/app/Task.h"
#include "ruvia/memory/MemoryPool.h"
#include "../db/DbInternal.h"
#include "../redis/RedisInternal.h"

namespace ruvia::detail {

class ConnectionScanner;
class RouteTable;

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
    void resetStartupState();
    void completeStartup(std::exception_ptr exception = nullptr) noexcept;
    void waitForStartupReady();
    void runIoContext() noexcept;
    Task<void> runWorker();
    Task<void> acceptLoop();
    Task<void> handleSession(asio::ip::tcp::socket socket);
    template <typename Stream>
    Task<void> handleStreamSession(Stream& stream, asio::ip::tcp::socket& socket);
    [[nodiscard]] std::optional<HttpResponse> tryDocumentRootResponse(
        const HttpRequest& request,
        RequestMemory& memory) const;

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::optional<asio::ssl::context> tlsContext_;
    asio::ip::tcp::endpoint endpoint_;
    const RouteTable& routes_;
    WorkerMemory memory_;
    HttpServerOptions options_;
    DbRegistry databases_;
    RedisRegistry redis_;
    std::unique_ptr<ConnectionScanner> connectionScanner_;
    std::size_t activeConnectionCount_{0};

    std::atomic_bool started_{false};
    std::jthread workerThread_;

    std::mutex startupMutex_;
    std::condition_variable startupCv_;
    std::exception_ptr startupException_;
    bool startupReady_{false};
};

}  // namespace ruvia::detail
