#include "ruvia/web/Context.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/detail/http/error/HttpErrorResponse.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byteBodyView(std::span<const std::byte> body) noexcept {
    return body.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
}

[[nodiscard]] bool responseHasHeaderName(const HttpResponse& response, std::string_view name) noexcept {
    return std::ranges::any_of(response.headers(), [name](const auto& header) noexcept { return detail::httpAsciiEqualsIgnoreCase(header.name(), name); });
}

[[nodiscard]] std::size_t responseHeaderValueCount(const HttpResponse& response, std::string_view name, std::string_view value) noexcept {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), name) && header.value() == value) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::size_t headerOccurrenceThrough(const HttpResponse& source, const HttpResponseHeader& target) noexcept {
    std::size_t count = 0;
    for (const auto& candidate : source.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(candidate.name(), target.name()) && candidate.value() == target.value()) {
            ++count;
        }
        if (&candidate == &target) {
            break;
        }
    }
    return count;
}

// A Context-built response already contains the active state. A raw response
// does not. Merge by occurrence count so both paths converge without treating
// repeated equal append fields as a set.
void mergeActiveResponseHeaders(HttpResponse& response, const HttpResponse& active) {
    const auto activeHeaderCount = active.headers().size();
    if (activeHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + activeHeaderCount);
    }
    for (const auto& header : active.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie) {
            detail::upsertResponseSetCookieValidated(response, value);
        } else if (detail::responseHeaderAppend(header)) {
            if (responseHeaderValueCount(response, name, value) < headerOccurrenceThrough(active, header)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
        } else if (!responseHasHeaderName(response, name)) {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

void assignActiveResponseHeaders(HttpResponse& response, const HttpResponse& active) {
    const auto activeHeaderCount = active.headers().size();
    if (activeHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + activeHeaderCount);
    }

    bool replacedSetCookie = false;
    for (const auto& header : active.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        if (knownBit == detail::kResponseHeaderContentType) {
            continue;
        }
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie) {
            if (!replacedSetCookie) {
                response.header("Set-Cookie", std::nullopt);
                replacedSetCookie = true;
            }
            detail::upsertResponseSetCookieValidated(response, value);
        } else if (detail::responseHeaderAppend(header)) {
            if (responseHeaderValueCount(response, name, value) < headerOccurrenceThrough(active, header)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

}  // namespace

void Context::status(HttpStatusCode statusCode) {
    responseState_.activeResponse().status(statusCode);
}

void* Context::workerStateInstance(const void* typeKey) const {
    auto* instance = workerStates_ == nullptr ? nullptr : workerStates_->instance(typeKey);
    if (instance == nullptr) {
        throw std::logic_error("worker state type is not registered: call App::useWorkerState<T>() before App::run()");
    }
    return instance;
}

BlockingPool& Context::blockingPool() const {
    if (blockingPool_ == nullptr) {
        throw std::logic_error("no blocking pool is configured: call App::setBlockingPool() before App::run()");
    }
    return *blockingPool_;
}

std::pmr::string Context::urlFor(std::string_view pattern, std::initializer_list<std::string_view> values) const {
    if (routes_ == nullptr) {
        throw std::logic_error("urlFor requires a route table bound to this context");
    }
    return routes_->urlFor(pattern, std::span<const std::string_view>(values.begin(), values.size()), resource());
}

Context& Context::removeResponseHeader(std::string_view name) {
    responseState_.activeResponse().header(name, std::nullopt);
    return *this;
}

void Context::header(std::string_view name, std::string_view value, HeaderOptions options) {
    responseState_.activeResponse().header(name, value, HttpResponse::HeaderOptions{.append = options.append});
}

void Context::header(std::string_view name, std::nullopt_t) {
    removeResponseHeader(name);
}

void Context::storeResponse(HttpResponse&& response) {
    if (&response == &responseState_.activeResponse()) {
        responseState_.finalizeActive();
        return;
    }
    mergeActiveResponseHeaders(response, responseState_.activeResponse());
    responseState_.finalize(std::move(response));
}

void Context::storeAssignedResponse(HttpResponse&& response) {
    if (&response == &responseState_.activeResponse()) {
        responseState_.finalizeActive();
        return;
    }
    assignActiveResponseHeaders(response, responseState_.activeResponse());
    responseState_.finalize(std::move(response));
}

HttpResponse Context::body(std::string_view body) const {
    HttpResponse response(resource());
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::nullptr_t) const {
    HttpResponse response(resource());
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::pmr::string&& body) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::span<const std::byte> body) const {
    HttpResponse response(resource());
    response.body(byteBodyView(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::bodyStaticView(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::text(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::text(std::pmr::string&& body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::textStaticView(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::jsonSerialized(std::pmr::string& body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "application/json");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::html(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::html(std::pmr::string&& body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::htmlStaticView(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::error(HttpStatusCode statusCode, std::string_view code, std::string_view message, std::string_view statusText) const {
    auto response = detail::makeDefaultErrorResponse(resource(), HttpErrorInfo(statusCode, code, message, statusText));
    applyResponseState(response, statusCode);
    return response;
}

Task<HttpResponse> Context::notFound() {
    if (notFoundHandler_ != nullptr) {
        co_return co_await notFoundHandler_(*this);
    }

    auto response = detail::makeDefaultErrorResponse(resource(), HttpErrorInfo(http_status::kNotFound, {}, "route not found"));
    applyResponseState(response, http_status::kNotFound);
    co_return response;
}

HttpResponse Context::streamingHead(std::string_view contentType) const {
    HttpResponse response(resource());
    if (!contentType.empty()) {
        response.header("Content-Type", contentType);
    }
    applyResponseState(response, std::nullopt);
    return response;
}

Context& Context::setStableResponseHeader(std::string_view name, std::string_view value) {
    detail::setResponseHeaderStableView(responseState_.activeResponse(), name, value);
    return *this;
}

void Context::applyResponseState(HttpResponse& response, std::optional<HttpStatusCode> statusCode) const {
    const auto& activeResponse = responseState_.activeResponse();
    const auto finalStatusCode = statusCode.value_or(activeResponse.status());
    response.status(finalStatusCode);
    const auto contextHeaderCount = activeResponse.headers().size();
    if (contextHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
    }
    for (const auto& header : activeResponse.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
            detail::appendResponseHeaderValidated(response, name, value, knownBit);
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

}  // namespace ruvia
