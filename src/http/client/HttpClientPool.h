#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrObject.h"
#include "../../core/PoolWaiterQueue.h"

namespace ruvia::detail {

struct PoolWaiterAwaiter;

class HttpClientPool final {
public:
    HttpClientPool(
        asio::io_context& ioContext,
        HttpClientConfig config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~HttpClientPool();

    HttpClientPool(const HttpClientPool&) = delete;
    HttpClientPool& operator=(const HttpClientPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;

    Task<FetchResponse> fetch(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource);

private:
    friend struct PoolWaiterAwaiter;

    // Non-movable: TLS stream holds a reference to rawSocket by address.
    struct Connection final {
        using TlsStream = asio::ssl::stream<asio::ip::tcp::socket&>;
        using TlsStreamDeleter = PmrObjectDeleter<TlsStream>;

        explicit Connection(asio::io_context& ctx, std::pmr::memory_resource* resource);
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) = delete;
        Connection& operator=(Connection&&) = delete;

        asio::ip::tcp::socket rawSocket;
        asio::ip::tcp::resolver resolver;
        std::unique_ptr<TlsStream, TlsStreamDeleter> tlsStream;
        std::pmr::memory_resource* resource;
        std::pmr::string requestBuffer;
        std::pmr::string responseReadBuffer;
        enum class DeadlineKind : std::uint8_t {
            kNone,
            kResolve,
            kSocket
        };
        std::chrono::steady_clock::time_point deadline{};
        DeadlineKind deadlineKind{DeadlineKind::kNone};
        bool connected{false};
        bool deadlineActive{false};
        bool timedOut{false};
    };

    class ConnectionGuard final {
    public:
        ConnectionGuard(HttpClientPool& pool, std::size_t index) noexcept;
        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
        ~ConnectionGuard();
        void discard() noexcept { discard_ = true; }
        [[nodiscard]] Connection& connection() noexcept;
    private:
        HttpClientPool* pool_{nullptr};
        std::size_t index_{0};
        bool discard_{false};
    };

    Task<std::size_t> acquire();
    void release(std::size_t index) noexcept;
    void setDeadline(Connection& conn, std::chrono::milliseconds timeout, Connection::DeadlineKind kind) noexcept;
    void clearDeadline(Connection& conn) noexcept;
    [[nodiscard]] bool finishDeadline(Connection& conn) noexcept;
    void closeConnection(Connection& conn) noexcept;
    void destroyConnection(Connection* conn) noexcept;
    Task<void> connectOne(Connection& conn);
    Task<FetchResponse> executeRequest(
        Connection& conn,
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource);

    asio::io_context& ioContext_;
    HttpClientConfig config_;
    std::pmr::memory_resource* resource_;
    std::pmr::string hostHeader_;
    std::optional<asio::ssl::context> sslContext_;
    // Owning raw pointers: Connection is non-movable (TLS stream holds rawSocket ref)
    std::pmr::vector<Connection*> connections_;
    std::pmr::vector<std::size_t> free_;
    PoolWaiterQueue waiters_;
    bool closing_{false};
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
