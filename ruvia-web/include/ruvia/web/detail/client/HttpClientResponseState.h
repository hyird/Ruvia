#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/HttpClientResponseBytes.h"
#include "ruvia/web/detail/http3/Http3ClientBodyBudget.h"

namespace ruvia {
class ResponseStreamWriter;
}

namespace ruvia::detail {

class HttpClientPool;
class HttpClientResultBudgetDomain;
class Http3ClientConnection;

enum class HttpClientResponseTransport : std::uint8_t {
    kUnassigned,
    kHttp1,
    kHttp2,
    kHttp3,
};

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
    [[nodiscard]] Task<HttpClientResponseBytes> readAll(std::size_t maxBytes);
    [[nodiscard]] Task<void> pipeTo(ResponseStreamWriter& output);
    void retainReference() noexcept;
    void releaseReference() noexcept;
    void notifyProducerSpace() noexcept;
    [[nodiscard]] bool bindHttp3BodyBudget(Http3ClientBodyBudget& budget) noexcept;
    void releaseHttp3BodyBudget() noexcept;
    [[nodiscard]] bool hasHttp3BodyBudget() const noexcept;
    [[nodiscard]] std::size_t producerBodyBytes() const noexcept;
    [[nodiscard]] std::size_t producerBodyBudgetAvailable() const noexcept;
    [[nodiscard]] bool retainProducerBodyBytes(std::size_t bytes) noexcept;
    void releaseProducerBodyBytes(std::size_t bytes) noexcept;
    void reconcileProducerBodyBytes() noexcept;
    [[nodiscard]] bool replaceProducerBodyBytes(std::size_t bytes) noexcept;
    void discardPendingBody() noexcept;
    void discardResponseBody() noexcept;
    void releaseConsumedBodyPrefix();

    WorkerSignal headSignal;
    WorkerSignal dataSignal;
    WorkerSignal spaceSignal;
    HttpClientPool* pool{nullptr};
    // Borrows the stable shared_ptr member owned by the client pool. It is only
    // copied by readAll(), after its single-result byte limit has passed.
    const std::shared_ptr<HttpClientResultBudgetDomain>* resultBudgetDomain{nullptr};
    std::pmr::memory_resource* resource;
    HttpStatusCode status{http_status::kOk};
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    HttpKnownMethod requestMethod{HttpKnownMethod::kUnknown};
    std::optional<HttpResponseBodyPlan> responseBodyPlan{};
    std::pmr::vector<HttpHeader> headers;
    std::pmr::vector<HttpHeader> trailers;
    // Declared before body strings so their storage is destroyed before the
    // receive reservation is returned to its worker-stable owner.
    Http3ClientBodyBudget::Lease http3BodyBudget;
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
    bool bodyDecodeRequired{false};
    // Any informational/final response event proves the peer processed some
    // part of the request; GOAWAY/REQUEST_REJECTED must not replay it.
    bool http3ResponseStarted{false};
    HttpClientResponseTransport transport{HttpClientResponseTransport::kUnassigned};
    Http3ClientConnection* http3Connection{nullptr};
    std::uint64_t http3RequestId{0};
    std::optional<::ruvia::Http2ReceivedDataCredit> http2DataCredit{};
    std::size_t connectionIndex{0};
    std::uint64_t requestId{0};
    std::uint64_t cancellationId{0};
    std::uint64_t streamId{0};
    // Declared last so the operation scope closes while every field borrowed
    // by a body coroutine is alive.
    ScopedOperationScope bodyOperationScope;

private:
    void promotePendingData();
};

}  // namespace ruvia::detail
