#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/ContextRequest.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/RequestFields.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/ValidationTypes.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/web/detail/ContextValues.h"
#include "ruvia/web/detail/ValidatedValues.h"
#include "ruvia/web/detail/http/ContextCapabilities.h"
#include "ruvia/core/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

namespace ruvia {

class Context;
class Env;
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
class RateLimiter;
struct ContextAccess;
class ContextServices;
struct SessionAccess;
// Assign `src` into `dst`, forcing storage in the backing memory resource rather
// than the small-string optimization's inline buffer. The Context's per-request
// arena outlives the Context, but a string object's inline SSO bytes do not — so
// without this, a short c.session()/c.req().text() value handed to c.text() (a borrowed
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
    friend class ContextRequest;
    friend struct detail::ContextAccess;
    friend const RequestNameValueList& detail::requestHeaderFields(const ContextRequest& request);
    friend const RequestNameValueList& detail::requestQueryFields(const ContextRequest& request);
    friend const RequestNameValueList& detail::requestCookieFields(const ContextRequest& request);
    friend const RequestNameValueList& detail::requestParamFields(const ContextRequest& request);
    friend std::string_view routePath(const Context& context) noexcept;
    friend std::span<const ContextRequest::MatchedRoute> matchedRoutes(const Context& context);
    friend ConnInfo getConnInfo(const Context& context) noexcept;
    friend struct detail::SessionAccess;
    template <typename T>
    friend void detail::setValidatedModel(Context& context, T&& model);

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        detail::ContextServices services) noexcept;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        std::string_view routePath,
        const std::string_view* paramNames,
        const std::string_view* paramValues,
        std::size_t paramCount,
        std::uintptr_t routeRateLimitScope,
        detail::ContextServices services,
        HttpKnownMethod routeMethod = HttpKnownMethod::kUnknown,
        std::size_t routeMiddlewareCount = 0) noexcept;

