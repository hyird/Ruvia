#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/web/context.h"
#include "ruvia/web/session.h"

#include "context/context_access.h"
#include "context/context_response_state.h"
#include "context/context_services.h"
#include "http/http_error_response.h"
#include "http/session_access.h"
#include "router/route_table.h"

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byte_body_view(std::span<const std::byte> body) noexcept {
    return body.empty() ? std::string_view{}
                        : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
}

void finalize_context_response(detail::context_response_state& state_value, http_response&& response,
    http_response_header_transfer transfer) {
    if (&response == &state_value.active_response()) {
        state_value.finalize_active();
        return;
    }
    if (const auto& metadata = state_value.metadata(); !metadata.headers().empty()) {
        response.transfer_headers_from(metadata, transfer);
    }
    state_value.finalize(std::move(response));
}

}  // namespace

void context::status(http_status_code status_code) {
    response_state().update_metadata([status_code](http_response& response) { response.status(status_code); });
}

std::pmr::string context::url_for(
    std::string_view pattern, std::initializer_list<std::string_view> values) const {
    if (services().routes() == nullptr) {
        throw std::logic_error("url_for requires a route table bound to this context");
    }
    return services().routes()->url_for(
        pattern, std::span<const std::string_view>(values.begin(), values.size()), arena());
}

void context::remove_header(std::string_view name) {
    response_state().update_metadata([name](http_response& response) { response.remove_header(name); });
}

void context::header(std::string_view name, std::string_view value, header_options_type options) {
    response_state().update_metadata(
        [&](http_response& response) { response.header(name, value, options); });
}

void context::store_response(http_response&& response) {
    finalize_context_response(response_state(), std::move(response), http_response_header_transfer::merge);
}

void context::store_assigned_response(http_response&& response) {
    finalize_context_response(response_state(), std::move(response), http_response_header_transfer::assign);
}

http_response context::body(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::body_owned(std::pmr::string&& body) const {
    http_response response({.resource_ = arena()});
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::body(std::span<const std::byte> body) const {
    http_response response({.resource_ = arena()});
    response.body(byte_body_view(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::body(static_text body) const {
    http_response response({.resource_ = arena()});
    response.static_body(body.view());
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::text(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::text_plain_utf8);
    response.body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::text_owned(std::pmr::string&& body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::text_plain_utf8);
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::text(static_text body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::text_plain_utf8);
    response.static_body(body.view());
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::json_serialized(std::pmr::string& body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::application_json);
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::html(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::text_html_utf8);
    response.body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::html_owned(std::pmr::string&& body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::text_html_utf8);
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::html(static_text body) const {
    http_response response({.resource_ = arena()});
    response.content_type(http_content_type::text_html_utf8);
    response.static_body(body.view());
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::error(http_error_info_options options) const {
    auto response = detail::make_default_error_response(arena(), http_error_info(options));
    apply_response_state(response, response.status());
    return response;
}

scoped_operation<http_response> context::not_found() {
    return ::ruvia::make_scoped_operation(operation_scope_, not_found_task());
}

task<http_response> context::not_found_task() {
    // Selected only here, on demand: every matched request carries the table,
    // and few of them ever ask for a not-found answer.
    const auto* routes = services().routes();
    const auto handler = routes == nullptr ? detail::http_not_found_handler_ref_type{nullptr}
                                           : routes->not_found_handler_for(request_.path());
    if (handler != nullptr) {
        co_return co_await handler(*this);
    }

    auto response = detail::make_default_error_response(arena(),
        http_error_info({.status_ = http_status::not_found, .message_ = "route not found"}));
    apply_response_state(response, http_status::not_found);
    co_return response;
}

http_response context::streaming_head() const {
    http_response response({.resource_ = arena()});
    apply_response_state(response, std::nullopt);
    return response;
}

task<http_response> detail::context_access::commit_streaming_head(context& context_value) {
    if (context_value.try_session()) {
        co_await session_access::commit(context_value);
    }
    co_return context_value.streaming_head();
}

context& context::set_stable_response_header(std::string_view name, std::string_view value) {
    response_state().update_metadata([&](http_response& response) { response.header_stable_view(name, value); });
    return *this;
}

void context::apply_response_state(
    http_response& response, std::optional<http_status_code> status_code) const {
    const auto& metadata = response_state().metadata();
    response.status(status_code.value_or(metadata.status()));
    if (!metadata.headers().empty()) {
        response.transfer_headers_from(metadata, http_response_header_transfer::apply);
    }
}

namespace detail {

void apply_middleware_response(context& context_value, http_response&& response) {
    context_value.respond(std::move(response));
}

}  // namespace detail

}  // namespace ruvia
