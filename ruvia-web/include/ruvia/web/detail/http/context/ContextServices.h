#pragma once

#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/core/WorkerHandle.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {
class BlockingPool;
class Env;
}  // namespace ruvia

namespace ruvia::detail {

class DbRegistry;
class RedisRegistry;
class RateLimiter;
class RouteTable;
class WorkerStateRegistry;

class ContextServices final {
public:
    ContextServices() noexcept
        : connInfo_(ConnInfo::plain({})) {}

    ContextServices(DbRegistry* db, RedisRegistry* redis, RateLimiter* rateLimiter = nullptr, std::size_t maxDecodedBodyBytes = kDefaultMaxBufferedBodyBytes, const WorkerHandle* worker = nullptr) noexcept
        : db_(db),
          redis_(redis),
          rateLimiter_(rateLimiter),
          maxDecodedBodyBytes_(maxDecodedBodyBytes),
          worker_(worker),
          connInfo_(ConnInfo::plain({})) {}

    [[nodiscard]] DbRegistry* db() const noexcept {
        return db_;
    }

    [[nodiscard]] RedisRegistry* redis() const noexcept {
        return redis_;
    }

    [[nodiscard]] RateLimiter* rateLimiter() const noexcept {
        return rateLimiter_;
    }

    [[nodiscard]] const Env* env() const noexcept {
        return env_;
    }

    [[nodiscard]] ContextServices withEnv(const Env& value) const noexcept {
        auto services = *this;
        services.env_ = &value;
        return services;
    }

    [[nodiscard]] constexpr std::size_t maxDecodedBodyBytes() const noexcept {
        return maxDecodedBodyBytes_;
    }

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        if (worker_ != nullptr) {
            return *worker_;
        }
        static const WorkerHandle invalidWorker;
        return invalidWorker;
    }

    // The handle is connection-owned and outlives every ContextServices copy.
    [[nodiscard]] ContextServices withWorker(const WorkerHandle& value) const noexcept {
        auto services = *this;
        services.worker_ = &value;
        return services;
    }
    ContextServices withWorker(WorkerHandle&&) const = delete;

    [[nodiscard]] HttpErrorHandler errorHandler() const noexcept {
        return errorHandler_;
    }

    [[nodiscard]] HttpNotFoundHandler notFoundHandler() const noexcept {
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

    [[nodiscard]] ContextServices withErrorHandler(HttpErrorHandler value) const noexcept {
        auto services = *this;
        services.errorHandler_ = value;
        return services;
    }

    [[nodiscard]] ContextServices withNotFoundHandler(HttpNotFoundHandler value) const noexcept {
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

    [[nodiscard]] BlockingPool* blockingPool() const noexcept {
        return blockingPool_;
    }

    // Process-wide and owned by App::run(), so it outlives every worker that
    // borrows it here.
    [[nodiscard]] ContextServices withBlockingPool(BlockingPool* value) const noexcept {
        auto services = *this;
        services.blockingPool_ = value;
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
    DbRegistry* db_{nullptr};
    RedisRegistry* redis_{nullptr};
    RateLimiter* rateLimiter_{nullptr};
    const Env* env_{nullptr};
    std::size_t maxDecodedBodyBytes_{kDefaultMaxBufferedBodyBytes};
    // Request/session services borrow the address-stable server-owned handle.
    // Every derived ContextServices value stays inside that server's dispatch.
    const WorkerHandle* worker_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};
    const RouteTable* routes_{nullptr};
    const WorkerStateRegistry* workerStates_{nullptr};
    BlockingPool* blockingPool_{nullptr};

    ContextRequestBodySource requestBodySource_;
    ContextResponseOutput responseOutput_;
    ConnInfo connInfo_;
};

}  // namespace ruvia::detail
