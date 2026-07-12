#pragma once

#include "ruvia/web/detail/body/HttpRequestBody.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/http/RequestBodyLoader.h"
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

[[nodiscard]] inline std::string_view beginHttpBodyRoute(
    const Http1ServerRequestParseState& parsed,
    const std::pmr::string& readBuffer,
    std::size_t usedBytes,
    Http1ServerConnectionPlan& connectionPlan,
    std::size_t& consumedBytes) noexcept {
    consumedBytes = parsed.headerBytes;
    connectionPlan = parsed.connectionPlan;
    return std::string_view(readBuffer.data() + parsed.headerBytes, usedBytes - parsed.headerBytes);
}

inline Task<Http1ServerConnectionPlan> completeFailedHttpBodyRoute(
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
    co_return http1FinalizeResponseConnection(
        response,
        parsed.connectionPlan.requireClose());
}

template <typename RestorePipeline>
[[nodiscard]] inline Http1ServerConnectionPlan completeSuccessfulHttpBodyRoute(
    ConnectionScanner::Entry& scannerEntry,
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    std::size_t& requestCount,
    std::size_t keepaliveRequests,
    Http1RequestBodyConsumption bodyConsumption,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted,
    RestorePipeline restorePipeline) {
    connectionPlan = finalizeBodyRouteResponse(
        response,
        connectionPlan,
        requestCount,
        keepaliveRequests,
        bodyConsumption);
    if (connectionPlan.disposition() == Http1ConnectionDisposition::kReuse) {
        restorePipeline(readBuffer, usedBytes);
        consumedBytes = 0;
        bufferAlreadyCompacted = true;
    }
    scannerEntry.touch();
    return connectionPlan;
}

}  // namespace ruvia::detail
