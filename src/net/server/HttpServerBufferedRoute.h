#pragma once

#include "../body/HttpRequestBody.h"
#include "ConnectionScanner.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpParser.h"
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
    const HttpParseResult& parsed,
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
    consumedBytes = parsed.headerBytes;
    keepAlive = shouldKeepAlive(parsed);
    const auto bodyAndPipeline = std::string_view(
        readBuffer.data() + parsed.headerBytes,
        usedBytes - parsed.headerBytes);

    std::exception_ptr exception;
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
        response = co_await routes.dispatch(
            parsed.request,
            routeResolution,
            requestMemory,
            baseRouteServices.withBodyLoader(&*bodyLoader));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        response = co_await routes.handleException(
            parsed.request,
            requestMemory,
            exception,
            true,
            baseRouteServices);
        response.materializeBody();
        keepAlive = false;
    } else {
        finalizeBodyRouteResponse(
            response,
            keepAlive,
            requestCount,
            options.maxRequestsPerConnection,
            lazyBody->consumed());
        if (keepAlive) {
            lazyBody->restorePipeline(readBuffer, usedBytes);
            consumedBytes = 0;
            bufferAlreadyCompacted = true;
        }
    }
    scannerEntry.touch();
}

}  // namespace ruvia::detail
