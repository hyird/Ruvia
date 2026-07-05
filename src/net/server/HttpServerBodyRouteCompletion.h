#pragma once

#include "../body/HttpRequestBody.h"
#include "ConnectionScanner.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
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
struct HttpLazyBufferedBodyRouteState final {
    std::optional<LazyBufferedBody<Stream>> body;
    std::optional<RequestBodyLoader> loader;

    void emplace(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource,
        std::string_view bodyAndPipeline,
        std::size_t contentLength,
        bool chunked,
        HttpTransferCodings transferCodings,
        std::size_t maxBodyBytes,
        ConnectionScanner::Entry& scannerEntry,
        bool sendContinue) {
        body.emplace(
            stream,
            workerAllocator,
            requestResource,
            bodyAndPipeline,
            contentLength,
            chunked,
            transferCodings,
            maxBodyBytes,
            scannerEntry,
            sendContinue);
        emplaceRequestBodyLoaderFacade(loader, *body);
    }

    [[nodiscard]] ContextServices withLoader(ContextServices services) noexcept {
        return services.withBodyLoader(*loader);
    }

    [[nodiscard]] bool consumed() const noexcept {
        return body->consumed();
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
    const HttpServerParseResult& parsed,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry) {
    state.emplace(
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
}

[[nodiscard]] inline std::string_view beginHttpBodyRoute(
    const HttpServerParseResult& parsed,
    const std::pmr::string& readBuffer,
    std::size_t usedBytes,
    bool& keepAlive,
    std::size_t& consumedBytes) noexcept {
    consumedBytes = parsed.headerBytes;
    keepAlive = shouldKeepAlive(parsed);
    return std::string_view(readBuffer.data() + parsed.headerBytes, usedBytes - parsed.headerBytes);
}

inline Task<void> completeFailedHttpBodyRoute(
    ConnectionScanner::Entry& scannerEntry,
    std::exception_ptr exception,
    const HttpServerParseResult& parsed,
    const RouteTable& routes,
    RequestMemory& requestMemory,
    ContextServices exceptionServices,
    HttpResponse& response,
    bool& keepAlive) {
    response = co_await routes.handleException(
        parsed.request,
        requestMemory,
        exception,
        true,
        exceptionServices);
    materializeResponseBody(response);
    keepAlive = false;
    scannerEntry.touch();
}

template <typename RestorePipeline>
inline void completeSuccessfulHttpBodyRoute(
    ConnectionScanner::Entry& scannerEntry,
    HttpResponse& response,
    bool& keepAlive,
    std::size_t& requestCount,
    std::size_t maxRequestsPerConnection,
    bool requestBodyComplete,
    bool needsKeepAliveSignal,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::size_t& consumedBytes,
    bool& bufferAlreadyCompacted,
    RestorePipeline restorePipeline) {
    finalizeBodyRouteResponse(
        response,
        keepAlive,
        requestCount,
        maxRequestsPerConnection,
        requestBodyComplete,
        needsKeepAliveSignal);
    if (keepAlive) {
        restorePipeline(readBuffer, usedBytes);
        consumedBytes = 0;
        bufferAlreadyCompacted = true;
    }
    scannerEntry.touch();
}

}  // namespace ruvia::detail
