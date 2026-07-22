#pragma once

#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/server/request/RequestBodyLimit.h"
#include "ruvia/web/detail/body/HttpStreamBodyReader.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/server/route/HttpServerBodyRouteCompletion.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

template <typename Stream>
Task<Http1SessionRequestCompletion> dispatchHttpStreamBodyRoute(
    Stream& stream,
    WorkerMemory& memory,
    ConnectionScanner::Entry& scannerEntry,
    const Http1ServerRequestParseState& parsed,
    const Http1ServerRequestHeadReady& requestHead,
    const RouteResolution& routeResolution,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices baseRouteServices,
    const HttpServerOptions& options,
    const std::pmr::string& readBuffer,
    std::size_t usedBytes,
    std::pmr::string& pipelineStash,
    HttpResponse& response,
    Http1RequestSequence& requestSequence) {
    const auto bodyAndPipeline = httpBodyAndPipeline(
        requestHead,
        readBuffer,
        usedBytes);

    std::exception_ptr exception;
    std::optional<BodyReaderBinding<StreamBodyReader<Stream>>> bodyReader;
    try {
        bodyReader.emplace(
            stream,
            memory.allocator<char>(),
            bodyAndPipeline,
            parsed.bodyPlan,
            requestBodyByteLimit(
                RequestBodyMode::kStream,
                options.maxStreamBodyBytes,
                options.maxBufferedBodyBytes),
            scannerEntry);
        response = co_await routes.dispatch(
            parsed.request,
            routeResolution,
            requestMemory,
            baseRouteServices.withStreamingRequestBody(
                bodyReader->facade()));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exceptionServices = baseRouteServices;
        if (bodyReader) {
            exceptionServices = exceptionServices.withStreamingRequestBody(
                bodyReader->facade());
        }
        co_return co_await completeFailedHttpBodyRoute(
            scannerEntry,
            exception,
            parsed,
            routes,
            requestMemory,
            exceptionServices,
            response);
    }

    co_return completeSuccessfulHttpBodyRoute(
        scannerEntry,
        response,
        parsed.connectionPlan,
        requestSequence,
        bodyReader->reader().consumption(),
        pipelineStash,
        [&bodyReader](std::pmr::string& stash) {
            bodyReader->reader().takePipeline(stash);
        });
}

}  // namespace ruvia::detail
