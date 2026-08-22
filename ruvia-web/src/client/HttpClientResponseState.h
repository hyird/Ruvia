#pragma once

#include <memory_resource>
#include <exception>
#include <optional>
#include <vector>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/HttpProtocolVersion.h"

namespace ruvia::detail {

class HttpClientPool;

// Address-stable response storage. Network stream registration and connection
// leases are added here rather than to the movable public response wrapper.
class HttpClientResponseState final {
public:
    HttpClientResponseState(const WorkerHandle& worker, std::pmr::memory_resource* resource)
        : headSignal(worker), dataSignal(worker), spaceSignal(worker), resource(resource),
          headers(resource), trailers(resource), buffered(resource), pending(resource) {}

    WorkerSignal headSignal;
    WorkerSignal dataSignal;
    WorkerSignal spaceSignal;
    HttpClientPool* pool{nullptr};
    std::pmr::memory_resource* resource;
    HttpStatusCode status{http_status::kOk};
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    std::pmr::vector<HttpClientResponseHeader> headers;
    std::pmr::vector<HttpClientResponseHeader> trailers;
    std::pmr::string buffered;
    // Producers only append here. The consumer swaps it with `buffered` before
    // returning a view, so later network progress cannot invalidate that view.
    std::pmr::string pending;
    std::size_t offset{0};
    std::size_t bufferedLimit{16 * 1024 * 1024};
    std::exception_ptr failure;
    std::optional<std::uint8_t> errorCode;
    std::size_t references{1};
    bool headReady{false};
    bool complete{false};
    bool abandoned{false};
    bool incrementalRead{false};
    bool collectAll{false};
    bool http2{false};
    bool http2DataPending{false};
    std::size_t connectionIndex{0};
    std::uint64_t requestId{0};
    std::uint64_t cancellationId{0};
    std::uint32_t streamId{0};
};

}  // namespace ruvia::detail
