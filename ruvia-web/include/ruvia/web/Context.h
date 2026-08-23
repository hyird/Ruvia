#pragma once

#include <chrono>
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
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/ContextRequest.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/RequestFields.h"
#include "ruvia/web/Session.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/ValidationTypes.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"
#include "ruvia/web/detail/integration/BlockingCapability.h"
#include "ruvia/web/detail/integration/WorkerStateCapability.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/http/context/ContextResponseState.h"
#include "ruvia/web/detail/http/context/ContextRequestStorage.h"
#include "ruvia/web/detail/http/context/ContextSessionState.h"
#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/Redis.h"
#endif

namespace ruvia {

class Context;
class Env;
class StaticRoot;
class HttpClientHandle;

#ifdef RUVIA_ENABLE_DATABASE
class DbHandle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class RedisHandle;
#endif
namespace detail {
class DbRegistry;
class RedisRegistry;
class HttpClientRegistry;
class RateLimiter;
class RouteTable;
class WorkerStateRegistry;
enum class StaticFileSelectionMode : std::uint8_t;
struct ContextAccess;
class ContextServices;
class RequestDeadline;
struct SessionAccess;
}  // namespace detail

struct RedirectResponseOptions final {
    BorrowedText location;
    HttpStatusCode status{http_status::kFound};
};

struct SetCookieOptions final {
    BorrowedText name;
    BorrowedText value;
    CookieOptions attributes{};
};

struct SetSignedCookieOptions final {
    BorrowedText name;
    BorrowedText value;
    BorrowedText secret;
    CookieOptions attributes{};
};

struct DeleteCookieOptions final {
    BorrowedText name;
    CookieOptions attributes{};
};

struct StaticFileResponseOptions final {
    BorrowedText relativePath;
    BorrowedText contentType;
};

struct FileResponseOptions final {
    std::filesystem::path path;
    BorrowedText contentType;
};

class Context final : public detail::BlockingCapability<Context>, public detail::WorkerStateCapability<Context> {
private:
    friend class ContextRequest;
    friend struct detail::ContextAccess;
    friend ConnInfo getConnInfo(const Context& context) noexcept;
    friend struct detail::SessionAccess;
    template <typename T>
    friend detail::RequestBindingHandle<T> detail::bindValidatedModel(Context& context, const T& model);
    template <typename T>
    friend detail::RequestBindingHandle<T> detail::bindValidatedJsonModel(Context& context, const T& model, std::string_view rawJson);

    Context(RequestMemory& memory, const HttpRequest& request, detail::ContextServices services) noexcept;

    Context(RequestMemory& memory, const HttpRequest& request, std::string_view routePath, const std::string_view* paramNames, const std::string_view* paramValues, std::size_t paramCount, std::uintptr_t routeRateLimitScope, detail::ContextServices services) noexcept;

    [[nodiscard]] HttpResponse staticFile(const StaticRoot& root, StaticFileResponseOptions options, detail::StaticFileSelectionMode mode) const;

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

    // The exception that failed the current middleware/handler dispatch, or
    // null. Distinct from error(status, code, message) which constructs an
    // error response.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return error_;
    }

    // Borrowed for this request. Copy the returned handle when it must outlive
    // the handler; the copy owns a terminal-safe dispatcher endpoint.
    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    // Whether this request's handler deadline elapsed. The token alone cannot
    // say: it trips for worker shutdown too, and a handler that catches the
    // cancellation its own await raised needs to tell those apart before
    // deciding whether to press on.
    [[nodiscard]] bool deadlineExceeded() const noexcept;

    // Stopped when this request should stop doing work: its handler deadline
    // elapsed (see ruvia::Deadline), or the owning worker began stopping.
    // db(), redis(), httpClient({...}).send(), and runBlocking() bind this
    // token automatically, which is how a deadline reaches a handler that
    // never mentions one. An explicit operation token is combined with it.
    //
    // Cooperative by necessity: a suspended coroutine cannot be abandoned in
    // C++, so this stops the WAITS rather than the handler. Every wait Ruvia
    // hands a handler observes this token, streaming sleep() included; a wait the
    // application built out of raw Asio without one is not stopped, and if it is
    // still suspended while the worker can run, the connection scanner's current
    // inactivity phase eventually drops the socket instead of answering on it.
    //
    // Historical note kept because it still holds: HTTP/1 cannot observe a peer
    // FIN while a handler is suspended without a concurrent transport read, so
    // this is not a promise of immediate client-disconnect detection.
    //
    [[nodiscard]] StopToken stopToken() const noexcept {
        return stopToken_;
    }

