#pragma once

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Task.h"

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

// Runtime policy for bytes that reached the HTTP/1 parser but do not look like
// an HTTP request line. A real HTTP-version token receives the protocol error;
// obvious non-HTTP traffic is dropped without reflecting an error response.
[[nodiscard]] inline bool shouldDropInvalidCleartextHttp1Input(
    std::string_view buffer,
    Http1ServerRequestParseFailureSource failureSource) noexcept {
    if (failureSource != Http1ServerRequestParseFailureSource::kRequestLine) {
        return false;
    }

    const auto lineEnd = buffer.find("\r\n");
    if (lineEnd == std::string_view::npos) {
        return false;
    }

    auto line = buffer.substr(0, lineEnd);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }

    const auto versionStart = line.find_last_of(" \t");
    if (versionStart == std::string_view::npos || versionStart + 1 >= line.size()) {
        return false;
    }
    const auto version = line.substr(versionStart + 1);
    return !version.starts_with("HTTP/");
}

[[nodiscard]] inline CleartextHttp2Probe probeCleartextHttp2Preface(
    std::string_view current,
    bool autoHttpsEnabled) noexcept {
    if (autoHttpsEnabled || current.empty()) {
        return CleartextHttp2Probe::kHttp1;
    }

    if (current.starts_with(kHttp2ClientPreface) ||
        kHttp2ClientPreface.starts_with(current)) {
        return current.size() >= kHttp2ClientPreface.size()
            ? CleartextHttp2Probe::kCompletePreface
            : CleartextHttp2Probe::kNeedMorePreface;
    }

    if (current.starts_with("PRI ")) {
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
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry,
    ContextServices services,
    const HttpServerWorkerState& workerState,
    std::string_view initialBytes = {}) {
    (void)socket;  // the sans-I/O session needs only the (possibly TLS) stream
    co_await runHttp2SansIoSession(
        stream,
        routes,
        memory,
        Http2SansIoSessionContext(
            services,
            options,
            scannerEntry,
            workerState),
        initialBytes);
}

template <typename Stream>
Task<CleartextHttp2DispatchResult> dispatchCleartextHttp2Preface(
    Stream& stream,
    asio::ip::tcp::socket& socket,
    WorkerMemory& memory,
    const RouteTable& routes,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry,
    const ContextServices& services,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    const HttpServerWorkerState& workerState) {
    const auto current = std::string_view(readBuffer.data(), usedBytes);
    switch (probeCleartextHttp2Preface(current, options.redirect() != nullptr)) {
    case CleartextHttp2Probe::kHttp1:
        co_return CleartextHttp2DispatchResult::kContinueHttp1;
    case CleartextHttp2Probe::kCompletePreface:
        co_await runHttp2ServerSession(
            stream,
            socket,
            memory,
            routes,
            options,
            scannerEntry,
            services,
            workerState,
            current);
        co_return CleartextHttp2DispatchResult::kSessionFinished;
    case CleartextHttp2Probe::kNeedMorePreface: {
        scannerEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
        auto readCompletion = co_await asyncAsio<std::size_t>(
            [&stream, &readBuffer, usedBytes](auto handler) mutable {
                stream.async_read_some(
                    asio::buffer(readBuffer.data() + usedBytes, readBuffer.size() - usedBytes),
                    std::move(handler));
            });
        const auto ec = readCompletion.errorCode();
        const auto bytesRead = readCompletion.result();
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

}  // namespace ruvia::detail
