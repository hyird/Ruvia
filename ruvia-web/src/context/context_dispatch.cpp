#include <array>
#include <charconv>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/error.hpp>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "router/route_table.h"
#include "server/http_request_admission.h"
#include "server/request_deadline.h"

namespace ruvia {

namespace {

// Subrequest framing headers are owned by dispatch, as on the wire.
constexpr std::size_t subrequest_field_method = 0;
constexpr std::size_t subrequest_field_target = 1;
constexpr std::size_t subrequest_field_body = 2;
constexpr std::size_t subrequest_field_headers = 3;
constexpr std::size_t max_subrequest_header_bytes = 64 * 1024;

}  // namespace

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
    validate_operation_options(options.operation_);
    if (options.body_.size() > services().max_decoded_body_bytes()) {
        throw std::invalid_argument("subrequest body exceeds the configured limit");
    }
    // The operation is lazy and the options borrow caller storage: take owned
    // copies now. The subrequest is built directly as a parsed request; it
    // never round-trips through a wire format.
    std::pmr::vector<std::pmr::string> fields(pool());
    fields.reserve(subrequest_field_headers + 2 * options.headers_.size());
    fields.emplace_back(options.method_);
    fields.emplace_back(options.target_);
    fields.emplace_back(options.body_);
    std::size_t header_bytes = options.method_.size() + options.target_.size();
    for (const auto& header : options.headers_) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid subrequest header");
        }
        for (const auto name : {"host", "content-length", "transfer-encoding", "connection", "upgrade", "expect", "trailer"}) {
            if (http_ascii_equals_ignore_case(header.name(), name)) {
                throw std::invalid_argument("subrequest framing headers are owned by dispatch");
            }
        }
        header_bytes += header.name().size() + header.value().size();
        if (header_bytes > max_subrequest_header_bytes) {
            throw std::invalid_argument("subrequest headers exceed the configured limit");
        }
        fields.emplace_back(header.name());
        fields.emplace_back(header.value());
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, dispatch_task(std::move(fields), std::move(options.operation_)));
}

task<dispatch_response> context::dispatch_task(std::pmr::vector<std::pmr::string> fields, operation_options options) {
    auto memory = memory_.fork();
    const std::string_view body = fields[subrequest_field_body];
    std::array<char, 20> content_length_text{};
    const auto content_length_end =
        std::to_chars(content_length_text.data(), content_length_text.data() + content_length_text.size(), body.size()).ptr;
    std::pmr::vector<http_header_view> headers(memory.resource());
    headers.reserve(2 + (fields.size() - subrequest_field_headers) / 2);
    headers.emplace_back("Host", "localhost");
    for (auto index = subrequest_field_headers; index + 1 < fields.size(); index += 2) {
        headers.emplace_back(fields[index], fields[index + 1]);
    }
    headers.emplace_back("Content-Length",
        std::string_view(content_length_text.data(), static_cast<std::size_t>(content_length_end - content_length_text.data())));
    const auto [request, parse_error] = make_parsed_http_request(fields[subrequest_field_method],
        fields[subrequest_field_target], headers, std::as_bytes(std::span(body.data(), body.size())), memory.resource());
    if (parse_error.has_value()) {
        throw std::invalid_argument("invalid subrequest");
    }
    const auto& routes = *services().routes();
    const auto resolution = routes.resolve(request);
    const auto stop = combine_stop_tokens(capabilities_.stop_token(), std::move(options.stop_token_));
    if (stop.stop_requested()) {
        throw std::system_error(asio::error::operation_aborted);
    }
    // The parent request already passed the application rate limit. Work it
    // dispatches is still replayable when the parent arrived as early data and
    // must meet the same policy (RFC 8470 §5.1); the operation timeout is the
    // subrequest's deployment deadline, which a route may only tighten.
    const detail::http_request_admission_policy policy{
        .handler_deadline_ = options.timeout_,
        .max_buffered_body_bytes_ = services().max_decoded_body_bytes(),
        .apply_rate_limit_ = false,
    };
    std::optional<detail::request_deadline> deadline;
    auto admission = detail::admit_http_request(
        {
            .request_ = request,
            .resolution_ = resolution,
            .coding_ = http_response_coding_selection_for(request),
            .resource_ = memory.resource(),
            .received_early_data_ = early_data_info_.received_from_early_data(),
        },
        policy, services().for_subrequest(conn_info_, early_data_info_, services().dispatch_depth() + 1, stop), deadline);
    const auto* rejection = admission.rejection();
    auto response = rejection != nullptr
                        ? co_await detail::respond_http_request_rejection(
                              routes, request, memory, *rejection, admission.services())
                        : co_await routes.dispatch(request, resolution, memory, admission.services());
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
