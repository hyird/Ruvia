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
#include <type_traits>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpPush.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/HttpClientInformationalResponse.h"
#include "ruvia/web/HttpClientResponseBytes.h"
#include "ruvia/web/detail/client/HttpClientTunnelState.h"
#include "ruvia/web/detail/client/HttpClientUploadState.h"
#include "ruvia/web/detail/http3/Http3ClientBodyBudget.h"

namespace ruvia {
class ResponseStreamWriter;
}

namespace ruvia::detail {

class HttpClientPool;
class HttpClientResponseMemoryDomain;
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
          informational(resource),
          headers(resource),
          trailers(resource),
          buffered(resource),
          pending(resource) {}
    explicit HttpClientResponseState(HttpClientResponseMemoryDomain& memoryDomain);
    HttpClientResponseState(WorkerHandle&&, std::pmr::memory_resource*) = delete;

    // Body algorithms run on the address-stable storage owner. The public
    // facade wraps them in bodyOperationScope to enforce the linear lane.
    template <typename view>
    [[nodiscard]] Task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> consume_body(ResponseStreamWriter* output = nullptr);
    [[nodiscard]] Task<HttpClientResponseBytes> readAll(std::size_t maxBytes);
    void retainReference() noexcept;
    void releaseReference() noexcept;
    [[nodiscard]] HttpClientResponseMemoryDomain* memoryDomain() const noexcept {
        return memoryDomain_;
    }
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

    void retainInformational(HttpStatusCode status, std::span<const HttpHeaderView> fields);

    [[nodiscard]] http_client_output_queue* output() noexcept {
        return tunnel ? &tunnel->output : upload ? &upload->output
                                                 : nullptr;
    }
    [[nodiscard]] bool receiveComplete() const noexcept {
        return complete || (tunnel && tunnel->accepted && tunnel->receiveEnded);
    }
    std::optional<HttpClientTunnelState> tunnel;
    std::optional<HttpClientUploadState> upload;
    WorkerSignal headSignal;
    WorkerSignal dataSignal;
    WorkerSignal spaceSignal;
    HttpClientPool* pool{nullptr};
    // Borrows the stable shared_ptr member owned by the response memory domain.
    // Only readAll() copies it after its single-result byte limit has passed.
    const std::shared_ptr<HttpClientResultBudgetDomain>* resultBudgetDomain{nullptr};
    std::pmr::memory_resource* resource;
    HttpStatusCode status{http_status::kOk};
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    HttpKnownMethod requestMethod{HttpKnownMethod::kUnknown};
    std::optional<HttpResponseBodyPlan> responseBodyPlan{};
    std::pmr::vector<HttpClientInformationalResponse> informational;
    std::size_t informationalFieldBytes{};
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
    ::ruvia::operation_scope bodyOperationScope;
    std::optional<HttpPushRequest> promisedRequest{};
    bool pushResponseTaken{false};
    ::ruvia::operation_scope pushResponseScope;

private:
    void promotePendingData();
    void detachTransportBindings() noexcept;

    HttpClientResponseMemoryDomain* memoryDomain_{};
    HttpClientResponseState* previousMemoryState_{};
    HttpClientResponseState* nextMemoryState_{};

    friend class HttpClientResponseMemoryDomain;
};

}  // namespace ruvia::detail
