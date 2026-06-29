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

class Context;
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
class RateLimiter;
class RequestBodyLoader;
struct ContextAccess;
class ContextServices;
struct RouteRateLimitOptions;
struct RouteRateLimitResult;
RouteRateLimitResult checkRouteRateLimit(Context& context, const RouteRateLimitOptions& options) noexcept;
struct SessionAccess;
[[noreturn]] void throwInvalidJsonContentType();
[[noreturn]] void throwInvalidJsonBody();
[[noreturn]] void throwInvalidFormContentType();
[[noreturn]] void throwInvalidFormBody();

// Assign `src` into `dst`, forcing storage in the backing memory resource rather
// than the small-string optimization's inline buffer. The Context's per-request
// arena outlives the Context, but a string object's inline SSO bytes do not — so
// without this, a short c.session()/c.body() value handed to c.text() (a borrowed
// view) would dangle once the Context is destroyed before the response is written.
// 32 clears every mainstream SSO threshold (libstdc++/MSVC 15, libc++ 22).
inline void assignStableString(std::pmr::string& dst, std::string_view src) {
    dst.clear();
    if (src.size() < 32) {
        dst.reserve(32);
    }
    dst.assign(src.data(), src.size());
}
}

class Context final {
private:
    friend struct detail::ContextAccess;
    friend struct detail::SessionAccess;
    friend detail::RouteRateLimitResult detail::checkRouteRateLimit(
        Context& context,
        const detail::RouteRateLimitOptions& options) noexcept;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        detail::ContextServices services) noexcept;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        const std::string_view* paramNames,
        const std::string_view* paramValues,
        std::size_t paramCount,
        std::uintptr_t routeRateLimitScope,
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

    [[nodiscard]] RequestValue decodedPath() const noexcept {
        return request_.decodedPath();
    }

    [[nodiscard]] ParamValue param(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < paramCount_; ++i) {
            if (paramNames_[i] == name) {
                return ParamValue(paramValues_[i], resource(), RequestValue::DecodeMode::kPercent);
            }
        }

        return ParamValue(std::nullopt, resource(), RequestValue::DecodeMode::kPercent);
    }

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
        return request_.header(name);
    }

    [[nodiscard]] QueryValue query(std::string_view name) const;

    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;

    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept;

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return request_.remoteAddress();
    }

    // The verified mutual-TLS client certificate subject DN, or empty if none.
    [[nodiscard]] std::string_view clientCertificate() const noexcept {
        return request_.clientCertificate();
    }

    // True when the request arrived over TLS (https / h2 over TLS).
    [[nodiscard]] bool isSecure() const noexcept {
        return request_.isSecure();
    }

    // Server-side session blob (persisted by a SessionMiddleware via Redis; the
    // application owns the blob's format). sessionId() is empty until a session
    // exists. setSession/clearSession mark it for persistence on the way out.
    [[nodiscard]] std::string_view sessionId() const noexcept {
        return sessionId_ == nullptr
            ? std::string_view{}
            : std::string_view(sessionId_->data(), sessionId_->size());
    }
    [[nodiscard]] std::string_view session() const noexcept {
        return sessionData_ == nullptr
            ? std::string_view{}
            : std::string_view(sessionData_->data(), sessionData_->size());
    }
    void setSession(std::string_view data) {
        detail::assignStableString(sessionDataStorage(), data);
        sessionDirty_ = true;
    }
    void clearSession() {
        if (sessionData_ != nullptr) {
            sessionData_->clear();
        }
        sessionDirty_ = true;
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

    struct RequestNameValueView final {
        std::string_view name;
        std::string_view value;
    };

    using RequestNameValueList = std::pmr::vector<RequestNameValueView>;

    [[nodiscard]] const RequestNameValueList& queryParams() const;
    [[nodiscard]] const RequestNameValueList& cookieParams() const;
    [[nodiscard]] std::pmr::string& decodedBody() const;
    [[nodiscard]] std::pmr::string& sessionIdStorage();
    [[nodiscard]] std::pmr::string& sessionDataStorage();

    static constexpr std::size_t kResponseIndexSlots = 22;

    RequestMemory& memory_;
    const HttpRequest& request_;
    const std::string_view* paramNames_{nullptr};
    const std::string_view* paramValues_{nullptr};
    std::size_t paramCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    [[maybe_unused]] detail::HttpClientRegistry* httpClients_{nullptr};
    detail::RateLimiter* rateLimiter_{nullptr};
    std::uintptr_t routeRateLimitScope_{0};
    BodyReader* bodyReader_{nullptr};
    detail::RequestBodyLoader* bodyLoader_{nullptr};
    WebSocket* webSocket_{nullptr};
    ResponseStreamWriter* responseStream_{nullptr};
    std::uint16_t responseStatusCode_{200};
    std::pmr::string responseStatusText_;
    HttpResponseHeaders responseHeaders_;
    // Holds the decoded request body when Content-Encoding was applied, so
    // body() can return a stable view; mutable because body() is const.
    mutable std::pmr::string* decodedBody_{nullptr};
    mutable RequestNameValueList* queryParams_{nullptr};
    mutable RequestNameValueList* cookieParams_{nullptr};
    std::pmr::string* sessionId_{nullptr};
    std::pmr::string* sessionData_{nullptr};
    mutable bool bodyDecoded_ : 1 {false};
    mutable bool queryLookupAttempted_ : 1 {false};
    mutable bool cookieLookupAttempted_ : 1 {false};
    bool sessionDirty_ : 1 {false};
    std::array<std::int16_t, kResponseIndexSlots> responseHeaderIndexes_{};

    detail::ValidatedValueStore validatedValues_;
};

}  // namespace ruvia

#include "ruvia/http/Context.inl"
