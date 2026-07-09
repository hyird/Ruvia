#pragma once

#include "ruvia/http/ErrorHandlers.h"

namespace ruvia {

class BodyReader;
class ResponseStreamWriter;
class WebSocket;

namespace detail {

class DbRegistry;
class HttpClientRegistry;
class RedisRegistry;
class RateLimiter;
class RequestBodyLoader;

class ContextServices final {
public:
    constexpr ContextServices() noexcept = default;

    constexpr ContextServices(
        DbRegistry* db,
        RedisRegistry* redis,
        HttpClientRegistry* httpClients,
        RateLimiter* rateLimiter = nullptr) noexcept
        : db_(db),
          redis_(redis),
          httpClients_(httpClients),
          rateLimiter_(rateLimiter) {}

    [[nodiscard]] DbRegistry* db() const noexcept {
        return db_;
    }

    [[nodiscard]] RedisRegistry* redis() const noexcept {
        return redis_;
    }

    [[nodiscard]] HttpClientRegistry* httpClients() const noexcept {
        return httpClients_;
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

    [[nodiscard]] BodyReader* bodyReader() const noexcept {
        return bodyReader_;
    }

    [[nodiscard]] RequestBodyLoader* bodyLoader() const noexcept {
        return bodyLoader_;
    }

    [[nodiscard]] WebSocket* webSocket() const noexcept {
        return webSocket_;
    }

    [[nodiscard]] ResponseStreamWriter* responseStream() const noexcept {
        return responseStream_;
    }

    [[nodiscard]] ContextServices withBodyReader(BodyReader& value) const noexcept {
        auto services = *this;
        services.bodyReader_ = &value;
        services.bodyLoader_ = nullptr;
        return services;
    }

    [[nodiscard]] ContextServices withBodyLoader(RequestBodyLoader& value) const noexcept {
        auto services = *this;
        services.bodyLoader_ = &value;
        services.bodyReader_ = nullptr;
        return services;
    }

    [[nodiscard]] ContextServices withResponseStream(ResponseStreamWriter& value) const noexcept {
        auto services = *this;
        services.responseStream_ = &value;
        services.webSocket_ = nullptr;
        return services;
    }

    [[nodiscard]] ContextServices withWebSocket(WebSocket& value) const noexcept {
        auto services = *this;
        services.webSocket_ = &value;
        services.responseStream_ = nullptr;
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

private:
    DbRegistry* db_{nullptr};
    RedisRegistry* redis_{nullptr};
    HttpClientRegistry* httpClients_{nullptr};
    RateLimiter* rateLimiter_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};

    BodyReader* bodyReader_{nullptr};
    RequestBodyLoader* bodyLoader_{nullptr};
    WebSocket* webSocket_{nullptr};
    ResponseStreamWriter* responseStream_{nullptr};
};

}  // namespace detail
}  // namespace ruvia
