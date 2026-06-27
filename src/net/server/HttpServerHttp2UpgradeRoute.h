#pragma once

#include "../body/HttpRequestBody.h"
#include "../http2/Http2Upgrade.h"
#include "../http2/Http2UpgradeHandshake.h"
#include "ConnectionScanner.h"
#include "HttpServerCleartextHttp2.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <asio/ip/tcp.hpp>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

enum class Http2UpgradeRouteResult {
    kWriteBufferedResponse,
    kSessionFinished
};

template <typename Stream>
Task<Http2UpgradeRouteResult> dispatchHttp2UpgradeRoute(
    Stream& stream,
    asio::ip::tcp::socket& socket,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const RouteTable& routes,
    DbRegistry& databases,
    RedisRegistry& redis,
    HttpClientRegistry& httpClients,
    const HttpServerOptions& options,
    std::string_view remoteAddress,
    RateLimiter* rateLimiter,
    const HttpServerParseResult& parsed,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    HttpResponse& response,
    bool& closeAfterWrite,
    const std::atomic_bool* serverStarted = nullptr) {
    auto upgrade = parseHttp2UpgradeRequest(parsed, memory.resource());
    if (!upgrade.valid) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            HttpErrorInfo{.statusCode = 400, .message = "invalid http2 upgrade"},
            true,
            baseRouteServices);
        markConnectionCloseAfterWrite(response, closeAfterWrite);
        co_return Http2UpgradeRouteResult::kWriteBufferedResponse;
    }
    if (!parsed.chunked && contentLengthExceedsLimit(parsed.contentLength, options.maxBufferedBodyBytes)) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            HttpErrorInfo{.statusCode = 413, .message = "request body is too large"},
            true,
            baseRouteServices);
        markConnectionCloseAfterWrite(response, closeAfterWrite);
        co_return Http2UpgradeRouteResult::kWriteBufferedResponse;
    }

    std::pmr::string upgradedBodyStorage(memory.resource());
    std::string_view pendingFrames;
    std::exception_ptr upgradeBodyException;
    try {
        const auto bodyAndPipeline = std::string_view(
            readBuffer.data() + parsed.headerBytes,
            usedBytes - parsed.headerBytes);
        LazyBufferedBody<Stream> upgradeBody(
            stream,
            memory.allocator<char>(),
            requestMemory.resource(),
            bodyAndPipeline,
            parsed.contentLength,
            parsed.chunked,
            parsed.transferCodings,
            options.maxBufferedBodyBytes,
            scannerEntry,
            false);
        const auto upgradedBody = co_await upgradeBody.readAll();
        upgradedBodyStorage.assign(upgradedBody.data(), upgradedBody.size());
        upgradeBody.restorePipeline(readBuffer, usedBytes);
        pendingFrames = std::string_view(readBuffer.data(), usedBytes);
    } catch (...) {
        upgradeBodyException = std::current_exception();
    }
    if (upgradeBodyException != nullptr) {
        response = co_await routes.handleException(
            parsed.request,
            requestMemory,
            upgradeBodyException,
            true,
            baseRouteServices);
        markConnectionCloseAfterWrite(response, closeAfterWrite);
        co_return Http2UpgradeRouteResult::kWriteBufferedResponse;
    }
    if (parsed.status != HttpParseStatus::kComplete) {
        response = co_await routes.handleError(
            parsed.request,
            requestMemory,
            HttpErrorInfo{
                .statusCode = parsed.status == HttpParseStatus::kError
                    ? httpParseErrorStatus(parsed.error)
                    : static_cast<std::uint16_t>(400),
                .message = parsed.status == HttpParseStatus::kError
                    ? httpParseErrorMessage(parsed.error)
                    : "incomplete http2 upgrade body"},
            true,
            baseRouteServices);
        markConnectionCloseAfterWrite(response, closeAfterWrite);
        co_return Http2UpgradeRouteResult::kWriteBufferedResponse;
    }

    scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
    if (!(co_await writeHttp2UpgradeHandshake(stream))) {
        co_return Http2UpgradeRouteResult::kSessionFinished;
    }
    scannerEntry.touch();

    co_await runUpgradedHttp2ServerSession(
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
        parsed,
        upgrade.settingsPayload,
        upgradedBodyStorage,
        pendingFrames,
        serverStarted);
    co_return Http2UpgradeRouteResult::kSessionFinished;
}

}  // namespace ruvia::detail
