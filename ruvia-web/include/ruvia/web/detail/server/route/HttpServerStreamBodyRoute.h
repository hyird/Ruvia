#pragma once

#include "ruvia/web/detail/server/route/Http1RouteDispatch.h"
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
Task<Http1SessionRequestCompletion> dispatchHttpStreamBodyRoute(Http1RouteDispatch<Stream> d, const Http1ServerRequestHeadReady& requestHead, const RouteResolution& routeResolution, const std::pmr::string& readBuffer, std::size_t usedBytes, std::pmr::string& pipelineStash) {
    const auto bodyAndPipeline = httpBodyAndPipeline(requestHead, readBuffer, usedBytes);

    std::exception_ptr exception;
    std::optional<BodyReaderBinding<StreamBodyReader<Stream>>> bodyReader;
    try {
        bodyReader.emplace(d.stream, d.memory.template allocator<char>(), bodyAndPipeline, d.parsed.bodyPlan, requestBodyByteLimit(RequestBodyMode::kStream, d.options.maxStreamBodyBytes, d.options.maxBufferedBodyBytes), d.scannerEntry);
        d.response = co_await d.routes.dispatch(d.parsed.request, routeResolution, d.requestMemory, d.baseRouteServices.withStreamingRequestBody(bodyReader->facade()));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exceptionServices = d.baseRouteServices;
        if (bodyReader) {
            exceptionServices = exceptionServices.withStreamingRequestBody(bodyReader->facade());
        }
        co_return co_await completeFailedHttpBodyRoute(d.scannerEntry, exception, d.parsed, d.routes, d.requestMemory, exceptionServices, d.response);
    }

    co_return completeSuccessfulHttpBodyRoute(d.scannerEntry, d.response, d.parsed.connectionPlan, d.requestSequence, bodyReader->reader().consumption(), pipelineStash, [&bodyReader](std::pmr::string& stash) { bodyReader->reader().takePipeline(stash); });
}

}  // namespace ruvia::detail
