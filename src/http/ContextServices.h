#pragma once

namespace ruvia {

class BodyReader;
class ResponseStreamWriter;
class WebSocket;

namespace detail {

class DbRegistry;
class HttpClientRegistry;
class RedisRegistry;
class RequestBodyLoader;

class ContextServices final {
public:
    constexpr ContextServices() noexcept = default;

    constexpr ContextServices(
        DbRegistry* db,
        RedisRegistry* redis,
        HttpClientRegistry* httpClients) noexcept
        : db_(db),
          redis_(redis),
          httpClients_(httpClients) {}

    [[nodiscard]] DbRegistry* db() const noexcept {
        return db_;
    }

    [[nodiscard]] RedisRegistry* redis() const noexcept {
        return redis_;
    }

    [[nodiscard]] HttpClientRegistry* httpClients() const noexcept {
        return httpClients_;
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

private:
    DbRegistry* db_{nullptr};
    RedisRegistry* redis_{nullptr};
    HttpClientRegistry* httpClients_{nullptr};

    BodyReader* bodyReader_{nullptr};
    RequestBodyLoader* bodyLoader_{nullptr};
    WebSocket* webSocket_{nullptr};
    ResponseStreamWriter* responseStream_{nullptr};
};

}  // namespace detail
}  // namespace ruvia
