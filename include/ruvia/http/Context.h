#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
#include "ruvia/http/detail/ContextValues.h"
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
class ContextRequest;
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
template <typename T>
void setValidatedBody(Context& context, ValidationTarget target, T&& body);
[[noreturn]] void throwInvalidJsonContentType();
[[noreturn]] void throwInvalidJsonBody();
[[noreturn]] void throwInvalidFormContentType();
[[noreturn]] void throwInvalidFormBody();

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

class ContextRequest final {
public:
    struct ParseBodyOptions final {
        bool all{false};
        bool dot{false};
    };

    struct RequestFormField final {
        RequestFormField(
            std::pmr::memory_resource* resource,
            std::pmr::string&& fieldName,
            std::pmr::string&& fieldValue,
            std::pmr::string&& fieldFilename = {},
            std::pmr::string&& fieldContentType = {},
            bool fieldFile = false,
            bool fieldArray = false)
            : name(std::move(fieldName)),
              value(std::move(fieldValue)),
              filename(std::move(fieldFilename)),
              contentType(std::move(fieldContentType)),
              path(resource),
              file(fieldFile),
              array(fieldArray) {}

        std::pmr::string name;
        std::pmr::string value;
        std::pmr::string filename;
        std::pmr::string contentType;
        std::pmr::vector<std::pmr::string> path;
        bool file{false};
        bool array{false};
    };

    using RequestFormFieldList = std::pmr::vector<RequestFormField>;

    [[nodiscard]] const HttpRequest& raw() const noexcept;

    [[nodiscard]] HttpMethod method() const noexcept;
    [[nodiscard]] std::string_view target() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] RequestValue decodedPath() const noexcept;
    [[nodiscard]] std::string_view queryString() const noexcept;
    [[nodiscard]] std::string_view httpVersion() const noexcept;
    [[nodiscard]] std::span<const HttpHeaderView> headers() const noexcept;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept;
    [[nodiscard]] QueryValue query(std::string_view name) const noexcept;
    [[nodiscard]] RequestNameValueList query() const;
    [[nodiscard]] std::pmr::vector<QueryValue> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;
    [[nodiscard]] RequestNameValueList cookies() const;
    [[nodiscard]] std::string_view remoteAddress() const noexcept;
    [[nodiscard]] std::string_view clientCertificate() const noexcept;
    [[nodiscard]] bool isSecure() const noexcept;
    [[nodiscard]] Task<std::string_view> text() const;
    Task<void> discardBody() const;

    template <typename T>
    [[nodiscard]] Task<T> json() const;

    template <typename T>
    [[nodiscard]] Task<T> form() const;

    template <typename T>
    [[nodiscard]] const T& valid() const;

    template <typename T>
    [[nodiscard]] const T& valid(ValidationTarget target) const;

    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> multipart() const;

    [[nodiscard]] Task<RequestFormFieldList> parseBody() const {
        return parseBody(ParseBodyOptions{});
    }

    [[nodiscard]] Task<RequestFormFieldList> parseBody(ParseBodyOptions options) const;

    [[nodiscard]] BodyReader& bodyReader() const;

    [[nodiscard]] MultipartReader multipartReader() const;

    [[nodiscard]] ParamValue param(std::string_view name) const noexcept;

    [[nodiscard]] const RequestNameValueList& param() const;

private:
    friend class Context;

    explicit constexpr ContextRequest(const Context& context) noexcept
        : context_(&context) {}

    const Context* context_{nullptr};
};

class Context final {
private:
    friend class ContextRequest;
    friend struct detail::ContextAccess;
    friend struct detail::SessionAccess;
    friend detail::RouteRateLimitResult detail::checkRouteRateLimit(
        Context& context,
        const detail::RouteRateLimitOptions& options) noexcept;
    template <typename T>
    friend void detail::setValidatedBody(Context& context, ValidationTarget target, T&& body);

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
    using Renderer = Task<HttpResponse> (*)(Context& context, std::string_view body);
    using ParseBodyOptions = ContextRequest::ParseBodyOptions;
    using RequestFormField = ContextRequest::RequestFormField;
    using RequestFormFieldList = ContextRequest::RequestFormFieldList;

