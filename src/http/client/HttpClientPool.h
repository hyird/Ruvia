#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrObject.h"
#include "HttpClientBackend.h"
#include "HttpClientResponseParser.h"
#include "../../core/PoolWaiterQueue.h"

namespace ruvia::detail {

struct PoolWaiterAwaiter;

class HttpClientPool final : public HttpClientBackend {
public:
    HttpClientPool(
        asio::io_context& ioContext,
        HttpClientConfig config,
        std::pmr::memory_resource* resource = nullptr);
    ~HttpClientPool() override;

    HttpClientPool(const HttpClientPool&) = delete;
    HttpClientPool& operator=(const HttpClientPool&) = delete;

    Task<void> connect() override;
    void closeNow() noexcept override;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept override;
    [[nodiscard]] bool hasAnyTimeout() const noexcept override;

    Task<FetchResponse> fetch(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource) override;
    Task<FetchResponseStream> fetchStream(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource) override;

    void destroy() noexcept override { destroyPmrObject(this, resource_); }

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
        ConnectionGuard(ConnectionGuard&& other) noexcept
            : pool_(other.pool_), index_(other.index_), discard_(other.discard_) {
            other.pool_ = nullptr;
        }
        ConnectionGuard& operator=(ConnectionGuard&&) = delete;
        ~ConnectionGuard();
        void discard() noexcept { discard_ = true; }
        [[nodiscard]] std::size_t index() const noexcept { return index_; }
        [[nodiscard]] Connection& connection() noexcept;
    private:
        HttpClientPool* pool_{nullptr};
        std::size_t index_{0};
        bool discard_{false};
    };

    friend class Http1StreamSource;

    Task<std::size_t> acquire();
    void release(std::size_t index) noexcept;
    // Arm a relative inactivity timeout (nginx-style: reset per I/O). 0 disables.
    void setDeadline(Connection& conn, std::chrono::milliseconds timeout, Connection::DeadlineKind kind) noexcept;
    void clearDeadline(Connection& conn) noexcept;
    [[nodiscard]] bool finishDeadline(Connection& conn) noexcept;
    void closeConnection(Connection& conn) noexcept;
    void destroyConnection(Connection* conn) noexcept;
    Task<void> connectOne(Connection& conn);
    Task<void> readChunkedResponseBody(
        Connection& conn,
        FetchResponse& response,
        std::size_t bodyOffset,
        std::chrono::milliseconds readTimeout);
    Task<void> readCloseDelimitedResponseBody(
        Connection& conn,
        FetchResponse& response,
        std::size_t bodyOffset,
        std::chrono::milliseconds readTimeout);

    // Connection I/O primitives (branch on TLS vs raw), shared by the buffered and streaming paths.
    Task<std::error_code> connWrite(Connection& conn, std::array<asio::const_buffer, 2> buffers);
    Task<std::pair<std::error_code, std::size_t>> connReadSome(Connection& conn, asio::mutable_buffer buffer);
    Task<std::pair<std::error_code, std::size_t>> connRead(Connection& conn, asio::mutable_buffer buffer);

    // Build + send the request and read/parse the response head, leaving any buffered body bytes
    // past head.bodyOffset in conn.responseReadBuffer. Shared by executeRequest (buffered) and
    // fetchStream (incremental). All I/O uses nginx-style inactivity timeouts: each write resets
    // sendTimeout (proxy_send_timeout), each read resets readTimeout (proxy_read_timeout).
    // Throws on write/read/parse failure.
    Task<HttpClientResponseHead> writeRequestAndReadHead(
        Connection& conn,
        std::string_view path,
        const FetchOptions& options,
        std::chrono::milliseconds readTimeout,
        std::chrono::milliseconds sendTimeout,
        FetchResponse& response,
        std::pmr::memory_resource* requestResource);
    Task<void> writeChunkedRequestBody(
        Connection& conn,
        const RequestBodyStream& bodyStream,
        std::chrono::milliseconds sendTimeout);
    Task<FetchResponse> executeRequest(
        Connection& conn,
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* requestResource);

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
