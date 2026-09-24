#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextResponseState.h"
#include "ruvia/web/detail/http/error/HttpErrorResponse.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/router/RouteTable.h"

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byteBodyView(std::span<const std::byte> body) noexcept {
    return body.empty() ? std::string_view{}
                        : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
}

void finalizeContextResponse(detail::ContextResponseState& state, HttpResponse&& response,
    HttpResponseHeaderTransfer transfer) {
    if (&response == &state.activeResponse()) {
        state.finalizeActive();
        return;
    }
    response.transferHeadersFrom(state.activeResponse(), transfer);
    state.finalize(std::move(response));
}

}  // namespace

void Context::status(HttpStatusCode statusCode) {
    responseState().activeResponse().status(statusCode);
}

void* Context::workerStateInstance(const void* typeKey) const {
    auto* instance = workerStates_ == nullptr ? nullptr : workerStates_->instance(typeKey);
    if (instance == nullptr) {
        throw std::logic_error(
            "worker state type is not registered: call App::useWorkerState<T>() before App::run()");
    }
    return instance;
}

BlockingPool& Context::blockingPool() const {
    if (blockingPool_ == nullptr) {
        throw std::logic_error("blocking pool is disabled");
    }
    return *blockingPool_;
}

std::pmr::string Context::urlFor(
    std::string_view pattern, std::initializer_list<std::string_view> values) const {
    if (routes_ == nullptr) {
        throw std::logic_error("urlFor requires a route table bound to this context");
    }
    return routes_->urlFor(
        pattern, std::span<const std::string_view>(values.begin(), values.size()), arena());
}

Context& Context::removeResponseHeader(std::string_view name) {
    responseState().activeResponse().removeHeader(name);
    return *this;
}

void Context::removeHeader(std::string_view name) {
    responseState().activeResponse().removeHeader(name);
}

void Context::header(std::string_view name, std::string_view value, HeaderOptions options) {
    responseState().activeResponse().header(
        name, value, HttpResponse::HeaderOptions{.mode = options.mode});
}

void Context::storeResponse(HttpResponse&& response) {
    finalizeContextResponse(responseState(), std::move(response), HttpResponseHeaderTransfer::kMerge);
}

void Context::storeAssignedResponse(HttpResponse&& response) {
    finalizeContextResponse(responseState(), std::move(response), HttpResponseHeaderTransfer::kAssign);
}

HttpResponse Context::body(std::string_view body) const {
    HttpResponse response({.resource = arena()});
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::nullptr_t) const {
    HttpResponse response({.resource = arena()});
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::pmr::string&& body) const {
    HttpResponse response({.resource = arena()});
    response.ownedBody(std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::span<const std::byte> body) const {
    HttpResponse response({.resource = arena()});
    response.body(byteBodyView(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::bodyStaticView(std::string_view body) const {
    HttpResponse response({.resource = arena()});
    response.staticBody(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::text(std::string_view body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "text/plain; charset=UTF-8");
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::text(std::pmr::string&& body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "text/plain; charset=UTF-8");
    response.ownedBody(std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::textStaticView(std::string_view body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "text/plain; charset=UTF-8");
    response.staticBody(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::jsonSerialized(std::pmr::string& body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "application/json");
    response.ownedBody(std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::html(std::string_view body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "text/html; charset=UTF-8");
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::html(std::pmr::string&& body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "text/html; charset=UTF-8");
    response.ownedBody(std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::htmlStaticView(std::string_view body) const {
    HttpResponse response({.resource = arena()});
    response.header("Content-Type", "text/html; charset=UTF-8");
    response.staticBody(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::error(HttpErrorInfoOptions options) const {
    auto response = detail::makeDefaultErrorResponse(arena(), HttpErrorInfo(options));
    applyResponseState(response, response.status());
    return response;
}

ScopedOperation<HttpResponse> Context::notFound() {
    return detail::makeScopedOperation(operationScope_, notFoundTask());
}

Task<HttpResponse> Context::notFoundTask() {
    if (notFoundHandler_ != nullptr) {
        co_return co_await notFoundHandler_(*this);
    }

    auto response = detail::makeDefaultErrorResponse(arena(),
        HttpErrorInfo({.status = http_status::kNotFound, .message = "route not found"}));
    applyResponseState(response, http_status::kNotFound);
    co_return response;
}

HttpResponse Context::streamingHead(std::string_view contentType) const {
    HttpResponse response({.resource = arena()});
    if (!contentType.empty()) {
        response.header("Content-Type", contentType);
    }
    applyResponseState(response, std::nullopt);
    return response;
}

Context& Context::setStableResponseHeader(std::string_view name, std::string_view value) {
    detail::setResponseHeaderStableView(responseState().activeResponse(), name, value);
    return *this;
}

void Context::applyResponseState(
    HttpResponse& response, std::optional<HttpStatusCode> statusCode) const {
    const auto& activeResponse = responseState().activeResponse();
    const auto finalStatusCode = statusCode.value_or(activeResponse.status());
    response.status(finalStatusCode);
    response.transferHeadersFrom(activeResponse, HttpResponseHeaderTransfer::kApply);
}

namespace detail {

void applyMiddlewareResponse(Context& context, HttpResponse&& response) {
    context.respond(std::move(response));
}

}  // namespace detail

}  // namespace ruvia
