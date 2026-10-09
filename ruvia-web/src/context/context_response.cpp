#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/web/context.h"

#include "context/context_response_state.h"
#include "context/context_services.h"
#include "http/http_error_response.h"
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
    response.transfer_headers_from(state_value.active_response(), transfer);
    state_value.finalize(std::move(response));
}

}  // namespace

void context::status(http_status_code status_code) {
    response_state().active_response().status(status_code);
}

std::pmr::string context::url_for(
    std::string_view pattern, std::initializer_list<std::string_view> values) const {
    if (services().routes() == nullptr) {
        throw std::logic_error("url_for requires a route table bound to this context");
    }
    return services().routes()->url_for(
        pattern, std::span<const std::string_view>(values.begin(), values.size()), arena());
}

context& context::remove_response_header(std::string_view name) {
    response_state().active_response().remove_header(name);
    return *this;
}

void context::remove_header(std::string_view name) {
    response_state().active_response().remove_header(name);
}

void context::header(std::string_view name, std::string_view value, header_options_type options) {
    response_state().active_response().header(
        name, value, http_response::header_options_type{.mode_ = options.mode_});
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

http_response context::body(std::nullptr_t) const {
    http_response response({.resource_ = arena()});
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::body(std::pmr::string&& body) const {
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

http_response context::body_static_view(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.static_body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::text(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "text/plain; charset=UTF-8");
    response.body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::text(std::pmr::string&& body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "text/plain; charset=UTF-8");
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::text_static_view(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "text/plain; charset=UTF-8");
    response.static_body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::json_serialized(std::pmr::string& body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "application/json");
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::html(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "text/html; charset=UTF-8");
    response.body(body);
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::html(std::pmr::string&& body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "text/html; charset=UTF-8");
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    return response;
}

http_response context::html_static_view(std::string_view body) const {
    http_response response({.resource_ = arena()});
    response.header("Content-Type", "text/html; charset=UTF-8");
    response.static_body(body);
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
    if (services().not_found_handler() != nullptr) {
        co_return co_await services().not_found_handler()(*this);
    }

    auto response = detail::make_default_error_response(arena(),
        http_error_info({.status_ = http_status::not_found, .message_ = "route not found"}));
    apply_response_state(response, http_status::not_found);
    co_return response;
}

http_response context::streaming_head(std::string_view content_type_value) const {
    http_response response({.resource_ = arena()});
    if (!content_type_value.empty()) {
        response.header("Content-Type", content_type_value);
    }
    apply_response_state(response, std::nullopt);
    return response;
}

context& context::set_stable_response_header(std::string_view name, std::string_view value) {
    response_state().active_response().header_stable_view(name, value);
    return *this;
}

void context::apply_response_state(
    http_response& response, std::optional<http_status_code> status_code) const {
    const auto& active_response = response_state().active_response();
    const auto final_status_code = status_code.value_or(active_response.status());
    response.status(final_status_code);
    response.transfer_headers_from(active_response, http_response_header_transfer::apply);
}

namespace detail {

void apply_middleware_response(context& context_value, http_response&& response) {
    context_value.respond(std::move(response));
}

}  // namespace detail

}  // namespace ruvia
