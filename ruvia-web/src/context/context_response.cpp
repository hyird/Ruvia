#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/json/json_scan.h"
#include "ruvia/web/session.h"

#include "auth/cookie_signature.h"
#include "context/context_access.h"
#include "context/context_services.h"
#include "http/http_error_response.h"
#include "http/inline_text_concat.h"
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
void set_default_content_type(http_response& response, http_content_type content_type) {
    if (!response.header("Content-Type")) {
        response.content_type(content_type);
    }
}

void mark_plain_text(http_response& response) {
    response.content_type(http_content_type::text_plain_utf8);
    if (!response.header("X-Content-Type-Options")) {
        response.header_stable_view("X-Content-Type-Options", "nosniff");
    }
}

void mark_plain_text_defaults(http_response& response) {
    set_default_content_type(response, http_content_type::text_plain_utf8);
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

std::string context::url_for(
    std::string_view pattern, std::span<const std::string_view> values) const {
    if (services().routes() == nullptr) {
        throw std::logic_error("url_for requires a route table bound to this context");
    }
    return services().routes()->url_for(pattern, values);
}

std::string context::url_for(
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
    auto response = response_state().take_draft_or_new();
    response.body(body);
    return response;
}

http_response context::body_owned(std::string&& body) {
    auto response = response_state().take_draft_or_new();
    response.owned_body(std::move(body));
    return response;
}

http_response context::body(std::span<const std::byte> body) {
    auto response = response_state().take_draft_or_new();
    response.body(byte_body_view(body));
    return response;
}

http_response context::body(static_text body) {
    auto response = response_state().take_draft_or_new();
    response.static_body(body.view());
    return response;
}

http_response context::text(std::string_view body) {
    auto response = response_state().take_draft_or_new();
    mark_plain_text_defaults(response);
    response.body(body);
    return response;
}

http_response context::text_owned(std::string&& body) {
    auto response = response_state().take_draft_or_new();
    mark_plain_text_defaults(response);
    response.owned_body(std::move(body));
    return response;
}

http_response context::text(static_text body) {
    auto response = response_state().take_draft_or_new();
    mark_plain_text_defaults(response);
    response.static_body(body.view());
    return response;
}

response_stream_writer& context::stream_text() {
    auto& writer = stream();
    if (!response_output().committed()) {
        mark_plain_text(response_state().draft());
    }
    return writer;
}

http_response context::json_serialized(std::string& body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::application_json);
    response.owned_body(std::move(body));
    return response;
}

http_response context::html(std::string_view body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::text_html_utf8);
    response.body(body);
    return response;
}

http_response context::html_owned(std::string&& body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::text_html_utf8);
    response.owned_body(std::move(body));
    return response;
}

http_response context::html(static_text body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::text_html_utf8);
    response.static_body(body.view());
    return response;
}

http_response context::error(http_error_info_options options) const {
    return error(http_error_info(options));
}

http_response context::error(const http_error_info& info) const {
    auto response = detail::make_default_error_response(info);
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

    co_return detail::make_route_not_found_response();
}

http_response context::streaming_head() {
    auto response = response_state().take_draft_or_new();
    response_state().apply_policy(response);
    return response;
}

