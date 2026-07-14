#pragma once

#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/http/ContextCapabilities.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/core/WorkerHandle.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ruvia::detail {

class DbRegistry;
class RedisRegistry;
class RateLimiter;

class ContextServices final {
public:
    constexpr ContextServices() noexcept
        : connInfo_(ConnInfo::plain({})) {}

    constexpr ContextServices(
        DbRegistry* db,
        RedisRegistry* redis,
        RateLimiter* rateLimiter = nullptr,
        std::size_t maxDecodedBodyBytes =
            kDefaultMaxBufferedBodyBytes,
        const WorkerHandle* worker = nullptr) noexcept
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

    [[nodiscard]] constexpr std::size_t maxDecodedBodyBytes() const noexcept {
        return maxDecodedBodyBytes_;
    }

    [[nodiscard]] const WorkerHandle* worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] HttpErrorHandler errorHandler() const noexcept {
        return errorHandler_;
    }

    [[nodiscard]] HttpNotFoundHandler notFoundHandler() const noexcept {
        return notFoundHandler_;
    }

    [[nodiscard]] constexpr const ContextRequestBodySource& requestBodySource()
        const noexcept {
        return requestBodySource_;
    }

    [[nodiscard]] constexpr const ContextResponseOutput& responseOutput()
        const noexcept {
        return responseOutput_;
    }

    [[nodiscard]] constexpr const ConnInfo& connInfo() const noexcept {
        return connInfo_;
    }

    [[nodiscard]] ContextServices withStreamingRequestBody(
        BodyReader& value) const noexcept {
        auto services = *this;
        services.requestBodySource_ = ContextRequestBodySource::streaming(value);
        return services;
    }

    [[nodiscard]] ContextServices withLazyRequestBody(
        RequestBodyLoader& value) const noexcept {
        auto services = *this;
        services.requestBodySource_ = ContextRequestBodySource::lazy(value);
        return services;
    }

    [[nodiscard]] ContextServices withResponseStream(ResponseStreamWriter& value) const noexcept {
        auto services = *this;
        services.responseOutput_ = ContextResponseOutput::responseStream(value);
        return services;
    }

    [[nodiscard]] ContextServices withWebSocket(WebSocket& value) const noexcept {
        auto services = *this;
        services.responseOutput_ = ContextResponseOutput::webSocket(value);
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

    // Views borrow connection-owned storage and remain valid for every Context
    // created while that connection is dispatched.
    [[nodiscard]] ContextServices withPlainTransport(
        std::string_view remoteAddress) const noexcept {
        auto services = *this;
        services.connInfo_ = ConnInfo::plain(remoteAddress);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withPlainTransport(
        std::basic_string<char, Traits, Allocator>&&) const = delete;

    [[nodiscard]] ContextServices withTlsTransport(
        std::string_view remoteAddress,
        std::string_view clientCertificateSubject = {}) const noexcept {
        auto services = *this;
        services.connInfo_ = ConnInfo::tls(
            remoteAddress,
            clientCertificateSubject);
        return services;
    }

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(
        std::basic_string<char, Traits, Allocator>&&,
        std::string_view = {}) const = delete;

    template <typename Traits, typename Allocator>
    ContextServices withTlsTransport(
        std::string_view,
        std::basic_string<char, Traits, Allocator>&&) const = delete;

private:
    DbRegistry* db_{nullptr};
    RedisRegistry* redis_{nullptr};
    RateLimiter* rateLimiter_{nullptr};
    std::size_t maxDecodedBodyBytes_{kDefaultMaxBufferedBodyBytes};
    const WorkerHandle* worker_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};

    ContextRequestBodySource requestBodySource_;
    ContextResponseOutput responseOutput_;
    ConnInfo connInfo_;
};

}  // namespace ruvia::detail
