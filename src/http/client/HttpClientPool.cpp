#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientPool.h"

#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <system_error>

#include "../../runtime/AsioAwait.h"

namespace ruvia::detail {

// ── Connection ────────────────────────────────────────────────────────────────

HttpClientPool::Connection::Connection(asio::io_context& ctx)
    : rawSocket(ctx) {}

// ── ConnectionGuard ───────────────────────────────────────────────────────────

HttpClientPool::ConnectionGuard::ConnectionGuard(HttpClientPool& pool, std::size_t index) noexcept
    : pool_(&pool), index_(index) {}

HttpClientPool::ConnectionGuard::~ConnectionGuard() {
    if (pool_ == nullptr) return;
    if (discard_) {
        pool_->closeConnection(*pool_->connections_[index_]);
    }
    pool_->release(index_);
}

HttpClientPool::Connection& HttpClientPool::ConnectionGuard::connection() noexcept {
    return *pool_->connections_[index_];
}

// ── HttpClientPool ────────────────────────────────────────────────────────────

HttpClientPool::HttpClientPool(
    asio::io_context& ioContext,
    HttpClientConfig config,
    std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(resource),
      connections_(resource),
      free_(resource) {
    if (config_.tls) {
        sslContext_.emplace(asio::ssl::context::tls_client);
        sslContext_->set_default_verify_paths();
        sslContext_->set_verify_mode(asio::ssl::verify_peer);
    }
    const auto n = config_.poolSizePerWorker == 0 ? 1 : config_.poolSizePerWorker;
    connections_.reserve(n);
    free_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        connections_.push_back(new Connection(ioContext_));
        free_.push_back(i);
    }
}

HttpClientPool::~HttpClientPool() {
    for (auto* conn : connections_) {
        delete conn;
    }
}

bool HttpClientPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 ||
           config_.requestTimeout.count() > 0 ||
           config_.acquireTimeout.count() > 0;
}

Task<void> HttpClientPool::connect() {
    for (auto* conn : connections_) {
        co_await connectOne(*conn);
    }
}

void HttpClientPool::closeNow() noexcept {
    closing_ = true;
    for (auto* conn : connections_) {
        closeConnection(*conn);
    }
    // Resume all waiters so they can observe closing_ and throw
    while (waiterHead_ != nullptr) {
        auto* w = waiterHead_;
        waiterHead_ = w->next;
        if (waiterHead_ != nullptr) waiterHead_->previous = nullptr;
        w->queued = false;
        *w->ready = true;
        if (w->handle) w->handle.resume();
    }
    waiterTail_ = nullptr;
}

// ── Slot management ───────────────────────────────────────────────────────────

namespace {

struct PoolWaiterAwaiter {
    HttpClientPool& pool;
    HttpClientPool::PoolWaiter waiter;
    bool ready{false};
    std::size_t index{0};

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (!pool.free_.empty()) {
            index = pool.free_.back();
            pool.free_.pop_back();
            ready = true;
            return false;
        }
        waiter.ready = &ready;
        waiter.index = &index;
        waiter.handle = handle;
        pool.enqueueWaiter(waiter);
        return true;
    }

    std::size_t await_resume() {
        if (pool.closing_) {
            throw std::runtime_error("http client pool is closed");
        }
        return index;
    }
};

}  // namespace

Task<std::size_t> HttpClientPool::acquire() {
    PoolWaiterAwaiter awaiter{*this};
    co_return co_await awaiter;
}

void HttpClientPool::release(std::size_t index) noexcept {
    if (!resumeNextWaiter(index)) {
        free_.push_back(index);
    }
}

void HttpClientPool::enqueueWaiter(PoolWaiter& w) noexcept {
    w.previous = waiterTail_;
    w.next = nullptr;
    if (waiterTail_ != nullptr) waiterTail_->next = &w;
    else waiterHead_ = &w;
    waiterTail_ = &w;
    w.queued = true;
}

void HttpClientPool::removeWaiter(PoolWaiter& w) noexcept {
    if (!w.queued) return;
    if (w.previous != nullptr) w.previous->next = w.next;
    else waiterHead_ = w.next;
    if (w.next != nullptr) w.next->previous = w.previous;
    else waiterTail_ = w.previous;
    w.queued = false;
}

bool HttpClientPool::resumeNextWaiter(std::size_t index) noexcept {
    if (waiterHead_ == nullptr) return false;
    auto* w = waiterHead_;
    removeWaiter(*w);
    *w->index = index;
    *w->ready = true;
    if (w->handle) w->handle.resume();
    return true;
}

