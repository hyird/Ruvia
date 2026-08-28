#include "ruvia/web/Testing.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"
#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/router/PrefixFallback.h"
#include "ruvia/web/detail/ratelimit/RateLimiter.h"

namespace ruvia {

namespace {

template <typename Handlers, typename Handler>
void appendPrefixHandler(Handlers& handlers, std::string_view prefix, Handler handler) {
    const auto normalized = detail::validateFallbackPrefix(handlers, prefix, handler);
    handlers.emplace_back(std::string(normalized), std::move(handler));
}

void appendSyntheticHeaderLine(std::string& head, std::string_view name, std::string_view value) {
    head.append(name);
    head.append(": ");
    head.append(value);
    head.append("\r\n");
}

}  // namespace

struct TestApp::Impl final {
    detail::Router router;
    detail::ControllerStore controllers;
    WorkerMemory memory;
    Env env;
    std::pmr::vector<detail::ControllerMiddlewareDescriptor> globalMiddlewares{
        detail::registrationResource()};
    std::pmr::vector<detail::WorkerStateDefinition> workerStateDefinitions{
        detail::registrationResource()};
    std::vector<std::pair<std::string, HttpErrorHandler>> prefixErrorHandlers;
    std::vector<std::pair<std::string, HttpNotFoundHandler>> prefixNotFoundHandlers;
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::optional<detail::WorkerStateRegistry> workerStates;
    std::optional<detail::RateLimiter> rateLimiter;
    bool finalized{false};

    ~Impl() {
        if (workerStates) {
            workerStates->shutdown();
        }
    }

    void requireConfigurable() const {
        if (finalized) {
            throw std::logic_error("TestApp must be configured before its first request()");
        }
    }

