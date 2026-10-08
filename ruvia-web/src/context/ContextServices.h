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

#include "context/ContextCapabilities.h"
#include "integration/WorkerClientRegistryView.h"
#include "server/TrustedProxies.h"

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

struct SteadyRateLimiterClock;
template <typename Clock>
class rate_limiter;
using RateLimiter = rate_limiter<SteadyRateLimiterClock>;
class HttpInterimResponseOutput;
class HttpConnectionAdvertisementOutput;
class HttpPushOutput;
class RouteTable;
class WorkerStateRegistry;

struct context_worker_services final {
    std::reference_wrapper<const WorkerHandle> worker;
    WorkerClientRegistryView clients{WorkerClientRegistryView::detached()};
    RateLimiter* rate_limiter{};
    const Env* env{};
    std::size_t max_decoded_body_bytes{kDefaultMaxBufferedBodyBytes};
    std::pmr::memory_resource* inbound_buffer_pool{};
    HttpErrorHandlerRef error_handler{nullptr};
    HttpNotFoundHandlerRef not_found_handler{nullptr};
    const RouteTable* routes{};
    const WorkerStateRegistry* states{};
    BlockingPool* blocking_pool{};
    bool precompressed_static_files{};
    const TrustedProxySet* trusted_proxies{};
};

struct context_connection_services final {
    std::string_view automatic_alt_svc{};
    ConnInfo info;
    http3_early_data_info early_data{};
};

struct context_request_services final {
    std::reference_wrapper<const StopToken> stop_token;
    ContextRequestBodySource body_source{};
    const HttpRequestTrailers* trailers{};
    const std::optional<HttpPriority>* priority_update{};
    HttpInterimResponseOutput* interim_output{};
    HttpConnectionAdvertisementOutput* connection_advertisements{};
    HttpPushOutput* push_output{};
    ContextResponseOutput response_output{};
    const RequestDeadline* deadline{};
    std::size_t dispatch_depth{};
};

class ContextServices final {
public:
    ContextServices() = delete;

    ContextServices(context_worker_services worker_services, const StopToken& stopToken)
        : worker_services_(worker_services),
          request_services_{.stop_token = stopToken} {
        (void)requireWorker(worker_services_.worker);
    }
    ContextServices(context_worker_services, StopToken&&) = delete;

