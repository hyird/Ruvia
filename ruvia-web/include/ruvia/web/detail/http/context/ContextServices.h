#pragma once

#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/detail/server/TrustedProxies.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/integration/WorkerClientRegistryView.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/StopToken.h"

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {

class HttpRequest;
namespace detail {
class RequestDeadline;
}
class BlockingPool;
class Env;
}  // namespace ruvia

namespace ruvia::detail {

class RateLimiter;
class RouteTable;
class WorkerStateRegistry;

class ContextServices final {
public:
    ContextServices() = delete;

    ContextServices(const WorkerHandle& worker, const StopToken& stopToken, WorkerClientRegistryView clientRegistries = WorkerClientRegistryView::detached(), RateLimiter* rateLimiter = nullptr, std::size_t maxDecodedBodyBytes = kDefaultMaxBufferedBodyBytes)
        : clientRegistries_(clientRegistries),
          rateLimiter_(rateLimiter),
          maxDecodedBodyBytes_(maxDecodedBodyBytes),
          worker_(requireWorker(worker)),
          stopToken_(stopToken),
          connInfo_(ConnInfo::plain({})) {}
    ContextServices(WorkerHandle&&, const StopToken&, WorkerClientRegistryView = WorkerClientRegistryView::detached(), RateLimiter* = nullptr, std::size_t = kDefaultMaxBufferedBodyBytes) = delete;
    ContextServices(const WorkerHandle&, StopToken&&, WorkerClientRegistryView = WorkerClientRegistryView::detached(), RateLimiter* = nullptr, std::size_t = kDefaultMaxBufferedBodyBytes) = delete;
    ContextServices(WorkerHandle&&, StopToken&&, WorkerClientRegistryView = WorkerClientRegistryView::detached(), RateLimiter* = nullptr, std::size_t = kDefaultMaxBufferedBodyBytes) = delete;

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

    [[nodiscard]] constexpr const ContextResponseOutput& responseOutput() const noexcept {
        return responseOutput_;
    }

    [[nodiscard]] constexpr const ConnInfo& connInfo() const noexcept {
        return connInfo_;
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
    [[nodiscard]] ContextServices withWorkerStates(const WorkerStateRegistry& value) const noexcept {
        auto services = *this;
        services.workerStates_ = &value;
        return services;
    }
    ContextServices withWorkerStates(WorkerStateRegistry&&) const = delete;

    // Views borrow connection-owned storage and remain valid for every Context
    // created while that connection is dispatched.
    [[nodiscard]] ContextServices withPlainTransport(std::string_view remoteAddress) const noexcept {
        auto services = *this;
        services.connInfo_ = ConnInfo::plain(remoteAddress);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withPlainTransport(std::basic_string<char, Traits, Allocator>&&) const = delete;

    [[nodiscard]] ContextServices withTlsTransport(std::string_view remoteAddress, std::string_view clientCertificateSubject = {}) const noexcept {
        auto services = *this;
        services.connInfo_ = ConnInfo::tls(remoteAddress, clientCertificateSubject);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(std::basic_string<char, Traits, Allocator>&&, std::string_view = {}) const = delete;

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(std::string_view, std::basic_string<char, Traits, Allocator>&&) const = delete;

private:
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
    ContextResponseOutput responseOutput_;
    ConnInfo connInfo_;
    const TrustedProxySet* trustedProxies_{nullptr};
    const RequestDeadline* requestDeadline_{nullptr};
};

}  // namespace ruvia::detail