    void finalize() {
        if (finalized) {
            return;
        }
        finalized = true;

        const auto controllerRegistrars = detail::sealControllerRegistrars();
        detail::registerControllers(router, controllers, controllerRegistrars);
        auto& routes = detail::RouterImpl::from(router);
        routes.setErrorHandler(detail::CallbackAccess::ref(errorHandler));
        routes.setNotFoundHandler(detail::CallbackAccess::ref(notFoundHandler));
        if (!prefixErrorHandlers.empty()) {
            std::pmr::vector<detail::HttpPrefixErrorHandler> views(detail::registrationResource());
            views.reserve(prefixErrorHandlers.size());
            for (const auto& [prefix, handler] : prefixErrorHandlers) {
                views.push_back({std::string_view(prefix), detail::CallbackAccess::ref(handler)});
            }
            routes.setPrefixErrorHandlers(views);
        }
        if (!prefixNotFoundHandlers.empty()) {
            std::pmr::vector<detail::HttpPrefixNotFoundHandler> views(
                detail::registrationResource());
            views.reserve(prefixNotFoundHandlers.size());
            for (const auto& [prefix, handler] : prefixNotFoundHandlers) {
                views.push_back({std::string_view(prefix), detail::CallbackAccess::ref(handler)});
            }
            routes.setPrefixNotFoundHandlers(views);
        }
        if (!globalMiddlewares.empty()) {
            routes.setGlobalMiddlewares(globalMiddlewares);
        }
        routes.finalize();
        if (routes.routeTable().hasRouteRateLimit()) {
            rateLimiter.emplace(
                std::nullopt, detail::RouteRateLimitPresence::kPresent, 1024, memory.resource());
        }
        workerStates.emplace(memory.resource(), workerStateDefinitions);
        workerStates->initialize();
    }
};

TestApp::TestApp()
    : impl_(std::make_unique<Impl>()) {}

TestApp::~TestApp() = default;

TestApp& TestApp::onError(HttpErrorHandler handler) {
    impl_->requireConfigurable();
    impl_->errorHandler = std::move(handler);
    return *this;
}

TestApp& TestApp::onNotFound(HttpNotFoundHandler handler) {
    impl_->requireConfigurable();
    impl_->notFoundHandler = std::move(handler);
    return *this;
}

TestApp& TestApp::onError(ScopedErrorHandlerOptions options) {
    impl_->requireConfigurable();
    appendPrefixHandler(impl_->prefixErrorHandlers, options.prefix, std::move(options.handler));
    return *this;
}

TestApp& TestApp::onNotFound(ScopedNotFoundHandlerOptions options) {
    impl_->requireConfigurable();
    appendPrefixHandler(impl_->prefixNotFoundHandlers, options.prefix, std::move(options.handler));
    return *this;
}

TestApp& TestApp::useMiddleware(detail::ControllerMiddlewareDescriptor descriptor) {
    impl_->requireConfigurable();
    impl_->globalMiddlewares.push_back(descriptor);
    return *this;
}

TestApp& TestApp::useWorkerStateDefinition(detail::WorkerStateDefinition definition) {
    impl_->requireConfigurable();
    detail::appendWorkerStateDefinition(impl_->workerStateDefinitions, std::move(definition));
    return *this;
}

TestResponse TestApp::request(const TestRequest& request) {
    impl_->finalize();

    RequestMemory requestMemory(impl_->memory);
    HttpRequest parsed = detail::HttpRequestAccess::make();
    detail::HttpRequestAccess::reset(parsed);
    detail::HttpRequestAccess::setResource(parsed, requestMemory.resource());
    detail::HttpRequestAccess::setMethod(parsed, request.method_);
    detail::HttpRequestAccess::setTarget(parsed, request.target_);

    std::optional<HttpParseError> parseError;
    detail::RequestTargetView targetView;
    if (!isValidHttpMethodToken(request.method_)) {
        parseError = HttpParseError::kInvalidRequestLine;
    } else if (!detail::parseRequestTarget(parsed.knownMethod(), request.target_, targetView)) {
        parseError = HttpParseError::kInvalidRequestTarget;
    } else {
        detail::HttpRequestAccess::setPath(parsed, targetView.path);
        detail::HttpRequestAccess::setQueryString(parsed, targetView.query);
    }

    std::string requestHead;
    if (!parseError.has_value()) {
        for (const auto& [name, value] : request.headers_) {
            if (!detail::isValidHttpHeaderName(name) || !detail::isValidHttpHeaderValue(value)) {
                parseError = HttpParseError::kInvalidHeader;
                break;
            }
        }
        if (!parseError.has_value() && !request.cookies_.empty() &&
            !detail::isValidHttpHeaderValue(request.cookies_)) {
            parseError = HttpParseError::kInvalidHeader;
        }
    }
    if (!parseError.has_value()) {
        requestHead.reserve(
            request.method_.size() + request.target_.size() + request.cookies_.size() + 16);
        requestHead.append(request.method_);
        requestHead.push_back(' ');
        requestHead.append(request.target_);
        requestHead.append(" HTTP/1.1\r\n");
        for (const auto& [name, value] : request.headers_) {
            appendSyntheticHeaderLine(requestHead, name, value);
        }
        if (!request.cookies_.empty()) {
            appendSyntheticHeaderLine(requestHead, "Cookie", request.cookies_);
        }
        requestHead.append("\r\n");

        detail::ParsedRequestHeaderBlock block;
        if (requestHead.size() > kMaxHttpHeaderBytes) {
            parseError = HttpParseError::kHeaderTooLarge;
        } else if (const auto error =
                       detail::parseHttpHeaderBlock(requestHead, requestHead.size(), block)) {
            parseError = *error;
        } else {
            const auto contentLength = block.contentLength.value();
            const auto transferEncoding = block.transferEncoding.value();
            const auto contentSemantics = detail::httpRequestContentSemantics(request.method_);
            if (transferEncoding.has_value() && contentLength.has_value()) {
                parseError = HttpParseError::kInvalidTransferEncoding;
            } else if (contentSemantics == detail::HttpRequestContentSemantics::kForbidden &&
                       transferEncoding.has_value()) {
                parseError = HttpParseError::kInvalidTransferEncoding;
            } else if (contentSemantics == detail::HttpRequestContentSemantics::kForbidden &&
                       contentLength.has_value()) {
                parseError = HttpParseError::kInvalidContentLength;
            } else if (transferEncoding.has_value() &&
                       transferEncoding->finalChunked() == nullptr) {
                parseError = HttpParseError::kInvalidTransferEncoding;
            } else if (contentSemantics ==
                           detail::HttpRequestContentSemantics::kContentTypeRequired &&
                       (contentLength.has_value() || transferEncoding.has_value()) &&
                       (block.seenHeaderBits & detail::singletonRequestHeaderBit(
                                                   detail::RequestHeaderKind::kContentType)) == 0) {
                parseError = HttpParseError::kInvalidHeader;
            }

            if (!parseError.has_value()) {
                const auto targetRebindsHost =
                    targetView.form == detail::HttpRequestTargetForm::kAbsolute ||
                    targetView.form == detail::HttpRequestTargetForm::kAuthority;
                for (std::size_t i = 0; i < block.headerCount; ++i) {
                    const auto& header = block.headers[i];
                    auto value = header.value.bind(requestHead);
                    if (targetRebindsHost && block.hostHeaderIndex >= 0 &&
                        i == static_cast<std::size_t>(block.hostHeaderIndex)) {
                        value = targetView.authority;
                    }
                    const HttpHeaderView view{header.name.bind(requestHead), value};
                    const auto slot = detail::requestHeaderKindKnownSlot(header.kind);
                    const bool added =
                        slot < detail::kRequestHeaderKindCount
                            ? detail::HttpRequestAccess::addHeader(parsed, view, slot)
                            : detail::HttpRequestAccess::addHeader(parsed, view);
                    if (!added) {
                        parseError = HttpParseError::kTooManyHeaders;
                        break;
                    }
                }
            }
        }
    }
    detail::HttpRequestAccess::setBody(parsed, request.body_);

    const auto& routes = detail::RouterImpl::from(impl_->router).routeTable();
    const auto resolution = routes.resolve(parsed);
    const auto* resolved = resolution.resolved();
    if (!parseError.has_value() && resolved != nullptr && resolved->route().deadlineMs() != 0) {
        throw std::logic_error(
            "TestApp cannot dispatch a route with Deadline; use a loopback WebWorkerRuntime");
    }

    detail::ContextServices services;
    if (impl_->rateLimiter.has_value()) {
        services = services.withRateLimiter(*impl_->rateLimiter);
    }
    services = services.withEnv(impl_->env).withWorkerStates(*impl_->workerStates);

    std::optional<HttpProtocolError> bodyLimitError;
    if (!parseError.has_value() && resolved != nullptr) {
        const auto routeLimit = resolved->route().maxRequestBodyBytes();
        if (routeLimit != 0 && request.body_.size() > routeLimit) {
            bodyLimitError = detail::HttpRequestBodyFailure::tooLarge().protocolError();
        }
    }

    asio::io_context context(1);
    auto dispatch = [&]() -> asio::awaitable<HttpResponse> {
        if (parseError.has_value()) {
            const auto error = httpParseProtocolError(*parseError);
            co_return co_await detail::taskAsAwaitable(routes.handleError(parsed, requestMemory,
                HttpErrorInfo({.status = error.status(), .message = error.what()}), services));
        }
        if (bodyLimitError.has_value()) {
            co_return co_await detail::taskAsAwaitable(routes.handleError(parsed, requestMemory,
                HttpErrorInfo(
                    {.status = bodyLimitError->status(), .message = bodyLimitError->what()}),
                services));
        }
        co_return co_await detail::taskAsAwaitable(routes.dispatchBufferedResponse(
            parsed, resolution, requestMemory, detail::DocumentRootBinding::none(), services));
    };
    auto future = asio::co_spawn(context, dispatch(), asio::use_future);
    context.run();
    auto response = future.get();

    // Copy everything out while the request arena is still alive.
    TestResponse result(response.status());
    result.headers_.reserve(response.headers().size());
    for (const auto& header : response.headers()) {
        result.headers_.emplace_back(std::string(header.name()), std::string(header.value()));
    }
    // Mirror wire semantics: the response writers suppress the body for HEAD
    // and content-forbidden statuses, so the facade must not surface one
    // either. Writer-synthesized fields (Content-Length, Date, Connection)
    // are framing concerns and stay absent here.
    const auto bodyPlan = detail::httpResponseBodyPlan(parsed.knownMethod(), response.status());
    if (!bodyPlan.bodySuppressed()) {
        const auto body = detail::responseBody(response).bytes();
        result.body_.assign(body.data(), body.size());
    }
    return result;
}

std::optional<std::string_view> TestResponse::header(std::string_view name) const& noexcept {
    for (const auto& [headerName, value] : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(headerName, name)) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

}  // namespace ruvia