    // Present only while SessionMiddleware is bound for this request.
    [[nodiscard]] Session session();
    [[nodiscard]] std::optional<Session> trySession() noexcept;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    // Request-scoped typed state: how a middleware hands a value it computed --
    // an authenticated user, a resolved tenant, a trace span -- to everything it
    // calls through next(). The counterpart of workerState<T>(), which lives for
    // the worker's whole life and is shared by every request on it; this lives
    // for one dynamic next() scope and is private to that request.
    //
    //   Task<void> handle(Context& c, Next& next) {
    //       const auto user = authenticate(c);          // owned by this frame
    //       const auto binding = c.bindRequestState(user);
    //       co_await next();                            // handler sees it
    //   }                                               // unbound here
    //
    // The binding stores the value by address and never copies it, so the bound
    // object must outlive the returned handle -- keep both in the middleware's
    // coroutine frame, as above. Binding a temporary is rejected at compile
    // time. One type is one slot: binding T again inside a nested scope shadows
    // the outer binding until the inner handle dies.
    //
    // Deliberately disjoint from req().validated<T>(): that answers "a validator
    // produced and checked this", and hand-bound state must never be able to
    // impersonate it.
    template <typename T>
    [[nodiscard]] RequestStateBinding<T> bindRequestState(const T& value) {
        return requestBindings_.bindState(value);
    }

    template <typename T>
        requires(!std::is_lvalue_reference_v<T>)
    [[nodiscard]] RequestStateBinding<std::remove_cvref_t<T>> bindRequestState(T&&) = delete;

    // Throws std::logic_error when nothing of this type is bound. Use
    // tryRequestState<T>() where absence is a normal outcome -- an optional
    // auth middleware, say.
    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>& requestState() const {
        return requestBindings_.getState<T>();
    }

    template <typename T>
    [[nodiscard]] const std::remove_cvref_t<T>* tryRequestState() const noexcept {
        return requestBindings_.tryGetState<T>();
    }

    [[nodiscard]] const Env& env() const noexcept;

    // Builds a request path from a registered route pattern; the pattern is
    // the route's identity: c.urlFor("/users/:id", {"42"}) -> "/users/42".
    // ":name" values are percent-encoded path segments and must be non-empty;
    // the value for a trailing "*" keeps its slashes and may be empty. Throws
    // std::invalid_argument for an unregistered pattern or a value-count
    // mismatch, and std::logic_error when the context carries no route table
    // (for example a hand-built test context).
    [[nodiscard]] std::pmr::string urlFor(std::string_view pattern, std::initializer_list<std::string_view> values = {}) const;

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif
    [[nodiscard]] HttpClientHandle httpClient() const;
    [[nodiscard]] HttpClientHandle httpClient(std::string_view alias) const;
    [[nodiscard]] WebSocket& webSocket() const;

    [[nodiscard]] ResponseStreamWriter& stream();

    [[nodiscard]] ResponseStreamWriter& streamText();

    [[nodiscard]] SseWriter streamSse();

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
    }

    // Route handlers construct one final response, so Context accepts only
    // 200..599. Informational heads belong to a dedicated protocol submit path.
    void status(HttpStatusCode statusCode);

    void header(std::string_view name, std::string_view value) {
        header(name, value, HeaderOptions{});
    }

    void header(std::string_view name, std::string_view value, HeaderOptions options);

    // Remove a response header set by this handler, before the response is
    // committed. Setting header(name, std::nullopt) to mean deletion was a
    // hidden sentinel; removal now has its own named entry point.
    void removeHeader(std::string_view name);

    void setCookie(SetCookieOptions options);
    void setSignedCookie(SetSignedCookieOptions options);
    void deleteCookie(DeleteCookieOptions options);

    // Observe the final response produced by downstream middleware or a terminal
    // handler. Internal provisional response storage is never exposed here.
    [[nodiscard]] const HttpResponse* response() const noexcept;

    // End middleware dispatch with an explicitly constructed response. Calling
    // next() after respond() is a control-flow error and becomes a 500 response;
    // a middleware may still call respond() after next() to replace an
    // uncommitted downstream response.
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
        requires detail::isResponseModel<T>
    [[nodiscard]] HttpResponse json(const T& value) const;

    [[nodiscard]] HttpResponse html(std::string_view body) const;
    [[nodiscard]] HttpResponse html(std::pmr::string&& body) const;
    [[nodiscard]] HttpResponse html(std::string& body) const = delete;
    [[nodiscard]] HttpResponse html(const std::string& body) const = delete;
    [[nodiscard]] HttpResponse html(std::string&& body) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(const char (&body)[N]) const;

    [[nodiscard]] HttpResponse redirect(RedirectResponseOptions options) const;

    [[nodiscard]] HttpResponse file(FileResponseOptions options) const;

    [[nodiscard]] HttpResponse staticFile(const StaticRoot& root, StaticFileResponseOptions options) const;

    [[nodiscard]] HttpResponse error(HttpErrorInfoOptions options) const;

    [[nodiscard]] ScopedOperation<HttpResponse> notFound();

