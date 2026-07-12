#pragma once

#include "ruvia/web/detail/body/HttpLazyBufferedBody.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/http/RequestBodyLoader.h"
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
struct HttpLazyBufferedBodyRouteState final {
    std::optional<LazyBufferedBody<Stream>> body;
    std::optional<RequestBodyLoader> loader;

    void emplace(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource,
        std::string_view bodyAndPipeline,
        Http1RequestBodyPlan bodyPlan,
        std::size_t maxBodyBytes,
        ConnectionScanner::Entry& scannerEntry) {
        body.emplace(
            stream,
            workerAllocator,
            requestResource,
            bodyAndPipeline,
            bodyPlan,
            maxBodyBytes,
            scannerEntry);
        emplaceRequestBodyLoaderFacade(loader, *body);
    }

    [[nodiscard]] ContextServices withLoader(ContextServices services) noexcept {
        return services.withLazyRequestBody(*loader);
    }

    [[nodiscard]] Http1RequestBodyConsumption consumption() const noexcept {
        return body->consumption();
    }

    void restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
        body->restorePipeline(readBuffer, usedBytes);
    }
};

template <typename Stream>
inline void prepareHttpLazyBufferedBodyRoute(
    HttpLazyBufferedBodyRouteState<Stream>& state,
    Stream& stream,
    WorkerMemory& memory,
    RequestMemory& requestMemory,
    std::string_view bodyAndPipeline,
    const Http1ServerRequestParseState& parsed,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry) {
    state.emplace(
        stream,
        memory.allocator<char>(),
        requestMemory.resource(),
        bodyAndPipeline,
        parsed.bodyPlan,
        options.maxBufferedBodyBytes,
        scannerEntry);
}

[[nodiscard]] inline std::string_view httpBodyAndPipeline(
    const Http1ServerRequestParseState& parsed,
    const std::pmr::string& readBuffer,
    std::size_t usedBytes) noexcept {
    return std::string_view(readBuffer.data() + parsed.headerBytes, usedBytes - parsed.headerBytes);
}

inline Task<Http1SessionRequestCompletion> completeFailedHttpBodyRoute(
    ConnectionScanner::Entry& scannerEntry,
    std::exception_ptr exception,
    const Http1ServerRequestParseState& parsed,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices exceptionServices,
    HttpResponse& response) {
    response = co_await routes.handleException(
        parsed.request,
        requestMemory,
        exception,
        exceptionServices);
    materializeResponseBody(response);
    scannerEntry.touch();
    const auto connectionPlan = http1FinalizeResponseConnection(
        response, parsed.connectionPlan.requireClose());
    co_return Http1SessionRequestCompletion::makeBufferedClosing(
        connectionPlan);
}

template <typename RestorePipeline>
[[nodiscard]] inline Http1SessionRequestCompletion
completeSuccessfulHttpBodyRoute(
    ConnectionScanner::Entry& scannerEntry,
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    Http1RequestSequence& requestSequence,
    Http1RequestBodyConsumption bodyConsumption,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    RestorePipeline restorePipeline) {
    connectionPlan = finalizeBodyRouteResponse(
        response,
        connectionPlan,
        requestSequence,
        bodyConsumption);
    if (connectionPlan.disposition() == Http1ConnectionDisposition::kReuse) {
        restorePipeline(readBuffer, usedBytes);
        scannerEntry.touch();
        return Http1SessionRequestCompletion::makeBufferedRestored(
            connectionPlan);
    }
    scannerEntry.touch();
    return Http1SessionRequestCompletion::makeBufferedClosing(
        connectionPlan);
}

}  // namespace ruvia::detail
