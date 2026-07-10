#pragma once

#include "ruvia/web/detail/body/HttpRequestBody.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/web/detail/router/RouteTable.h"
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
Task<void> dispatchHttpStreamBodyRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const HttpServerParseResult& parsed,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted) {
    const auto bodyAndPipeline = beginHttpBodyRoute(parsed, readBuffer, usedBytes, keepAlive, consumedBytes);

    std::exception_ptr exception;
    std::optional<StreamBodyReader<Stream>> streamReader;
    std::optional<BodyReader> bodyReader;
    try {
        streamReader.emplace(
            stream,
            memory.allocator<char>(),
            bodyAndPipeline,
            parsed.contentLength,
            parsed.chunked,
            parsed.transferCodings,
            options.maxStreamBodyBytes,
            scannerEntry,
            (parsed.contentLength > 0 || parsed.chunked) && http1WantsContinue(parsed));
        emplaceBodyReaderFacade(bodyReader, *streamReader);
        response = co_await routes.dispatch(
            parsed.request,
            routeResolution,
            requestMemory,
            baseRouteServices.withBodyReader(*bodyReader));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exceptionServices = baseRouteServices;
        if (bodyReader) {
            exceptionServices = exceptionServices.withBodyReader(*bodyReader);
        }
        co_await completeFailedHttpBodyRoute(
            scannerEntry,
            exception,
            parsed,
            routes,
            requestMemory,
            exceptionServices,
            response,
            keepAlive);
        co_return;
    }

    completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        keepAlive,
        requestCount,
        options.keepaliveRequests,
        streamReader->finished(),
        http1RequestNeedsKeepAliveSignal(parsed.request.httpVersion()),
        readBuffer,
        usedBytes,
        consumedBytes,
        bufferAlreadyCompacted,
        [&streamReader](std::pmr::string& buffer, std::size_t& size) {
            streamReader->restorePipeline(buffer, size);
        });
}

}  // namespace ruvia::detail