    ContextServices(const WorkerHandle& worker, const StopToken& stopToken,
        WorkerClientRegistryView clientRegistries = WorkerClientRegistryView::detached(),
        RateLimiter* rateLimiter = nullptr,
        std::size_t maxDecodedBodyBytes = kDefaultMaxBufferedBodyBytes)
        : ContextServices(context_worker_services{
                              .worker = requireWorker(worker),
                              .clients = clientRegistries,
                              .rate_limiter = rateLimiter,
                              .max_decoded_body_bytes = maxDecodedBodyBytes,
                          },
              stopToken) {}
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
        return worker_services_.clients;
    }

    [[nodiscard]] RateLimiter* rateLimiter() const noexcept {
        return worker_services_.rate_limiter;
    }

    [[nodiscard]] ContextServices withRateLimiter(RateLimiter& value) const noexcept {
        auto services = *this;
        services.worker_services_.rate_limiter = &value;
        return services;
    }

    [[nodiscard]] const Env* env() const noexcept {
        return worker_services_.env;
    }

    [[nodiscard]] ContextServices withEnv(const Env& value) const noexcept {
        auto services = *this;
        services.worker_services_.env = &value;
        return services;
    }
    ContextServices withEnv(Env&&) const = delete;

    [[nodiscard]] std::pmr::memory_resource* inbound_buffer_pool() const noexcept {
        return worker_services_.inbound_buffer_pool;
    }

    [[nodiscard]] ContextServices with_inbound_buffer_pool(std::pmr::memory_resource& resource) const noexcept {
        auto services = *this;
        services.worker_services_.inbound_buffer_pool = &resource;
        return services;
    }
    ContextServices with_inbound_buffer_pool(std::pmr::memory_resource&&) const = delete;

    [[nodiscard]] constexpr std::size_t maxDecodedBodyBytes() const noexcept {
        return worker_services_.max_decoded_body_bytes;
    }

    [[nodiscard]] ContextServices withMaxDecodedBodyBytes(std::size_t value) const noexcept {
        auto services = *this;
        services.worker_services_.max_decoded_body_bytes = value;
        return services;
    }

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_services_.worker.get();
    }

    [[nodiscard]] const StopToken& stopToken() const noexcept {
        return request_services_.stop_token.get();
    }

    [[nodiscard]] HttpErrorHandlerRef errorHandler() const noexcept {
        return worker_services_.error_handler;
    }

    [[nodiscard]] HttpNotFoundHandlerRef notFoundHandler() const noexcept {
        return worker_services_.not_found_handler;
    }

    [[nodiscard]] constexpr ContextRequestBodySource& requestBodySource() noexcept {
        return request_services_.body_source;
    }

    [[nodiscard]] constexpr const ContextRequestBodySource& requestBodySource() const noexcept {
        return request_services_.body_source;
    }
    [[nodiscard]] HttpInterimResponseOutput* interimOutput() const noexcept {
        return request_services_.interim_output;
    }
    [[nodiscard]] ContextServices withInterimOutput(HttpInterimResponseOutput& output) const noexcept {
        auto services = *this;
        services.request_services_.interim_output = &output;
        return services;
    }
    ContextServices withInterimOutput(HttpInterimResponseOutput&&) const = delete;
    [[nodiscard]] HttpConnectionAdvertisementOutput* connectionAdvertisements() const noexcept {
        return request_services_.connection_advertisements;
    }
    [[nodiscard]] ContextServices withConnectionAdvertisements(HttpConnectionAdvertisementOutput& output) const noexcept {
        auto services = *this;
        services.request_services_.connection_advertisements = &output;
        return services;
    }
    ContextServices withConnectionAdvertisements(HttpConnectionAdvertisementOutput&&) const = delete;

    [[nodiscard]] HttpPushOutput* pushOutput() const noexcept {
        return request_services_.push_output;
    }
    [[nodiscard]] ContextServices withPushOutput(HttpPushOutput& output) const noexcept {
        auto services = *this;
        services.request_services_.push_output = &output;
        return services;
    }
    ContextServices withPushOutput(HttpPushOutput&&) const = delete;

    [[nodiscard]] const HttpRequestTrailers* requestTrailers() const noexcept {
        return request_services_.trailers;
    }
    [[nodiscard]] ContextServices withRequestTrailers(const HttpRequestTrailers& trailers) const noexcept {
        auto result = *this;
        result.request_services_.trailers = &trailers;
        return result;
    }
    ContextServices withRequestTrailers(HttpRequestTrailers&&) const = delete;

    [[nodiscard]] const std::optional<HttpPriority>* requestPriorityUpdate() const noexcept {
        return request_services_.priority_update;
    }
    [[nodiscard]] ContextServices withRequestPriorityUpdate(const std::optional<HttpPriority>& priority) const noexcept {
        auto services = *this;
        services.request_services_.priority_update = &priority;
        return services;
    }
    ContextServices withRequestPriorityUpdate(std::optional<HttpPriority>&&) const = delete;

    [[nodiscard]] constexpr ContextResponseOutput& responseOutput() noexcept {
        return request_services_.response_output;
    }

    [[nodiscard]] constexpr const ContextResponseOutput& responseOutput() const noexcept {
        return request_services_.response_output;
    }

    [[nodiscard]] constexpr std::string_view automaticAltSvc() const noexcept {
        return connection_services_.automatic_alt_svc;
    }

    // The listener owns this value until every request Context has retired.
    // An application header with the same name replaces this default.
    [[nodiscard]] ContextServices withAutomaticAltSvc(std::string_view value) const noexcept {
        auto services = *this;
        services.connection_services_.automatic_alt_svc = value;
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withAutomaticAltSvc(
        std::basic_string<char, Traits, Allocator>&&) const = delete;

    [[nodiscard]] constexpr const ConnInfo& connInfo() const noexcept {
        return connection_services_.info;
    }

    [[nodiscard]] constexpr http3_early_data_info early_data_info() const noexcept {
        return connection_services_.early_data;
    }

    [[nodiscard]] constexpr ContextServices with_early_data_info(
        http3_early_data_info value) const noexcept {
        auto services = *this;
        services.connection_services_.early_data = value;
        return services;
    }

    [[nodiscard]] ContextServices withRequestDeadline(const RequestDeadline& value) const noexcept;
    ContextServices withRequestDeadline(RequestDeadline&&) const = delete;

    [[nodiscard]] const RequestDeadline* requestDeadline() const noexcept {
        return request_services_.deadline;
    }

    [[nodiscard]] ContextServices withTrustedProxies(const TrustedProxySet& value) const noexcept {
        auto services = *this;
        services.worker_services_.trusted_proxies = &value;
        return services;
    }
    ContextServices withTrustedProxies(TrustedProxySet&&) const = delete;

    [[nodiscard]] const TrustedProxySet* trustedProxies() const noexcept {
        return worker_services_.trusted_proxies;
    }

    // The connection metadata one request sees. Identical to connInfo() unless
    // the peer is a configured trusted proxy, in which case its forwarding
    // headers name the client. Resolved per request rather than per connection:
    // one HTTP/2 connection carries many requests, each with its own headers.
    [[nodiscard]] ConnInfo resolveConnInfo(const HttpRequest& request) const noexcept;

    [[nodiscard]] ContextServices withStreamingRequestBody(BodyReader& value) const noexcept {
        auto services = *this;
        services.request_services_.body_source = ContextRequestBodySource::streaming(value);
        return services;
    }

    [[nodiscard]] ContextServices withLazyRequestBody(RequestBodyLoader& value) const noexcept {
        auto services = *this;
        services.request_services_.body_source = ContextRequestBodySource::lazy(value);
        return services;
    }

    [[nodiscard]] ContextServices withResponseStream(ResponseStreamWriter& value) const noexcept {
        auto services = *this;
        services.request_services_.response_output = ContextResponseOutput::responseStream(value);
        return services;
    }

    [[nodiscard]] ContextServices withErrorHandler(HttpErrorHandlerRef value) const noexcept {
        auto services = *this;
        services.worker_services_.error_handler = value;
        return services;
    }

    [[nodiscard]] ContextServices withNotFoundHandler(HttpNotFoundHandlerRef value) const noexcept {
        auto services = *this;
        services.worker_services_.not_found_handler = value;
        return services;
    }

    [[nodiscard]] const RouteTable* routes() const noexcept {
        return worker_services_.routes;
    }

    [[nodiscard]] std::size_t dispatchDepth() const noexcept {
        return request_services_.dispatch_depth;
    }
    [[nodiscard]] ContextServices for_subrequest(const ConnInfo& connection, std::size_t depth,
        const StopToken& stopToken) const {
        ContextServices services(worker_services_, stopToken);
        services.connection_services_.info = connection;
        services.request_services_.dispatch_depth = depth;
        return services;
    }

    // The route table is server-owned and outlives every dispatched request.
    [[nodiscard]] ContextServices withRoutes(const RouteTable& value) const noexcept {
        auto services = *this;
        services.worker_services_.routes = &value;
        return services;
    }
    ContextServices withRoutes(RouteTable&&) const = delete;

    [[nodiscard]] BlockingPool* blockingPool() const noexcept {
        return worker_services_.blocking_pool;
    }

    // Server-owned static-file policy. When response compression is enabled,
    // staticFile() may negotiate an indexed precompressed variant. It never
    // performs request-time file compression.
    [[nodiscard]] constexpr bool precompressedStaticFiles() const noexcept {
        return worker_services_.precompressed_static_files;
    }

    [[nodiscard]] ContextServices withPrecompressedStaticFiles(bool enabled = true) const noexcept {
        auto services = *this;
        services.worker_services_.precompressed_static_files = enabled;
        return services;
    }

    // Process-wide and owned by App::run(), so it outlives every worker that
    // borrows it here.
    [[nodiscard]] ContextServices withBlockingPool(BlockingPool& value) const noexcept {
        auto services = *this;
        services.worker_services_.blocking_pool = &value;
        return services;
    }

    [[nodiscard]] const WorkerStateRegistry* workerStates() const noexcept {
        return worker_services_.states;
    }

    // The registry is worker-owned and outlives every dispatched request.
    [[nodiscard]] ContextServices withWorkerStates(
        const WorkerStateRegistry& value) const noexcept {
        auto services = *this;
        services.worker_services_.states = &value;
        return services;
    }
    ContextServices withWorkerStates(WorkerStateRegistry&&) const = delete;

    // Views borrow connection-owned storage and remain valid for every Context
    // created while that connection is dispatched.
    [[nodiscard]] ContextServices withPlainTransport(
        std::string_view remoteAddress, std::uint16_t remotePort = 0) const noexcept {
        auto services = *this;
        services.connection_services_.info = ConnInfo::plain(remoteAddress);
        services.connection_services_.info.setRemotePort(remotePort);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withPlainTransport(
        std::basic_string<char, Traits, Allocator>&&, std::uint16_t = 0) const = delete;

    [[nodiscard]] ContextServices withTlsTransport(std::string_view remoteAddress,
        std::string_view clientCertificateSubject = {},
        std::uint16_t remotePort = 0) const noexcept {
        auto services = *this;
        services.connection_services_.info = ConnInfo::tls(remoteAddress, clientCertificateSubject);
        services.connection_services_.info.setRemotePort(remotePort);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(std::basic_string<char, Traits, Allocator>&&,
        std::string_view = {}, std::uint16_t = 0) const = delete;

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(std::string_view,
        std::basic_string<char, Traits, Allocator>&&, std::uint16_t = 0) const = delete;

private:
    [[nodiscard]] static const WorkerHandle& requireWorker(const WorkerHandle& worker) {
        if (!worker.valid()) {
            throw std::invalid_argument("context services require a valid worker");
        }
        return worker;
    }

    context_worker_services worker_services_;
    context_connection_services connection_services_{.info = ConnInfo::plain({})};
    context_request_services request_services_;
};

}  // namespace ruvia::detail