public:
    struct RenderOptions final {
        std::string_view head{};
        std::string_view title{};
    };

    using Renderer = Task<HttpResponse> (*)(
        Context& context,
        std::string_view body,
        RenderOptions options);

    using Layout = Task<HttpResponse> (*)(
        Context& context,
        std::string_view body,
        RenderOptions options);

    struct HeaderOptions final {
        bool append{false};
    };

    class ResponseHeaderInit final {
    public:
        constexpr ResponseHeaderInit() noexcept = default;

        constexpr ResponseHeaderInit(std::span<const HttpHeaderView> headers) noexcept
            : headers_(headers) {}

        template <std::size_t N>
        constexpr ResponseHeaderInit(const HttpHeaderView (&headers)[N]) noexcept
            : headers_(headers, N) {}

        constexpr ResponseHeaderInit(std::initializer_list<HttpHeaderView>) = delete;

        [[nodiscard]] constexpr operator std::span<const HttpHeaderView>() const noexcept {
            return headers_;
        }

    private:
        std::span<const HttpHeaderView> headers_{};
    };

    struct ResponseInit final {
        std::optional<std::uint16_t> status;
        ResponseHeaderInit headers{};
    };

    class Vars final {
    public:
        explicit constexpr Vars(Context& context) noexcept
            : context_(&context) {}

        template <typename T>
        [[nodiscard]] T* get(std::string_view name) const noexcept {
            return context_->template get<T>(name);
        }

        template <typename T>
        [[nodiscard]] T* get(ContextKey<T> key) const noexcept {
            return context_->template get<T>(key);
        }

        template <typename T>
        [[nodiscard]] T& operator[](ContextKey<T> key) const {
            if (auto* value = get(key)) {
                return *value;
            }
            throw std::logic_error("context value is not available");
        }

    private:
        Context* context_;
    };

    class ConstVars final {
    public:
        explicit constexpr ConstVars(const Context& context) noexcept
            : context_(&context) {}

        template <typename T>
        [[nodiscard]] const T* get(std::string_view name) const noexcept {
            return context_->template get<T>(name);
        }

        template <typename T>
        [[nodiscard]] const T* get(ContextKey<T> key) const noexcept {
            return context_->template get<T>(key);
        }

        template <typename T>
        [[nodiscard]] const T& operator[](ContextKey<T> key) const {
            if (const auto* value = get(key)) {
                return *value;
            }
            throw std::logic_error("context value is not available");
        }

    private:
        const Context* context_;
    };

    ~Context() = default;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] ContextRequest req() const noexcept {
        return ContextRequest(*this);
    }

    [[nodiscard]] std::exception_ptr error() const noexcept {
        return error_;
    }

    // Server-side session blob (persisted by a SessionMiddleware via Redis; the
    // application owns the blob's format). setSession/clearSession mark it for
    // persistence on the way out.
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
    // Force a fresh session id when the middleware persists on the way out, and
    // drop the blob under the old id. Call this on any privilege change (e.g. after
    // authenticating a user) to defeat session fixation: even a session whose id
    // was recognized in the store gets a new, server-chosen id the client could not
    // have planted. Mirrors PHP session_regenerate_id(true) / express regenerate.
    void regenerateSession() {
        sessionRegenerate_ = true;
        sessionDirty_ = true;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    [[nodiscard]] const Env& env() const noexcept;

#ifdef RUVIA_ENABLE_MARIADB
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif
    [[nodiscard]] WebSocket& webSocket() const;

    [[nodiscard]] ResponseStreamWriter& stream() const;

    [[nodiscard]] ResponseStreamWriter& streamText();

    [[nodiscard]] SseWriter streamSSE() const;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
    }

    template <typename T>
    void set(std::string_view name, T&& value) {
        values().set(name, std::forward<T>(value));
    }

    template <typename T, typename ValueT>
    void set(ContextKey<T> key, ValueT&& value) {
        values().template setAs<T>(key.name(), std::forward<ValueT>(value));
    }

    template <typename T>
    [[nodiscard]] T* get(std::string_view name) noexcept {
        auto* store = valuesIf();
        return store == nullptr ? nullptr : store->template getIf<T>(name);
    }

    template <typename T>
    [[nodiscard]] const T* get(std::string_view name) const noexcept {
        const auto* store = valuesIf();
        return store == nullptr ? nullptr : store->template getIf<T>(name);
    }

    template <typename T>
    [[nodiscard]] T* get(ContextKey<T> key) noexcept {
        return get<T>(key.name());
    }

    template <typename T>
    [[nodiscard]] const T* get(ContextKey<T> key) const noexcept {
        return get<T>(key.name());
    }

    [[nodiscard]] Vars var() noexcept {
        return Vars(*this);
    }

    [[nodiscard]] ConstVars var() const noexcept {
        return ConstVars(*this);
    }

    // Route handlers construct one final response, so Context accepts only
    // 200..599. Informational heads belong to a dedicated protocol submit path.
    void status(std::uint16_t statusCode);

    void header(std::string_view name, std::string_view value) {
        header(name, value, HeaderOptions{});
    }

    void header(std::string_view name, std::string_view value, HeaderOptions options);

    void header(std::string_view name, std::nullopt_t);

    void setCookie(std::string_view name, std::string_view value, const CookieOptions& options = {});
    void setSignedCookie(
        std::string_view name,
        std::string_view value,
        std::string_view secret,
        const CookieOptions& options = {});
    // Serialize a Set-Cookie header value without touching the response.
    [[nodiscard]] std::pmr::string generateCookie(
        std::string_view name,
        std::string_view value,
        const CookieOptions& options = {}) const;
    [[nodiscard]] std::pmr::string generateSignedCookie(
        std::string_view name,
        std::string_view value,
        std::string_view secret,
        const CookieOptions& options = {}) const;
    [[nodiscard]] std::optional<std::string_view> deleteCookie(std::string_view name, CookieOptions options = {});

    [[nodiscard]] HttpResponse& res();

    void res(HttpResponse&& response);

    [[nodiscard]] bool finalized() const noexcept {
        return responseFinalized_;
    }

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse body(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        std::nullptr_t,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse body(
        std::nullptr_t,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::nullptr_t,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse body(std::nullptr_t, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse body(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        std::span<const std::byte> body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse body(
        std::span<const std::byte> body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::span<const std::byte> body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse body(std::span<const std::byte> body, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        const std::pmr::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse body(
        std::pmr::string&& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse body(
        std::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse body(
        const std::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse body(
        std::string&& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(const char (&body)[N], ResponseInit init) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse text(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse text(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse text(
        const std::pmr::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse text(
        std::pmr::string&& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse text(
        const std::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string&& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(const char (&body)[N], ResponseInit init) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    template <typename T>
    [[nodiscard]] HttpResponse json(const T& value, ResponseInit init) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse html(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    [[nodiscard]] HttpResponse html(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse html(
        const std::pmr::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse html(
        std::pmr::string&& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse html(
        std::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse html(
        const std::string& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    [[nodiscard]] HttpResponse html(
        std::string&& body,
        std::optional<std::uint16_t> statusCode = std::nullopt) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode = std::nullopt) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::optional<std::uint16_t> statusCode,
        std::initializer_list<HttpHeaderView> headers) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(const char (&body)[N], ResponseInit init) const;

    void renderer(Renderer renderer) noexcept;

    [[nodiscard]] Layout layout(Layout layout) noexcept;

    [[nodiscard]] Layout layout() const noexcept;

    [[nodiscard]] Task<HttpResponse> render(std::string_view body);

    [[nodiscard]] Task<HttpResponse> render(std::string_view body, std::string_view head);

    [[nodiscard]] Task<HttpResponse> render(std::string_view body, RenderOptions options);

    [[nodiscard]] HttpResponse redirect(
        std::string_view location,
        std::uint16_t statusCode = 302) const;

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

    [[nodiscard]] Task<HttpResponse> notFound();

private:
    [[nodiscard]] HttpResponse streamingHead(std::string_view contentType = {}) const;

    [[nodiscard]] Task<std::string_view> requestBody() const;
    Task<void> requestDiscardBody() const;
    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> requestMultipart() const;
    [[nodiscard]] Task<ContextRequest::RequestFormData> parseRequestBody(
        ContextRequest::ParseBodyOptions options) const;
    [[nodiscard]] BodyReader& requestBodyReader() const;
    [[nodiscard]] MultipartReader requestMultipartReader() const;
    [[nodiscard]] std::optional<std::string_view> routeParam(std::string_view name) const;
    [[nodiscard]] bool requestAccepts(std::string_view mediaType) const noexcept;
    void ensureRequestQuery() const;
    [[nodiscard]] std::optional<std::string_view> requestQuery(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& requestQuery() const;
    [[nodiscard]] const RequestValueGroupList& requestQueries() const;
    [[nodiscard]] std::optional<std::string_view> requestCookie(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& requestCookies() const;
    [[nodiscard]] const std::pmr::vector<ContextRequest::MatchedRoute>& requestMatchedRoutes() const;

    [[nodiscard]] MultipartBoundary multipartBoundary() const;

    [[nodiscard]] bool requestContentTypeMatches(std::string_view expected) const noexcept;

    Context& setStableResponseHeader(std::string_view name, std::string_view value);
    Context& removeResponseHeader(std::string_view name);
    void rebuildResponseHeaderIndexes() noexcept;

    [[nodiscard]] HttpResponseHeader* findResponseHeaderForUpdate(
        std::string_view name,
        std::uint32_t knownBit) noexcept;

    void recordResponseKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept;

    void applyResponseState(
        HttpResponse& response,
        std::optional<std::uint16_t> statusCode,
        std::span<const HttpHeaderView> headers = {}) const;

    void applyExplicitResponseHeaders(
        HttpResponse& response,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse textStaticView(
        std::string_view body,
        std::optional<std::uint16_t> statusCode) const;

    [[nodiscard]] HttpResponse jsonSerialized(
        std::pmr::string& body,
        std::optional<std::uint16_t> statusCode) const;

    [[nodiscard]] const RequestNameValueList& requestHeaders() const;
    [[nodiscard]] std::optional<std::string_view> requestHeader(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& routeParams() const;
    [[nodiscard]] std::pmr::string& decodedBody() const;
    [[nodiscard]] std::string_view sessionId() const noexcept {
        return sessionId_ == nullptr
            ? std::string_view{}
            : std::string_view(sessionId_->data(), sessionId_->size());
    }
    [[nodiscard]] std::pmr::string& sessionIdStorage();
    [[nodiscard]] std::pmr::string& sessionDataStorage();
    [[nodiscard]] detail::ContextValueStore& values();
    [[nodiscard]] HttpResponse& responseStorage();
    void storeResponse(HttpResponse&& response);
    void storeAssignedResponse(HttpResponse&& response);
    void storeError(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool hasResponse() const noexcept {
        return responseFinalized_;
    }
    [[nodiscard]] HttpResponse takeResponse();
    [[nodiscard]] detail::ContextValueStore* valuesIf() noexcept {
        return values_;
    }
    [[nodiscard]] const detail::ContextValueStore* valuesIf() const noexcept {
        return values_;
    }

    static constexpr std::size_t kResponseIndexSlots = 22;

    RequestMemory& memory_;
    const HttpRequest& request_;
    ConnInfo connInfo_;
    std::string_view routePath_;
    HttpKnownMethod routeMethod_{HttpKnownMethod::kUnknown};
    const std::string_view* paramNames_{nullptr};
    const std::string_view* paramValues_{nullptr};
    std::size_t paramCount_{0};
    std::size_t routeMiddlewareCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    detail::RateLimiter* rateLimiter_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};
    std::uintptr_t routeRateLimitScope_{0};
    std::size_t maxDecodedBodyBytes_{0};
    detail::ContextRequestBodySource requestBodySource_;
    detail::ContextResponseOutput responseOutput_;
    Renderer renderer_{nullptr};
    Layout layout_{nullptr};
    std::uint16_t responseStatusCode_{200};
    HttpResponseHeaders responseHeaders_;
    // Holds the decoded request body when Content-Encoding was applied, so
    // body() can return a stable view; mutable because body() is const.
    mutable std::pmr::string* decodedBody_{nullptr};
    mutable RequestNameValueList* requestHeaders_{nullptr};
    mutable std::pmr::vector<std::pmr::string>* requestQueryStorage_{nullptr};
    mutable RequestNameValueList* requestQuery_{nullptr};
    mutable std::pmr::vector<std::pmr::string>* requestQueriesStorage_{nullptr};
    mutable RequestValueGroupList* requestQueries_{nullptr};
    mutable RequestNameValueList* requestCookies_{nullptr};
    mutable std::pmr::vector<std::pmr::string>* routeParamStorage_{nullptr};
    mutable RequestNameValueList* routeParams_{nullptr};
    mutable std::pmr::vector<ContextRequest::MatchedRoute>* matchedRoutes_{nullptr};
    std::pmr::string* sessionId_{nullptr};
    std::pmr::string* sessionData_{nullptr};
    detail::ContextValueStore* values_{nullptr};
    HttpResponse* response_{nullptr};
    std::exception_ptr error_;
    mutable bool bodyDecoded_ : 1 {false};
    bool sessionDirty_ : 1 {false};
    bool sessionRegenerate_ : 1 {false};
    bool responseFinalized_ : 1 {false};
    std::array<std::int16_t, kResponseIndexSlots> responseHeaderIndexes_{};

    detail::ValidatedValueStore validatedValues_;
};

inline const HttpRequest& ContextRequest::raw() const noexcept {
    return context_->request_;
}

inline std::string_view ContextRequest::method() const noexcept {
    return raw().method();
}

inline HttpKnownMethod ContextRequest::knownMethod() const noexcept {
    return raw().knownMethod();
}

inline std::pmr::string ContextRequest::url() const {
    const auto requestTarget = raw().target();
    std::pmr::string result(context_->resource());
    if (requestTarget.starts_with("http://") || requestTarget.starts_with("https://")) {
        result.assign(requestTarget.data(), requestTarget.size());
        return result;
    }

    const auto host = header("Host");
    if (!host || host->empty() || requestTarget.empty() || requestTarget.front() != '/') {
        result.assign(requestTarget.data(), requestTarget.size());
        return result;
    }

    result.append(context_->connInfo_.tls() != nullptr ? "https://" : "http://");
    result.append(host->data(), host->size());
    result.append(requestTarget.data(), requestTarget.size());
    return result;
}

inline std::string_view ContextRequest::path() const noexcept {
    return raw().path();
}

inline std::string_view routePath(const Context& context) noexcept {
    return context.routePath_;
}

inline std::span<const ContextRequest::MatchedRoute> matchedRoutes(const Context& context) {
    return context.requestMatchedRoutes();
}

inline std::string_view routePath(const Context& context, std::ptrdiff_t index) {
    const auto routes = matchedRoutes(context);
    if (routes.empty()) {
        return {};
    }

    auto resolved = index;
    if (resolved < 0) {
        resolved += static_cast<std::ptrdiff_t>(routes.size());
    }
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= routes.size()) {
        return {};
    }
    return routes[static_cast<std::size_t>(resolved)].path;
}

inline std::optional<std::string_view> ContextRequest::header(std::string_view name) const {
    return context_->requestHeader(name);
}

inline bool ContextRequest::accepts(std::string_view mediaType) const noexcept {
    return context_->requestAccepts(mediaType);
}

inline std::optional<std::string_view> ContextRequest::query(std::string_view name) const {
    return context_->requestQuery(name);
}

inline std::optional<std::span<const std::string_view>> ContextRequest::queries(std::string_view name) const {
    auto values = context_->requestQueries().values(name);
    if (values.empty()) {
        return std::nullopt;
    }
    return values;
}

inline std::optional<std::string_view> ContextRequest::cookie(std::string_view name) const {
    return context_->requestCookie(name);
}

namespace detail {

inline const RequestNameValueList& requestHeaderFields(const ContextRequest& request) {
    return request.context_->requestHeaders();
}

inline const RequestNameValueList& requestQueryFields(const ContextRequest& request) {
    return request.context_->requestQuery();
}

inline const RequestNameValueList& requestCookieFields(const ContextRequest& request) {
    return request.context_->requestCookies();
}

inline const RequestNameValueList& requestParamFields(const ContextRequest& request) {
    return request.context_->routeParams();
}

}  // namespace detail


inline Task<std::string_view> ContextRequest::text() const {
    return context_->requestBody();
}

inline Task<std::span<const std::byte>> ContextRequest::bytes() const {
    const auto body = co_await text();
    co_return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()),
        body.size());
}

inline Task<ContextRequest::RequestBlob> ContextRequest::blob() const {
    auto bytes = co_await this->bytes();
    co_return RequestBlob(bytes, header("Content-Type").value_or(std::string_view{}));
}

inline Task<void> ContextRequest::discardBody() const {
    return context_->requestDiscardBody();
}

inline Task<std::pmr::vector<MultipartPart>> ContextRequest::multipart() const {
    return context_->requestMultipart();
}

inline Task<ContextRequest::RequestFormData> ContextRequest::parseBody(ParseBodyOptions options) const {
    return context_->parseRequestBody(options);
}

inline BodyReader& ContextRequest::bodyReader() const {
    return context_->requestBodyReader();
}

inline MultipartReader ContextRequest::multipartReader() const {
    return context_->requestMultipartReader();
}

inline std::optional<std::string_view> ContextRequest::param(std::string_view name) const {
    return context_->routeParam(name);
}

namespace detail {

template <typename T>
void setValidatedModel(Context& context, T&& model) {
    context.validatedValues_.set(std::forward<T>(model), context.resource());
}

}  // namespace detail

}  // namespace ruvia

#include "ruvia/web/detail/http/Context.inl"
#include "ruvia/web/detail/http/ContextModel.inl"
