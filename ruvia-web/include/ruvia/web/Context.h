#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/Cookies.h"
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
#include "ruvia/web/detail/ValidatedValues.h"
#include "ruvia/web/detail/WorkerState.h"
#include "ruvia/web/detail/http/ContextCapabilities.h"
#include "ruvia/web/detail/http/ContextResponseState.h"
#include "ruvia/web/detail/http/ContextRequestStorage.h"
#include "ruvia/web/detail/http/ContextSessionState.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

namespace ruvia {

class Context;
class Env;
class StaticRoot;

#ifdef RUVIA_ENABLE_DATABASE
class DbHandle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class RedisHandle;
#endif
namespace detail {
class DbRegistry;
class RedisRegistry;
class RateLimiter;
class RouteTable;
class WorkerStateRegistry;
struct ContextAccess;
class ContextServices;
struct SessionAccess;
}

class Context final {
private:
    friend class ContextRequest;
    friend struct detail::ContextAccess;
    friend const RequestNameValueList& detail::requestHeaderFields(const ContextRequest& request);
    friend const RequestNameValueList& detail::requestQueryFields(const ContextRequest& request);
    friend const RequestNameValueList& detail::requestCookieFields(const ContextRequest& request);
    friend const RequestNameValueList& detail::requestParamFields(const ContextRequest& request);
    friend ConnInfo getConnInfo(const Context& context) noexcept;
    friend struct detail::SessionAccess;
    template <typename T>
    friend detail::ValidatedModelBinding<T>
    detail::bindValidatedModel(Context& context, const T& model);

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
        detail::ContextServices services) noexcept;

public:
    using HeaderOptions = HttpResponse::HeaderOptions;

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

