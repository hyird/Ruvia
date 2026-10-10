#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "router/route_early_data.h"
#include "router/route_table.h"
#include "server/request_deadline.h"

namespace ruvia {

std::optional<std::string_view> dispatch_response::header(std::string_view name) const& noexcept {
    for (const auto& [key, value] : headers_) {
        if (http_ascii_equals_ignore_case(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

scoped_operation<dispatch_response> context::dispatch(dispatch_options options) {
    if (!services().routes() || !capabilities_.worker().is_current()) {
        throw std::logic_error("dispatch requires a routed context on its owning worker");
    }
    if (services().dispatch_depth() >= 8) {
        throw std::logic_error("subrequest nesting limit exceeded");
    }
    if (!is_valid_http_method_token(options.method_) || !options.target_.starts_with('/') || options.target_.starts_with("//") ||
        options.target_.find_first_of("\r\n\t #") != std::string_view::npos || options.target_.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("dispatch requires a valid method and origin-form target");
    }
    detail::validate_operation_options(options.operation_);
    if (options.body_.size() > services().max_decoded_body_bytes()) {
        throw std::invalid_argument("subrequest body exceeds the configured limit");
    }
    std::pmr::string wire(pool());
    wire.append(options.method_).append(" ").append(options.target_).append(" HTTP/1.1\r\nHost: localhost\r\n");
    for (const auto& header : options.headers_) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid subrequest header");
        }
        for (const auto name : {"host", "content-length", "transfer-encoding", "connection", "upgrade", "expect", "trailer"}) {
            if (http_ascii_equals_ignore_case(header.name(), name)) {
                throw std::invalid_argument("subrequest framing headers are owned by dispatch");
            }
        }
        wire.append(header.name()).append(": ").append(header.value()).append("\r\n");
    }
    if (wire.size() > 64 * 1024) {
        throw std::invalid_argument("subrequest headers exceed the configured limit");
    }
    wire.append("Content-Length: ").append(std::to_string(options.body_.size())).append("\r\n\r\n").append(options.body_);
    return ::ruvia::make_scoped_operation(operation_scope_, dispatch_task(std::move(wire), std::move(options.operation_)));
}

task<dispatch_response> context::dispatch_task(std::pmr::string wire, operation_options options) {
    auto memory = memory_.fork();
    const auto parsed_value = http1_request_parser{}.parse(std::string_view(wire));
    if (!parsed_value.parsed() || parsed_value.parsed()->consumed_bytes() != wire.size()) {
        throw std::invalid_argument("invalid subrequest");
    }
    const auto& request = parsed_value.parsed()->request();
    const auto resolution = services().routes()->resolve(request);
    const auto stop = combine_stop_tokens(capabilities_.stop_token(), std::move(options.stop_token_));
    if (stop.stop_requested()) {
        throw std::system_error(asio::error::operation_aborted);
    }
    detail::request_deadline deadline(stop);
    auto subrequest_services = services().for_subrequest(conn_info_, early_data_info_, services().dispatch_depth() + 1, stop).with_request_deadline(deadline);
    auto timeout = options.timeout_;
    std::optional<http_response> rejected;
    if (early_data_info_.received_from_early_data() &&
        !detail::early_data_request_allowed(request.known_method(), !parsed_value.parsed()->wire_body().empty(), resolution)) {
        // The parent passed the early-data policy; work it dispatches is still
        // replayable and must meet the same policy (RFC 8470 §5.1).
        rejected = co_await services().routes()->handle_error(request, memory,
            http_error_info({.status_ = http_status::too_early, .message_ = "subrequest is not replay-safe for early data"}), subrequest_services);
    } else if (const auto* resolved = resolution.resolved()) {
        const auto& route = resolved->route();
        if (route.deadline_ms() > 0) {
            const auto route_timeout = std::chrono::milliseconds(route.deadline_ms());
            if (!timeout || route_timeout < *timeout) {
                timeout = route_timeout;
            }
        }
        if (route.max_request_body_bytes() && parsed_value.parsed()->wire_body().size() > route.max_request_body_bytes()) {
            rejected = co_await services().routes()->handle_error(request, memory,
                http_error_info({.status_ = http_status::content_too_large, .message_ = "subrequest body exceeds route limit"}), subrequest_services);
        }
    }
    if (timeout) {
        deadline.arm(capabilities_.worker(), *timeout);
    }
    auto response = rejected ? std::move(*rejected) : co_await services().routes()->dispatch(request, resolution, memory, subrequest_services);
    if (response.file_body()) {
        throw std::logic_error("dispatch requires a buffered response");
    }
    dispatch_response result(pool());
    result.status_ = response.status();
    result.body_.assign(response.body_bytes());
    for (const auto& header : response.headers()) {
        result.headers_.emplace_back(std::pmr::string(header.name(), pool()), std::pmr::string(header.value(), pool()));
    }
    co_return result;
}

}  // namespace ruvia
