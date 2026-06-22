#pragma once

#include "ConnectionScanner.h"
#include "HttpServerRequestState.h"
#include "HttpServerResponseState.h"
#include "../../http/HttpParserInternal.h"
#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

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
    RouteServices exceptionServices,
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
        requestBodyComplete);
    if (keepAlive) {
        restorePipeline(readBuffer, usedBytes);
        consumedBytes = 0;
        bufferAlreadyCompacted = true;
    }
    scannerEntry.touch();
}

}  // namespace ruvia::detail