void HttpClientPool::closeConnection(Connection& conn) noexcept {
    conn.connected = false;
    std::error_code ignored;
    if (conn.tlsStream) {
        conn.tlsStream->lowest_layer().cancel(ignored);
        conn.tlsStream->lowest_layer().close(ignored);
    } else {
        conn.rawSocket.cancel(ignored);
        conn.rawSocket.close(ignored);
    }
}

// ── Connect ───────────────────────────────────────────────────────────────────

Task<void> HttpClientPool::connectOne(Connection& conn) {
    if (conn.rawSocket.is_open()) {
        std::error_code ignored;
        if (conn.tlsStream) {
            conn.tlsStream->lowest_layer().close(ignored);
        } else {
            conn.rawSocket.close(ignored);
        }
        conn.connected = false;
    }
    // Create a fresh socket for each reconnect attempt
    conn.rawSocket = asio::ip::tcp::socket(ioContext_);

    asio::ip::tcp::resolver resolver(ioContext_);
    const auto portStr = std::to_string(config_.port);
    auto [resolveEc, endpoints] = co_await asyncResult<asio::ip::tcp::resolver::results_type>(
        [&](auto handler) {
            resolver.async_resolve(
                std::string(config_.host),
                portStr,
                std::move(handler));
        });
    if (resolveEc) {
        throw std::system_error(
            resolveEc,
            "http client: resolve failed for " + std::string(config_.host));
    }

    auto [connectEc, ep] = co_await asyncResult<asio::ip::tcp::endpoint>(
        [&](auto handler) {
            asio::async_connect(conn.rawSocket, endpoints, std::move(handler));
        });
    (void)ep;
    if (connectEc) {
        throw std::system_error(connectEc, "http client: connect failed");
    }

    {
        std::error_code ignored;
        conn.rawSocket.set_option(asio::ip::tcp::no_delay(true), ignored);
    }

    if (config_.tls) {
        if (!conn.tlsStream) {
            conn.tlsStream = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(
                conn.rawSocket, *sslContext_);
        }
        const auto handshakeEc = co_await asyncError([&](auto handler) {
            conn.tlsStream->async_handshake(
                asio::ssl::stream_base::client, std::move(handler));
        });
        if (handshakeEc) {
            throw std::system_error(handshakeEc, "http client: TLS handshake failed");
        }
    }

    conn.connected = true;
}

// ── Request ───────────────────────────────────────────────────────────────────

Task<FetchResponse> HttpClientPool::fetch(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    if (closing_) {
        throw std::runtime_error("http client pool is closed");
    }

    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& conn = guard.connection();

    if (!conn.connected) {
        co_await connectOne(conn);
    }

    try {
        co_return co_await executeRequest(conn, path, options, resource);
    } catch (...) {
        guard.discard();
        throw;
    }
}

