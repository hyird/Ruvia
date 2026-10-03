#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpPriority.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/Http3EarlyDataInfo.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/integration/WorkerClientRegistryView.h"
#include "ruvia/web/detail/server/TrustedProxies.h"

namespace ruvia {

class HttpRequest;
class HttpRequestTrailers;
namespace detail {
class RequestDeadline;
}
class BlockingPool;
class Env;
}  // namespace ruvia

namespace ruvia::detail {

class RateLimiter;
class HttpInterimResponseOutput;
class HttpConnectionAdvertisementOutput;
class HttpPushOutput;
class RouteTable;
class WorkerStateRegistry;

class ContextServices final {
public:
    ContextServices() = delete;

    ContextServices(const WorkerHandle& worker, const StopToken& stopToken,
        WorkerClientRegistryView clientRegistries = WorkerClientRegistryView::detached(),
        RateLimiter* rateLimiter = nullptr,
        std::size_t maxDecodedBodyBytes = kDefaultMaxBufferedBodyBytes)
        : clientRegistries_(clientRegistries),
          rateLimiter_(rateLimiter),
          maxDecodedBodyBytes_(maxDecodedBodyBytes),
          worker_(requireWorker(worker)),
          stopToken_(stopToken),
          connInfo_(ConnInfo::plain({})) {}
    ContextServices(WorkerHandle&&, const StopToken&,
        WorkerClientRegistryView = WorkerClientRegistryView::detached(), RateLimiter* = nullptr,
        std::size_t = kDefaultMaxBufferedBodyBytes) = delete;
    ContextServices(const WorkerHandle&, StopToken&&,
        WorkerClientRegistryView = WorkerClientRegistryView::detached(), RateLimiter* = nullptr,
        std::size_t = kDefaultMaxBufferedBodyBytes) = delete;
    ContextServices(WorkerHandle&&, StopToken&&,
        WorkerClientRegistryView = WorkerClientRegistryView::detached(), RateLimiter* = nullptr,
        std::size_t = kDefaultMaxBufferedBodyBytes) = delete;

    [[nodiscard]] constexpr WorkerClientRegistryView clientRegistries() const noexcept {
        return clientRegistries_;
    }

    [[nodiscard]] RateLimiter* rateLimiter() const noexcept {
        return rateLimiter_;
    }

    [[nodiscard]] ContextServices withRateLimiter(RateLimiter& value) const noexcept {
        auto services = *this;
        services.rateLimiter_ = &value;
        return services;
    }

    [[nodiscard]] const Env* env() const noexcept {
        return env_;
    }

    [[nodiscard]] ContextServices withEnv(const Env& value) const noexcept {
        auto services = *this;
        services.env_ = &value;
        return services;
    }
    ContextServices withEnv(Env&&) const = delete;

    [[nodiscard]] std::pmr::memory_resource* inbound_buffer_pool() const noexcept {
        return inbound_buffer_pool_;
    }

    [[nodiscard]] ContextServices with_inbound_buffer_pool(std::pmr::memory_resource& resource) const noexcept {
        auto services = *this;
        services.inbound_buffer_pool_ = &resource;
        return services;
    }
    ContextServices with_inbound_buffer_pool(std::pmr::memory_resource&&) const = delete;

    [[nodiscard]] constexpr std::size_t maxDecodedBodyBytes() const noexcept {
        return maxDecodedBodyBytes_;
    }

    [[nodiscard]] ContextServices withMaxDecodedBodyBytes(std::size_t value) const noexcept {
        auto services = *this;
        services.maxDecodedBodyBytes_ = value;
        return services;
    }

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_.get();
    }

    [[nodiscard]] const StopToken& stopToken() const noexcept {
        return stopToken_.get();
    }

    [[nodiscard]] HttpErrorHandlerRef errorHandler() const noexcept {
        return errorHandler_;
    }

    [[nodiscard]] HttpNotFoundHandlerRef notFoundHandler() const noexcept {
        return notFoundHandler_;
    }

    [[nodiscard]] constexpr const ContextRequestBodySource& requestBodySource() const noexcept {
        return requestBodySource_;
    }
    [[nodiscard]] HttpInterimResponseOutput* interimOutput() const noexcept {
        return interimOutput_;
    }
    [[nodiscard]] ContextServices withInterimOutput(HttpInterimResponseOutput& output) const noexcept {
        auto services = *this;
        services.interimOutput_ = &output;
        return services;
    }
    ContextServices withInterimOutput(HttpInterimResponseOutput&&) const = delete;
    [[nodiscard]] HttpConnectionAdvertisementOutput* connectionAdvertisements() const noexcept {
        return connectionAdvertisements_;
    }
    [[nodiscard]] ContextServices withConnectionAdvertisements(HttpConnectionAdvertisementOutput& output) const noexcept {
        auto services = *this;
        services.connectionAdvertisements_ = &output;
        return services;
    }
    ContextServices withConnectionAdvertisements(HttpConnectionAdvertisementOutput&&) const = delete;

