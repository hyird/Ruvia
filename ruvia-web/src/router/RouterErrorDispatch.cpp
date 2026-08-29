#include "ruvia/web/detail/router/RouteTable.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/Validation.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/error/HttpErrorResponse.h"
#include "ruvia/web/detail/http/request/UnsupportedRequestContentCoding.h"
#include "ruvia/web/detail/router/PrefixFallback.h"
#include "ruvia/web/detail/router/RouteDispatchServices.h"

// Turning a failed request into a response: the error a thrown exception really
// carries, the metadata that survives onto the response, and the scoped error /
// not-found handlers that get the last word.

namespace ruvia {

namespace {

struct OwnedHttpErrorInfo;
void assignExceptionError(OwnedHttpErrorInfo& errorInfo, const std::exception_ptr& exception);

struct OwnedHttpErrorInfo final {
    HttpErrorInfo info{};
    std::pmr::string statusText;
    std::pmr::string code;
    std::pmr::string message;
    std::pmr::vector<ValidationIssue> validationIssues;

    explicit OwnedHttpErrorInfo(HttpErrorInfo source, std::pmr::memory_resource* resource)
        : statusText(resource),
          code(resource),
          message(resource),
          validationIssues(resource) {
        assign(source);
    }

    OwnedHttpErrorInfo(std::pmr::memory_resource* resource, const std::exception_ptr& exception)
        : OwnedHttpErrorInfo(HttpErrorInfo({.status = ruvia::http_status::kInternalServerError,
                                 .message = "unhandled exception"}),
              resource) {
        assignExceptionError(*this, exception);
    }

    void assign(HttpErrorInfo source) {
        statusText.assign(source.statusText().data(), source.statusText().size());
        code.assign(source.code().data(), source.code().size());
        message.assign(source.message().data(), source.message().size());
        std::pmr::vector<ValidationIssue> copied(validationIssues.get_allocator().resource());
        copied.reserve(source.validationIssues().size());
        for (const auto& issue : source.validationIssues()) {
            copied.push_back(
                detail::ValidationIssueAccess::copy(issue, copied.get_allocator().resource()));
        }
        validationIssues = std::move(copied);

        info = HttpErrorInfo({.status = source.status(),
            .code = code,
            .message = message,
            .statusText = statusText,
            .validationIssues = validationIssues});
    }
};

void assignExceptionError(OwnedHttpErrorInfo& errorInfo, const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const ValidationError& error) {
        errorInfo.assign(error.info());
    } catch (const detail::UnsupportedRequestContentCoding& error) {
        errorInfo.assign(HttpErrorInfo({.status = HttpUnsupportedContentCoding::status(),
            .code = "unsupported_content_coding",
            .message = error.what()}));
    } catch (const HttpError& error) {
        errorInfo.assign(error.info());
    } catch (const HttpProtocolError& error) {
        errorInfo.assign(HttpErrorInfo({.status = error.status(), .message = error.what()}));
    } catch (const BlockingOperationRejected& error) {
        // The blocking pool refused the work or the worker is going away. That
        // is capacity, not a bug in the request: answer it like any other
        // overload instead of a 500. The message is the framework's own and
        // names no application internals.
        errorInfo.assign(HttpErrorInfo({.status = ruvia::http_status::kServiceUnavailable,
            .code = "blocking_pool_unavailable",
            .message = error.what()}));
    } catch (const std::exception&) {
        // An unexpected exception (e.g. a database/library error) may carry
        // internal detail -- table names, query fragments, file paths. Do NOT echo
        // what() to the client: normalizeError renders a generic "Internal Server
        // Error" body. The exception_ptr is still set on the Context, so an onError
        // handler can log or inspect the full detail server-side.
        errorInfo.assign(HttpErrorInfo({.status = ruvia::http_status::kInternalServerError}));
    } catch (...) {
        errorInfo.assign(HttpErrorInfo({.status = ruvia::http_status::kInternalServerError}));
    }
}

[[nodiscard]] bool isUnsupportedRequestContentCoding(const std::exception_ptr& exception) noexcept {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const detail::UnsupportedRequestContentCoding&) {
        return true;
        // Classification only; the caller still owns and dispatches the exception.
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
    return false;
}

void applyExceptionResponseMetadata(HttpResponse& response, const std::exception_ptr& exception) {
    if (isUnsupportedRequestContentCoding(exception)) {
        response.header("Accept-Encoding", detail::httpSupportedRequestContentCodings());
    }
}

}  // namespace

Task<HttpResponse> detail::RouteTable::handleError(const HttpRequest& request,
    RequestMemory& memory, HttpErrorInfo error, ContextServices services) const {
    const auto errorHandler = errorHandlerFor(request.path());
    // Nothing to wrap and no handler: the original allocation-free path.
    if (errorHandler == nullptr && unmatchedMiddlewareCount_ == 0) {
        co_return makeDefaultErrorResponse(memory.resource(), error);
    }

    auto context = detail::ContextAccess::make(memory, request,
        withRouteHandlers(services, *this, errorHandler, notFoundHandlerFor(request.path())));
    auto terminal = [this, error, errorHandler](Context& terminalContext) -> Task<HttpResponse> {
        if (errorHandler == nullptr) {
            co_return makeDefaultErrorResponse(terminalContext.resource(), error);
        }
        co_return co_await handleError(terminalContext, error);
    };
    const auto terminalRef = makeCallableRef<HttpResponse, Context&>(terminal);
    co_return co_await runUnmatchedChain(context, terminalRef);
}