    struct HeaderOptions final {
        bool append{false};
    };

    ~Context() = default;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] ContextRequest req() const noexcept {
        return ContextRequest(*this);
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

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

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

    [[nodiscard]] WebSocket& webSocket() const;

    [[nodiscard]] ResponseStreamWriter& stream() const;

    [[nodiscard]] ResponseStreamWriter& streamText();

    [[nodiscard]] SseWriter streamSSE() const;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
    }

    template <typename T>
    std::decay_t<T>& set(std::string_view name, T&& value) {
        return values().set(name, std::forward<T>(value));
    }

    template <typename T, typename ValueT>
    T& set(ContextKey<T> key, ValueT&& value) {
        return values().template setAs<T>(key.name(), std::forward<ValueT>(value));
    }

    template <typename T>
    [[nodiscard]] T& get(std::string_view name) {
        return values().template get<T>(name);
    }

    template <typename T>
    [[nodiscard]] const T& get(std::string_view name) const {
        const auto* store = valuesIf();
        if (store == nullptr) {
            throw std::logic_error("context value is not available");
        }
        return store->template get<T>(name);
    }

    template <typename T>
    [[nodiscard]] T& get(ContextKey<T> key) {
        return get<T>(key.name());
    }

    template <typename T>
    [[nodiscard]] const T& get(ContextKey<T> key) const {
        return get<T>(key.name());
    }

    template <typename T>
    [[nodiscard]] T& var(std::string_view name) {
        return get<T>(name);
    }

    template <typename T>
    [[nodiscard]] const T& var(std::string_view name) const {
        return get<T>(name);
    }

    template <typename T>
    [[nodiscard]] T& var(ContextKey<T> key) {
        return get(key);
    }

    template <typename T>
    [[nodiscard]] const T& var(ContextKey<T> key) const {
        return get(key);
    }

    Context& status(std::uint16_t statusCode, std::string_view statusText = {});

    Context& header(std::string_view name, std::string_view value) {
        return header(name, value, HeaderOptions{});
    }

    Context& header(std::string_view name, std::string_view value, HeaderOptions options);

    Context& setCookie(std::string_view name, std::string_view value, const CookieOptions& options = {});

    [[nodiscard]] HttpResponse& res();

    Context& res(HttpResponse&& response);

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

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

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    Context& setRenderer(Renderer renderer) noexcept;

    [[nodiscard]] Task<HttpResponse> render(std::string_view body);

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