task<http_response> detail::context_access::commit_streaming_head(context& context_value) {
    if (context_access::has_response(context_value)) {
        throw std::logic_error("a complete response is already selected");
    }
    if (session_access::available(context_value)) {
        co_await session_access::commit(context_value);
    }
    auto response = context_value.streaming_head();
    detail::apply_alternative_service(response, context_value.services().alternative_service());
    co_return response;
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

namespace {

void write_cookie(http_response& response, std::string_view name, std::string_view value,
    const cookie_options& options) {
    const set_cookie_plan plan(name, value, options);
    response.set_cookie(plan);
}

[[nodiscard]] bool redirect_location_contains_line_break(std::string_view location) noexcept {
    for (const char ch : location) {
        if (ch == '\r' || ch == '\n') {
            return true;
        }
    }
    return false;
}

// RFC 3986 section 3.2 authority of an absolute or network-path reference:
// it follows "//" and ends at the first "/", "?" or "#". Browsers treat "\" as
// a path separator for http(s) (WHATWG URL), so a percent-encoded "\" inside
// the authority would move the host the browser contacts, e.g.
// "http://good.example\@evil.example/".
[[nodiscard]] bool redirect_authority_contains_backslash(std::string_view location) noexcept {
    std::size_t start = 0;
    if (!location.starts_with("//")) {
        const auto colon = location.find(':');
        if (colon == std::string_view::npos || colon == 0 || location.substr(colon + 1, 2) != "//") {
            return false;
        }
        for (std::size_t index = 0; index < colon; ++index) {
            const char ch = location[index];
            const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            if (!alpha && (index == 0 || !((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'))) {
                return false;
            }
        }
        start = colon + 1;
    }
    const auto authority = location.substr(start + 2);
    return authority.substr(0, authority.find_first_of("/?#")).find('\\') != std::string_view::npos;
}

}  // namespace

void context_response_editor::set_cookie(set_cookie_options options) {
    write_cookie(target(), options.name_.view(), options.value_.view(), options.attributes_);
}

void context_response_editor::set_signed_cookie(set_signed_cookie_options options) {
    const auto name = options.name_.view();
    const auto value = options.value_.view();
    const auto secret = options.secret_.view();
    const auto prefix = options.attributes_.prefix_
                            ? std::string_view(ruvia::http_cookie_prefix_text(*options.attributes_.prefix_))
                            : std::string_view{};
    const detail::inline_text_concat<128> wire_name({prefix, name});
    char signature[detail::cookie_signature_size];
    detail::write_cookie_signature(signature, secret, wire_name.view(), value);
    const detail::inline_text_concat<256> signed_value(
        {value, ".", std::string_view(signature, sizeof(signature))});
    write_cookie(target(), name, signed_value.view(), options.attributes_);
}

void context_response_editor::delete_cookie(delete_cookie_options options) {
    options.attributes_.max_age_.reset();
    options.attributes_.expires_ = cookie_deletion_expires;
    write_cookie(target(), options.name_.view(), "", options.attributes_);
}

http_response context::redirect(redirect_response_options options) {
    const auto location = options.location_.view();
    const auto status_code = options.status_;
    if (!is_http_redirect_status(status_code)) {
        throw std::invalid_argument("redirect status must be 301, 302, 303, 307, or 308");
    }
    if (redirect_location_contains_line_break(location)) {
        throw std::invalid_argument("redirect location must not contain CR or LF");
    }
    if (redirect_authority_contains_backslash(location)) {
        throw std::invalid_argument("redirect location authority must not contain a backslash");
    }
    http_response response;
    apply_response_state(response, status_code);
    std::string encoded_location;
    response.header("Location", percent_encode_uri_reference(location, encoded_location));
    response_state().reset_draft();
    return response;
}

http_response make_health_response(context& context_value) {
    return detail::context_access::make_static_json_response(context_value, R"({"status":"ok"})");
}

http_response make_readiness_response(context& context_value, readiness_response_options options) {
    switch (options.state_) {
        case readiness_state::ready:
            return detail::context_access::make_static_json_response(
                context_value, R"({"status":"ready"})");
        case readiness_state::unavailable:
            break;
        default:
            throw std::invalid_argument("readiness state is invalid");
    }

    context_value.draft().status(http_status::service_unavailable);
    const auto reason = options.unavailable_reason_.view();
    if (reason.empty()) {
        return detail::context_access::make_static_json_response(
            context_value, R"({"status":"not_ready"})");
    }

    static constexpr std::string_view prefix = R"({"status":"not_ready","reason":)";
    std::string body;
    body.reserve(prefix.size() + detail::json_string_size_hint(reason) + 1U);
    body.append(prefix);
    detail::append_json_string(body, reason);
    body.push_back('}');
    return detail::context_access::make_owned_json_response(context_value, body);
}
}  // namespace ruvia