    // Borrowed for this request. Copy the returned handle when it must outlive
    // the handler; the copy owns a terminal-safe dispatcher endpoint.
    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    // Server-side session blob (persisted by a SessionMiddleware via Redis; the
    // application owns the blob's format). setSession/clearSession mark it for
    // persistence on the way out.
    [[nodiscard]] std::string_view session() const noexcept {
        return sessionState_.data();
    }
    void setSession(std::string_view data) {
        sessionState_.set(data);
    }
    void clearSession() {
        sessionState_.clear();
    }
    // Force a fresh session id when the middleware persists on the way out, and
    // drop the blob under the old id. Call this on any privilege change (e.g. after
    // authenticating a user) to defeat session fixation: even a session whose id
    // was recognized in the store gets a new, server-chosen id the client could not
    // have planted. Mirrors PHP session_regenerate_id(true) / express regenerate.
    void regenerateSession() {
        sessionState_.regenerate();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    [[nodiscard]] const Env& env() const noexcept;

    // Builds a request path from a registered route pattern; the pattern is
    // the route's identity: c.urlFor("/users/:id", {"42"}) -> "/users/42".
    // ":name" values are percent-encoded path segments and must be non-empty;
    // the value for a trailing "*" keeps its slashes and may be empty. Throws
    // std::invalid_argument for an unregistered pattern or a value-count
    // mismatch, and std::logic_error when the context carries no route table
    // (for example a hand-built test context).
    [[nodiscard]] std::pmr::string urlFor(
        std::string_view pattern,
        std::initializer_list<std::string_view> values = {}) const;

    // This worker's instance of an App::useWorkerState<T>() registration.
    // The reference is worker-local: it stays valid for the worker's lifetime
    // but must never be handed to another worker. Throws std::logic_error for
    // a type that was not registered before app().run().
    template <typename T>
    [[nodiscard]] T& workerState() const {
        return *static_cast<T*>(
            workerStateInstance(detail::workerStateTypeKey<T>()));
    }

#ifdef RUVIA_ENABLE_DATABASE
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

    [[nodiscard]] SseWriter streamSse();

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
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
    void deleteCookie(std::string_view name, CookieOptions options = {});

    // Observe the final response produced by downstream middleware or a terminal
    // handler. Internal provisional response storage is never exposed here.
    [[nodiscard]] const HttpResponse* response() const noexcept;

    // End middleware dispatch with an explicitly constructed response.
    void respond(HttpResponse&& response);

    [[nodiscard]] HttpResponse body(std::string_view body) const;
    [[nodiscard]] HttpResponse body(std::nullptr_t) const;
    [[nodiscard]] HttpResponse body(std::pmr::string&& body) const;
    [[nodiscard]] HttpResponse body(std::span<const std::byte> body) const;
    [[nodiscard]] HttpResponse body(std::string& body) const = delete;
    [[nodiscard]] HttpResponse body(const std::string& body) const = delete;
    [[nodiscard]] HttpResponse body(std::string&& body) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(const char (&body)[N]) const;

    [[nodiscard]] HttpResponse text(std::string_view body) const;
    [[nodiscard]] HttpResponse text(std::pmr::string&& body) const;
    [[nodiscard]] HttpResponse text(std::string& body) const = delete;
    [[nodiscard]] HttpResponse text(const std::string& body) const = delete;
    [[nodiscard]] HttpResponse text(std::string&& body) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(const char (&body)[N]) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(const T& value) const;

    [[nodiscard]] HttpResponse html(std::string_view body) const;
    [[nodiscard]] HttpResponse html(std::pmr::string&& body) const;
    [[nodiscard]] HttpResponse html(std::string& body) const = delete;
    [[nodiscard]] HttpResponse html(const std::string& body) const = delete;
    [[nodiscard]] HttpResponse html(std::string&& body) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(const char (&body)[N]) const;

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
    void ensureRouteParams() const;
    [[nodiscard]] bool requestAccepts(std::string_view mediaType) const noexcept;
    void ensureRequestQuery() const;
    [[nodiscard]] std::optional<std::string_view> requestQuery(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& requestQuery() const;
    [[nodiscard]] const detail::RequestQueryValues& requestQueries() const;
    [[nodiscard]] std::optional<std::string_view> requestCookie(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& requestCookies() const;
    [[nodiscard]] MultipartBoundary multipartBoundary() const;

    [[nodiscard]] bool requestContentTypeMatches(std::string_view expected) const noexcept;

    Context& setStableResponseHeader(std::string_view name, std::string_view value);
    Context& removeResponseHeader(std::string_view name);
    void applyResponseState(
        HttpResponse& response,
        std::optional<std::uint16_t> statusCode) const;

    [[nodiscard]] HttpResponse bodyStaticView(std::string_view body) const;
    [[nodiscard]] HttpResponse textStaticView(std::string_view body) const;
    [[nodiscard]] HttpResponse htmlStaticView(std::string_view body) const;

    [[nodiscard]] HttpResponse jsonSerialized(std::pmr::string& body) const;

    [[nodiscard]] const RequestNameValueList& requestHeaders() const;
    [[nodiscard]] std::optional<std::string_view> requestHeader(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& routeParams() const;
    [[nodiscard]] std::pmr::string& decodedBody() const;
    [[nodiscard]] detail::ContextRequestStorage& requestStorage() const;
    [[nodiscard]] HttpResponse& responseStorage();
    void storeResponse(HttpResponse&& response);
    void storeAssignedResponse(HttpResponse&& response);
    void storeError(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool hasResponse() const noexcept {
        return responseState_.final() != nullptr;
    }
    [[nodiscard]] HttpResponse takeResponse();
    [[nodiscard]] void* workerStateInstance(const void* typeKey) const;

    RequestMemory& memory_;
    const HttpRequest& request_;
    ConnInfo connInfo_;
    // Context cannot escape request dispatch and therefore borrows the stable
    // server-owned handle without touching its shared ownership count.
    const WorkerHandle& worker_;
    std::string_view routePath_;
    const std::string_view* paramNames_{nullptr};
    const std::string_view* paramValues_{nullptr};
    std::size_t paramCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    detail::RateLimiter* rateLimiter_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};
    const detail::RouteTable* routes_{nullptr};
    const detail::WorkerStateRegistry* workerStates_{nullptr};
    std::uintptr_t routeRateLimitScope_{0};
    std::size_t maxDecodedBodyBytes_{0};
    detail::ContextRequestBodySource requestBodySource_;
    using RequestStorageOwner = std::unique_ptr<
        detail::ContextRequestStorage,
        detail::PmrObjectDeleter<detail::ContextRequestStorage>>;
    // One typed arena allocation owns all lazy request caches. It is destroyed
    // after response/session state borrowers but before RequestMemory releases
    // their backing arena.
    mutable RequestStorageOwner requestStorage_;
    detail::ContextResponseOutput responseOutput_;
    detail::ContextResponseState responseState_;
    detail::ContextSessionState sessionState_;
    std::exception_ptr error_;
    mutable bool bodyDecoded_ : 1 {false};

    detail::ValidatedModelBindings validatedModels_;
    // Declared last so it closes first, while every request-owned object and its
    // memory resource are still alive.
    mutable detail::ScopedOperationScope operationScope_;
};

namespace detail {

template <typename T>
ValidatedModelBinding<T> bindValidatedModel(Context& context, const T& model) {
    return context.validatedModels_.bind(model);
}

}  // namespace detail

}  // namespace ruvia

#include "ruvia/web/detail/http/Context.inl"
#include "ruvia/web/detail/http/ContextModel.inl"
