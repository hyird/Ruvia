#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientPool.h"
#include "HttpClientConfigValidation.h"
#include "HttpClientTlsVerification.h"
#include "ruvia/memory/PmrResource.h"

#include <asio/ssl/context.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <memory_resource>
#include <optional>
#include <system_error>
#include <utility>

namespace ruvia::detail {

HttpClientPool::Connection::Connection(asio::io_context& ctx, std::pmr::memory_resource* memoryResource)
    : rawSocket(ctx),
      resolver(ctx),
      tlsStream(nullptr, TlsStreamDeleter{memoryResource}),
      resource(memoryResource),
      requestBuffer(memoryResource),
      responseReadBuffer(memoryResource) {}

HttpClientPool::HttpClientPool(
    asio::io_context& ioContext,
    HttpClientConfig config,
    std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(pmrResourceOrDefault(resource)),
      hostHeader_(makeHttpClientHostHeader(config_, resource_)),
      connections_(resource_),
      free_(resource_) {
    if (config_.tls) {
        configureClientTlsContext(sslContext_, config_.tlsOptions);
    }
    const auto n = config_.poolSizePerWorker;
    connections_.reserve(n);
    free_.reserve(n);
    try {
        for (std::size_t i = 0; i < n; ++i) {
            auto* connection = constructPmrObject<Connection>(resource_, ioContext_, resource_);
            try {
                connections_.push_back(connection);
            } catch (...) {
                destroyPmrObject(connection, resource_);
                throw;
            }
            free_.push_back(i);
        }
    } catch (...) {
        for (auto* conn : connections_) {
            destroyConnection(conn);
        }
        connections_.clear();
        free_.clear();
        throw;
    }
}

HttpClientPool::~HttpClientPool() {
    for (auto* conn : connections_) {
        destroyConnection(conn);
    }
}

bool HttpClientPool::hasAnyTimeout() const noexcept {
    return config_.proxyConnectTimeout.count() > 0 ||
           config_.proxyReadTimeout.count() > 0 ||
           config_.proxySendTimeout.count() > 0 ||
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
    waiters_.closeAll(connections_.size());
}

void HttpClientPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    if (config_.acquireTimeout.count() > 0) {
        waiters_.expireDeadlines(now);
    }

    for (auto* conn : connections_) {
        if (conn == nullptr || !conn->deadlineActive || conn->deadline > now) {
            continue;
        }
        conn->timedOut = true;
        std::error_code ignored;
        if (conn->deadlineKind == Connection::DeadlineKind::kResolve) {
            conn->resolver.cancel();
        } else if (conn->deadlineKind == Connection::DeadlineKind::kSocket) {
            if (conn->tlsStream) {
                conn->tlsStream->lowest_layer().cancel(ignored);
            } else {
                conn->rawSocket.cancel(ignored);
            }
        }
    }
}

void HttpClientPool::setDeadline(
    Connection& conn,
    std::chrono::milliseconds timeout,
    Connection::DeadlineKind kind) noexcept {
    conn.deadlineKind = kind;
    conn.timedOut = false;
    if (timeout.count() <= 0) {
        conn.deadlineActive = false;
        return;
    }
    conn.deadline = std::chrono::steady_clock::now() + timeout;
    conn.deadlineActive = true;
}

void HttpClientPool::clearDeadline(Connection& conn) noexcept {
    conn.deadlineActive = false;
    conn.deadlineKind = Connection::DeadlineKind::kNone;
}

bool HttpClientPool::finishDeadline(Connection& conn) noexcept {
    const auto timedOut = conn.timedOut;
    clearDeadline(conn);
    return timedOut;
}

void HttpClientPool::closeConnection(Connection& conn) noexcept {
    conn.connected = false;
    clearDeadline(conn);
    std::error_code ignored;
    conn.resolver.cancel();
    if (conn.tlsStream) {
        conn.tlsStream->lowest_layer().cancel(ignored);
        conn.tlsStream->lowest_layer().close(ignored);
    } else {
        conn.rawSocket.cancel(ignored);
        conn.rawSocket.close(ignored);
    }
}

void HttpClientPool::destroyConnection(Connection* conn) noexcept {
    destroyPmrObject(conn, resource_);
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