    [[nodiscard]] HttpPushOutput* pushOutput() const noexcept {
        return pushOutput_;
    }
    [[nodiscard]] ContextServices withPushOutput(HttpPushOutput& output) const noexcept {
        auto services = *this;
        services.pushOutput_ = &output;
        return services;
    }
    ContextServices withPushOutput(HttpPushOutput&&) const = delete;

    [[nodiscard]] const HttpRequestTrailers* requestTrailers() const noexcept {
        return requestTrailers_;
    }
    [[nodiscard]] ContextServices withRequestTrailers(const HttpRequestTrailers& trailers) const noexcept {
        auto result = *this;
        result.requestTrailers_ = &trailers;
        return result;
    }
    ContextServices withRequestTrailers(HttpRequestTrailers&&) const = delete;

    [[nodiscard]] const std::optional<HttpPriority>* requestPriorityUpdate() const noexcept {
        return requestPriorityUpdate_;
    }
    [[nodiscard]] ContextServices withRequestPriorityUpdate(const std::optional<HttpPriority>& priority) const noexcept {
        auto services = *this;
        services.requestPriorityUpdate_ = &priority;
        return services;
    }
    ContextServices withRequestPriorityUpdate(std::optional<HttpPriority>&&) const = delete;

    [[nodiscard]] constexpr const ContextResponseOutput& responseOutput() const noexcept {
        return responseOutput_;
    }

    [[nodiscard]] constexpr std::string_view automaticAltSvc() const noexcept {
        return automaticAltSvc_;
    }

