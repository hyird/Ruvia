#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http1ServerRequestParser.h"
#include "ruvia/http/HttpResponse.h"

#include "body/HttpRequestBodyFacade.h"
#include "body/HttpStreamBodyReader.h"
#include "router/RouteTable.h"
#include "server/Http1RouteDispatch.h"
#include "server/HttpServerBodyRouteCompletion.h"
#include "server/HttpServerOptions.h"
#include "server/RequestBodyLimit.h"

namespace ruvia::detail {

template <typename Stream>
Task<Http1SessionRequestCompletion> dispatchHttpStreamBodyRoute(Http1RouteDispatch<Stream> d,
    const Http1ServerRequestHeadReady& requestHead, const RouteResolution& routeResolution,
    const std::pmr::string& readBuffer, std::size_t usedBytes, std::pmr::string& pipelineStash) {
    const auto bodyAndPipeline = httpBodyAndPipeline(requestHead, readBuffer, usedBytes);

    std::exception_ptr exception;
    std::optional<BodyReaderBinding<StreamBodyReader<Stream>>> bodyReader;
    try {
        const auto* resolved = routeResolution.resolved();
        const auto routeLimit =
            resolved != nullptr ? resolved->route().maxRequestBodyBytes() : std::size_t{0};
        bodyReader.emplace(d.stream, d.memory.template allocator<char>(), bodyAndPipeline,
            d.parsed.bodyPlan,
            requestBodyByteLimit(RequestBodyMode::kStream, d.options.max_stream_body_bytes,
                d.options.max_buffered_body_bytes, routeLimit),
            d.scannerEntry);
        d.response = co_await d.routes.dispatch(d.parsed.request, routeResolution, d.requestMemory,
            d.baseRouteServices.withStreamingRequestBody(bodyReader->facade()).withRequestTrailers(bodyReader->reader().trailers()));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exceptionServices = d.baseRouteServices;
        if (bodyReader) {
            exceptionServices = exceptionServices.withStreamingRequestBody(bodyReader->facade()).withRequestTrailers(bodyReader->reader().trailers());
        }
        co_return co_await completeFailedHttpBodyRoute(d.scannerEntry, exception, d.parsed,
            d.routes, d.requestMemory, exceptionServices, d.response);
    }

    co_return completeSuccessfulHttpBodyRoute(d.scannerEntry, d.response, d.parsed.connectionPlan,
        d.requestSequence, bodyReader->reader().consumption(), pipelineStash,
        [&bodyReader](std::pmr::string& stash) { bodyReader->reader().takePipeline(stash); });
}

}  // namespace ruvia::detail