private:
    [[nodiscard]] HttpResponse streamingHead(std::string_view contentType = {}) const;

    [[nodiscard]] Task<std::string_view> requestBody() const;
    Task<void> requestDiscardBody() const;
    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> requestMultipart() const;
    [[nodiscard]] Task<RequestFormFieldList> parseRequestBody(ParseBodyOptions options) const;
    [[nodiscard]] BodyReader& requestBodyReader() const;
    [[nodiscard]] MultipartReader requestMultipartReader() const;
    [[nodiscard]] ParamValue routeParam(std::string_view name) const noexcept;
    [[nodiscard]] bool requestAccepts(std::string_view mediaType) const noexcept;

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
        std::string_view statusText,
        std::span<const HttpHeaderView> headers = {}) const;

    void applyExplicitResponseHeaders(
        HttpResponse& response,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse textStaticView(
        std::string_view body,
        std::uint16_t statusCode,
        std::string_view statusText) const;

    [[nodiscard]] HttpResponse jsonSerialized(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::string_view statusText) const;

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
    [[nodiscard]] bool hasResponse() const noexcept {
        return response_ != nullptr;
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
    Renderer renderer_{nullptr};
    std::uint16_t responseStatusCode_{200};
    std::pmr::string responseStatusText_;
    HttpResponseHeaders responseHeaders_;
    // Holds the decoded request body when Content-Encoding was applied, so
    // body() can return a stable view; mutable because body() is const.
    mutable std::pmr::string* decodedBody_{nullptr};
    mutable RequestNameValueList* routeParams_{nullptr};
    std::pmr::string* sessionId_{nullptr};
    std::pmr::string* sessionData_{nullptr};
    detail::ContextValueStore* values_{nullptr};
    HttpResponse* response_{nullptr};
    mutable bool bodyDecoded_ : 1 {false};
    bool sessionDirty_ : 1 {false};
    std::array<std::int16_t, kResponseIndexSlots> responseHeaderIndexes_{};

    detail::ValidatedValueStore validatedValues_;
};

inline const HttpRequest& ContextRequest::raw() const noexcept {
    return context_->request_;
}

inline HttpMethod ContextRequest::method() const noexcept {
    return raw().method();
}

inline std::string_view ContextRequest::target() const noexcept {
    return raw().target();
}

inline std::string_view ContextRequest::path() const noexcept {
    return raw().path();
}

inline RequestValue ContextRequest::decodedPath() const noexcept {
    return raw().decodedPath();
}

inline std::string_view ContextRequest::queryString() const noexcept {
    return raw().queryString();
}

inline std::string_view ContextRequest::httpVersion() const noexcept {
    return raw().httpVersion();
}

inline std::span<const HttpHeaderView> ContextRequest::headers() const noexcept {
    return raw().headers();
}

inline std::string_view ContextRequest::header(std::string_view name) const noexcept {
    return raw().header(name);
}

inline bool ContextRequest::accepts(std::string_view mediaType) const noexcept {
    return context_->requestAccepts(mediaType);
}

inline QueryValue ContextRequest::query(std::string_view name) const noexcept {
    return raw().query(name);
}

inline RequestNameValueList ContextRequest::query() const {
    return raw().query();
}

inline std::pmr::vector<QueryValue> ContextRequest::queries(std::string_view name) const {
    return raw().queries(name);
}

inline std::optional<std::string_view> ContextRequest::cookie(std::string_view name) const noexcept {
    return raw().cookie(name);
}

inline RequestNameValueList ContextRequest::cookies() const {
    return raw().cookies();
}

inline std::string_view ContextRequest::remoteAddress() const noexcept {
    return raw().remoteAddress();
}

inline std::string_view ContextRequest::clientCertificate() const noexcept {
    return raw().clientCertificate();
}

inline bool ContextRequest::isSecure() const noexcept {
    return raw().isSecure();
}

inline Task<std::string_view> ContextRequest::text() const {
    return context_->requestBody();
}

inline Task<void> ContextRequest::discardBody() const {
    return context_->requestDiscardBody();
}

inline Task<std::pmr::vector<MultipartPart>> ContextRequest::multipart() const {
    return context_->requestMultipart();
}

inline Task<ContextRequest::RequestFormFieldList> ContextRequest::parseBody(ParseBodyOptions options) const {
    return context_->parseRequestBody(options);
}

inline BodyReader& ContextRequest::bodyReader() const {
    return context_->requestBodyReader();
}

inline MultipartReader ContextRequest::multipartReader() const {
    return context_->requestMultipartReader();
}

inline ParamValue ContextRequest::param(std::string_view name) const noexcept {
    return context_->routeParam(name);
}

inline const RequestNameValueList& ContextRequest::param() const {
    return context_->routeParams();
}

template <typename T>
inline const T& ContextRequest::valid() const {
    return valid<T>(ValidationTarget::kJson);
}

template <typename T>
inline const T& ContextRequest::valid(ValidationTarget target) const {
    return context_->validatedValues_.get<T>(target);
}

namespace detail {

template <typename T>
void setValidatedBody(Context& context, ValidationTarget target, T&& body) {
    context.validatedValues_.set(target, std::forward<T>(body), context.resource());
}

}  // namespace detail

}  // namespace ruvia

#include "ruvia/http/Context.inl"
