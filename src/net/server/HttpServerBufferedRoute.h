#pragma once

#include "../body/HttpRequestBody.h"
#include "ConnectionScanner.h"
#include "HttpServerBodyRouteCompletion.h"
#include "../../http/RequestBodyLoader.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

template <typename Stream>
Task<void> dispatchHttpBufferedBodyRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const HttpServerParseResult& parsed,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    RouteServices baseRouteServices,
    const HttpServerOptions& options,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted) {
    const auto bodyAndPipeline = beginHttpBodyRoute(parsed, readBuffer, usedBytes, keepAlive, consumedBytes);

    // The body reader/loader are this transport's own state, and their setup can
    // throw (e.g. constructing a transfer-coding decoder for a bad
    // Transfer-Encoding), so it stays guarded here. The dispatch itself is the
    // routing layer's concern and never throws — dispatchBuffered turns any
    // handler or routing failure into a response — so it sits outside the guard.
    std::exception_ptr setupException;
    std::optional<LazyBufferedBody<Stream>> lazyBody;
    std::optional<RequestBodyLoader> bodyLoader;
    try {
        lazyBody.emplace(
            stream,
            memory.allocator<char>(),
            requestMemory.resource(),
            bodyAndPipeline,
            parsed.contentLength,
            parsed.chunked,
            parsed.transferCodings,
            options.maxBufferedBodyBytes,
            scannerEntry,
            (parsed.contentLength > 0 || parsed.chunked) && wantsContinue(parsed));
        bodyLoader.emplace(
            &*lazyBody,
            &LazyBufferedBody<Stream>::readAllThunk,
            &LazyBufferedBody<Stream>::discardThunk);
    } catch (...) {
        setupException = std::current_exception();
    }

    if (setupException != nullptr) {
        co_await completeFailedHttpBodyRoute(
            scannerEntry,
            setupException,
            parsed,
            routes,
            requestMemory,
            baseRouteServices,
            response,
            keepAlive);
        co_return;
    }

    response = co_await routes.dispatchBuffered(
        parsed.request,
        routeResolution,
        requestMemory,
        true,
        baseRouteServices.withBodyLoader(&*bodyLoader));

    completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        keepAlive,
        requestCount,
        options.maxRequestsPerConnection,
        lazyBody->consumed(),
        readBuffer,
        usedBytes,
        consumedBytes,
        bufferAlreadyCompacted,
        [&lazyBody](std::pmr::string& buffer, std::size_t& size) {
            lazyBody->restorePipeline(buffer, size);
        });
}

}  // namespace ruvia::detail
