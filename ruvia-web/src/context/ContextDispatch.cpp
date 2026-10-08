#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Context.h"

#include "context/ContextServices.h"
#include "router/RouteTable.h"
#include "server/RequestDeadline.h"

namespace ruvia {

std::optional<std::string_view> DispatchResponse::header(std::string_view name) const& noexcept {
    for (const auto& [key, value] : headers_) {
        if (httpAsciiEqualsIgnoreCase(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

ScopedOperation<DispatchResponse> Context::dispatch(DispatchOptions options) {
    if (!services().routes() || !capabilities_.worker().isCurrent()) {
        throw std::logic_error("dispatch requires a routed context on its owning worker");
    }
    if (services().dispatchDepth() >= 8) {
        throw std::logic_error("subrequest nesting limit exceeded");
    }
    if (!isValidHttpMethodToken(options.method) || !options.target.starts_with('/') || options.target.starts_with("//") ||
        options.target.find_first_of("\r\n\t #") != std::string_view::npos || options.target.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("dispatch requires a valid method and origin-form target");
    }
    detail::validateOperationOptions(options.operation);
    if (options.body.size() > services().maxDecodedBodyBytes()) {
        throw std::invalid_argument("subrequest body exceeds the configured limit");
    }
    std::pmr::string wire(pool());
    wire.append(options.method).append(" ").append(options.target).append(" HTTP/1.1\r\nHost: localhost\r\n");
    for (const auto& header : options.headers) {
        if (!isValidHttpHeaderName(header.name()) || !isValidHttpHeaderValue(header.value())) {
            throw std::invalid_argument("invalid subrequest header");
        }
        for (const auto name : {"host", "content-length", "transfer-encoding", "connection", "upgrade", "expect", "trailer"}) {
            if (httpAsciiEqualsIgnoreCase(header.name(), name)) {
                throw std::invalid_argument("subrequest framing headers are owned by dispatch");
            }
        }
        wire.append(header.name()).append(": ").append(header.value()).append("\r\n");
    }
    if (wire.size() > 64 * 1024) {
        throw std::invalid_argument("subrequest headers exceed the configured limit");
    }
    wire.append("Content-Length: ").append(std::to_string(options.body.size())).append("\r\n\r\n").append(options.body);
    return ::ruvia::make_scoped_operation(operationScope_, dispatchTask(std::move(wire), std::move(options.operation)));
}

Task<DispatchResponse> Context::dispatchTask(std::pmr::string wire, OperationOptions options) {
    auto memory = memory_.fork();
    const auto parsed = Http1RequestParser{}.parse(std::string_view(wire));
    if (!parsed.parsed() || parsed.parsed()->consumedBytes() != wire.size()) {
        throw std::invalid_argument("invalid subrequest");
    }
    const auto& request = parsed.parsed()->request();
    const auto resolution = services().routes()->resolve(request);
    const auto stop = combineStopTokens(capabilities_.stop_token(), std::move(options.stopToken));
    if (stop.stopRequested()) {
        throw std::system_error(asio::error::operation_aborted);
    }
    detail::RequestDeadline deadline(stop);
    auto subrequest_services = services().for_subrequest(connInfo_, services().dispatchDepth() + 1, stop).withRequestDeadline(deadline);
    auto timeout = options.timeout;
    std::optional<HttpResponse> rejected;
    if (const auto* resolved = resolution.resolved()) {
        const auto& route = resolved->route();
        if (route.deadlineMs() > 0) {
            const auto routeTimeout = std::chrono::milliseconds(route.deadlineMs());
            if (!timeout || routeTimeout < *timeout) {
                timeout = routeTimeout;
            }
        }
        if (route.maxRequestBodyBytes() && parsed.parsed()->wireBody().size() > route.maxRequestBodyBytes()) {
            rejected = co_await services().routes()->handleError(request, memory,
                HttpErrorInfo({.status = http_status::kContentTooLarge, .message = "subrequest body exceeds route limit"}), subrequest_services);
        }
    }
    if (timeout) {
        deadline.arm(capabilities_.worker(), *timeout);
    }
    auto response = rejected ? std::move(*rejected) : co_await services().routes()->dispatch(request, resolution, memory, subrequest_services);
    if (response.fileBody()) {
        throw std::logic_error("dispatch requires a buffered response");
    }
    DispatchResponse result(pool());
    result.status_ = response.status();
    result.body_.assign(response.bodyBytes());
    for (const auto& header : response.headers()) {
        result.headers_.emplace_back(std::pmr::string(header.name(), pool()), std::pmr::string(header.value(), pool()));
    }
    co_return result;
}

}  // namespace ruvia
