#pragma once

#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/http/ContextCapabilities.h"

#include <string_view>

namespace ruvia::detail {

class DbRegistry;
class RedisRegistry;
class RateLimiter;

class ContextServices final {
public:
    constexpr ContextServices() noexcept = default;

    constexpr ContextServices(
        DbRegistry* db,
        RedisRegistry* redis,
        RateLimiter* rateLimiter = nullptr) noexcept
        : db_(db),
          redis_(redis),
          rateLimiter_(rateLimiter) {}

    [[nodiscard]] DbRegistry* db() const noexcept {
        return db_;
    }

    [[nodiscard]] RedisRegistry* redis() const noexcept {
        return redis_;
    }

    [[nodiscard]] RateLimiter* rateLimiter() const noexcept {
        return rateLimiter_;
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

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

    [[nodiscard]] std::string_view clientCertificateSubject() const noexcept {
        return clientCertificateSubject_;
    }

    [[nodiscard]] bool secure() const noexcept {
        return secure_;
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
    [[nodiscard]] ContextServices withTransport(
        std::string_view remoteAddress,
        std::string_view clientCertificateSubject,
        bool secure) const noexcept {
        auto services = *this;
        services.remoteAddress_ = remoteAddress;
        services.clientCertificateSubject_ = clientCertificateSubject;
        services.secure_ = secure;
        return services;
    }

private:
    DbRegistry* db_{nullptr};
    RedisRegistry* redis_{nullptr};
    RateLimiter* rateLimiter_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};

    ContextRequestBodySource requestBodySource_;
    ContextResponseOutput responseOutput_;
    std::string_view remoteAddress_;
    std::string_view clientCertificateSubject_;
    bool secure_{false};
};

}  // namespace ruvia::detail
