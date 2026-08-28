#pragma once

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/web/detail/http2/Http2ServerSessionSetup.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Task.h"

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia::detail {

enum class CleartextHttp2Probe : std::uint8_t {
    kHttp1,
    kNeedMorePreface,
    kCompletePreface,
    kDropConnection,
};

enum class CleartextHttp2DispatchResult : std::uint8_t {
    kContinueHttp1,
    kContinueReadLoop,
    kSessionFinished,
};

// Runtime policy for bytes that reached the HTTP/1 parser but do not look like
// an HTTP request line. A real HTTP-version token receives the protocol error;
// obvious non-HTTP traffic is dropped without reflecting an error response.
[[nodiscard]] inline bool shouldDropInvalidCleartextHttp1Input(std::string_view buffer, Http1ServerRequestParseFailureSource failureSource) noexcept {
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

[[nodiscard]] inline CleartextHttp2Probe probeCleartextHttp2Preface(std::string_view current, bool autoHttpsEnabled) noexcept {
    if (autoHttpsEnabled || current.empty()) {
        return CleartextHttp2Probe::kHttp1;
    }

    if (current.starts_with(kHttp2ClientPreface) || kHttp2ClientPreface.starts_with(current)) {
        return current.size() >= kHttp2ClientPreface.size() ? CleartextHttp2Probe::kCompletePreface : CleartextHttp2Probe::kNeedMorePreface;
    }

    if (current.starts_with("PRI ")) {
        return CleartextHttp2Probe::kDropConnection;
    }
    return CleartextHttp2Probe::kHttp1;
}

// Entry point for a direct HTTP/2 connection (TLS ALPN h2, or a cleartext client
// preface). Runs the sans-I/O session (the coroutine Http2ServerSession is replaced).
template <typename Stream>
Task<void> runHttp2ServerSession(Http2ServerSessionSetup<Stream> setup, std::string_view initialBytes = {}) {
    (void)setup.socket;  // the sans-I/O session needs only the (possibly TLS) setup.stream
    co_await runHttp2SansIoSession(setup.stream, setup.routes, setup.memory, Http2SansIoSessionContext(std::move(setup.services), setup.options, setup.scannerEntry, setup.workerState), initialBytes);
}

template <typename Stream>
Task<CleartextHttp2DispatchResult> dispatchCleartextHttp2Preface(Http2ServerSessionSetup<Stream> setup, std::pmr::string& readBuffer, std::size_t& usedBytes, bool autoHttpsEnabled) {
    const auto current = std::string_view(readBuffer.data(), usedBytes);
    switch (probeCleartextHttp2Preface(current, autoHttpsEnabled)) {
        case CleartextHttp2Probe::kHttp1:
            co_return CleartextHttp2DispatchResult::kContinueHttp1;
        case CleartextHttp2Probe::kCompletePreface:
            co_await runHttp2ServerSession(setup, current);
            co_return CleartextHttp2DispatchResult::kSessionFinished;
        case CleartextHttp2Probe::kNeedMorePreface: {
            setup.scannerEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
            auto readCompletion = co_await asyncAsio<std::size_t>([&setup, &readBuffer, usedBytes](auto handler) mutable { setup.stream.async_read_some(asio::buffer(readBuffer.data() + usedBytes, readBuffer.size() - usedBytes), std::move(handler)); });
            const auto ec = readCompletion.errorCode();
            const auto bytesRead = readCompletion.result();
            if (ec) {
                co_return CleartextHttp2DispatchResult::kSessionFinished;
            }
            usedBytes += bytesRead;
            setup.scannerEntry.touch();
            co_return CleartextHttp2DispatchResult::kContinueReadLoop;
        }
        case CleartextHttp2Probe::kDropConnection:
            co_return CleartextHttp2DispatchResult::kSessionFinished;
    }

    co_return CleartextHttp2DispatchResult::kSessionFinished;
}

}  // namespace ruvia::detail
