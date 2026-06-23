#pragma once

#include "../http2/Http2Frame.h"
#include "../http2/Http2ServerSession.h"
#include "../../http/HttpParserInternal.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <algorithm>
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
    RateLimiter& rateLimiter,
    std::string_view clientCertificate = {},
    std::string_view initialBytes = {}) {
    Http2ServerSession<Stream> session(
        stream,
        socket,
        memory,
        routes,
        &databases,
        &redis,
        &httpClients,
        options,
        scannerEntry,
        remoteAddress,
        &rateLimiter,
        clientCertificate);
    co_await session.run(initialBytes);
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
    RateLimiter& rateLimiter,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes) {
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
            current);
        co_return CleartextHttp2DispatchResult::kSessionFinished;
    case CleartextHttp2Probe::kNeedMorePreface: {
        scannerEntry.setPhase(ConnectionScanner::Phase::kReadingHeader);
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
    RateLimiter& rateLimiter,
    const HttpServerParseResult& parsed,
    std::string_view settingsPayload,
    std::string_view body,
    std::string_view initialBytes) {
    Http2ServerSession<Stream> session(
        stream,
        socket,
        memory,
        routes,
        &databases,
        &redis,
        &httpClients,
        options,
        scannerEntry,
        remoteAddress,
        &rateLimiter);
    co_await session.runUpgraded(parsed, settingsPayload, body, initialBytes);
}

}  // namespace ruvia::detail
