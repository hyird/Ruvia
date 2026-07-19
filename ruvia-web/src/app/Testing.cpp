#include "ruvia/web/Testing.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/router/RouterInternal.h"

namespace ruvia {

struct TestApp::Impl final {
    Router router;
    detail::ControllerStore controllers;
    WorkerMemory memory;
    std::pmr::vector<detail::ControllerMiddlewareDescriptor> globalMiddlewares{
        detail::registrationResource()};
    std::pmr::vector<detail::WorkerStateDefinition> workerStateDefinitions{
        detail::registrationResource()};
    std::vector<std::pair<std::string, HttpErrorHandler>> prefixErrorHandlers;
    std::vector<std::pair<std::string, HttpNotFoundHandler>> prefixNotFoundHandlers;
    HttpErrorHandler errorHandler{nullptr};
    HttpNotFoundHandler notFoundHandler{nullptr};
    std::optional<detail::WorkerStateRegistry> workerStates;
    bool finalized{false};

    void requireConfigurable() const {
        if (finalized) {
            throw std::logic_error(
                "TestApp must be configured before its first request()");
        }
    }

    void finalize() {
        if (finalized) {
            return;
        }
        finalized = true;

        detail::registerControllers(router, controllers);
        auto& routes = detail::RouterImpl::from(router);
        routes.setErrorHandler(errorHandler);
        routes.setNotFoundHandler(notFoundHandler);
        if (!prefixErrorHandlers.empty()) {
            std::pmr::vector<detail::HttpPrefixErrorHandler> views(
                detail::registrationResource());
            views.reserve(prefixErrorHandlers.size());
            for (const auto& [prefix, handler] : prefixErrorHandlers) {
                views.push_back({std::string_view(prefix), handler});
            }
            routes.setPrefixErrorHandlers(views);
        }
        if (!prefixNotFoundHandlers.empty()) {
            std::pmr::vector<detail::HttpPrefixNotFoundHandler> views(
                detail::registrationResource());
            views.reserve(prefixNotFoundHandlers.size());
            for (const auto& [prefix, handler] : prefixNotFoundHandlers) {
                views.push_back({std::string_view(prefix), handler});
            }
            routes.setPrefixNotFoundHandlers(views);
        }
        if (!globalMiddlewares.empty()) {
            routes.setGlobalMiddlewares(globalMiddlewares);
        }
        routes.finalize();
        workerStates.emplace(memory.resource(), workerStateDefinitions);
    }
};

void TestApp::ImplDeleter::operator()(Impl* impl) const noexcept {
    delete impl;
}

TestApp::TestApp() : impl_(new Impl()) {}

TestApp::~TestApp() = default;

TestApp& TestApp::onError(HttpErrorHandler handler) {
    impl_->requireConfigurable();
    impl_->errorHandler = handler;
    return *this;
}

TestApp& TestApp::notFound(HttpNotFoundHandler handler) {
    impl_->requireConfigurable();
    impl_->notFoundHandler = handler;
    return *this;
}

TestApp& TestApp::onError(std::string_view prefix, HttpErrorHandler handler) {
    impl_->requireConfigurable();
    for (auto& [existing, existingHandler] : impl_->prefixErrorHandlers) {
        if (existing == prefix) {
            existingHandler = handler;
            return *this;
        }
    }
    impl_->prefixErrorHandlers.emplace_back(std::string(prefix), handler);
    return *this;
}

TestApp& TestApp::notFound(std::string_view prefix, HttpNotFoundHandler handler) {
    impl_->requireConfigurable();
    for (auto& [existing, existingHandler] : impl_->prefixNotFoundHandlers) {
        if (existing == prefix) {
            existingHandler = handler;
            return *this;
        }
    }
    impl_->prefixNotFoundHandlers.emplace_back(std::string(prefix), handler);
    return *this;
}

TestApp& TestApp::useMiddleware(detail::ControllerMiddlewareDescriptor descriptor) {
    impl_->requireConfigurable();
    impl_->globalMiddlewares.push_back(descriptor);
    return *this;
}

TestApp& TestApp::useWorkerStateDefinition(detail::WorkerStateDefinition definition) {
    impl_->requireConfigurable();
    for (const auto& existing : impl_->workerStateDefinitions) {
        if (existing.typeKey() == definition.typeKey()) {
            throw std::invalid_argument("worker state type is already registered");
        }
    }
    impl_->workerStateDefinitions.push_back(std::move(definition));
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
    const std::string_view target(request.target_);
    const auto queryAt = target.find('?');
    detail::HttpRequestAccess::setPath(
        parsed,
        queryAt == std::string_view::npos ? target : target.substr(0, queryAt));
    if (queryAt != std::string_view::npos) {
        detail::HttpRequestAccess::setQueryString(
            parsed, target.substr(queryAt + 1));
    }

    // Route each header through the parser's own classifier so known-header
    // slots (Content-Type, Cookie, Accept, ...) behave exactly as they do for
    // a parsed wire request.
    const auto addHeader = [&parsed](std::string_view name, std::string_view value) {
        const HttpHeaderView view{name, value};
        const auto kind = detail::classifyRequestHeader(name);
        const auto slot = detail::requestHeaderKindKnownSlot(kind);
        if (slot < detail::kRequestHeaderKindCount) {
            (void)detail::HttpRequestAccess::addHeader(parsed, view, slot);
        } else {
            (void)detail::HttpRequestAccess::addHeader(parsed, view);
        }
    };
    for (const auto& [name, value] : request.headers_) {
        addHeader(name, value);
    }
    if (!request.cookies_.empty()) {
        addHeader("Cookie", request.cookies_);
    }
    detail::HttpRequestAccess::setBody(parsed, request.body_);

    detail::ContextServices services =
        detail::ContextServices{}.withWorkerStates(*impl_->workerStates);

    const auto& routes = detail::RouterImpl::from(impl_->router).routeTable();
    const auto resolution = routes.resolve(parsed);

    asio::io_context context(1);
    auto future = asio::co_spawn(
        context,
        detail::taskAsAwaitable(routes.dispatchBufferedResponse(
            parsed, resolution, requestMemory, nullptr, services)),
        asio::use_future);
    context.run();
    auto response = future.get();

    // Copy everything out while the request arena is still alive.
    TestResponse result(response.status());
    result.headers_.reserve(response.headers().size());
    for (const auto& header : response.headers()) {
        result.headers_.emplace_back(
            std::string(header.name()), std::string(header.value()));
    }
    // Mirror wire semantics: the response writers suppress the body for HEAD
    // and content-forbidden statuses, so the facade must not surface one
    // either. Writer-synthesized fields (Content-Length, Date, Connection)
    // are framing concerns and stay absent here.
    const auto bodyPlan =
        detail::httpResponseBodyPlan(parsed.knownMethod(), response.status());
    if (!bodyPlan.bodySuppressed()) {
        const auto body = detail::responseBody(response).bytes();
        result.body_.assign(body.data(), body.size());
    }
    return result;
}

std::optional<std::string_view> TestResponse::header(
    std::string_view name) const & noexcept {
    for (const auto& [headerName, value] : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(headerName, name)) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

}  // namespace ruvia
