#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/Cookies.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/MultipartReader.h"
#include "ruvia/http/Streaming.h"
#include "ruvia/http/ValidationTypes.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/http/detail/ValidatedValues.h"
#include "ruvia/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/redis/Redis.h"
#endif

#ifdef RUVIA_ENABLE_HTTP_CLIENT
#include "ruvia/http/HttpClient.h"
#endif

namespace ruvia {

class StaticRoot;

#ifdef RUVIA_ENABLE_MARIADB
class DbHandle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class RedisHandle;
#endif
namespace detail {
class DbRegistry;
class RedisRegistry;
class HttpClientRegistry;
class HttpClientPool;
class RequestBodyLoader;
struct ContextAccess;
struct ContextServices;
[[noreturn]] void throwInvalidJsonContentType();
[[noreturn]] void throwInvalidJsonBody();
[[noreturn]] void throwInvalidFormContentType();
[[noreturn]] void throwInvalidFormBody();
}

class Context final {
private:
    friend struct detail::ContextAccess;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        detail::ContextServices services) noexcept;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        const std::array<RouteParamView, kMaxRouteParams>& params,
        std::size_t paramCount,
        detail::ContextServices services) noexcept;

public:
    ~Context() = default;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] const HttpRequest& req() const noexcept {
        return request_;
    }

    [[nodiscard]] std::optional<std::pmr::string> decodedPath() const {
        return request_.decodedPath();
    }

    [[nodiscard]] ParamValue param(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < paramCount_; ++i) {
            if (params_[i].name == name) {
                return ParamValue(params_[i].value, resource(), RequestValue::DecodeMode::kPercent);
            }
        }

        return ParamValue(std::nullopt, resource(), RequestValue::DecodeMode::kPercent);
    }

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
        return request_.header(name);
    }

    [[nodiscard]] QueryValue query(std::string_view name) const noexcept {
        return request_.query(name);
    }

    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept {
        return request_.cookie(name);
    }

    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept;

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return request_.remoteAddress();
    }

    // The verified mutual-TLS client certificate subject DN, or empty if none.
    [[nodiscard]] std::string_view clientCertificate() const noexcept {
        return request_.clientCertificate();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    [[nodiscard]] Task<std::string_view> body() const;

    template <typename T>
    [[nodiscard]] Task<T> json() const;

    template <typename T>
    [[nodiscard]] Task<T> form() const;

    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> multipart() const;

    Task<void> discardBody() const;

#ifdef RUVIA_ENABLE_MARIADB
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_HTTP_CLIENT
    // path is an HTTP/1.1 origin-form target: empty maps to "/", otherwise use "/..." or "*".
    [[nodiscard]] Task<FetchResponse> fetch(
        std::string_view path,
        FetchOptions options = {}) {
        return fetch(detail::kDefaultHttpClientAlias, path, std::move(options));
    }

    // path is an HTTP/1.1 origin-form target: empty maps to "/", otherwise use "/..." or "*".
    [[nodiscard]] Task<FetchResponse> fetch(
        std::string_view alias,
        std::string_view path,
        FetchOptions options = {});
#endif

    [[nodiscard]] BodyReader& bodyReader() const;

    [[nodiscard]] MultipartReader multipartReader() const;

    [[nodiscard]] WebSocket& webSocket() const;

    [[nodiscard]] ResponseStreamWriter& stream() const;

    [[nodiscard]] ResponseStreamWriter& streamText();

    [[nodiscard]] SseWriter streamSSE() const;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
    }

    template <typename T>
    [[nodiscard]] const T& valid() const {
        return valid<T>(ValidationTarget::kJson);
    }

    template <typename T>
    [[nodiscard]] const T& valid(ValidationTarget target) const {
        return validatedValues_.get<T>(target);
    }

    template <typename T>
    void setValid(ValidationTarget target, T&& body) {
        validatedValues_.set(target, std::forward<T>(body), resource());
    }

    Context& status(std::uint16_t statusCode, std::string_view statusText = {});

    Context& setHeader(std::string_view name, std::string_view value);

    Context& setCookie(std::string_view name, std::string_view value, const CookieOptions& options = {});

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse text(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse redirect(
        std::string_view location,
        std::uint16_t statusCode = 302,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse file(
        const std::filesystem::path& path,
        std::string_view contentType = {}) const;

    [[nodiscard]] HttpResponse staticFile(
        const StaticRoot& root,
        std::string_view relativePath,
        std::string_view contentType = {}) const;

    [[nodiscard]] HttpResponse error(
        std::uint16_t statusCode,
        std::string_view code,
        std::string_view message,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse streamingHead(std::string_view contentType = {}) const;

private:
    [[nodiscard]] std::string_view multipartBoundary() const;

    [[nodiscard]] bool requestContentTypeMatches(std::string_view expected) const noexcept;

    Context& setStableResponseHeader(std::string_view name, std::string_view value);

    [[nodiscard]] HttpResponseHeader* findResponseHeaderForUpdate(
        std::string_view name,
        std::uint32_t knownBit) noexcept;

    void recordResponseKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept;

    void applyResponseState(
        HttpResponse& response,
        std::uint16_t statusCode,
        std::string_view statusText) const;

    [[nodiscard]] HttpResponse textStaticView(
        std::string_view body,
        std::uint16_t statusCode,
        std::string_view statusText) const;

    [[nodiscard]] HttpResponse jsonSerialized(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::string_view statusText) const;

    static constexpr std::size_t kResponseIndexSlots = 22;

    RequestMemory& memory_;
    const HttpRequest& request_;
    const RouteParamView* params_{nullptr};
    std::size_t paramCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    [[maybe_unused]] detail::HttpClientRegistry* httpClients_{nullptr};
    BodyReader* bodyReader_{nullptr};
    detail::RequestBodyLoader* bodyLoader_{nullptr};
    WebSocket* webSocket_{nullptr};
    ResponseStreamWriter* responseStream_{nullptr};
    std::uint16_t responseStatusCode_{200};
    std::pmr::string responseStatusText_;
    HttpResponseHeaders responseHeaders_;
    // Holds the decoded request body when Content-Encoding was applied, so
    // body() can return a stable view; mutable because body() is const.
    mutable std::pmr::string decodedBody_;
    mutable bool bodyDecoded_{false};
    std::array<std::int16_t, kResponseIndexSlots> responseHeaderIndexes_{};

    detail::ValidatedValueStore validatedValues_;
};

}  // namespace ruvia

#include "ruvia/http/Context.inl"