Task<FetchResponse> HttpClientPool::executeRequest(
    Connection& conn,
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    // Build request
    const auto method = options.method.empty() ? std::string_view("GET") : options.method;
    const auto effectivePath = path.empty() ? std::string_view("/") : path;

    std::pmr::string requestBuf(resource_);
    requestBuf.reserve(256);
    requestBuf.append(method.data(), method.size());
    requestBuf.push_back(' ');
    requestBuf.append(effectivePath.data(), effectivePath.size());
    requestBuf.append(" HTTP/1.1\r\nHost: ");
    requestBuf.append(config_.host.data(), config_.host.size());
    requestBuf.append("\r\nConnection: keep-alive\r\n");

    for (const auto& hdr : options.headers) {
        requestBuf.append(hdr.name.data(), hdr.name.size());
        requestBuf.append(": ");
        requestBuf.append(hdr.value.data(), hdr.value.size());
        requestBuf.append("\r\n");
    }

    if (!options.body.empty()) {
        std::array<char, 24> lenBuf{};
        const auto [ptr, ec] = std::to_chars(
            lenBuf.data(), lenBuf.data() + lenBuf.size(), options.body.size());
        requestBuf.append("Content-Length: ");
        requestBuf.append(lenBuf.data(), static_cast<std::size_t>(ptr - lenBuf.data()));
        requestBuf.append("\r\n");
    }
    requestBuf.append("\r\n");
    if (!options.body.empty()) {
        requestBuf.append(options.body.data(), options.body.size());
    }

    // Write request
    const auto writeEc = co_await asyncError([&](auto handler) {
        if (conn.tlsStream) {
            asio::async_write(
                *conn.tlsStream,
                asio::buffer(requestBuf.data(), requestBuf.size()),
                std::move(handler));
        } else {
            asio::async_write(
                conn.rawSocket,
                asio::buffer(requestBuf.data(), requestBuf.size()),
                std::move(handler));
        }
    });
    if (writeEc) {
        conn.connected = false;
        throw std::system_error(writeEc, "http client: write failed");
    }

    // Read response headers
    std::pmr::string readBuf(resource_);
    readBuf.reserve(4096);

    while (readBuf.find("\r\n\r\n") == std::pmr::string::npos) {
        std::array<char, 4096> chunk{};
        auto [readEc, n] = co_await asyncResult<std::size_t>([&](auto handler) {
            if (conn.tlsStream) {
                conn.tlsStream->async_read_some(asio::buffer(chunk), std::move(handler));
            } else {
                conn.rawSocket.async_read_some(asio::buffer(chunk), std::move(handler));
            }
        });
        if (readEc) {
            conn.connected = false;
            throw std::system_error(readEc, "http client: read failed");
        }
        readBuf.append(chunk.data(), n);
    }

    const auto sep = readBuf.find("\r\n\r\n");
    const auto headerSection = std::string_view(readBuf.data(), sep);
    const std::size_t bodyOffset = sep + 4;

    // Parse status line
    const auto crlfPos = headerSection.find("\r\n");
    const auto firstLine = crlfPos == std::string_view::npos
        ? headerSection
        : headerSection.substr(0, crlfPos);
    const auto sp1 = firstLine.find(' ');
    int statusCode = 200;
    if (sp1 != std::string_view::npos) {
        const auto sp2 = firstLine.find(' ', sp1 + 1);
        const auto codeStr = firstLine.substr(
            sp1 + 1,
            sp2 == std::string_view::npos ? std::string_view::npos : sp2 - sp1 - 1);
        std::from_chars(codeStr.data(), codeStr.data() + codeStr.size(), statusCode);
    }

    FetchResponse response(resource);
    response.statusCode = statusCode;

    // Parse headers and find Content-Length
    std::size_t contentLength = 0;
    auto remaining = crlfPos == std::string_view::npos
        ? std::string_view{}
        : headerSection.substr(crlfPos + 2);
    while (!remaining.empty()) {
        const auto lineEnd = remaining.find("\r\n");
        const auto line = lineEnd == std::string_view::npos
            ? remaining
            : remaining.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            // Case-insensitive Content-Length check
            if (name.size() == 14) {
                bool isCL = true;
                constexpr std::string_view kCL = "Content-Length";
                for (std::size_t i = 0; i < 14 && isCL; ++i) {
                    isCL = std::tolower(static_cast<unsigned char>(name[i])) ==
                           std::tolower(static_cast<unsigned char>(kCL[i]));
                }
                if (isCL) {
                    std::from_chars(value.data(), value.data() + value.size(), contentLength);
                }
            }
            response.headers.push_back({
                std::pmr::string(name.data(), name.size(), resource),
                std::pmr::string(value.data(), value.size(), resource)});
        }
        if (lineEnd == std::string_view::npos) break;
        remaining = remaining.substr(lineEnd + 2);
    }

    // Collect body
    if (contentLength > 0) {
        response.body.resize(contentLength, '\0');
        const auto alreadyRead = readBuf.size() > bodyOffset
            ? readBuf.size() - bodyOffset
            : std::size_t{0};
        const auto toCopy = std::min(alreadyRead, contentLength);
        if (toCopy > 0) {
            std::copy_n(readBuf.data() + bodyOffset, toCopy, response.body.data());
        }
        if (toCopy < contentLength) {
            auto [bodyEc, bodyN] = co_await asyncResult<std::size_t>([&](auto handler) {
                if (conn.tlsStream) {
                    asio::async_read(
                        *conn.tlsStream,
                        asio::buffer(
                            response.body.data() + toCopy,
                            contentLength - toCopy),
                        std::move(handler));
                } else {
                    asio::async_read(
                        conn.rawSocket,
                        asio::buffer(
                            response.body.data() + toCopy,
                            contentLength - toCopy),
                        std::move(handler));
                }
            });
            (void)bodyN;
            if (bodyEc && bodyEc != asio::error::eof) {
                conn.connected = false;
                throw std::system_error(bodyEc, "http client: read body failed");
            }
        }
    }

    co_return response;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