private:
    [[nodiscard]] HttpResponse streamingHead(std::string_view contentType = {}) const;

    [[nodiscard]] Task<HttpResponse> notFoundTask();
    [[nodiscard]] Task<std::string_view> requestBody() const;
    Task<void> requestDiscardBody() const;
    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> requestMultipart() const;
    [[nodiscard]] Task<ContextRequest::RequestFormData> parseRequestBody(ContextRequest::ParseBodyOptions options) const;
    [[nodiscard]] BodyReader& requestBodyReader() const;
    [[nodiscard]] MultipartReader requestMultipartReader() const;
    [[nodiscard]] std::optional<std::string_view> routeParam(std::string_view name) const;
    void ensureRouteParams() const;
    [[nodiscard]] bool requestAccepts(std::string_view mediaType) const noexcept;
    [[nodiscard]] std::optional<std::string_view> requestNegotiate(ContextRequest::Negotiable field, std::span<const std::string_view> supported) const noexcept;
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
    void applyResponseState(HttpResponse& response, std::optional<HttpStatusCode> statusCode) const;

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
    friend class detail::BlockingCapability<Context>;
    friend class detail::WorkerStateCapability<Context>;
    [[nodiscard]] BlockingPool& blockingPool() const;
    [[nodiscard]] const WorkerHandle& blockingWorker() const noexcept {
        return worker_;
    }
    [[nodiscard]] StopToken blockingStopToken() const noexcept {
        return stopToken_;
    }

    RequestMemory& memory_;
    const HttpRequest& request_;
    ConnInfo connInfo_;
    // Context cannot escape request dispatch and therefore borrows the stable
    // server-owned handle without touching its shared ownership count.
    const WorkerHandle& worker_;
    const StopToken& stopToken_;
    const detail::RequestDeadline* requestDeadline_{nullptr};
    std::string_view routePath_;
    const std::string_view* paramNames_{nullptr};
    const std::string_view* paramValues_{nullptr};
    std::size_t paramCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    detail::HttpClientRegistry* httpClients_{nullptr};
    detail::RateLimiter* rateLimiter_{nullptr};
    const Env* env_{nullptr};
    detail::HttpErrorHandlerRef errorHandler_{nullptr};
    detail::HttpNotFoundHandlerRef notFoundHandler_{nullptr};
    const detail::RouteTable* routes_{nullptr};
    const detail::WorkerStateRegistry* workerStates_{nullptr};
    BlockingPool* blockingPool_{nullptr};
    bool precompressedStaticFiles_{false};
    std::uintptr_t routeRateLimitScope_{0};
    std::size_t maxDecodedBodyBytes_{0};
    detail::ContextRequestBodySource requestBodySource_;
    using RequestStorageOwner = std::unique_ptr<detail::ContextRequestStorage, detail::PmrObjectDeleter<detail::ContextRequestStorage>>;
    // One typed arena allocation owns all lazy request caches. It is destroyed
    // after response/session state borrowers but before RequestMemory releases
    // their backing arena.
    mutable RequestStorageOwner requestStorage_;
    detail::ContextResponseOutput responseOutput_;
    detail::ContextResponseState responseState_;
    detail::ContextSessionState sessionState_;
    std::exception_ptr error_;
    mutable bool bodyDecoded_ : 1 {false};

    detail::RequestBindings requestBindings_;
    // Declared last so it closes first, while every request-owned object and its
    // memory resource are still alive.
    mutable detail::ScopedOperationScope operationScope_;
};

namespace detail {

template <typename T>
RequestBindingHandle<T> bindValidatedModel(Context& context, const T& model) {
    return context.requestBindings_.bindValidated(model);
}

template <typename T>
RequestBindingHandle<T> bindValidatedJsonModel(Context& context, const T& model, std::string_view rawJson) {
    return context.requestBindings_.bindValidated(model, rawJson);
}

}  // namespace detail

}  // namespace ruvia

#include "ruvia/web/detail/http/context/Context.inl"
#include "ruvia/web/detail/http/context/ContextModel.inl"
