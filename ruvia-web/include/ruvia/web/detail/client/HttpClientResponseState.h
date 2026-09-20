#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia {
class ResponseStreamWriter;
}

namespace ruvia::detail {

class HttpClientPool;

// Address-stable response storage. Network stream registration and connection
// leases are added here rather than to the movable public response wrapper.
class HttpClientResponseState final {
public:
    HttpClientResponseState(const WorkerHandle& worker, std::pmr::memory_resource* resource)
        : headSignal(worker),
          dataSignal(worker),
          spaceSignal(worker),
          resource(resource),
          headers(resource),
          trailers(resource),
          buffered(resource),
          pending(resource) {}
    HttpClientResponseState(WorkerHandle&&, std::pmr::memory_resource*) = delete;

    // Body algorithms run on the address-stable storage owner. The public
    // facade wraps them in bodyOperationScope to enforce the linear lane.
    template <typename View>
    [[nodiscard]] Task<std::optional<View>> read();
    [[nodiscard]] Task<std::pmr::vector<std::byte>> readAll(std::size_t maxBytes);
    [[nodiscard]] Task<void> pipeTo(ResponseStreamWriter& output);

    WorkerSignal headSignal;
    WorkerSignal dataSignal;
    WorkerSignal spaceSignal;
    HttpClientPool* pool{nullptr};
    std::pmr::memory_resource* resource;
    HttpStatusCode status{http_status::kOk};
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    std::pmr::vector<HttpHeader> headers;
    std::pmr::vector<HttpHeader> trailers;
    std::pmr::string buffered;
    // Producers only append here. The consumer swaps it with `buffered` before
    // returning a view, so later network progress cannot invalidate that view.
    std::pmr::string pending;
    std::size_t offset{0};
    std::size_t bufferedLimit{kDefaultMaxBufferedBodyBytes};
    std::exception_ptr failure;
    std::optional<std::uint8_t> errorCode;
    std::size_t references{1};
    bool headReady{false};
    bool complete{false};
    bool abandoned{false};
    bool incrementalRead{false};
    bool collectAll{false};
    bool http2{false};
    std::optional<::ruvia::Http2ReceivedDataCredit> http2DataCredit{};
    std::size_t connectionIndex{0};
    std::uint64_t requestId{0};
    std::uint64_t cancellationId{0};
    std::uint32_t streamId{0};
    // Declared last so the operation scope closes while every field borrowed
    // by a body coroutine is alive.
    ScopedOperationScope bodyOperationScope;

private:
    void promotePendingData();
};

}  // namespace ruvia::detail
