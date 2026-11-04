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

// text() and stream_text(): plain UTF-8 text plus nosniff, so a browser never
// sniffs application text into an active content type. An
// X-Content-Type-Options the application set keeps its value.
void mark_plain_text(http_response& response) {
    response.content_type(http_content_type::text_plain_utf8);
    if (!response.header("X-Content-Type-Options")) {
        response.header_stable_view("X-Content-Type-Options", "nosniff");
    }
}

}  // namespace

http_response& context_response_editor::target() const {
    if (owner_->response_output().committed()) {
        throw std::logic_error("response metadata is already committed");
    }
    if (target_ == target_type::draft) {
        return owner_->response_state().draft();
    }
    if (target_ == target_type::policy) {
        return owner_->response_state().policy();
    }
    auto* response = owner_->response_state().final();
    if (response == nullptr) {
        throw std::logic_error("no selected response to edit");
    }
    return *response;
}

void context_response_editor::status(http_status_code status_code) {
    target().status(status_code);
    if (target_ == target_type::policy) {
        owner_->response_state().policy_status(status_code);
    }
}

std::pmr::string context::url_for(
    std::string_view pattern, std::span<const std::string_view> values) const {
    if (services().routes() == nullptr) {
        throw std::logic_error("url_for requires a route table bound to this context");
    }
    return services().routes()->url_for(pattern, values, arena());
}

std::pmr::string context::url_for(
    std::string_view pattern, std::initializer_list<std::string_view> values) const {
    return url_for(pattern, std::span<const std::string_view>(values.begin(), values.size()));
}

bool context::committed() const noexcept {
    return response_output().committed();
}

void context_response_editor::remove_header(std::string_view name) {
    target().remove_header(name);
}

void context_response_editor::header(std::string_view name, std::string_view value, http_response_header_options options) {
    target().header(name, value, options);
}

void context::store_response(http_response&& response) {
    if (response_output().committed()) {
        throw std::logic_error("response is already committed");
    }
    if (&response == response_state().final()) {
        return;
    }
    response_state().finalize(std::move(response));
}

http_response context::body(std::string_view body) {
    http_response response(arena());
    response.body(body);
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::body_owned(std::pmr::string&& body) {
    http_response response(arena());
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::body(std::span<const std::byte> body) {
    http_response response(arena());
    response.body(byte_body_view(body));
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::body(static_text body) {
    http_response response(arena());
    response.static_body(body.view());
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::text(std::string_view body) {
    http_response response(arena());
    mark_plain_text(response);
    response.body(body);
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::text_owned(std::pmr::string&& body) {
    http_response response(arena());
    mark_plain_text(response);
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::text(static_text body) {
    http_response response(arena());
    mark_plain_text(response);
    response.static_body(body.view());
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

response_stream_writer& context::stream_text() {
    auto& writer = stream();
    if (!response_output().committed()) {
        mark_plain_text(response_state().draft());
    }
    return writer;
}

http_response context::json_serialized(std::pmr::string& body) {
    http_response response(arena());
    response.content_type(http_content_type::application_json);
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::html(std::string_view body) {
    http_response response(arena());
    response.content_type(http_content_type::text_html_utf8);
    response.body(body);
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::html_owned(std::pmr::string&& body) {
    http_response response(arena());
    response.content_type(http_content_type::text_html_utf8);
    response.owned_body(std::move(body));
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::html(static_text body) {
    http_response response(arena());
    response.content_type(http_content_type::text_html_utf8);
    response.static_body(body.view());
    apply_response_state(response, std::nullopt);
    response_state().reset_draft();
    return response;
}

http_response context::error(http_error_info_options options) const {
    return error(http_error_info(options));
}

http_response context::error(const http_error_info& info) const {
    auto response = detail::make_default_error_response(arena(), info);
    return response;
}

task<http_response> context::not_found() {
    return ::ruvia::make_scoped_operation(operation_scope_, not_found_task());
}

task<http_response> context::not_found_task() {
    // Selected only here, on demand: every matched request carries the table,
    // and few of them ever ask for a not-found answer.
    const auto* routes = services().routes();
    const auto handler = routes == nullptr ? detail::http_not_found_handler_ref{nullptr}
                                           : routes->not_found_handler_for(request_.path());
    if (handler != nullptr) {
        co_return co_await handler(*this);
    }

    auto response = detail::make_default_error_response(arena(),
        http_error_info({.status_ = http_status::not_found, .message_ = "route not found"}));
    co_return response;
}

http_response context::streaming_head() {
    http_response response(arena());
    apply_response_state(response, std::nullopt);
    response_state().apply_policy(response);
    response_state().reset_draft();
    return response;
}

task<http_response> detail::context_access::commit_streaming_head(context& context_value) {
    if (session_access::available(context_value)) {
        co_await session_access::commit(context_value);
    }
    co_return context_value.streaming_head();
}

context& context::set_stable_response_header(std::string_view name, std::string_view value) {
    if (response_output().committed()) {
        throw std::logic_error("response headers are already committed");
    }
    response_state().draft().header_stable_view(name, value);
    return *this;
}

void context::apply_response_state(
    http_response& response, std::optional<http_status_code> status_code) {
    if (!response_state().has_draft()) {
        if (status_code.has_value()) {
            response.status(*status_code);
        }
        return;
    }
    const auto& metadata = response_state().draft();
    response.status(status_code.value_or(metadata.status()));
    if (!metadata.headers().empty()) {
        response.transfer_headers_from(metadata, http_response_header_transfer::apply);
    }
}

}  // namespace ruvia