Task<HttpResponse> detail::RouteTable::handleException(const HttpRequest& request,
    RequestMemory& memory, std::exception_ptr exception, ContextServices services) const {
    if (errorHandlerFor(request.path()) == nullptr) {
        OwnedHttpErrorInfo errorInfo(memory.resource(), exception);
        auto response = makeDefaultErrorResponse(memory.resource(), errorInfo.info);
        applyExceptionResponseMetadata(response, exception);
        co_return response;
    }

    auto context = detail::ContextAccess::make(memory, request,
        withRouteHandlers(
            services, *this, errorHandlerFor(request.path()), notFoundHandlerFor(request.path())));
    co_return co_await handleException(context, exception);
}

Task<HttpResponse> detail::RouteTable::handleError(Context& context, HttpErrorInfo error) const {
    return invokeErrorHandler(
        context, error, errorHandlerFor(detail::ContextAccess::request(context).path()));
}

Task<HttpResponse> detail::RouteTable::handleNotFound(
    const HttpRequest& request, RequestMemory& memory, ContextServices services) const {
    const auto notFoundHandler = notFoundHandlerFor(request.path());
    if (notFoundHandler == nullptr && unmatchedMiddlewareCount_ == 0) {
        co_return makeDefaultErrorResponse(memory.resource(),
            HttpErrorInfo({.status = ruvia::http_status::kNotFound, .message = "route not found"}));
    }

    auto context = detail::ContextAccess::make(memory, request,
        withRouteHandlers(services, *this, errorHandlerFor(request.path()), notFoundHandler));

    // The handler's own failure keeps going through handleException, exactly as
    // before; wrapping it in the chain must not change which layer answers it.
    auto terminal = [this, notFoundHandler](Context& terminalContext) -> Task<HttpResponse> {
        if (notFoundHandler == nullptr) {
            co_return makeDefaultErrorResponse(terminalContext.resource(),
                HttpErrorInfo(
                    {.status = ruvia::http_status::kNotFound, .message = "route not found"}));
        }
        std::exception_ptr exception;
        try {
            co_return co_await notFoundHandler(terminalContext);
        } catch (...) {
            exception = std::current_exception();
        }
        co_return co_await handleException(terminalContext, exception);
    };
    const auto terminalRef = makeCallableRef<HttpResponse, Context&>(terminal);
    co_return co_await runUnmatchedChain(context, terminalRef);
}

Task<HttpResponse> detail::RouteTable::handleException(
    Context& context, std::exception_ptr exception) const {
    detail::ContextAccess::setError(context, exception);
    OwnedHttpErrorInfo errorInfo(context.resource(), exception);

    auto response = co_await handleError(context, errorInfo.info);
    applyExceptionResponseMetadata(response, exception);
    co_return response;
}

// Which handler answers a failure on a given path: the most specific prefix
// scope that covers it, or the table-wide fallback. The dispatch above is the
// only caller.

void detail::RouteTable::setErrorHandler(HttpErrorHandlerRef handler) noexcept {
    errorHandler_ = handler;
}

void detail::RouteTable::setNotFoundHandler(HttpNotFoundHandlerRef handler) noexcept {
    notFoundHandler_ = handler;
}

namespace {

template <typename Stored, typename Registration>
void replacePrefixHandlers(std::pmr::vector<Stored>& stored, std::pmr::memory_resource* resource,
    std::span<const Registration> handlers) {
    std::pmr::vector<Stored> normalized(resource);
    normalized.reserve(handlers.size());
    for (const auto& registration : handlers) {
        if (registration.handler == nullptr) {
            throw std::invalid_argument("fallback handler must not be null");
        }
        const auto prefix = detail::normalizeFallbackPrefix(registration.prefix);
        for (const auto& existing : normalized) {
            if (std::string_view(existing.prefix) == prefix) {
                throw std::invalid_argument("duplicate fallback prefix");
            }
        }
        normalized.emplace_back(resource, prefix, registration.handler);
    }
    // Longest prefix first: selection is a first-match scan. Equal lengths
    // cannot nest, so their relative order is irrelevant; keep it stable.
    std::ranges::stable_sort(normalized, [](const Stored& left, const Stored& right) noexcept {
        return left.prefix.size() > right.prefix.size();
    });
    stored = std::move(normalized);
}

// Longest-first stored order: the first hit is the tightest scope. A prefix
// matches on whole path segments only, so "/api" scopes "/api" and "/api/x"
// but never "/apix".
template <typename Stored>
[[nodiscard]] auto selectPrefixHandler(
    const std::pmr::vector<Stored>& stored, std::string_view path) noexcept {
    for (const auto& candidate : stored) {
        const std::string_view prefix(candidate.prefix);
        if (detail::pathIsUnderPrefix(path, prefix)) {
            return candidate.handler;
        }
    }
    return decltype(stored.front().handler){nullptr};
}

}  // namespace

void detail::RouteTable::setPrefixErrorHandlers(std::span<const HttpPrefixErrorHandler> handlers) {
    replacePrefixHandlers(prefixErrorHandlers_, resource_, handlers);
}

void detail::RouteTable::setPrefixNotFoundHandlers(
    std::span<const HttpPrefixNotFoundHandler> handlers) {
    replacePrefixHandlers(prefixNotFoundHandlers_, resource_, handlers);
}

detail::HttpErrorHandlerRef detail::RouteTable::errorHandlerFor(
    std::string_view path) const noexcept {
    const auto handler = selectPrefixHandler(prefixErrorHandlers_, path);
    return handler != nullptr ? handler : errorHandler_;
}

detail::HttpNotFoundHandlerRef detail::RouteTable::notFoundHandlerFor(
    std::string_view path) const noexcept {
    const auto handler = selectPrefixHandler(prefixNotFoundHandlers_, path);
    return handler != nullptr ? handler : notFoundHandler_;
}

}  // namespace ruvia
