#pragma once

#include "ruvia/http/detail/http2/Http2Frame.h"
#include "ruvia/web/detail/server/Http2SansIoSession.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/app/Task.h"

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia::detail {

enum class CleartextHttp2Probe {
    kHttp1,
    kNeedMorePreface,
    kCompletePreface,
    kDropConnection
};

enum class CleartextHttp2DispatchResult {
    kContinueHttp1,
    kContinueReadLoop,
    kSessionFinished
};

[[nodiscard]] inline CleartextHttp2Probe probeCleartextHttp2Preface(
    std::string_view current,
    bool autoHttpsEnabled) noexcept {
    if (autoHttpsEnabled || current.empty()) {
        return CleartextHttp2Probe::kHttp1;
    }

    const auto prefixSize = std::min(current.size(), kHttp2ClientPreface.size());
    if (current.substr(0, prefixSize) == kHttp2ClientPreface.substr(0, prefixSize)) {
        return current.size() >= kHttp2ClientPreface.size()
            ? CleartextHttp2Probe::kCompletePreface
            : CleartextHttp2Probe::kNeedMorePreface;
    }

    if (current.size() >= 4 && current.substr(0, 4) == "PRI ") {
        return CleartextHttp2Probe::kDropConnection;
    }
    return CleartextHttp2Probe::kHttp1;
}

// Entry point for a direct HTTP/2 connection (TLS ALPN h2, or a cleartext client
// preface). Runs the sans-I/O session (the coroutine Http2ServerSession is replaced).
template <typename Stream>
Task<void> runHttp2ServerSession(
    Stream& stream,
    asio::ip::tcp::socket& socket,
    WorkerMemory& memory,
    const RouteTable& routes,
    DbRegistry& databases,
    RedisRegistry& redis,
    HttpClientRegistry& httpClients,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry,
    std::string_view remoteAddress,
    RateLimiter* rateLimiter,
    std::string_view clientCertificate = {},
    std::string_view initialBytes = {},
    const std::atomic_bool* serverStarted = nullptr) {
    (void)socket;  // the sans-I/O session needs only the (possibly TLS) stream
    Http2SansIoSessionEnv env;
    env.databases = &databases;
    env.redis = &redis;
    env.httpClients = &httpClients;
    env.rateLimiter = rateLimiter;
    env.options = &options;
    env.scannerEntry = &scannerEntry;
    env.clientCertificate = clientCertificate;
    env.serverStarted = serverStarted;
    co_await runHttp2SansIoSession(stream, routes, memory, remoteAddress, env, initialBytes);
}

template <typename Stream>
Task<CleartextHttp2DispatchResult> dispatchCleartextHttp2Preface(
    Stream& stream,
    asio::ip::tcp::socket& socket,
    WorkerMemory& memory,
    const RouteTable& routes,
    DbRegistry& databases,
    RedisRegistry& redis,
    HttpClientRegistry& httpClients,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry,
    std::string_view remoteAddress,
    RateLimiter* rateLimiter,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    const std::atomic_bool* serverStarted = nullptr) {
    const auto current = std::string_view(readBuffer.data(), usedBytes);
    switch (probeCleartextHttp2Preface(current, options.autoHttps.enabled)) {
    case CleartextHttp2Probe::kHttp1:
        co_return CleartextHttp2DispatchResult::kContinueHttp1;
    case CleartextHttp2Probe::kCompletePreface:
        co_await runHttp2ServerSession(
            stream,
            socket,
            memory,
            routes,
            databases,
            redis,
            httpClients,
            options,
            scannerEntry,
            remoteAddress,
            rateLimiter,
            {},
            current,
            serverStarted);
        co_return CleartextHttp2DispatchResult::kSessionFinished;
    case CleartextHttp2Probe::kNeedMorePreface: {
        scannerEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
        auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [&stream, &readBuffer, usedBytes](auto handler) mutable {
                stream.async_read_some(
                    asio::buffer(readBuffer.data() + usedBytes, readBuffer.size() - usedBytes),
                    std::move(handler));
            });
        if (ec) {
            co_return CleartextHttp2DispatchResult::kSessionFinished;
        }
        usedBytes += bytesRead;
        scannerEntry.touch();
        co_return CleartextHttp2DispatchResult::kContinueReadLoop;
    }
    case CleartextHttp2Probe::kDropConnection:
        co_return CleartextHttp2DispatchResult::kSessionFinished;
    }

    co_return CleartextHttp2DispatchResult::kSessionFinished;
}

// Entry point for an h2c-upgraded connection (RFC 7540 §3.2): the parsed h1 request
// seeds stream 1, then the sans-I/O session takes over the connection.
template <typename Stream>
Task<void> runUpgradedHttp2ServerSession(
    Stream& stream,
    asio::ip::tcp::socket& socket,
    WorkerMemory& memory,
    const RouteTable& routes,
    DbRegistry& databases,
    RedisRegistry& redis,
    HttpClientRegistry& httpClients,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry,
    std::string_view remoteAddress,
    RateLimiter* rateLimiter,
    const HttpServerParseResult& parsed,
    std::string_view settingsPayload,
    std::string_view body,
    std::string_view initialBytes,
    const std::atomic_bool* serverStarted = nullptr) {
    (void)socket;
    const Http2SansIoUpgradeSeed seed{&parsed, settingsPayload, body};
    Http2SansIoSessionEnv env;
    env.databases = &databases;
    env.redis = &redis;
    env.httpClients = &httpClients;
    env.rateLimiter = rateLimiter;
    env.options = &options;
    env.scannerEntry = &scannerEntry;
    env.clientCertificate = {};
    env.serverStarted = serverStarted;
    env.upgrade = &seed;
    co_await runHttp2SansIoSession(stream, routes, memory, remoteAddress, env, initialBytes);
}

}  // namespace ruvia::detail