    // The listener owns this value until every request Context has retired.
    // An application header with the same name replaces this default.
    [[nodiscard]] ContextServices withAutomaticAltSvc(std::string_view value) const noexcept {
        auto services = *this;
        services.automaticAltSvc_ = value;
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withAutomaticAltSvc(
        std::basic_string<char, Traits, Allocator>&&) const = delete;

    [[nodiscard]] constexpr const ConnInfo& connInfo() const noexcept {
        return connInfo_;
    }

    [[nodiscard]] constexpr http3_early_data_info early_data_info() const noexcept {
        return early_data_info_;
    }

    [[nodiscard]] constexpr ContextServices with_early_data_info(
        http3_early_data_info value) const noexcept {
        auto services = *this;
        services.early_data_info_ = value;
        return services;
    }

    [[nodiscard]] ContextServices withRequestDeadline(const RequestDeadline& value) const noexcept;
    ContextServices withRequestDeadline(RequestDeadline&&) const = delete;

    [[nodiscard]] const RequestDeadline* requestDeadline() const noexcept {
        return requestDeadline_;
    }

    [[nodiscard]] ContextServices withTrustedProxies(const TrustedProxySet& value) const noexcept {
        auto services = *this;
        services.trustedProxies_ = &value;
        return services;
    }
    ContextServices withTrustedProxies(TrustedProxySet&&) const = delete;

    [[nodiscard]] const TrustedProxySet* trustedProxies() const noexcept {
        return trustedProxies_;
    }

    // The connection metadata one request sees. Identical to connInfo() unless
    // the peer is a configured trusted proxy, in which case its forwarding
    // headers name the client. Resolved per request rather than per connection:
    // one HTTP/2 connection carries many requests, each with its own headers.
    [[nodiscard]] ConnInfo resolveConnInfo(const HttpRequest& request) const noexcept;

    [[nodiscard]] ContextServices withStreamingRequestBody(BodyReader& value) const noexcept {
        auto services = *this;
        services.requestBodySource_ = ContextRequestBodySource::streaming(value);
        return services;
    }

    [[nodiscard]] ContextServices withLazyRequestBody(RequestBodyLoader& value) const noexcept {
        auto services = *this;
        services.requestBodySource_ = ContextRequestBodySource::lazy(value);
        return services;
    }

    [[nodiscard]] ContextServices withResponseStream(ResponseStreamWriter& value) const noexcept {
        auto services = *this;
        services.responseOutput_ = ContextResponseOutput::responseStream(value);
        return services;
    }

    [[nodiscard]] ContextServices withErrorHandler(HttpErrorHandlerRef value) const noexcept {
        auto services = *this;
        services.errorHandler_ = value;
        return services;
    }

    [[nodiscard]] ContextServices withNotFoundHandler(HttpNotFoundHandlerRef value) const noexcept {
        auto services = *this;
        services.notFoundHandler_ = value;
        return services;
    }

    [[nodiscard]] const RouteTable* routes() const noexcept {
        return routes_;
    }

    [[nodiscard]] std::size_t dispatchDepth() const noexcept {
        return dispatchDepth_;
    }
    [[nodiscard]] ContextServices withSubrequest(const ConnInfo& connection, std::size_t depth) const noexcept {
        auto services = *this;
        services.connInfo_ = connection;
        services.dispatchDepth_ = depth;
        return services;
    }

    // The route table is server-owned and outlives every dispatched request.
    [[nodiscard]] ContextServices withRoutes(const RouteTable& value) const noexcept {
        auto services = *this;
        services.routes_ = &value;
        return services;
    }
    ContextServices withRoutes(RouteTable&&) const = delete;

    [[nodiscard]] BlockingPool* blockingPool() const noexcept {
        return blockingPool_;
    }

    // Server-owned static-file policy. When response compression is enabled,
    // staticFile() may negotiate an indexed precompressed variant. It never
    // performs request-time file compression.
    [[nodiscard]] constexpr bool precompressedStaticFiles() const noexcept {
        return precompressedStaticFiles_;
    }

    [[nodiscard]] ContextServices withPrecompressedStaticFiles(bool enabled = true) const noexcept {
        auto services = *this;
        services.precompressedStaticFiles_ = enabled;
        return services;
    }

    // Process-wide and owned by App::run(), so it outlives every worker that
    // borrows it here.
    [[nodiscard]] ContextServices withBlockingPool(BlockingPool& value) const noexcept {
        auto services = *this;
        services.blockingPool_ = &value;
        return services;
    }

    [[nodiscard]] const WorkerStateRegistry* workerStates() const noexcept {
        return workerStates_;
    }

    // The registry is worker-owned and outlives every dispatched request.
    [[nodiscard]] ContextServices withWorkerStates(
        const WorkerStateRegistry& value) const noexcept {
        auto services = *this;
        services.workerStates_ = &value;
        return services;
    }
    ContextServices withWorkerStates(WorkerStateRegistry&&) const = delete;

    // Views borrow connection-owned storage and remain valid for every Context
    // created while that connection is dispatched.
    [[nodiscard]] ContextServices withPlainTransport(
        std::string_view remoteAddress, std::uint16_t remotePort = 0) const noexcept {
        auto services = *this;
        services.connInfo_ = ConnInfo::plain(remoteAddress);
        services.connInfo_.setRemotePort(remotePort);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withPlainTransport(
        std::basic_string<char, Traits, Allocator>&&, std::uint16_t = 0) const = delete;

    [[nodiscard]] ContextServices withTlsTransport(std::string_view remoteAddress,
        std::string_view clientCertificateSubject = {},
        std::uint16_t remotePort = 0) const noexcept {
        auto services = *this;
        services.connInfo_ = ConnInfo::tls(remoteAddress, clientCertificateSubject);
        services.connInfo_.setRemotePort(remotePort);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(std::basic_string<char, Traits, Allocator>&&,
        std::string_view = {}, std::uint16_t = 0) const = delete;

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(std::string_view,
        std::basic_string<char, Traits, Allocator>&&, std::uint16_t = 0) const = delete;

private:
    std::size_t dispatchDepth_{0};
    [[nodiscard]] static const WorkerHandle& requireWorker(const WorkerHandle& worker) {
        if (!worker.valid()) {
            throw std::invalid_argument("context services require a valid worker");
        }
        return worker;
    }

    WorkerClientRegistryView clientRegistries_{WorkerClientRegistryView::detached()};
    RateLimiter* rateLimiter_{nullptr};
    const Env* env_{nullptr};
    std::size_t maxDecodedBodyBytes_{kDefaultMaxBufferedBodyBytes};
    std::pmr::memory_resource* inbound_buffer_pool_{};
    // Request/session services borrow the address-stable server-owned worker
    // handle and its stop token. Every derived value stays in that dispatch.
    std::reference_wrapper<const WorkerHandle> worker_;
    std::reference_wrapper<const StopToken> stopToken_;
    HttpErrorHandlerRef errorHandler_{nullptr};
    HttpNotFoundHandlerRef notFoundHandler_{nullptr};
    const RouteTable* routes_{nullptr};
    const WorkerStateRegistry* workerStates_{nullptr};
    BlockingPool* blockingPool_{nullptr};
    bool precompressedStaticFiles_{false};

    ContextRequestBodySource requestBodySource_;
    const HttpRequestTrailers* requestTrailers_{};
    const std::optional<HttpPriority>* requestPriorityUpdate_{};
    HttpInterimResponseOutput* interimOutput_{};
    HttpConnectionAdvertisementOutput* connectionAdvertisements_{};
    HttpPushOutput* pushOutput_{};
    ContextResponseOutput responseOutput_;
    std::string_view automaticAltSvc_;
    ConnInfo connInfo_;
    http3_early_data_info early_data_info_{};
    const TrustedProxySet* trustedProxies_{nullptr};
    const RequestDeadline* requestDeadline_{nullptr};
};

}  // namespace ruvia::detail
