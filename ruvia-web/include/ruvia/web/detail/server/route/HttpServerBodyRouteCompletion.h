#pragma once

#include "ruvia/web/detail/body/HttpLazyBufferedBody.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/server/http1/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/http/request/RequestBodyLoader.h"
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
    std::optional<RequestBodyLoaderBinding<LazyBufferedBody<Stream>>> body;

    void emplace(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource,
        std::string_view bodyAndPipeline,
        Http1RequestBodyPlan bodyPlan,
        ProtocolByteLimit bodyLimit,
        ConnectionScanner::Entry& scannerEntry) {
        body.emplace(
            stream,
            workerAllocator,
            requestResource,
            bodyAndPipeline,
            bodyPlan,
            bodyLimit,
            scannerEntry);
    }

    [[nodiscard]] ContextServices withLoader(ContextServices services) noexcept {
        return services.withLazyRequestBody(body->facade());
    }

    [[nodiscard]] Http1RequestBodyConsumption consumption() const noexcept {
        return body->loader().consumption();
    }

    void takePipeline(std::pmr::string& stash) {
        body->loader().takePipeline(stash);
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
        ProtocolByteLimit::limited(options.maxBufferedBodyBytes),
        scannerEntry);
}

[[nodiscard]] inline std::string_view httpBodyAndPipeline(
    const Http1ServerRequestHeadReady& requestHead,
    const std::pmr::string& readBuffer,
    std::size_t usedBytes) noexcept {
    const auto headerBytes = requestHead.headerBytes();
    return std::string_view(
        readBuffer.data() + headerBytes,
        usedBytes - headerBytes);
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
    const auto connectionPlan = requireHttp1FinalResponseCommit(
        response, parsed.connectionPlan.requireClose());
    co_return Http1SessionRequestCompletion::makeBufferedClosing(
        connectionPlan);
}

// `pipelineStash` must be request-scoped storage that outlives the returned
// completion: it carries the next pipelined request until the session installs
// it. The read buffer is not touched here -- the response has not been written
// and the access log not recorded yet, and both still read views that borrow it.
template <typename TakePipeline>
[[nodiscard]] inline Http1SessionRequestCompletion
completeSuccessfulHttpBodyRoute(
    ConnectionScanner::Entry& scannerEntry,
    HttpResponse& response,
    Http1ServerConnectionPlan connectionPlan,
    Http1RequestSequence& requestSequence,
    Http1RequestBodyConsumption bodyConsumption,
    std::pmr::string& pipelineStash,
    TakePipeline takePipeline) {
    connectionPlan = finalizeBodyRouteResponse(
        response,
        connectionPlan,
        requestSequence,
        bodyConsumption);
    if (connectionPlan.disposition() == Http1ConnectionDisposition::kReuse) {
        takePipeline(pipelineStash);
        scannerEntry.touch();
        return Http1SessionRequestCompletion::makeBufferedPipelineRestore(
            connectionPlan,
            std::string_view(pipelineStash));
    }
    scannerEntry.touch();
    return Http1SessionRequestCompletion::makeBufferedClosing(
        connectionPlan);
}

}  // namespace ruvia::detail
