#pragma once

#include "ruvia/web/detail/body/HttpRequestBody.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

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
    const Http1ServerRequestParseState& parsed,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    HttpResponse& response,
    Http1ServerConnectionPlan& connectionPlan,
    std::size_t& requestCount,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted) {
    const auto bodyAndPipeline = beginHttpBodyRoute(
        parsed,
        readBuffer,
        usedBytes,
        connectionPlan,
        consumedBytes);

    std::exception_ptr exception;
    std::optional<StreamBodyReader<Stream>> streamReader;
    std::optional<BodyReader> bodyReader;
    try {
        streamReader.emplace(
            stream,
            memory.allocator<char>(),
            bodyAndPipeline,
            parsed.bodyPlan,
            options.maxStreamBodyBytes,
            scannerEntry);
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
        connectionPlan = co_await completeFailedHttpBodyRoute(
            scannerEntry,
            exception,
            parsed,
            routes,
            requestMemory,
            exceptionServices,
            response);
        co_return;
    }

    connectionPlan = completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        connectionPlan,
        requestCount,
        options.keepaliveRequests,
        streamReader->consumption(),
        readBuffer,
        usedBytes,
        consumedBytes,
        bufferAlreadyCompacted,
        [&streamReader](std::pmr::string& buffer, std::size_t& size) {
            streamReader->restorePipeline(buffer, size);
        });
}

}  // namespace ruvia::detail
