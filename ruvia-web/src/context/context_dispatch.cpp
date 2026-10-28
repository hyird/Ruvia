#include <array>
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
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_protocol_managed_fields.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "router/route_table.h"
#include "server/http_request_admission.h"

namespace ruvia {

namespace {

// Subrequest framing headers are owned by dispatch, as on the wire.
constexpr std::size_t max_subrequest_header_bytes = 64 * 1024;
}  // namespace

std::optional<std::string_view> dispatch_response::header(std::string_view name) const& noexcept {
    for (const auto& header : headers_) {
        if (http_ascii_equals_ignore_case(header.name(), name)) {
            return header.value();
        }
    }
    return std::nullopt;
}

task<dispatch_response> context::dispatch(dispatch_options options) {
    if (!services().routes() || !capabilities_.worker().is_current()) {
        throw std::logic_error("dispatch requires a routed context on its owning worker");
    }
    if (services().dispatch_depth() >= 8) {
        throw std::logic_error("subrequest nesting limit exceeded");
    }
    if (!is_valid_http_method_token(options.method_) ||
        classify_http_method(options.method_) == http_known_method::connect ||
        !is_valid_http_origin_form_target(options.target_)) {
        throw std::invalid_argument("dispatch requires a valid method and origin-form target");
    }
    if (options.body_.size() > services().max_decoded_body_bytes()) {
        throw std::invalid_argument("subrequest body exceeds the configured limit");
    }
    std::size_t header_bytes = options.method_.size() + options.target_.size();
    for (const auto& header : options.headers_) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid subrequest header");
        }
        if (is_http_protocol_managed_request_field(header.name())) {
            throw std::invalid_argument("subrequest framing headers are owned by dispatch");
        }
        header_bytes += header.name().size() + header.value().size();
        if (header_bytes > max_subrequest_header_bytes) {
            throw std::invalid_argument("subrequest headers exceed the configured limit");
        }
    }
    // The operation is lazy and the options borrow caller storage: take owned
    // copies now. The subrequest is built directly as a parsed request; it
    // never round-trips through a wire format.
    std::pmr::string method(options.method_, pool());
    std::pmr::string target(options.target_, pool());
    std::pmr::string body(options.body_, pool());
    std::pmr::vector<http_header> headers(pool());
    if (!options.headers_.empty()) {
        headers.reserve(options.headers_.size());
        for (const auto& header : options.headers_) {
            headers.emplace_back(header.name(), header.value(), pool());
        }
    }
    return ::ruvia::make_scoped_operation(operation_scope_,
        dispatch_task(std::move(method), std::move(target), std::move(body), std::move(headers),
            std::move(options.stop_token_)));
}

task<dispatch_response> context::dispatch_task(std::pmr::string method, std::pmr::string target,
    std::pmr::string body_storage, std::pmr::vector<http_header> headers,
    ::ruvia::stop_token stop_token_value) {
    auto memory = memory_.fork();
    const std::string_view body = body_storage;
    std::array<char, http_max_unsigned_decimal_size> content_length_text;
    const auto content_length = http_format_unsigned(content_length_text, body.size());
    const auto header_count = 2U + headers.size();
    static constexpr std::size_t inline_header_capacity = 16;
    std::array<http_header_view, inline_header_capacity> inline_headers{};
    std::pmr::vector<http_header_view> heap_headers(memory.resource());
    std::span<const http_header_view> active_headers;
    if (header_count <= inline_header_capacity) {
        std::size_t pos = 0;
        inline_headers[pos++] = http_header_view("Host", "localhost");
        for (const auto& header : headers) {
            inline_headers[pos++] = http_header_view(header.name(), header.value());
        }
        inline_headers[pos++] = http_header_view("Content-Length", content_length);
        active_headers = std::span<const http_header_view>(inline_headers.data(), pos);
    } else {
        heap_headers.reserve(header_count);
        heap_headers.emplace_back("Host", "localhost");
        for (const auto& header : headers) {
            heap_headers.emplace_back(header.name(), header.value());
        }
        heap_headers.emplace_back("Content-Length", content_length);
        active_headers = heap_headers;
    }
    const auto request = [&]() {
        try {
            return make_parsed_http_request(method, target, active_headers,
                std::as_bytes(std::span(body.data(), body.size())), memory.resource());
        } catch (const http_protocol_error&) {
            throw std::invalid_argument("invalid subrequest");
        }
    }();
    const auto& routes = *services().routes();
    const auto resolution = routes.resolve(request);
    if (const auto* resolved = resolution.resolved()) {
        const auto& endpoint = resolved->route().endpoint();
        if (endpoint.buffered() == nullptr ||
            endpoint.request_body_mode() != detail::request_body_mode::buffered) {
            throw std::logic_error("dispatch requires a buffered route");
        }
    }
    const auto stop = combine_stop_tokens(capabilities_.stop_token(), std::move(stop_token_value));
    if (stop.stop_requested()) {
        throw std::system_error(asio::error::operation_aborted);
    }
    // The parent request already passed the application rate limit and the
    // target-origin check of its connection.
    const detail::http_request_admission_policy policy{
        .max_buffered_body_bytes_ = services().max_decoded_body_bytes(),
        .apply_rate_limit_ = false,
        .verify_target_origin_ = false,
    };
    http_response response(memory.resource());
    auto admission = co_await detail::admit_and_dispatch_buffered_request(routes,
        {
            .request_ = request,
            .resolution_ = resolution,
            .coding_ = http_response_coding_selection_for(request),
            .resource_ = memory.resource(),
        },
        policy, services().for_subrequest(stop), memory, response);
    if (detail::http_response_needs_not_acceptable(admission.tail().coding_policy(), request, response) &&
        !response.status().is_error()) {
        response = co_await routes.handle_error(request, memory,
            http_error_info(detail::http_not_acceptable_coding_error()), admission.services());
    }
    // The error handler answers the 406 with any response shape, so the file
    // check covers whichever response is final.
    if (response.file_body() || response.has_multipart_file_body()) {
        throw std::logic_error("dispatch requires a buffered response");
    }
    const auto known_method = request.known_method();
    {
        const auto discarded_method = std::move(method);
        const auto discarded_target = std::move(target);
        const auto discarded_body = std::move(body_storage);
        const auto discarded_headers = std::move(headers);
    }
    dispatch_response result(pool());
    result.status_ = response.status();
    // As on the wire: HEAD and content-forbidden statuses carry no body.
    if (!plan_http_response_body(known_method, response.status()).body_suppressed()) {
        result.body_.assign(response.body_bytes());
    }
    if (!response.headers().empty()) {
        result.headers_.reserve(response.headers().size());
        for (const auto& header : response.headers()) {
            result.headers_.emplace_back(header.name(), header.value(), pool());
        }
    }
    co_return result;
}

}  // namespace ruvia
