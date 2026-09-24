#include "ruvia/web/Testing.h"

#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/HttpRequestBodyFailure.h"
#include "ruvia/http/HttpRequestContentSemantics.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/router/PrefixFallback.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/RequestDeadline.h"

namespace ruvia {

namespace {

template <typename Handlers, typename Handler>
void appendPrefixHandler(Handlers& handlers, std::string_view prefix, Handler handler) {
    const auto normalized = detail::validateFallbackPrefix(handlers, prefix, handler);
    handlers.emplace_back(std::string(normalized), std::move(handler));
}

Task<void> startTestWorker(
    ruvia::ConnectionScanner& scanner, detail::WorkerCapabilities& capabilities) {
    try {
        capabilities.initializeWorkerState();
        scanner.start();
        co_await capabilities.connect();
    } catch (...) {
        scanner.stop();
        capabilities.shutdownWorkerState();
        throw;
    }
}

Task<void> stopTestWorker(
    ruvia::ConnectionScanner& scanner, detail::WorkerCapabilities& capabilities) {
    scanner.stop();
    scanner.closeAll();
    capabilities.closeNow();
    try {
        co_await capabilities.join();
    } catch (...) {
        capabilities.shutdownWorkerState();
        throw;
    }
    capabilities.shutdownWorkerState();
}

}  // namespace

struct TestApp::Impl final {
    enum class Lifecycle { kConfiguring,
        kInitializing,
        kReady,
        kFailed };

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
    asio::io_context eventLoopContext{};
    EventLoopAttachment eventLoopAttachment{attachEventLoop(eventLoopContext)};
    EventLoop eventLoop{eventLoopAttachment.loop()};
    WorkerHandle worker{eventLoop.handle()};
    std::thread eventLoopThread;
    StopSource stopSource;
    StopToken stopToken{stopSource.token()};
    std::optional<ruvia::ConnectionScanner> connectionScanner;
    std::optional<detail::WorkerCapabilities> capabilities;
    Lifecycle lifecycle{Lifecycle::kConfiguring};
    std::exception_ptr startupFailure{};
    bool eventLoopStarted{false};

    ~Impl() {
        if (lifecycle == Lifecycle::kReady) {
            stopSource.requestStop();
            try {
                eventLoop.start(stopTestWorker(*connectionScanner, *capabilities)).get();
            } catch (...) {
                std::terminate();
            }
        }
        if (eventLoopStarted) {
            eventLoopAttachment.stop();
            if (eventLoopThread.joinable()) {
                eventLoopThread.join();
            }
        }
    }

    void requireConfigurable() const {
        if (lifecycle != Lifecycle::kConfiguring) {
            throw std::logic_error("TestApp must be configured before its first request()");
        }
    }

    void finalize() {
        if (lifecycle == Lifecycle::kReady) {
            return;
        }
        if (lifecycle == Lifecycle::kInitializing) {
            throw std::logic_error("TestApp request() cannot be reentered during startup");
        }
        if (lifecycle == Lifecycle::kFailed) {
            std::rethrow_exception(startupFailure);
        }
        lifecycle = Lifecycle::kInitializing;

        try {
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

            connectionScanner.emplace(worker, ruvia::ConnectionScannerOptions{});
            capabilities.emplace(eventLoop.ioContext(), worker, memory.resource(),
                detail::WorkerCapabilityDefinitions{.workerStates = workerStateDefinitions},
                detail::WorkerCapabilityOptions{
                    .routeRateLimits = routes.routeTable().hasRouteRateLimit()
                                           ? detail::RouteRateLimitPresence::kPresent
                                           : detail::RouteRateLimitPresence::kAbsent,
                    .rateLimitCapacity = 1024,
                    .env = &env,
                });
            eventLoopThread = std::thread([this] {
                try {
                    eventLoopAttachment.run();
                } catch (...) {
                    std::terminate();
                }
            });
            eventLoopStarted = true;
            eventLoop.start(startTestWorker(*connectionScanner, *capabilities)).get();
            lifecycle = Lifecycle::kReady;
        } catch (...) {
            startupFailure = std::current_exception();
            lifecycle = Lifecycle::kFailed;
            throw;
        }
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

    // Keep the request arena and every arena-backed response on the worker
    // until the response has been copied into its owning facade type.
    auto operation = [this, &request]() -> Task<TestResponse> {
        RequestMemory requestMemory(impl_->memory);
        std::vector<HttpHeaderView> headers;
        headers.reserve(request.headers_.size() + (!request.cookies_.empty() ? 1U : 0U));
        for (const auto& [name, value] : request.headers_) {
            headers.emplace_back(name, value);
        }
        if (!request.cookies_.empty()) {
            headers.emplace_back("Cookie", request.cookies_);
        }
        auto [parsed, parseError] = makeParsedHttpRequest(request.method_, request.target_, headers,
            std::as_bytes(std::span(request.body_.data(), request.body_.size())),
            requestMemory.resource());

        const auto& routes = detail::RouterImpl::from(impl_->router).routeTable();
        const auto resolution = routes.resolve(parsed);
        const auto* resolved = resolution.resolved();

        const auto services = impl_->capabilities->contextServices(impl_->stopToken);

        std::optional<HttpProtocolError> bodyLimitError;
        if (!parseError.has_value() && resolved != nullptr) {
            const auto routeLimit = resolved->route().maxRequestBodyBytes();
            if (routeLimit != 0 && request.body_.size() > routeLimit) {
                bodyLimitError = HttpRequestBodyFailure::tooLarge().protocolError();
            }
        }

        auto dispatch = [&]() -> Task<HttpResponse> {
            auto requestServices = services;
            std::optional<detail::RequestDeadline> requestDeadline;
            if (!parseError.has_value() && !bodyLimitError.has_value() && resolved != nullptr &&
                resolved->route().deadlineMs() != 0) {
                requestDeadline.emplace(requestServices.stopToken());
                requestDeadline->arm(requestServices.worker(),
                    std::chrono::milliseconds(resolved->route().deadlineMs()));
                requestServices = requestServices.withRequestDeadline(*requestDeadline);
            }
            if (parseError.has_value()) {
                const auto error = httpParseProtocolError(*parseError);
                co_return co_await routes.handleError(parsed, requestMemory,
                    HttpErrorInfo({.status = error.status(), .message = error.what()}),
                    requestServices);
            }
            if (bodyLimitError.has_value()) {
                co_return co_await routes.handleError(parsed, requestMemory,
                    HttpErrorInfo(
                        {.status = bodyLimitError->status(), .message = bodyLimitError->what()}),
                    requestServices);
            }
            co_return co_await routes.dispatchBufferedResponse(parsed, resolution, requestMemory,
                detail::DocumentRootBinding::none(), requestServices);
        };
        auto response = co_await dispatch();

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
        const auto writePlan = planBufferedHttpResponseWrite(parsed.knownMethod(), response);
        if (!writePlan.bodySuppressed()) {
            const auto body = response.bodyBytes();
            result.body_.assign(reinterpret_cast<const char*>(body.data()), body.size());
        }
        co_return result;
    };
    return impl_->eventLoop.start(operation()).get();
}

std::optional<std::string_view> TestResponse::header(std::string_view name) const& noexcept {
    for (const auto& [headerName, value] : headers_) {
        if (httpAsciiEqualsIgnoreCase(headerName, name)) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

}  // namespace ruvia
