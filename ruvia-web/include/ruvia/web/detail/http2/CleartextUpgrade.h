#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/Async.h"
#include "ruvia/http/Http2CleartextPreface.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/http2/Http2ServerSessionSetup.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia::detail {

enum class CleartextHttp2DispatchResult : std::uint8_t {
    kContinueHttp1,
    kContinueReadLoop,
    kSessionFinished,
};

// AutoHTTPS reserves the cleartext listener for HTTP/1 redirects and therefore
// refuses prior-knowledge HTTP/2. Preface classification itself is protocol.
[[nodiscard]] inline Http2CleartextPrefaceProbe probeCleartextHttp2Preface(
    std::string_view current, bool autoHttpsEnabled) noexcept {
    if (autoHttpsEnabled) {
        return Http2CleartextPrefaceProbe::kHttp1;
    }
    return probeHttp2CleartextPreface(current);
}

// Entry point for a direct HTTP/2 connection (TLS ALPN h2, or a cleartext client
// preface). Runs the sans-I/O session (the coroutine Http2ServerSession is replaced).
template <typename Stream>
Task<void> runHttp2ServerSession(
    Http2ServerSessionSetup<Stream> setup, std::string_view initialBytes = {}) {
    (void)setup.socket;  // the sans-I/O session needs only the (possibly TLS) setup.stream
    co_await runHttp2SansIoSession(setup.stream, setup.routes, setup.memory,
        Http2SansIoSessionContext(
            std::move(setup.services), setup.options, setup.scannerEntry, setup.workerState),
        initialBytes);
}

template <typename Stream>
Task<CleartextHttp2DispatchResult> dispatchCleartextHttp2Preface(
    Http2ServerSessionSetup<Stream> setup, std::pmr::string& readBuffer, std::size_t& usedBytes,
    bool autoHttpsEnabled) {
    const auto current = std::string_view(readBuffer.data(), usedBytes);
    switch (probeCleartextHttp2Preface(current, autoHttpsEnabled)) {
        case Http2CleartextPrefaceProbe::kHttp1:
            co_return CleartextHttp2DispatchResult::kContinueHttp1;
        case Http2CleartextPrefaceProbe::kCompletePreface:
            co_await runHttp2ServerSession(setup, current);
            co_return CleartextHttp2DispatchResult::kSessionFinished;
        case Http2CleartextPrefaceProbe::kNeedMorePreface: {
            setup.scannerEntry.setPhase(ruvia::ConnectionScanner::Phase::kReadingInitial);
            auto readCompletion = co_await ruvia::asyncAsio<std::size_t>(
                [&setup, &readBuffer, usedBytes](auto handler) mutable {
                    setup.stream.async_read_some(
                        asio::buffer(readBuffer.data() + usedBytes, readBuffer.size() - usedBytes),
                        std::move(handler));
                });
            const auto ec = readCompletion.errorCode();
            const auto bytesRead = readCompletion.result();
            if (ec) {
                co_return CleartextHttp2DispatchResult::kSessionFinished;
            }
            usedBytes += bytesRead;
            setup.scannerEntry.touch();
            co_return CleartextHttp2DispatchResult::kContinueReadLoop;
        }
        case Http2CleartextPrefaceProbe::kDropConnection:
            co_return CleartextHttp2DispatchResult::kSessionFinished;
    }

    co_return CleartextHttp2DispatchResult::kSessionFinished;
}

}  // namespace ruvia::detail
