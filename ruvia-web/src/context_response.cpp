#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <asio/error.hpp>
#include <asio/ssl/error.hpp>
#include <asio/system_error.hpp>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/streaming.h"

#include "connection_lifecycle.h"
#include "context_access.h"
#include "http_client_pool.h"
#include "route_table.h"

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

namespace ruvia {
void context::advertise_origins(std::span<const std::string_view> origins) {
    if (services().connection_advertisements() == nullptr || conn().tls() == nullptr) {
        throw std::logic_error("ORIGIN requires an HTTPS connection with advertisement output");
    }
    services().connection_advertisements()->advertise_origins(origins);
}
void context::advertise_alternative_service(std::string_view value) {
    if (services().connection_advertisements() == nullptr) {
        throw std::logic_error("this dispatch has no connection advertisement output");
    }
    services().connection_advertisements()->advertise_alternative_service(value);
}
bool context::push(http_push_request_view request) {
    if (services().push_output() == nullptr) {
        throw std::logic_error("server push requires an HTTP/2 request");
    }
    return services().push_output()->push(request);
}
task<void> context::inform(const http_interim_response_head& response, operation_options operation) & {
    if (services().interim_output() == nullptr) {
        throw std::logic_error("this dispatch has no transport response output");
    }
    return services().interim_output()->inform(response, std::move(operation), *this);
}
}  // namespace ruvia

namespace ruvia::detail {
void http_connection_advertisement_output::advertise_origins(std::span<const std::string_view> origins) {
    if (origins_ == nullptr) {
        throw std::logic_error("this HTTP version does not support ORIGIN frames");
    }
    if (origins.size() > max_http_header_bytes / 2) {
        throw std::length_error("too many advertised origins");
    }
    auto bytes_value = origins.size() * 2;
    for (const auto origin : origins) {
        if (origin.size() > max_http_header_bytes - std::min(bytes_value, max_http_header_bytes)) {
            throw std::length_error("origin advertisement exceeds its byte bound");
        }
        bytes_value += origin.size();
    }
    origins_(target_, origins);
}
void http_connection_advertisement_output::advertise_alternative_service(std::string_view value) {
    if (service_ == nullptr) {
        throw std::logic_error("ALTSVC frames require HTTP/2");
    }
    if (value.size() > max_http_header_bytes) {
        throw std::length_error("alternative service advertisement exceeds its byte bound");
    }
    service_(target_, value);
}
task<void> http_interim_response_output::inform(
    const http_interim_response_head& response, operation_options operation, context& context_value) {
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::vector<http_header> owned;
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.emplace_back(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(
        scope_, write_owned(response.status(), std::move(owned), std::move(operation), context_value));
}
task<void> http_interim_response_output::write_owned(http_status_code status,
    std::vector<http_header> fields_value, operation_options operation, context& context_value) {
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    const detail::inline_http_header_views<16> active_views(fields_value);
    const http_interim_response_head head(status, active_views.view());
    operation_interrupt interrupt(interrupts_, context_value.worker(), operation, &context_value,
        &context_access::reset_interrupted_request, "interim response write");
    try {
        co_await write_(target_, head);
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
    interrupt.throw_if_interrupted();
}
task<void> http_interim_response_output::send_continue() {
    if (final_committed_) {
        co_return;
    }
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    co_await write_(target_, http_interim_response_head(http_status::continue_value));
}
}  // namespace ruvia::detail

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

void dispatch_response::copy_head_from(const http_response& response) {
    status_ = response.status();
    if (!response.headers().empty()) {
        headers_.reserve(response.headers().size());
        for (const auto& header : response.headers()) {
            headers_.emplace_back(header.name(), header.value());
        }
    }
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
    if (options.method_.size() > max_subrequest_header_bytes ||
        options.target_.size() > max_subrequest_header_bytes - options.method_.size()) {
        throw std::invalid_argument("subrequest headers exceed the configured limit");
    }
    std::size_t header_bytes = options.method_.size() + options.target_.size();
    for (const auto& header : options.headers_) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid subrequest header");
        }
        if (is_http_protocol_managed_request_field(header.name())) {
            throw std::invalid_argument("subrequest framing headers are owned by dispatch");
        }
        if (header.name().size() > max_subrequest_header_bytes - header_bytes ||
            header.value().size() > max_subrequest_header_bytes - header_bytes - header.name().size()) {
            throw std::invalid_argument("subrequest headers exceed the configured limit");
        }
        header_bytes += header.name().size() + header.value().size();
    }
    // The operation is lazy and the options borrow caller storage: take owned
    // copies now. The subrequest is built directly as a parsed request; it
    // never round-trips through a wire format.
    std::string method(options.method_.view());
    std::string target(options.target_.view());
    std::string body(options.body_.view());
    std::vector<http_header> headers;
    if (!options.headers_.empty()) {
        headers.reserve(options.headers_.size());
        for (const auto& header : options.headers_) {
            headers.emplace_back(header.name(), header.value());
        }
    }
    return ::ruvia::make_scoped_operation(operation_scope_,
        dispatch_task(std::move(method), std::move(target), std::move(body), std::move(headers),
            std::move(options.stop_token_)));
}

task<dispatch_response> context::dispatch_task(std::string method, std::string target,
    std::string body_storage, std::vector<http_header> headers,
    ::ruvia::stop_token stop_token_value) {
    const std::string_view body = body_storage;
    std::array<char, http_max_unsigned_decimal_size> content_length_text;
    const auto content_length = http_format_unsigned(content_length_text, body.size());
    detail::inline_http_header_views<16> active_headers(2U + headers.size());
    active_headers.push_back(http_header_view("Host", "localhost"));
    for (const auto& header : headers) {
        active_headers.push_back(http_header_view(header.name(), header.value()));
    }
    active_headers.push_back(http_header_view("Content-Length", content_length));
    const auto request = [&]() {
        try {
            return make_parsed_http_request(method, target, active_headers.view(),
                std::as_bytes(std::span(body.data(), body.size())));
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
    const stop_group stop_owner(capabilities_.stop_token(), std::move(stop_token_value));
    const auto stop = stop_owner.token();
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
    http_response response;
    auto admission = co_await detail::admit_and_dispatch_buffered_request(routes,
        {
            .request_ = request,
            .resolution_ = resolution,
            .coding_ = http_response_coding_selection_for(request),
        },
        policy, services().for_subrequest(stop), response);
    if (detail::http_response_needs_not_acceptable(admission.tail().coding_policy(), request, response) &&
        !response.status().is_error()) {
        response = co_await routes.handle_error(request,
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
    dispatch_response result;
    result.copy_head_from(response);
    // As on the wire: HEAD and content-forbidden statuses carry no body.
    if (!plan_http_response_body(known_method, response.status()).body_suppressed()) {
        result.body_.assign(response.body_bytes());
    }
    co_return result;
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_DATABASE
#endif

#ifdef RUVIA_ENABLE_REDIS
#endif
namespace ruvia {

context::context(const http_request& request, const detail::context_services& services)
    : context(request, {}, nullptr, nullptr, 0, 0, services) {}

context::context(const http_request& request,
    std::string_view route_path, const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope,
    const detail::context_services& services)
    : request_(request),
      capabilities_(services.worker(), services.get_stop_token(), services.worker_states()),
      route_path_(route_path),
      param_names_(param_names),
      param_values_(param_values),
      param_count_(param_count < detail::max_route_params ? param_count : detail::max_route_params),
      route_rate_limit_scope_(route_rate_limit_scope),
      request_storage_(std::make_unique<detail::context_request_storage>(services)) {
    operation_scope_.bind_worker(services.worker());
}
context::~context() = default;

const conn_info& context::conn() const& noexcept {
    return services().get_conn_info();
}

std::shared_ptr<void> context::connection_state() const noexcept {
    return services().connection_state();
}

void context::abort_connection() noexcept {
    services().abort_connection();
}

bool context::is_subrequest() const noexcept {
    return services().dispatch_depth() != 0;
}

bool context::early_data() const noexcept {
    return services().early_data();
}

detail::context_services& context::services() noexcept {
    return request_storage().services_;
}

const detail::context_services& context::services() const noexcept {
    return request_storage().services_;
}

const ruvia::env& context::env() const noexcept {
    static const ruvia::env empty;
    return services().env() != nullptr ? *services().env() : empty;
}

std::string& context::decoded_body() const {
    auto& storage = request_storage();
    if (!storage.decoded_body_) {
        storage.decoded_body_.emplace();
    }
    return *storage.decoded_body_;
}

detail::context_request_storage& context::request_storage() const {
    return *request_storage_;
}

const detail::context_request_body_source& context::request_body_source() const noexcept {
    return services().request_body_source();
}

detail::context_response_output& context::response_output() noexcept {
    return services().response_output();
}

const detail::context_response_output& context::response_output() const noexcept {
    return services().response_output();
}

detail::context_response_state& context::response_state() noexcept {
    return request_storage().response_state_;
}

const detail::context_response_state& context::response_state() const noexcept {
    return request_storage().response_state_;
}

detail::context_session_state& context::session_state() noexcept {
    return request_storage().session_state_;
}

const detail::context_session_state& context::session_state() const noexcept {
    return request_storage().session_state_;
}

detail::request_bindings& context::request_bindings() noexcept {
    return request_storage().request_bindings_;
}

const detail::request_bindings& context::request_bindings() const noexcept {
    return request_storage().request_bindings_;
}

const http_response* context::response() const noexcept {
    const auto* final = response_state().final();
    return final;
}

bool context::has_response() const noexcept {
    return response_state().final() != nullptr;
}

void context::respond(http_response&& response) {
    store_response(std::move(response));
}

#ifdef RUVIA_ENABLE_DATABASE
db_session context::db() const {
    const auto& services_value = services();
    return services_value.client_registries().db(
        operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

db_session context::db(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().db(
        alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session context::redis() const {
    const auto& services_value = services();
    return services_value.client_registries().redis(
        operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

redis_session context::redis(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().redis(
        alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

bool detail::context_response_output::committed() const noexcept {
    if (buffered() != nullptr) {
        return false;
    }
    if (const auto* stream = response_stream()) {
        return stream->writer().committed();
    }
    return true;
}

conn_info detail::context_services::resolve_conn_info(const http_request& request) const noexcept {
    auto resolved = connection_services_.info_;
    if (!connection_services_.peer_trusted_ || worker_services_->trusted_proxies_ == nullptr) {
        return resolved;
    }
    const auto forwarded =
        worker_services_->trusted_proxies_->resolve(resolved.remote().address(), request);
    if (forwarded.from_forwarded_header()) {
        resolved.apply_forwarded(forwarded.address(), forwarded.scheme());
    }
    return resolved;
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Copies the file a borrowed path names into `output` in NUL-terminated
// native form: a borrowed std::filesystem::path's native string as is, text
// terminated and, on Windows, widened. False when the path cannot name a file
// (empty, an embedded NUL, or text that is not UTF-8 on Windows).
[[nodiscard]] bool assign_native_file_path(borrowed_file_path path, http_native_path_string_type& output) {
    if (const auto native = path.native(); !native.empty()) {
        output.assign(native);
        return true;
    }
    const auto text = path.text();
    if (text.empty() || text.find('\0') != std::string_view::npos) {
        return false;
    }
#if defined(_WIN32)
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto text_length = static_cast<int>(text.size());
    const auto wide_length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, nullptr, 0);
    if (wide_length <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(wide_length));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, output.data(),
            wide_length) != wide_length) {
        return false;
    }
#else
    output.assign(text.data(), text.size());
#endif
    return true;
}

inline constexpr std::size_t file_response_header_reserve = 7;

[[nodiscard]] http_error static_file_changed_error() {
    return http_error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "static_file_changed",
        .message_ = "static file changed since its index was built"});
}

// context::file's open, run on the blocking pool. The path travels with it in
// the synchronized process resource -- an abandoned open frees it on the pool
// thread -- and comes back with the opened file, so the response is framed
// from the same copy.
struct opened_file_path final {
    http_native_path_string_type path_;
    detail::opened_response_file file_{};
    std::error_code error_{};
};

// Runs a file response's open (and, for an indexed file, its identity check)
// on the worker's blocking pool, never on the worker. The wait is bounded like
// the body reads the transport waits on later: the server write_timeout_ and
// the request's stop token. An abandoned open keeps running on the pool and
// frees what it owns there.
template <typename open_type>
[[nodiscard]] task<blocking_result<std::invoke_result_t<open_type&>>> run_file_open(
    context& context_value, open_type open) {
    return run_blocking(context_value.worker(),
        {.timeout_ = detail::context_access::services(context_value).file_io_timeout(),
            .stop_token_ = context_value.stop_token()},
        std::move(open));
}

// The open's value. An open that never completed is answered as a file body
// read that did not complete: 503 file_read_unavailable, after a real fault
// (no blocking pool, a stalled open) went to the connection failure outlet.
// The callable's own exception is rethrown unchanged.
template <typename value_type>
[[nodiscard]] value_type take_file_open(context& context_value, blocking_result<value_type>&& result) {
    if (result.status() != blocking_status::completed) {
        const auto failure = detail::response_file_failure::read_not_completed(result.status());
        detail::context_access::services(context_value)
            .connection_failure()
            .invoke(detail::context_access::services(context_value).remote_address(), failure.fault());
        std::rethrow_exception(failure.response_error());
    }
    return std::move(result).take_value();
}

// Names the response file by a borrowed native path that outlives
// make_file_response: a static_root index entry or context::file's opened
// copy. http_response copies the path into its own storage, so neither
// borrow reaches the returned body. The handle opened for the response goes
// with the body, so the transport reads it on the blocking pool and never
// reopens the path.
class file_response_path final {
public:
    // `path` must be NUL-terminated (a c_str() or path::native() view).
    file_response_path(http_native_path_view_type path, http_response_file_identity identity,
        http_response_file_descriptor opened)
        : path_(path),
          identity_(identity),
          opened_(std::move(opened)) {
        if (path_.empty() || !opened_) {
            throw std::logic_error("file response has no native path or opened handle");
        }
    }

    void set_body(http_response& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        response.file_body(path_, size, offset, length, identity_, std::move(opened_));
    }

    void set_multipart_body(http_response& response, std::uint64_t size,
        http_multipart_byte_range_plan&& plan) {
        response.multipart_file_body(path_, size, identity_, std::move(plan), std::move(opened_));
    }

private:
    http_native_path_view_type path_;
    http_response_file_identity identity_;
    http_response_file_descriptor opened_;
};

class file_response_body_source final {
public:
    [[nodiscard]] static file_response_body_source file(file_response_path path) {
        return file_response_body_source(std::move(path));
    }

    [[nodiscard]] static file_response_body_source bytes_value(std::string_view bytes_value) {
        return file_response_body_source(bytes_value);
    }

    void set_body(http_response& response, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length) {
        if (auto* path = std::get_if<file_response_path>(&value_)) {
            path->set_body(response, size, offset, length);
            return;
        }
        const auto bytes_value = std::get<std::string_view>(value_);
        if (offset > bytes_value.size() || length > bytes_value.size() - offset) {
            throw std::logic_error("static memory response slice is out of range");
        }
        // Copied, not borrowed: the snapshot is not guaranteed to outlive the
        // response. A document-root lease ends when dispatch returns, before
        // the transport writes the body, and a caller's static_root carries no
        // lease at all. Borrowing would need a per-response reference on the
        // snapshot (shared ownership on the hot path) or a lease release hook
        // inside http_response.
        std::string owned(
            bytes_value.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)));
        response.file_bytes_body(std::move(owned));
    }

    void set_full_body(
        http_response& response, std::uint64_t size) {
        set_body(response, size, 0, size);
    }

    void set_multipart_body(http_response& response,
        std::uint64_t size, http_multipart_byte_range_plan&& plan) {
        if (auto* path = std::get_if<file_response_path>(&value_)) {
            path->set_multipart_body(response, size, std::move(plan));
            return;
        }
        const auto source_value = std::get<std::string_view>(value_);
        std::size_t total_size = 0;
        for (const auto& segment : plan.segments()) {
            total_size += segment.kind_ == http_multipart_byte_range_plan::segment_kind::file
                              ? static_cast<std::size_t>(segment.file_length_)
                              : segment.metadata_length_;
        }
        std::string body;
        body.reserve(total_size);
        for (const auto& segment : plan.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                body.append(source_value.substr(static_cast<std::size_t>(segment.file_offset_),
                    static_cast<std::size_t>(segment.file_length_)));
            } else {
                body.append(plan.metadata().substr(segment.metadata_offset_, segment.metadata_length_));
            }
        }
        response.file_bytes_body(std::move(body));
    }

private:
    explicit file_response_body_source(file_response_path path) noexcept
        : value_(std::move(path)) {}

    explicit file_response_body_source(std::string_view bytes_value) noexcept
        : value_(bytes_value) {}

    std::variant<file_response_path, std::string_view> value_;
};

// What one file response describes: which bytes, when they last changed, and
// the policy the serving route attached to them. Fifteen positional arguments
// at a call site said none of that; designated initializers do. It is known
// before any file is opened, so the response plan is too.
struct file_response_source final {
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    // What a computed ETag digests; unchecked for bytes served from memory.
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    // Never empty: the caller's type, the index entry's, or the one guessed
    // from the path.
    std::string_view content_type_;
    std::string_view cache_control_;
    static_range_request_policy range_requests_{static_range_request_policy::ignore};
    static_response_validator_policy response_validators_{static_response_validator_policy::omit};
    std::string_view precomputed_etag_;
    std::string_view precomputed_last_modified_;
    http_content_coding content_coding_{http_content_coding::identity};
    bool negotiates_encoding_{false};
};

// The validators one file response sends and evaluates preconditions
// against. The views borrow the caller's storage or the source's precomputed
// values.
struct file_validators final {
    std::string_view etag_;
    // Empty when the date is unavailable.
    std::string_view last_modified_;
    std::time_t modified_seconds_{0};
};

[[nodiscard]] file_validators compute_file_validators(const file_response_source& source_value,
    std::array<char, detail::static_file_etag_size>& etag_storage,
    std::array<char, http_imf_fixdate_size>& last_modified_storage) noexcept {
    file_validators validators;
    // RFC 9110 §8.8.2.1 forbids an origin server from emitting a
    // Last-Modified value later than the message origination time. Filesystems
    // can legitimately contain future mtimes (clock skew, archives, or an
    // explicit timestamp), so use this response's current second for the wire
    // validator and every date precondition evaluated against it. The clamped
    // value is not the representation's actual validator and therefore cannot
    // be a strong If-Range validator (RFC 9110 §13.1.5).
    const auto response_seconds = std::time(nullptr);
    const bool has_response_time = response_seconds != std::time_t{-1};
    const bool last_modified_is_actual = has_response_time && source_value.modified_seconds_ <= response_seconds;
    validators.modified_seconds_ = last_modified_is_actual ? source_value.modified_seconds_ : response_seconds;
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        if (source_value.precomputed_etag_.empty()) {
            etag_storage = detail::format_static_file_snapshot_etag(
                source_value.size_, source_value.modified_token_, source_value.identity_);
            validators.etag_ = std::string_view(etag_storage.data(), etag_storage.size());
        } else {
            validators.etag_ = source_value.precomputed_etag_;
        }
    }
    // Date preconditions also apply when response validators are not emitted.
    // An unrepresentable date is unavailable, not a truncated wire validator.
    if (has_response_time) {
        if (source_value.precomputed_last_modified_.empty() || !last_modified_is_actual) {
            if (const auto date = format_http_date(validators.modified_seconds_)) {
                last_modified_storage = *date;
                validators.last_modified_ =
                    std::string_view(last_modified_storage.data(), last_modified_storage.size());
            }
        } else {
            validators.last_modified_ = source_value.precomputed_last_modified_;
        }
    }
    return validators;
}

[[nodiscard]] http_representation_response_plan plan_file_representation(const context& context_value,
    const file_response_source& source_value, const file_validators& validators) {
    return plan_http_representation_response(detail::context_access::request(context_value),
        http_selected_representation_metadata{
            .length_ = source_value.size_,
            .etag_ = validators.etag_,
            .last_modified_ = validators.last_modified_.empty() ? std::nullopt
                                                                : std::optional(validators.modified_seconds_),
            // Filesystem mtime cannot establish that a representation changed
            // at most once during the second in its Last-Modified value.
            .strong_date_validator_ = false,
            .content_coded_ = source_value.content_coding_ != http_content_coding::identity,
        },
        http_representation_response_options{
            .normal_status_ = detail::context_access::response_status(context_value),
            .range_policy_ = source_value.range_requests_ == static_range_request_policy::honor
                                 ? http_range_request_policy::honor_byte_ranges
                                 : http_range_request_policy::ignore,
        });
}

// The validators and the RFC 9110 response one file response's request
// selects, decided from the source alone, before any file is opened. The
// validator views may borrow this object's storage, so it stays where it was
// built.
class file_response_plan final {
public:
    file_response_plan(const context& context_value, const file_response_source& source_value)
        : validators_(compute_file_validators(source_value, etag_storage_, last_modified_storage_)),
          representation_(plan_file_representation(context_value, source_value, validators_)),
          head_(detail::context_access::request(context_value).known_method() == http_known_method::head) {}

    file_response_plan(const file_response_plan&) = delete;
    file_response_plan& operator=(const file_response_plan&) = delete;
    file_response_plan(file_response_plan&&) = delete;
    file_response_plan& operator=(file_response_plan&&) = delete;

    [[nodiscard]] const file_validators& validators() const noexcept {
        return validators_;
    }

    [[nodiscard]] const http_representation_response_plan& representation() const noexcept {
        return representation_;
    }

    // Whether the response sends the file's bytes. A HEAD, 304, 412 or 416
    // only describes them, so no transport read ever checks the file behind
    // such a response; its open has to.
    [[nodiscard]] bool sends_file_bytes() const noexcept {
        return !head_ && representation_.precondition_failed() == nullptr &&
               representation_.not_modified() == nullptr && representation_.range_unsatisfiable() == nullptr;
    }

private:
    std::array<char, detail::static_file_etag_size> etag_storage_{};
    std::array<char, http_imf_fixdate_size> last_modified_storage_{};
    file_validators validators_;
    http_representation_response_plan representation_;
    bool head_;
};

// Which representation metadata a response repeats. RFC 9110 15.4.5: a 304
// carries the validators and cache-guiding fields a 200 would, but no other
// representation metadata; Last-Modified guides the cache update only when no
// ETag is sent. RFC 9110 15.3.7: a 206 answering a matched If-Range SHOULD NOT
// repeat representation fields beyond the required ones (ETag, Cache-Control,
// Content-Range or the multipart Content-Type), because the client already
// holds them. Content-Encoding stays: it names the coding of the bytes this
// message carries, and the response coding policy reads it to keep a
// precompressed variant acceptable to a client that refuses identity.
// RFC 9110 15.5.17: a 416 has no content, so Content-Type and Content-Encoding
// would describe bytes that are not sent; it keeps Content-Range and the
// validator and cache fields.
enum class file_header_set : std::uint8_t {
    full,
    if_range_partial,
    not_modified,
    range_unsatisfied,
};

void add_file_headers(http_response& response, const file_response_source& source_value,
    const file_validators& validators, file_header_set header_set = file_header_set::full,
    bool emit_content_type = true) {
    const bool full = header_set == file_header_set::full;
    const bool carries_content =
        header_set == file_header_set::full || header_set == file_header_set::if_range_partial;
    response.reserve_headers(file_response_header_reserve);
    if (full && emit_content_type) {
        response.header("Content-Type", source_value.content_type_);
    }
    if (!source_value.cache_control_.empty()) {
        response.header("Cache-Control", source_value.cache_control_);
    }
    // A precompressed variant carries the original Content-Type with the
    // encoding declared here.
    if (carries_content && source_value.content_coding_ != http_content_coding::identity) {
        response.header_stable_view(
            "Content-Encoding", http_content_coding_token(source_value.content_coding_));
    }
    if (header_set != file_header_set::not_modified &&
        source_value.range_requests_ == static_range_request_policy::honor) {
        response.header_stable_view("Accept-Ranges", "bytes");
    }
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        response.header("ETag", validators.etag_);
        if ((full || header_set == file_header_set::range_unsatisfied) && !validators.last_modified_.empty()) {
            response.header("Last-Modified", validators.last_modified_);
        }
    }
}

[[nodiscard]] file_header_set partial_file_header_set(const http_representation_response_plan& plan) noexcept {
    return plan.authorized_by_if_range() ? file_header_set::if_range_partial : file_header_set::full;
}

void apply_file_response_state(context& context_value, const file_response_source& source_value,
    http_response& response, std::optional<http_status_code> status_code) {
    detail::context_access::apply_response_state(context_value, response, status_code);
    // Declare the negotiation dimension after context response metadata is
    // applied. A caller-provided Vary value must be merged, not allowed to
    // overwrite Accept-Encoding and make differently encoded variants share
    // one cache entry (RFC 9110 12.5.5 / RFC 9111 4.1). context::file does no
    // Accept-Encoding negotiation and stays Vary-free.
    if (source_value.negotiates_encoding_) {
        response.add_vary_token("Accept-Encoding");
    }
}

[[nodiscard]] http_response make_not_modified_response(context& context_value,
    const file_response_source& source_value, const file_validators& validators, http_status_code status_code) {
    http_response response;
    add_file_headers(response, source_value, validators, file_header_set::not_modified);
    apply_file_response_state(context_value, source_value, response, status_code);
    return response;
}

[[nodiscard]] http_response make_full_file_response(context& context_value,
    const file_response_source& source_value, file_response_body_source& body, const file_validators& validators,
    std::optional<http_status_code> status_code) {
    http_response response;
    add_file_headers(response, source_value, validators);
    body.set_full_body(response, source_value.size_);
    apply_file_response_state(context_value, source_value, response, status_code);
    return response;
}

[[nodiscard]] http_response make_multipart_response(context& context_value,
    const file_response_source& source_value, file_response_body_source& body, const file_validators& validators,
    const http_byte_range_set& ranges, file_header_set header_set) {
    std::array<char, detail::secure_token_size> boundary_token;
    const auto token = detail::generate_secure_token(boundary_token);
    http_response response;
    add_file_headers(response, source_value, validators, header_set, false);
    apply_file_response_state(context_value, source_value, response, http_status::partial_content);
    const auto media_type = response.header("Content-Type");
    const auto selected_media_type = media_type.value_or(source_value.content_type_);
    auto multipart_plan = make_http_multipart_byte_range_plan(
        ranges, source_value.size_, selected_media_type, token);

    // The outer representation is the multipart body, which is not
    // content-coded; coded representations never reach this path.
    response.header("Content-Type", multipart_plan.content_type());
    response.remove_header("Content-Range");
    response.remove_header("Content-Encoding");
    response.remove_header("Content-Length");
    body.set_multipart_body(response, source_value.size_, std::move(multipart_plan));
    return response;
}

// Builds the response `planned` selected. An indexed file behind a response
// that sends none of its bytes was already checked by its open.
[[nodiscard]] http_response make_file_response(context& context_value, const file_response_source& source_value,
    file_response_body_source& body, const file_response_plan& planned) {
    const auto& validators = planned.validators();
    const auto& plan = planned.representation();
    if (plan.precondition_failed()) {
        throw http_error({.status_ = plan.status(),
            .code_ = "precondition_failed",
            .message_ = "file precondition failed"});
    }
    if (plan.not_modified()) {
        return make_not_modified_response(context_value, source_value, validators, plan.status());
    }
    if (plan.range_unsatisfiable()) {
        http_response response;
        response.content_range_unsatisfied(source_value.size_);
        add_file_headers(response, source_value, validators, file_header_set::range_unsatisfied);
        apply_file_response_state(context_value, source_value, response, plan.status());
        return response;
    }
    if (const auto* ranges = plan.multipart_ranges()) {
        return make_multipart_response(
            context_value, source_value, body, validators, *ranges, partial_file_header_set(plan));
    }
    if (const auto* range = plan.partial()) {
        http_response response;
        add_file_headers(response, source_value, validators, partial_file_header_set(plan));
        response.content_range(range->offset_, range->length_, source_value.size_);
        body.set_body(response, source_value.size_, range->offset_, range->length_);
        apply_file_response_state(context_value, source_value, response, plan.status());
        return response;
    }
    return make_full_file_response(context_value, source_value, body, validators, plan.status());
}

[[nodiscard]] http_error file_not_found_error() {
    return http_error({.status_ = ruvia::http_status::not_found,
        .code_ = "not_found",
        .message_ = "file not found"});
}

// context::file once its inputs are owned: `native_path` in the process
// resource, since it travels to the pool thread, and `content_type` in the
// request pool. An empty `content_type` selects the type guessed from the
// path.
[[nodiscard]] task<http_response> serve_file(
    context& context_value, http_native_path_string_type native_path, std::string content_type) {
    const auto content_type_value = content_type.empty() ? detail::guess_static_file_content_type_from_path_view(
                                                               http_native_path_view_type(native_path))
                                                         : std::string_view(content_type);
    // Open once, on the blocking pool: the response is framed from this
    // handle's metadata, and the body comes from the same handle, so no path
    // is resolved twice.
    auto opened = take_file_open(context_value,
        co_await run_file_open(context_value, [path = std::move(native_path)]() mutable {
            opened_file_path result{.path_ = std::move(path)};
            result.file_ = detail::open_response_file(result.path_.c_str(), result.error_);
            return result;
        }));
    if (opened.error_) {
        throw file_not_found_error();
    }
    const auto& snapshot = opened.file_.snapshot_;
    const file_response_source source_value{
        .size_ = snapshot.size_,
        .modified_token_ = snapshot.modified_token_,
        .modified_seconds_ = snapshot.modified_seconds_,
        .identity_ = snapshot.identity_,
        .content_type_ = content_type_value,
        .cache_control_ = {},
        .range_requests_ = static_range_request_policy::honor,
        .response_validators_ = static_response_validator_policy::emit,
        .precomputed_etag_ = {},
        .precomputed_last_modified_ = {},
        .content_coding_ = http_content_coding::identity,
        .negotiates_encoding_ = false,
    };
    const file_response_plan planned(context_value, source_value);
    auto body = file_response_body_source::file(
        file_response_path(opened.path_, snapshot.identity_, std::move(opened.file_.handle_)));
    auto response = make_file_response(context_value, source_value, body, planned);
    detail::context_access::discard_draft(context_value);
    co_return response;
}

// static_file once its representation is selected. `served` and `base_entry`
// view `root`'s snapshot, which the caller keeps alive until this completes;
// `content_type` is owned in the request pool, and an empty one selects the
// base entry's type.
[[nodiscard]] task<http_response> serve_static_file(context& context_value, const static_root& root,
    static_file_representation served, detail::static_root_entry_view base_entry, std::string content_type) {
    const auto& served_entry = served.entry();
    const auto* const memory_variant = served.memory_variant();
    // A document-root snapshot keeps small files' bytes; they describe exactly
    // the indexed metadata, so the response is framed and sent from memory.
    const auto* const resident_bytes = memory_variant == nullptr ? served_entry.resident_bytes() : nullptr;
    const bool file_backed = memory_variant == nullptr && resident_bytes == nullptr;
    const file_response_source source_value{
        .size_ = memory_variant == nullptr ? served_entry.size() : memory_variant->size(),
        .modified_token_ = memory_variant == nullptr ? served_entry.modified_token() : memory_variant->modified_token(),
        .modified_seconds_ =
            memory_variant == nullptr ? served_entry.modified_seconds() : memory_variant->modified_seconds(),
        .identity_ = file_backed ? served_entry.identity() : http_response_file_identity::unchecked(),
        .content_type_ = content_type.empty() ? base_entry.content_type() : std::string_view(content_type),
        .cache_control_ = base_entry.get_cache_control(),
        .range_requests_ = base_entry.range_requests(),
        .response_validators_ = base_entry.response_validators(),
        .precomputed_etag_ = memory_variant == nullptr ? served_entry.etag() : memory_variant->etag(),
        .precomputed_last_modified_ =
            memory_variant == nullptr ? served_entry.last_modified() : memory_variant->last_modified(),
        .content_coding_ = served.content_coding(),
        // static_file negotiates the representation by Accept-Encoding.
        .negotiates_encoding_ = true,
    };
    const file_response_plan planned(context_value, source_value);

    // The indexed file is opened beneath its root on the blocking pool. One
    // that cannot be opened is gone or no longer where the index found it,
    // and its path is never resolved another way. A response that sends none
    // of its bytes checks the opened handle against the index there too.
    http_response_file_descriptor opened;
    if (file_backed) {
        opened = take_file_open(context_value,
            co_await run_file_open(context_value,
                detail::static_root_access::entry_open(root, served_entry, !planned.sends_file_bytes())));
        if (!opened) {
            throw static_file_changed_error();
        }
    }
    auto body = memory_variant != nullptr ? file_response_body_source::bytes_value(memory_variant->bytes())
                : resident_bytes != nullptr
                    ? file_response_body_source::bytes_value(*resident_bytes)
                    : file_response_body_source::file(file_response_path(
                          http_native_path_view_type(served_entry.file_path()), served_entry.identity(),
                          std::move(opened)));
    auto response = make_file_response(context_value, source_value, body, planned);
    detail::context_access::discard_draft(context_value);
    co_return response;
}

}  // namespace

// Not a coroutine: the options are owned before the task is returned, so a
// stored task never reads the caller's path or content type.
task<http_response> context::file(file_response_options options) {
    http_native_path_string_type native_path;
    if (!assign_native_file_path(options.path_, native_path)) {
        throw file_not_found_error();
    }
    return serve_file(*this, std::move(native_path), std::string(options.content_type_.view()));
}

// Not a coroutine: the path is resolved and the content type owned before the
// task is returned; only `root` stays borrowed.
task<http_response> context::static_file(const static_root& root, static_file_response_options options) {
    // relative_path_ is a decoded file path -- a wildcard capture from
    // req().param("*") is passed as is -- so it is never percent-decoded here:
    // a second decode would turn "%252E%252E" into a ".." no route or path
    // scope ever saw. normalize_static_relative_path rejects "." and ".."
    // segments, absolute paths and NUL, and the lookup is a byte-exact
    // index compare that never joins the path onto the filesystem.
    std::string relative_storage;
    auto relative = options.relative_path_.view();
    const auto entry_value = detail::static_root_access::resolve(root, relative, relative_storage);
    if (!entry_value.has_value()) {
        throw file_not_found_error();
    }
    const auto mode = services().precompressed_static_files() ? detail::static_file_selection_mode::precompressed
                                                              : detail::static_file_selection_mode::identity_only;
    return detail::context_access::static_file_resolved(
        *this, root, relative, options.content_type_.view(), *entry_value, mode);
}

task<http_response> detail::context_access::static_file_resolved(
    context& context_value, const static_root& root, std::string_view relative,
    std::string_view content_type_value, detail::static_root_entry_view base_entry,
    detail::static_file_selection_mode mode) {
    // Serve a precompressed variant when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    // Admission already folded Accept-Encoding; only a context created outside
    // admission negotiates from the request here.
    const auto served = [&]() -> std::optional<static_file_representation> {
        if (mode == detail::static_file_selection_mode::identity_only) {
            return static_file_representation(base_entry, http_content_coding::identity);
        }
        if (const auto* admitted = context_value.services().response_coding()) {
            return select_static_file_representation(root, relative, *admitted, base_entry);
        }
        return select_static_file_representation(
            root, relative, http_response_coding_selection_for(context_value.request_), base_entry);
    }();
    if (!served.has_value()) {
        throw http_error(detail::http_not_acceptable_coding_error());
    }
    return serve_static_file(
        context_value, root, *served, base_entry, std::string(content_type_value));
}

}  // namespace ruvia

namespace ruvia {

namespace detail {

// A media-type mismatch is the client speaking the wrong format at a valid
// endpoint: RFC 9110 15.5.16 assigns that 415, distinct from the 400 a
// malformed body of the RIGHT type earns below.
[[noreturn]] void throw_invalid_json_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type,
        .code_ = "unsupported_media_type",
        .message_ = "request body must be application/json"});
}

[[noreturn]] void throw_invalid_json_body() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid json body"});
}

[[noreturn]] void throw_invalid_form_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type,
        .code_ = "unsupported_media_type",
        .message_ = "request body must be application/x-www-form-urlencoded"});
}

[[noreturn]] void throw_invalid_query() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid query"});
}

[[noreturn]] void throw_invalid_param() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid route parameter"});
}

}  // namespace detail

namespace {

[[nodiscard]] const request_name_value_list& empty_request_fields() noexcept {
    static const request_name_value_list empty =
        detail::request_name_value_list_access::make();
    return empty;
}

// Unencoded names and values borrow the request query string; decoded ones
// live in the request arena. Form decoding keeps an invalid '%' literally.
void ensure_request_query(const http_request& request,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.query_;
    if (cache) {
        return;
    }
    std::size_t pair_count = 0;
    std::size_t decoded_capacity = 0;
    (void)visit_url_encoded_pairs(request.query_string(), [&](std::string_view key, std::string_view value) {
        ++pair_count;
        if (has_url_encoding(key, url_decode_mode::form)) {
            decoded_capacity += key.size();
        }
        if (has_url_encoding(value, url_decode_mode::form)) {
            decoded_capacity += value.size();
        }
        return true;
    });
    auto& cached = cache.emplace();
    auto& decoded_bytes = cached.decoded_bytes();
    decoded_bytes.resize(decoded_capacity);
    std::size_t cursor = 0;
    const auto decode_form = [&](std::string_view input) noexcept -> std::string_view {
        if (!has_url_encoding(input, url_decode_mode::form)) {
            return input;
        }
        char* const out = decoded_bytes.data() + cursor;
        const auto size = *detail::decode_url_bytes(input, out, url_decode_mode::form, false);
        cursor += size;
        return std::string_view(out, size);
    };
    auto& query = cached.mutable_fields();
    detail::request_name_value_list_access::reserve(query, pair_count);
    (void)visit_url_encoded_pairs(request.query_string(), [&](std::string_view key, std::string_view value) {
        const auto name = decode_form(key);
        const auto decoded_value = decode_form(value);
        detail::request_name_value_list_access::push_back(query, http_header_view{name, decoded_value});
        return true;
    });
}

// Counts encoded captures only; decoding validates them once, in ensure_route_params.
[[nodiscard]] std::size_t count_encoded_route_params(const std::string_view* param_values,
    std::size_t param_count, detail::context_request_storage& request_storage) {
    if (request_storage.route_params_unencoded_) {
        return 0;
    }
    std::size_t encoded_count = 0;
    for (std::size_t i = 0; i < param_count; ++i) {
        encoded_count += has_url_encoding(param_values[i], url_decode_mode::percent);
    }
    if (encoded_count == 0) {
        request_storage.route_params_unencoded_ = true;
    }
    return encoded_count;
}

// Route names and unencoded captures borrow stable route/request storage; a
// "*" capture rejects escapes revealing a path separator, as static paths do.
void ensure_route_params(const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.route_params_;
    if (cache) {
        return;
    }
    // A capture that failed to decode keeps failing without another decode.
    if (request_storage.route_params_invalid_) {
        detail::throw_invalid_param();
    }
    std::size_t decoded_capacity = 0;
    for (std::size_t i = 0; i < param_count; ++i) {
        if (has_url_encoding(param_values[i], url_decode_mode::percent)) {
            decoded_capacity += param_values[i].size();
        }
    }
    auto& decoded_bytes = request_storage.decoded_route_params_;
    decoded_bytes.clear();
    decoded_bytes.resize(decoded_capacity);
    std::size_t cursor = 0;
    auto params = detail::request_name_value_list_access::make();
    detail::request_name_value_list_access::reserve(params, param_count);
    for (std::size_t i = 0; i < param_count; ++i) {
        std::string_view decoded_value = param_values[i];
        if (has_url_encoding(decoded_value, url_decode_mode::percent)) {
            char* const out = decoded_bytes.data() + cursor;
            const auto size = detail::decode_url_bytes(
                decoded_value, out, url_decode_mode::percent, param_names[i] == "*");
            if (!size) {
                decoded_bytes.clear();
                request_storage.route_params_invalid_ = true;
                detail::throw_invalid_param();
            }
            decoded_value = std::string_view(out, *size);
            cursor += *size;
        }
        detail::request_name_value_list_access::push_back(params, http_header_view{param_names[i], decoded_value});
    }
    cache.emplace(std::move(params));
}

[[nodiscard]] std::string_view negotiation_header_name(context_request::negotiable_field field) noexcept {
    switch (field) {
        case context_request::negotiable_field::language:
            return "Accept-Language";
        case context_request::negotiable_field::encoding:
            return "Accept-Encoding";
        case context_request::negotiable_field::charset:
            return "Accept-Charset";
        case context_request::negotiable_field::media_type:
            break;
    }
    return "Accept";
}

[[nodiscard]] multipart_boundary get_multipart_boundary(const http_request& request) {
    const auto boundary = parse_multipart_boundary(
        request.header("Content-Type").value_or(std::string_view{}));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (boundary.not_applicable() != nullptr) {
        throw http_error({.status_ = http_status::unsupported_media_type,
            .code_ = "unsupported_media_type",
            .message_ = "request body must be multipart/form-data"});
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocol_error();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

// Decoded bytes a request body may produce on the event loop. A body that
// decodes past it (a large body, or a few-KiB compression bomb) is decoded
// again on the worker's bounded blocking pool. Without a pool, or when the pool
// rejects the job, it is decoded inline: the handler requires the content, and
// the route's decoded-size cap still bounds the work.
inline constexpr std::size_t inline_request_decode_bytes = 64 * 1024;

[[nodiscard]] task<std::variant<std::string, http_content_decode_error>> decode_request_content(
    const worker_ref& worker_value, const stop_token& stop_token_value,
    std::span<const http_content_coding> codings, std::string_view raw,
    http_content_decode_options options) {
    if (worker_value.blocking_pool() != nullptr && options.max_decoded_bytes_ > inline_request_decode_bytes) {
        if (raw.size() <= inline_request_decode_bytes) {
            auto inline_result = decode_http_content(codings, raw,
                {.max_decoded_bytes_ = inline_request_decode_bytes});
            const auto* failure = std::get_if<http_content_decode_error>(&inline_result);
            if (failure == nullptr || *failure != http_content_decode_error::decoded_size_exceeded) {
                co_return inline_result;
            }
        }
        // The job carries only its own copies; request memory stays on this worker.
        std::array<http_content_coding, max_content_codings> owned_codings{};
        const auto coding_count = (std::min)(codings.size(), max_content_codings);
        std::copy_n(codings.data(), coding_count, owned_codings.data());
        std::string owned_raw(raw);
        auto offloaded = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value},
            [owned_codings, coding_count, owned_raw = std::move(owned_raw),
                limit = options.max_decoded_bytes_]() {
                return decode_http_content(std::span<const http_content_coding>(owned_codings.data(), coding_count),
                    owned_raw, {.max_decoded_bytes_ = limit});
            });
        // A codec failure on the pool is rethrown here; pool saturation or
        // shutdown throws blocking_operation_rejected (mapped to 503).
        co_return std::move(offloaded).take_value();
    }
    co_return decode_http_content(codings, raw, options);
}

// Rejects an invalid or unsupported Content-Encoding; the result owns the codings span.
[[nodiscard]] http_content_coding_field_result require_decodable_request_coding(
    const http_request& request, detail::unsupported_request_content_coding::read_type read) {
    auto parsed_coding = request_content_coding(request);
    if (const auto* invalid = parsed_coding.invalid()) {
        throw http_protocol_error(
            invalid->status(), "invalid_content_encoding", "invalid request Content-Encoding");
    }
    if (parsed_coding.unsupported() != nullptr) {
        throw detail::unsupported_request_content_coding(read);
    }
    return parsed_coding;
}

task<std::string_view> read_request_text(const context& context_value, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "request body read");
    }
    if (const auto decoded = detail::context_access::decoded_body(context_value)) {
        co_return *decoded;
    }

    const auto& body_source = detail::context_access::request_body_source(context_value);
    const auto& request = detail::context_access::request(context_value);
    std::string_view raw;
    if (const auto* lazy = body_source.lazy()) {
        raw = co_await lazy->loader().read_all(std::move(operation));
    } else if (body_source.streaming() != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = as_chars(request.body_bytes());
    }
    if (detail::context_access::body_identity(context_value)) {
        co_return raw;
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto& services = detail::context_access::services(context_value);
    const auto parsed_coding = require_decodable_request_coding(
        request, detail::unsupported_request_content_coding::read_type::decoded);
    const auto codings = parsed_coding.codings();
    if (codings.empty()) {
        detail::context_access::mark_body_identity(context_value);
        co_return raw;
    }
    auto decode_result = co_await decode_request_content(services.worker(), context_value.stop_token(), codings, raw,
        {.max_decoded_bytes_ = services.max_decoded_request_body_bytes()});
    auto* decoded_content = std::get_if<std::string>(&decode_result);
    if (decoded_content == nullptr) {
        throw http_request_content_decode_error(std::get<http_content_decode_error>(decode_result));
    }
    co_return detail::context_access::store_decoded_body(context_value, std::move(*decoded_content));
}

task<std::span<const std::byte>> read_request_bytes(const context& context_value, operation_options operation) {
    const auto body = co_await read_request_text(context_value, std::move(operation));
    co_return std::as_bytes(std::span(body));
}

task<std::vector<multipart_part>> read_request_multipart(const context& context_value, operation_options operation) {
    const auto boundary = get_multipart_boundary(detail::context_access::request(context_value));
    const auto request_body = co_await read_request_text(context_value, std::move(operation));
    co_return parse_multipart_body(request_body, {.boundary_ = boundary});
}

task<void> discard_request_body(const context& context_value, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "request body read");
    }
    const auto& body_source = detail::context_access::request_body_source(context_value);
    if (const auto* lazy = body_source.lazy()) {
        co_await lazy->loader().discard(std::move(operation));
        co_return;
    }
    if (const auto* streaming = body_source.streaming()) {
        const detail::operation_steps steps(std::move(operation));
        while (co_await streaming->reader().read(steps.next("request body read"))) {
        }
    }
}

}  // namespace

std::string_view context_request::method() const noexcept {
    return context_->request_.method();
}

http_known_method context_request::known_method() const noexcept {
    return context_->request_.known_method();
}

std::string_view context_request::path() const noexcept {
    return context_->request_.path();
}

std::string_view context_request::scheme() const noexcept {
    return context_->request_.scheme();
}

std::string_view context_request::authority() const noexcept {
    return context_->request_.authority();
}

std::string_view context_request::target() const noexcept {
    return context_->request_.target();
}

http_protocol_version context_request::protocol_version() const noexcept {
    return context_->request_.protocol_version();
}

http_request_target_form context_request::target_form() const noexcept {
    return context_->request_.target_form();
}

std::string_view context_request::route_path() const noexcept {
    return context_->route_path_;
}

http_priority context_request::priority() const noexcept {
    if (const auto* update = context_->services().request_priority_update(); update != nullptr && *update) {
        return **update;
    }
    const auto parsed_value = parse_http_priority(context_->request_.headers());
    return (parsed_value.index() == 0) ? std::get<0>(parsed_value).request_priority() : http_priority{};
}

void context_response_editor::priority(http_priority_fields fields_value) {
    std::array<char, 12> value{};
    const auto encoded = encode_http_priority(value, fields_value);
    if ((encoded.index() != 0)) {
        throw std::invalid_argument("invalid HTTP priority parameters");
    }
    header("Priority", std::string_view(value.data(), std::get<0>(encoded)));
}

std::optional<std::string_view> context_request::header(std::string_view name) const {
    return context_->request_.header(name);
}

std::span<const http_header> context_request::trailers() const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->fields()
                                                              : std::span<const http_header>{};
}

std::optional<std::string_view> context_request::trailer(std::string_view name) const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->field(name)
                                                              : std::nullopt;
}

const request_name_value_list& context_request::header_fields() const {
    auto& cache = context_->request_storage().headers_;
    if (!cache) {
        cache.emplace(detail::request_name_value_list_access::borrow_headers(context_->request_.headers()));
    }
    return *cache;
}

const request_name_value_list& context_request::query_fields() const {
    if (context_->request_.query_string().empty()) {
        return empty_request_fields();
    }
    auto& storage = context_->request_storage();
    ensure_request_query(context_->request_, storage);
    return storage.query_->fields();
}

std::optional<std::string_view> context_request::query(std::string_view name) const {
    if (context_->request_.query_string().empty()) {
        return std::nullopt;
    }
    auto& storage = context_->request_storage();
    ensure_request_query(context_->request_, storage);
    return storage.query_->fields().get(name);
}

std::span<const std::string_view> context_request::queries(std::string_view name) const {
    if (context_->request_.query_string().empty()) {
        return {};
    }
    auto& storage = context_->request_storage();
    ensure_request_query(context_->request_, storage);
    return storage.query_->values().values(name);
}

std::optional<std::string_view> context_request::cookie(std::string_view name) const {
    if (const auto& cache = context_->request_storage().cookies_) {
        return cache->get(name);
    }
    return context_->request_.cookie(name);
}

const request_name_value_list& context_request::cookie_fields() const {
    auto& cache = context_->request_storage().cookies_;
    if (cache) {
        return *cache;
    }
    // Pool-backed growth returns replaced descriptor blocks immediately.
    // Keep the candidate private until every Cookie field has been visited.
    std::optional<request_name_value_list> cookies;
    for (const auto& header_field : context_->request_.headers()) {
        if (header_field.name().size() != 6U ||
            !http_ascii_equals_ignore_case(header_field.name(), "Cookie")) {
            continue;
        }
        if (!cookies) {
            cookies.emplace(detail::request_name_value_list_access::make());
        }
        http_visit_cookie_pairs(header_field.value(),
            [&cookies](std::string_view key, std::string_view value) {
                detail::request_name_value_list_access::push_back(
                    *cookies, http_header_view{key, value});
                return true;
            });
    }
    if (!cookies) {
        return empty_request_fields();
    }
    cache.emplace(std::move(*cookies));
    return *cache;
}

const request_name_value_list& context_request::param_fields() const {
    if (context_->param_count_ == 0) {
        return empty_request_fields();
    }
    auto& storage = context_->request_storage();
    ensure_route_params(context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return *storage.route_params_;
}

std::optional<std::string_view> context_request::param(std::string_view name) const {
    if (context_->param_count_ == 0) {
        return std::nullopt;
    }
    auto& storage = context_->request_storage();
    if (storage.route_params_) {
        return storage.route_params_->get(name);
    }
    if (count_encoded_route_params(context_->param_values_, context_->param_count_, storage) == 0) {
        for (std::size_t i = context_->param_count_; i > 0; --i) {
            if (context_->param_names_[i - 1] == name) {
                return context_->param_values_[i - 1];
            }
        }
        return std::nullopt;
    }
    ensure_route_params(context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return storage.route_params_->get(name);
}

std::optional<std::string_view> context_request::negotiate(
    negotiable_field field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const bool media_type = field == negotiable_field::media_type;
    const bool encoding = field == negotiable_field::encoding;
    const auto header_name = negotiation_header_name(field);
    // Only Accept-Language needs prefix matching in the generic token parser.
    const auto token_mode = field == negotiable_field::language
                                ? http_accept_token_match_mode::language_prefix
                                : http_accept_token_match_mode::exact;

    static constexpr std::size_t inline_value_capacity = 8;
    std::array<std::string_view, inline_value_capacity> inline_values{};
    std::size_t matching_count = 0;
    bool saw_header = false;
    const auto headers = context_->request_.headers();
    for (const auto& header_field : headers) {
        if (!http_ascii_equals_ignore_case(header_field.name(), header_name)) {
            continue;
        }
        saw_header = true;
        if (header_field.value().empty()) {
            continue;
        }
        if (matching_count < inline_values.size()) {
            inline_values[matching_count] = header_field.value();
        }
        ++matching_count;
    }
    if (!saw_header) {
        return supported.front();
    }
    if (matching_count == 0 && !encoding) {
        return std::nullopt;
    }
    std::optional<std::string_view> best;
    int best_quality = 0;
    const bool use_inline_values = matching_count <= inline_values.size();
    for (const auto offered : supported) {
        http_accept_match match;
        http_accepted_encoding_quality encoding_quality;
        const auto accumulate = [&](std::string_view field_value) noexcept {
            if (media_type) {
                match.update_media_type(field_value, offered);
            } else if (encoding) {
                encoding_quality.update(field_value, offered);
            } else {
                match.update_token(field_value, offered, token_mode);
            }
        };
        if (use_inline_values) {
            for (std::size_t i = 0; i < matching_count; ++i) {
                accumulate(inline_values[i]);
            }
        } else {
            for (const auto& header_field : headers) {
                if (http_ascii_equals_ignore_case(header_field.name(), header_name) &&
                    !header_field.value().empty()) {
                    accumulate(header_field.value());
                }
            }
        }
        const auto quality = encoding ? encoding_quality.quality(http_ascii_equals_ignore_case(offered, "identity"))
                                      : match.quality();
        if (quality == 0) {
            continue;
        }
        // Strictly greater, so `supported` order breaks the client's ties and
        // reads as the server's own preference.
        if (quality > best_quality) {
            best_quality = quality;
            best = offered;
        }
    }

    // An empty Accept-Encoding accepts identity; other empty negotiation fields
    // match no representation.
    return best;
}

task<std::string_view> context_request::text(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_text(*context_, std::move(operation)));
}

task<std::span<const std::byte>> context_request::bytes(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_bytes(*context_, std::move(operation)));
}

std::optional<std::string_view> context_request::signed_cookie(
    signed_cookie_lookup_options options) const {
    const auto name = options.name_.view();
    const auto secret = options.secret_.view();
    const auto stored = cookie(name);
    if (!stored.has_value() || stored->size() <= detail::cookie_signature_size) {
        return std::nullopt;
    }
    const auto value_size = stored->size() - detail::cookie_signature_size - 1;
    if ((*stored)[value_size] != '.') {
        return std::nullopt;
    }
    const auto value = stored->substr(0, value_size);
    const auto signature = stored->substr(value_size + 1);
    char expected[detail::cookie_signature_size];
    detail::write_cookie_signature(expected, secret, name, value);
    if (!detail::constant_time_bytes_equal(signature, std::string_view(expected, sizeof(expected)))) {
        return std::nullopt;
    }
    return value;
}

task<std::vector<multipart_part>> context_request::multipart(operation_options operation) const {
    return ::ruvia::make_scoped_operation(
        context_->operation_scope_, read_request_multipart(*context_, std::move(operation)));
}

task<void> context_request::discard_body(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, discard_request_body(*context_, std::move(operation)));
}

body_reader& context_request::body_reader() const {
    const auto* streaming = context_->request_body_source().streaming();
    if (streaming == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return streaming->reader();
}

multipart_reader context_request::multipart_reader() const {
    const auto boundary = get_multipart_boundary(context_->request_);
    // The reader parses raw body bytes as they arrive and decodes no content
    // coding, so only an identity body is accepted.
    const auto parsed_coding = require_decodable_request_coding(
        context_->request_, detail::unsupported_request_content_coding::read_type::streaming);
    if (std::ranges::any_of(parsed_coding.codings(),
            [](http_content_coding coding) { return coding != http_content_coding::identity; })) {
        throw detail::unsupported_request_content_coding(
            detail::unsupported_request_content_coding::read_type::streaming);
    }
    return ::ruvia::multipart_reader(body_reader(), {.boundary_ = boundary});
}
}  // namespace ruvia

namespace ruvia::detail {
class http_capsule_stream_state final {
public:
    http_capsule_stream_state(http_tunnel& tunnel, http_capsule_config config)
        : worker_(tunnel.worker_),
          server_(&tunnel),
          decoder_(config),
          config_(config),
          inbound_(tunnel.inbound_),
          payload_reservation_(inbound_) {}
    http_capsule_stream_state(http_client_tunnel&& tunnel, http_capsule_config config)
        : worker_(tunnel.response_.state_->memory_domain().worker()),
          memory_pin_(http_client_response(tunnel.response_.state_)),
          client_(std::move(tunnel)),
          decoder_(config),
          config_(config) {}
    void require() const {
        if (closed_ || (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr))) {
            throw std::logic_error("capsule stream is closed");
        }
        if (!worker_.is_current()) {
            throw std::logic_error("capsule stream requires its owner worker");
        }
    }
    [[nodiscard]] operation_lane_lease claim_lane(bool output) {
        require();
        return claim_operation_lane(output ? output_active_ : read_active_, "capsule stream operation is already active");
    }
    void retain() noexcept {
        ++references_;
    }
    void release() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (--references_ != 0) {
            return;
        }
        auto pin = std::move(memory_pin_);
        delete this;
    }
    void abort() noexcept {
        abort_input(false);
    }
    void abort_malformed() noexcept {
        abort_input(true);
    }
    void abort_input(bool malformed) noexcept {
        if (closed_) {
            return;
        }
        if (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr)) {
            closed_ = true;
            return;
        }
        if (!worker_.is_current()) {
            std::terminate();
        }
        closed_ = true;
        if (server_) {
            server_->abort_input(malformed);
        } else if (client_) {
            if (malformed) {
                client_->abort_malformed();
            } else {
                client_->abort();
            }
        }
    }
    void clear_input() noexcept {
        input_ = {};
        owned_input_.reset();
        offset_ = 0;
        native_input_ = false;
    }
    void close() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (read_active_ || output_active_) {
            std::terminate();
        }
        if (client_) {
            abort();
        } else {
            closed_ = true;
        }
        read_scope_.close();
        output_scope_.close();
        clear_input();
        server_ = nullptr;
    }
    [[nodiscard]] bool poll_capsule(bool fin, http_capsule_retention retention) {
        const auto collect = [](void* raw, http_capsule_event event) {
            auto& state = *static_cast<http_capsule_stream_state*>(raw);
            state.type_ = event.type_;
            if (!event.payload_.empty()) {
                state.payload_.append(event.payload_.data(), event.payload_.size());
            }
        };
        const auto bytes_value = std::span<const char>(input_).subspan(offset_);
        const auto result_value = decoder_.feed_one(bytes_value, fin, collect, this, retention);
        offset_ += result_value.consumed_bytes_;
        if (result_value.status_ == http_capsule_status::limit || result_value.status_ == http_capsule_status::truncated) {
            abort_malformed();
            throw std::runtime_error("invalid or oversized capsule stream");
        }
        if (result_value.status_ == http_capsule_status::end) {
            receive_ended_ = true;
        }
        return result_value.capsule_complete_;
    }
    // Interruptions are std::system_error (timed_out / operation_canceled);
    // a client tunnel reports them as its http_client_error subclass.
    [[noreturn]] void throw_interrupted(operation_interrupt_cause cause, const char* operation_name) const {
        if (client_) {
            throw http_client_error(std::make_error_code(cause == operation_interrupt_cause::timed_out
                                                             ? std::errc::timed_out
                                                             : std::errc::operation_canceled),
                operation_name);
        }
        operation_interrupt::throw_cause(cause, operation_name);
    }
    // A stop requested before an operation starts leaves the stream untouched.
    void throw_if_stopped_before_start(const operation_options& operation, const char* operation_name) const {
        if (operation.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt_cause::cancelled, operation_name);
        }
    }
    [[nodiscard]] operation_options next_step(const operation_steps& steps, const char* operation_name) const {
        auto step = steps.try_next();
        if (!step.has_value()) {
            throw_interrupted(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }
    // Whether the exception being handled is a step interrupted by `steps`.
    [[nodiscard]] static bool interrupted(const operation_steps& steps) noexcept {
        try {
            throw;
        } catch (const std::system_error& error) {
            return steps.interrupted(error);
        } catch (...) {
            return false;
        }
    }
    [[nodiscard]] task<bool> read_input(bool datagrams, const operation_steps& steps) {
        clear_input();
        if (datagrams && has_native_transport()) {
            std::optional<http_tunnel_input> chunk;
            if (server_) {
                chunk = co_await server_->read_datagram_raw(next_step(steps, "capsule read"));
            }
#ifdef RUVIA_ENABLE_HTTP3
            else {
                auto& state = *client_->response_.state_;
                chunk = co_await state.pool_->read_http3_tunnel_input(state, next_step(steps, "capsule read"));
            }
#endif
            if (!chunk) {
                co_return false;
            }
            native_input_ = chunk->datagram_;
            owned_input_.emplace(std::move(chunk->bytes_));
            input_ = *owned_input_;
            co_return true;
        }
        if (server_) {
            auto chunk = co_await server_->read_raw(next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            owned_input_.emplace(std::move(*chunk));
            input_ = *owned_input_;
        } else {
            auto chunk = co_await client_->read(next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            input_ = as_chars(*chunk);
        }
        co_return true;
    }
    [[nodiscard]] bool has_native_transport() const noexcept {
        if (server_) {
            return server_->read_datagram_ != nullptr && server_->write_datagram_ != nullptr;
        }
#ifdef RUVIA_ENABLE_HTTP3
        return client_ && client_->response_.state_->transport_ == http_client_response_transport::http3;
#else
        return false;
#endif
    }
    [[nodiscard]] task<bool> send_datagram(std::string_view bytes, const operation_steps& steps) {
        if (server_) {
            co_return co_await server_->write_datagram_raw(bytes, next_step(steps, "capsule write"));
        }
#ifdef RUVIA_ENABLE_HTTP3
        auto& state = *client_->response_.state_;
        co_return co_await state.pool_->send_http3_datagram(state, bytes, next_step(steps, "capsule write"));
#else
        co_return false;
#endif
    }
    [[nodiscard]] task<void> send_bytes(std::string&& bytes_value, const operation_steps& steps) {
        if (server_ != nullptr) {
            co_await server_->write_raw(std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        const auto limit = client_->response_.state_->tunnel_->config_.max_chunk_bytes_;
        if (bytes_value.size() <= limit) {
            co_await client_->write_owned(std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        std::string_view remaining = bytes_value;
        while (!remaining.empty()) {
            const auto chunk = remaining.substr(0, limit);
            co_await client_->write(chunk, next_step(steps, "capsule write"));
            remaining.remove_prefix(chunk.size());
        }
    }
    const worker_ref& worker_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* server_{};
    std::optional<http_client_tunnel> client_;
    http_capsule_decoder decoder_;
    http_capsule_config config_;
    std::optional<std::string> owned_input_;
    std::string_view input_;
    std::size_t offset_{};
    bool native_input_{};
    inbound_buffer_account* inbound_{nullptr};
    std::string payload_;
    inbound_buffer_reservation payload_reservation_{};
    std::uint64_t type_{};
    bool receive_ended_{};
    bool send_ended_{};
    bool closed_{};
    bool read_active_{};
    bool output_active_{};
    std::size_t references_{1};
    ::ruvia::operation_scope read_scope_;
    ::ruvia::operation_scope output_scope_;
};
class capsule_state_pin final {
public:
    explicit capsule_state_pin(http_capsule_stream_state& state_value) noexcept
        : state_(&state_value) {
        state_->retain();
    }
    capsule_state_pin(capsule_state_pin&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
    capsule_state_pin(const capsule_state_pin&) = delete;
    ~capsule_state_pin() {
        if (state_) {
            state_->release();
        }
    }
    [[nodiscard]] http_capsule_stream_state& state() const noexcept {
        return *state_;
    }

private:
    http_capsule_stream_state* state_;
};
struct capsule_write_input final {
    capsule_state_pin pin_;
    std::string bytes_;
    bool datagram_{};
    operation_options operation_;
};
// Failure aborts the stream, except an interrupted read, which keeps the
// partly received capsule for the next read.
class running_capsule_operation final {
public:
    running_capsule_operation(http_capsule_stream_state& state_value, bool output)
        : state_(state_value),
          exceptions_(std::uncaught_exceptions()),
          lane_(state_value.claim_lane(output)) {}
    ~running_capsule_operation() {
        if (aborts_ && std::uncaught_exceptions() > exceptions_) {
            state_.abort();
        }
    }
    void keep_stream() noexcept {
        aborts_ = false;
    }

private:
    http_capsule_stream_state& state_;
    int exceptions_;
    bool aborts_{true};
    operation_lane_lease lane_;
};
}  // namespace ruvia::detail

namespace ruvia {
namespace {
constexpr const char* tunnel_lane_in_progress = "HTTP tunnel operation already in progress on this direction";
template <typename input_type>
task<std::optional<input_type>> read_tunnel(void* target,
    task<std::optional<input_type>> (*read)(void*, operation_options), bool& read_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, tunnel_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view, operation_options),
    std::string bytes_value, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_await write(target, bytes_value, std::move(operation));
}
task<bool> write_tunnel_datagram(void* target, task<bool> (*write)(void*, std::string_view, operation_options),
    std::string_view bytes, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_return co_await write(target, bytes, std::move(operation));
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*, operation_options),
    bool& ended, bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (!ended) {
        co_await finish_value(target, std::move(operation));
        ended = true;
    }
}
}  // namespace

http_tunnel::http_tunnel(detail::inbound_buffer_account* inbound, const worker_ref& worker_value,
    void* target, read_type read, write_type write, finish_type finish_value, abort_type abort,
    bool datagrams, read_datagram_type read_datagram, write_datagram_type write_datagram) noexcept
    : inbound_(inbound),
      worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort),
      read_datagram_(read_datagram),
      write_datagram_(write_datagram),
      datagrams_(datagrams) {
    operations_.bind_worker(worker_);
}

void http_tunnel::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("HTTP tunnel session has expired");
    }
}
void http_tunnel::require_unadapted() const {
    if (adapted_) {
        throw std::logic_error("HTTP tunnel is already adapted to a capsule stream");
    }
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read(operation_options operation) & {
    require_active();
    require_unadapted();
    return ::ruvia::make_scoped_operation(operations_, read_borrowed(std::move(operation)));
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read_borrowed(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, tunnel_lane_in_progress);
    // The previous view expires as this read starts.
    read_chunk_.reset();
    const detail::operation_steps steps(std::move(operation));
    for (;;) {
        auto chunk = co_await read_(target_, steps.next("HTTP tunnel read"));
        if (!chunk) {
            co_return std::nullopt;
        }
        if (!chunk->empty()) {
            read_chunk_.emplace(std::move(*chunk));
            co_return as_bytes(std::string_view(*read_chunk_));
        }
    }
}
task<std::optional<std::string>> http_tunnel::read_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, read_active_, std::move(operation)));
}
task<std::optional<detail::http_tunnel_input>> http_tunnel::read_datagram_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, read_tunnel(target_, read_datagram_, read_active_, std::move(operation)));
}
task<bool> http_tunnel::write_datagram_raw(std::string_view bytes, operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_,
        write_tunnel_datagram(target_, write_datagram_, bytes, write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::write(std::string_view bytes_value, operation_options operation) & {
    require_active();
    require_unadapted();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    return write_raw(std::string(bytes_value), std::move(operation));
}
task<void> http_tunnel::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(as_chars(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_owned(std::string&& bytes_value, operation_options operation) {
    require_active();
    require_unadapted();
    return write_raw(std::move(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_raw(std::string&& bytes_value, operation_options operation) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    auto owned = std::move(bytes_value);
    return ::ruvia::make_scoped_operation(operations_,
        write_tunnel(target_, write_, std::move(owned), write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::finish(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, finish_tunnel(target_, finish_, send_ended_, write_active_, std::move(operation)));
}
void http_tunnel::abort() & noexcept {
    abort_input(false);
}
void http_tunnel::abort_input(bool malformed) noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_, malformed);
    }
}

http_capsule::http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::string payload_value) noexcept
    : state_(&state_value),
      type_(type),
      payload_(std::move(payload_value)) {
    state_->retain();
}
http_capsule::http_capsule(http_capsule&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      type_(other.type_),
      payload_(std::move(other.payload_)) {
    other.payload_.reset();
}
http_capsule& http_capsule::operator=(http_capsule&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
        type_ = other.type_;
        // Reconstruct rather than propagate the previous capsule's allocator.
        if (other.payload_) {
            payload_.emplace(std::move(*other.payload_));
            other.payload_.reset();
        }
    }
    return *this;
}
http_capsule::~http_capsule() {
    release();
}
void http_capsule::release() noexcept {
    // Payload storage is returned before its domain pin can retire the pool.
    if (state_ && !state_->worker_.is_current()) {
        std::terminate();
    }
    payload_.reset();
    if (auto* state = std::exchange(state_, nullptr)) {
        state->release();
    }
}
http_capsule_stream::http_capsule_stream(http_tunnel& tunnel, http_capsule_config config) {
    tunnel.require_active();
    tunnel.require_unadapted();
    state_ = new detail::http_capsule_stream_state(tunnel, config);
    tunnel.adapted_ = true;
}
http_capsule_stream::http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config) {
    if (!tunnel.response_.state_ || !tunnel.response_.state_->memory_domain().worker().is_current()) {
        throw std::logic_error("capsule stream requires a live tunnel on its worker");
    }
    // Validate before consuming the caller's owner.
    http_capsule_decoder validation(config);
    state_ = new detail::http_capsule_stream_state(std::move(tunnel), config);
}
http_capsule_stream::http_capsule_stream(http_capsule_stream&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
http_capsule_stream& http_capsule_stream::operator=(http_capsule_stream&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
http_capsule_stream::~http_capsule_stream() {
    release();
}
void http_capsule_stream::release() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) {
        state->close();
        state->release();
    }
}
void http_capsule_stream::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
// datagrams() and udp() build on this one gate.
http_capsule_stream http_tunnel::capsules(http_capsule_config config) & {
    // The route protocol owns Capsule Protocol negotiation (RFC 9297 section
    // 3.4); a route that did not declare datagrams never advertised it.
    if (!datagrams_) {
        throw std::logic_error("HTTP tunnel route did not declare datagrams");
    }
    require_unadapted();
    return http_capsule_stream(*this, config);
}
http_capsule_stream http_client_tunnel::capsules(http_capsule_config config) && {
    return http_capsule_stream(std::move(*this), config);
}
task<std::optional<http_capsule>> http_capsule_stream::read(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    return ::ruvia::make_scoped_operation(
        state_->read_scope_, read_owned(detail::capsule_state_pin(*state_), std::move(operation)));
}
task<std::optional<http_capsule>> http_capsule_stream::read_owned(detail::capsule_state_pin pin, operation_options operation) {
    auto& state_value = pin.state();
    state_value.throw_if_stopped_before_start(operation, "capsule read");
    const detail::operation_steps steps(std::move(operation));
    detail::running_capsule_operation running(state_value, false);
    if (state_value.receive_ended_) {
        co_return std::nullopt;
    }
    try {
        for (;;) {
            bool fin{};
            if (state_value.offset_ == state_value.input_.size()) {
                fin = !co_await state_value.read_input(false, steps);
            }
            if (state_value.poll_capsule(fin, http_capsule_retention::all)) {
                auto payload_value = std::move(state_value.payload_);
                state_value.payload_ = std::string{};
                co_return http_capsule(state_value, state_value.type_, std::move(payload_value));
            }
            if (state_value.receive_ended_) {
                co_return std::nullopt;
            }
        }
    } catch (...) {
        if (detail::http_capsule_stream_state::interrupted(steps)) {
            running.keep_stream();
        }
        throw;
    }
}
task<void> http_capsule_stream::write(std::uint64_t type, std::span<const std::byte> payload_value, operation_options operation) & {
    return write(type, std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_capsule_stream::write(std::uint64_t type, std::string_view payload_value, operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    if (state_->send_ended_) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encode_http_capsule_header(header, type, payload_value.size());
    if ((encoded.index() != 0) || payload_value.size() > state_->config_.max_capsule_length_) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return write_frame(std::string_view(header.data(), std::get<0>(encoded)), payload_value, false, std::move(operation));
}
task<void> http_capsule_stream::write_owned(
    std::uint64_t type, std::string&& payload_value, operation_options operation) {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    if (state_->send_ended_) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encode_http_capsule_header(header, type, payload_value.size());
    if ((encoded.index() != 0) || payload_value.size() > state_->config_.max_capsule_length_) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return write_frame_owned(std::string_view(header.data(), std::get<0>(encoded)),
        std::move(payload_value), false, std::move(operation));
}
task<void> http_capsule_stream::write_frame(
    std::string_view prefix, std::string_view payload_value, bool datagram, operation_options operation) {
    const auto write = +[](detail::capsule_write_input input) -> task<void> {
        auto& state_value = input.pin_.state();
        state_value.throw_if_stopped_before_start(input.operation_, "capsule write");
        const detail::operation_steps steps(std::move(input.operation_));
        detail::running_capsule_operation running(state_value, true);
        if (state_value.send_ended_) {
            throw std::logic_error("capsule sending direction is closed");
        }
        try {
            if (input.datagram_) {
                if (co_await state_value.send_datagram(input.bytes_, steps)) {
                    co_return;
                }
                std::array<char, 16> header;
                const auto encoded = encode_http_capsule_header(header, http_datagram_capsule_type, input.bytes_.size());
                if (encoded.index() != 0) {
                    throw std::length_error("HTTP Datagram exceeds capsule bound");
                }
                input.bytes_.insert(0, header.data(), std::get<0>(encoded));
            }
            co_await state_value.send_bytes(std::move(input.bytes_), steps);
        } catch (...) {
            state_value.abort();
            throw;
        }
    };
    std::string bytes_value;
    bytes_value.reserve(prefix.size() + payload_value.size());
    bytes_value.append(prefix);
    bytes_value.append(payload_value);
    return ::ruvia::make_scoped_operation(state_->output_scope_,
        write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value), datagram, std::move(operation)}));
}
task<void> http_capsule_stream::write_frame_owned(
    std::string_view prefix, std::string&& payload_value, bool datagram, operation_options operation) {
    if (prefix.empty()) {
        const auto write = +[](detail::capsule_write_input input) -> task<void> {
            auto& state_value = input.pin_.state();
            state_value.throw_if_stopped_before_start(input.operation_, "capsule write");
            const detail::operation_steps steps(std::move(input.operation_));
            detail::running_capsule_operation running(state_value, true);
            if (state_value.send_ended_) {
                throw std::logic_error("capsule sending direction is closed");
            }
            try {
                if (input.datagram_) {
                    if (co_await state_value.send_datagram(input.bytes_, steps)) {
                        co_return;
                    }
                    std::array<char, 16> header;
                    const auto encoded = encode_http_capsule_header(header, http_datagram_capsule_type, input.bytes_.size());
                    if (encoded.index() != 0) {
                        throw std::length_error("HTTP Datagram exceeds capsule bound");
                    }
                    input.bytes_.insert(0, header.data(), std::get<0>(encoded));
                }
                co_await state_value.send_bytes(std::move(input.bytes_), steps);
            } catch (...) {
                state_value.abort();
                throw;
            }
        };
        auto owned = std::move(payload_value);
        return ::ruvia::make_scoped_operation(state_->output_scope_,
            write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(owned), datagram, std::move(operation)}));
    }
    return write_frame(prefix, payload_value, datagram, std::move(operation));
}
task<void> http_capsule_stream::finish(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    const auto finish_value = +[](detail::capsule_state_pin pin, operation_options finish_operation) -> task<void> {
        auto& state_value = pin.state();
        state_value.throw_if_stopped_before_start(finish_operation, "capsule finish");
        detail::running_capsule_operation running(state_value, true);
        if (!state_value.send_ended_) {
            if (state_value.server_) {
                co_await state_value.server_->finish(std::move(finish_operation));
            } else {
                co_await state_value.client_->finish(std::move(finish_operation));
            }
            state_value.send_ended_ = true;
        }
    };
    return ::ruvia::make_scoped_operation(
        state_->output_scope_, finish_value(detail::capsule_state_pin(*state_), std::move(operation)));
}

http_datagram_stream::http_datagram_stream(http_capsule_stream&& stream)
    : stream_(std::move(stream)) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
}
http_datagram_stream http_tunnel::datagrams(http_capsule_config config) & {
    return http_datagram_stream(capsules(config));
}
http_udp_tunnel http_tunnel::udp(http_capsule_config config) & {
    return http_udp_tunnel(datagrams(config));
}
http_datagram_stream http_client_tunnel::datagrams(http_capsule_config config) && {
    if (!response_.state_ || !response_.state_->tunnel_ || !response_.state_->tunnel_->udp_ || !response_.state_->tunnel_->accepted_) {
        throw std::logic_error("HTTP Datagrams require an accepted datagram tunnel");
    }
    return http_datagram_stream(std::move(*this).capsules(config));
}
task<std::optional<http_datagram>> http_datagram_stream::read(operation_options operation) & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require();
    return ::ruvia::make_scoped_operation(
        stream_.state_->read_scope_, read_owned(detail::capsule_state_pin(*stream_.state_), false, std::move(operation)));
}
task<std::optional<http_datagram>> http_datagram_stream::read_owned(
    detail::capsule_state_pin pin, bool udp, operation_options operation) {
    auto& state_value = pin.state();
    state_value.throw_if_stopped_before_start(operation, "HTTP Datagram read");
    const detail::operation_steps steps(std::move(operation));
    detail::running_capsule_operation running(state_value, false);
    try {
        for (;;) {
            if (state_value.receive_ended_) {
                co_return std::nullopt;
            }
            bool fin{};
            if (state_value.offset_ == state_value.input_.size()) {
                fin = !co_await state_value.read_input(true, steps);
            }
            // The decoder drops other capsule types and, for CONNECT-UDP, unknown
            // Context IDs before the length limit applies (RFC 9297 section 3.2).
            if (!state_value.native_input_ && !state_value.poll_capsule(
                                                  fin, udp ? http_capsule_retention::udp_payloads : http_capsule_retention::datagrams)) {
                continue;
            }
            const bool native = state_value.native_input_;
            auto& owned = native ? *state_value.owned_input_ : state_value.payload_;
            if (native) {
                state_value.offset_ = state_value.input_.size();
            }
            auto bytes_value = std::span<const char>(owned);
            http_datagram_session datagrams;
            std::size_t offset{};
            if (udp) {
                auto decoded = datagrams.receive_udp_datagram(bytes_value);
                if ((decoded.index() != 0)) {
                    state_value.abort_malformed();
                    throw std::runtime_error("malformed CONNECT-UDP datagram");
                }
                if (!std::get<0>(decoded)) {
                    if (!native) {
                        owned.clear();
                    }
                    continue;
                }
                offset = bytes_value.size() - (std::get<0>(decoded))->payload_.size();
            } else {
                auto decoded = datagrams.receive_datagram(bytes_value);
                if (!decoded) {
                    if (!native) {
                        owned.clear();
                    }
                    continue;
                }
                offset = bytes_value.size() - decoded->size();
            }
            if (native && owned.size() > state_value.config_.max_capsule_length_) {
                state_value.abort_malformed();
                throw std::runtime_error("oversized HTTP Datagram");
            }
            auto result_value = std::move(owned);
            if (native) {
                state_value.clear_input();
            } else {
                state_value.payload_ = std::string{};
            }
            co_return http_datagram(http_capsule(state_value, http_datagram_capsule_type, std::move(result_value)), offset);
        }
    } catch (...) {
        if (detail::http_capsule_stream_state::interrupted(steps)) {
            running.keep_stream();
        }
        throw;
    }
}
task<void> http_datagram_stream::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_datagram_stream::send(std::string_view payload_value, operation_options operation) & {
    return send_payload(payload_value, false, std::move(operation));
}
task<void> http_datagram_stream::send_owned(
    std::string&& payload_value, operation_options operation) {
    return send_payload_owned(std::move(payload_value), false, std::move(operation));
}
task<void> http_datagram_stream::send_payload(std::string_view payload_value, bool udp, operation_options operation) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ ||
        payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()))
                          : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame(udp ? std::string_view("\0", 1) : std::string_view{}, payload_value, true, std::move(operation));
    }
    return stream_.write_frame(std::get<0>(plan).prefix(), payload_value, false, std::move(operation));
}
task<void> http_datagram_stream::send_payload_owned(
    std::string&& payload_value, bool udp, operation_options operation) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ ||
        payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()))
                          : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame_owned(udp ? std::string_view("\0", 1) : std::string_view{},
            std::move(payload_value), true, std::move(operation));
    }
    return stream_.write_frame_owned(
        std::get<0>(plan).prefix(), std::move(payload_value), false, std::move(operation));
}
task<std::optional<http_datagram>> http_udp_tunnel::read(operation_options operation) & {
    auto* state_value = stream_.stream_.state_;
    if (!state_value) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state_value->require();
    return ::ruvia::make_scoped_operation(state_value->read_scope_,
        http_datagram_stream::read_owned(detail::capsule_state_pin(*state_value), true, std::move(operation)));
}
task<void> http_udp_tunnel::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_udp_tunnel::send(std::string_view payload_value, operation_options operation) & {
    return stream_.send_payload(payload_value, true, std::move(operation));
}
task<void> http_udp_tunnel::send_owned(
    std::string&& payload_value, operation_options operation) {
    return stream_.send_payload_owned(std::move(payload_value), true, std::move(operation));
}
}  // namespace ruvia

namespace ruvia::detail {

const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> tls_tunnel_output::bio_method_ = [] {
    std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(
        BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Ruvia TLS tunnel output"), &BIO_meth_free);
    if (!method || BIO_meth_set_create(method.get(), &create_bio) != 1 ||
        BIO_meth_set_destroy(method.get(), &destroy_bio) != 1 ||
        BIO_meth_set_write_ex(method.get(), &write_bio) != 1 ||
        BIO_meth_set_ctrl(method.get(), &control_bio) != 1) {
        throw std::bad_alloc();
    }
    return method;
}();

tls_tunnel_output::tls_tunnel_output(SSL& ssl, asio::ip::tcp::socket& socket,
    const worker_ref& worker_value, connection_lifecycle* lifecycle)
    : ssl_(ssl),
      socket_(socket),
      worker_(worker_value),
      lifecycle_(lifecycle),
      available_(worker_value),
      flushed_(worker_value),
      tasks_(worker_value) {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!SSL_is_init_finished(&ssl_)) {
        throw std::logic_error("TLS tunnel output requires an established TLS connection");
    }
    // Application writes are flushed in 4 KiB pieces. Space also covers TLS
    // control output generated from one buffered input record while a write waits.
    bytes_.resize(64 * 1024);
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(bio_method_.get()), &BIO_free);
    if (!bio) {
        throw std::bad_alloc();
    }
    previous_ = SSL_get_wbio(&ssl_);
    if (previous_ == nullptr || BIO_up_ref(previous_) != 1) {
        previous_ = nullptr;
        throw std::bad_alloc();
    }
    BIO_set_data(bio.get(), this);
    SSL_set0_wbio(&ssl_, bio.release());
}

tls_tunnel_output::~tls_tunnel_output() {
    if (!worker_.is_current() || tasks_.size() != 0) {
        std::terminate();
    }
    SSL_set0_wbio(&ssl_, previous_);
}

void tls_tunnel_output::start() {
    if (!worker_.is_current() || started_) {
        std::terminate();
    }
    tasks_.spawn(run_writer());
    started_ = true;
}

int tls_tunnel_output::create_bio(BIO* bio) noexcept {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    return 1;
}
int tls_tunnel_output::destroy_bio(BIO* bio) noexcept {
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
}
int tls_tunnel_output::write_bio(BIO* bio, const char* bytes_value, std::size_t size, std::size_t* written) noexcept {
    *written = 0;
    auto& output = *static_cast<tls_tunnel_output*>(BIO_get_data(bio));
    if (!output.worker_.is_current()) {
        std::terminate();
    }
    BIO_clear_retry_flags(bio);
    if (size == 0) {
        return 1;
    }
    const auto count = size;
    if (output.error_ || output.stopping_ || count > output.bytes_.size() - output.size_) {
        output.fail(std::make_error_code(std::errc::no_buffer_space));
        return 0;
    }
    const auto tail = (output.head_ + output.size_) % output.bytes_.size();
    const auto first = std::min(count, output.bytes_.size() - tail);
    std::memcpy(output.bytes_.data() + tail, bytes_value, first);
    std::memcpy(output.bytes_.data(), bytes_value + first, count - first);
    output.size_ += count;
    output.available_.notify();
    *written = size;
    return 1;
}
long tls_tunnel_output::control_bio(BIO*, int command, long, void*) noexcept {
    return command == BIO_CTRL_FLUSH ? 1 : 0;
}
void tls_tunnel_output::fail(std::error_code error) noexcept {
    if (!error_) {
        if (lifecycle_ != nullptr) {
            lifecycle_->note_error(error);
        }
        error_ = error;
    }
    close_socket(socket_);
    available_.notify();
    flushed_.notify();
}
void tls_tunnel_output::abort() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    fail(std::make_error_code(std::errc::operation_canceled));
}
task<void> tls_tunnel_output::run_writer() {
    try {
        co_await run_writer_inner();
    } catch (...) {
        fail(std::make_error_code(std::errc::io_error));
        throw;
    }
}
task<void> tls_tunnel_output::run_writer_inner() {
    for (;;) {
        if (error_) {
            co_return;
        }
        if (size_ == 0) {
            flushed_.notify();
            if (stopping_) {
                co_return;
            }
            co_await available_.wait();
            continue;
        }
        const auto count = std::min(size_, bytes_.size() - head_);
        const auto result_value = co_await async_asio<std::size_t>([this, count](auto handler) {
            asio::async_write(socket_, asio::buffer(bytes_.data() + head_, count), std::move(handler));
        });
        if (lifecycle_ != nullptr) {
            lifecycle_->note_write(result_value.result());
        }
        if (result_value.error_code()) {
            fail(result_value.error_code());
            co_return;
        }
        head_ = (head_ + result_value.result()) % bytes_.size();
        size_ -= result_value.result();
        flushed_.notify();
    }
}
task<std::error_code> tls_tunnel_output::flush() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    while (!error_ && size_ != 0) {
        co_await flushed_.wait();
    }
    co_return error_;
}
task<std::error_code> tls_tunnel_output::finish() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    if (error_) {
        co_return error_;
    }
    if (!send_ended_) {
        ERR_clear_error();
        const auto result_value = SSL_shutdown(&ssl_);
        if (result_value < 0) {
            const auto code = ERR_get_error();
            fail(code == 0 ? std::make_error_code(std::errc::protocol_error)
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category()));
            co_return error_;
        }
        send_ended_ = true;
    }
    co_return co_await flush();
}
task<void> tls_tunnel_output::join() {
    stopping_ = true;
    available_.notify();
    co_await tasks_.join();
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

using security_callback = int (*)(const SSL*, const SSL_CTX*, int, int, int, void*, void*);

int enforce_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept;
void guard_native_handshake(const SSL*, int, int) noexcept;

struct tls_context_policy final {
    tls_context_policy(SSL_CTX& context, std::string_view host, std::uint64_t limit)
        : host_(host),
          record_limit_(limit),
          original_security_(SSL_CTX_get_security_callback(&context)),
          original_security_data_(SSL_CTX_get0_security_ex_data(&context)),
          original_info_(SSL_CTX_get_info_callback(&context)) {
        if (original_security_ == enforce_security) {
            const auto* previous = static_cast<const tls_context_policy*>(original_security_data_);
            original_security_ = previous->original_security_;
            original_security_data_ = previous->original_security_data_;
            if (original_info_ == guard_native_handshake) {
                original_info_ = previous->original_info_;
            }
        }
        if (!is_client_ip_address(host_) && host_.ends_with('.')) {
            host_.pop_back();
        }
    }
    std::string host_;
    std::uint64_t record_limit_;
    security_callback original_security_;
    void* original_security_data_;
    void (*original_info_)(const SSL*, int, int);
};

int enforce_security(const SSL* ssl, const SSL_CTX* context, int operation, int bits, int nid,
    void* other, void* opaque) noexcept {
    const auto& policy = *static_cast<const tls_context_policy*>(opaque);
    if (operation == SSL_SECOP_VERSION) {
        if (nid < TLS1_1_VERSION) {
            return 0;
        }
        if (nid == TLS1_1_VERSION) {
            return 1;
        }
    }
    if (operation == SSL_SECOP_COMPRESSION) {
        return 0;
    }
    if (operation == SSL_SECOP_CIPHER_SUPPORTED || operation == SSL_SECOP_CIPHER_SHARED ||
        operation == SSL_SECOP_CIPHER_CHECK) {
        const auto* cipher = static_cast<const SSL_CIPHER*>(other);
        switch (SSL_CIPHER_get_id(cipher)) {
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
            case TLS1_CK_ECDHE_RSA_WITH_CHACHA20_POLY1305:
                break;
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_CBC_SHA:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_CBC_SHA:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_CBC_SHA:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_CBC_SHA:
                // SUPPORTED runs before the peer's version is known. Advertise
                // CBC for fallback, but never select or accept it with TLS 1.2.
                if (operation != SSL_SECOP_CIPHER_SUPPORTED && (!ssl || SSL_version(ssl) != TLS1_1_VERSION)) {
                    return 0;
                }
                break;
            default:
                if (std::string_view(SSL_CIPHER_get_version(cipher)) != "TLSv1.3") {
                    return 0;
                }
        }
    }
    if (ssl && SSL_version(ssl) == TLS1_1_VERSION &&
        (nid == NID_md5_sha1 || nid == NID_sha1) &&
        (operation == SSL_SECOP_SIGALG_SUPPORTED || operation == SSL_SECOP_SIGALG_SHARED ||
            operation == SSL_SECOP_SIGALG_CHECK || operation == SSL_SECOP_SIGALG_MASK)) {
        // These are handshake signature operations only. In particular,
        // SSL_SECOP_CA_MD, EE_KEY and CA_KEY are never exempted.
        return 1;
    }
    return policy.original_security_ &&
           policy.original_security_(ssl, context, operation, bits, nid, other, policy.original_security_data_);
}

int deny_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept {
    return 0;
}

struct tls_record_state final {
    std::uint64_t limit_;
    std::uint64_t records_{0};
    bool blocked_{false};
};

void free_context_policy(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    delete static_cast<tls_context_policy*>(value);
}

void free_record_state(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    delete static_cast<tls_record_state*>(value);
}

int copy_record_state(CRYPTO_EX_DATA*, const CRYPTO_EX_DATA*, void** value, int, long, void*) noexcept {
    try {
        if (*value) {
            *value = new tls_record_state(*static_cast<tls_record_state*>(*value));
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

int context_index = -1;
int record_index = -1;
int bio_index = -1;

void guard_native_handshake(const SSL* ssl, int where, int result) noexcept {
    if ((where & SSL_CB_HANDSHAKE_START) != 0) {
        // Unlike SSL_new's ex_data notification, this runs only after the
        // connection is initialized and before any handshake is processed.
        SSL_set_options(const_cast<SSL*>(ssl), SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
    }
    const auto* policy = static_cast<const tls_context_policy*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
    if (policy && policy->original_info_) {
        policy->original_info_(ssl, where, result);
    }
}

long deny_output(BIO* bio, int operation, const char*, std::size_t, int, long, int result,
    std::size_t* processed) noexcept {
    if (operation == BIO_CB_WRITE) {
        ERR_raise_data(ERR_LIB_SSL, ERR_R_OPERATION_FAIL, "TLS outgoing record limit or policy allocation failure");
        BIO_clear_retry_flags(bio);
        if (processed) {
            *processed = 0;
        }
        return 0;
    }
    return result;
}

long guard_output(BIO* bio, int operation, const char* bytes, std::size_t size, int arg,
    long larg, int result, std::size_t* processed) noexcept {
    if (operation == BIO_CB_WRITE) {
        const auto* state = static_cast<const tls_record_state*>(BIO_get_ex_data(bio, bio_index));
        if (!state || state->blocked_) {
            return deny_output(bio, operation, bytes, size, arg, larg, result, processed);
        }
    }
    return result;
}

void block_messages(int writing, int, int, const void*, std::size_t, SSL* ssl, void*) noexcept {
    if (writing) {
        if (auto* bio = SSL_get_wbio(ssl)) {
            BIO_set_callback_ex(bio, deny_output);
        }
    }
}

}  // namespace

void count_tls_records(int writing, int, int content_type, const void*, std::size_t, SSL* ssl, void*) noexcept {
    if (!writing) {
        return;
    }
    auto* bio = SSL_get_wbio(ssl);
    if (!bio) {
        return;
    }
    try {
        auto* state = static_cast<tls_record_state*>(SSL_get_ex_data(ssl, record_index));
        if (!state) {
            const auto* policy = static_cast<const tls_context_policy*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
            if (!policy) {
                BIO_set_callback_ex(bio, deny_output);
                return;
            }
            auto owner = std::make_unique<tls_record_state>(tls_record_state{policy->record_limit_});
            if (SSL_set_ex_data(ssl, record_index, owner.get()) != 1) {
                throw std::bad_alloc();
            }
            state = owner.release();
        }
        if (content_type == SSL3_RT_HEADER) {
            if (state->records_ == state->limit_) {
                state->blocked_ = true;
            } else {
                ++state->records_;
            }
        }
        // CONNECT 会替换写 BIO；所有密文生产者仍绑定同一连接级计数。
        if (BIO_get_ex_data(bio, bio_index) != state && BIO_set_ex_data(bio, bio_index, state) != 1) {
            throw std::bad_alloc();
        }
        BIO_set_callback_ex(bio, guard_output);
    } catch (...) {
        ERR_raise(ERR_LIB_SSL, ERR_R_MALLOC_FAILURE);
        BIO_set_callback_ex(bio, deny_output);
    }
}

namespace {

int accept_verified_chain(int valid, X509_STORE_CTX*) noexcept {
    return valid;
}

int deny_certificate(X509_STORE_CTX*, void*) noexcept {
    return 0;
}

int verify_native_certificate(X509_STORE_CTX* store, void* opaque) noexcept {
    const auto& policy = *static_cast<const tls_context_policy*>(opaque);
    // MariaDB 的默认回调会延迟并容忍部分错误；这里在握手内完成严格校验。
    X509_STORE_CTX_set_verify_cb(store, accept_verified_chain);
    if (X509_verify_cert(store) != 1) {
        return 0;
    }
    auto* leaf = X509_STORE_CTX_get0_cert(store);
    if (!leaf) {
        X509_STORE_CTX_set_error(store, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    const bool ip = is_client_ip_address(policy.host_);
    const auto matched = ip ? X509_check_ip_asc(leaf, policy.host_.c_str(), 0)
                            : X509_check_host(leaf, policy.host_.c_str(), policy.host_.size(),
                                  X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr);
    if (matched == 1) {
        return 1;
    }
    X509_STORE_CTX_set_error(store, matched == 0
                                        ? (ip ? X509_V_ERR_IP_ADDRESS_MISMATCH : X509_V_ERR_HOSTNAME_MISMATCH)
                                        : X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
}

void install_context_callbacks(SSL_CTX& context, tls_context_policy* policy) noexcept {
    SSL_CTX_set0_security_ex_data(&context, policy);
    SSL_CTX_set_security_callback(&context, enforce_security);
    SSL_CTX_set_msg_callback(&context, count_tls_records);
    SSL_CTX_set_msg_callback_arg(&context, nullptr);
    if (!policy->host_.empty()) {
        SSL_CTX_set_cert_verify_callback(&context, verify_native_certificate, policy);
        SSL_CTX_set_info_callback(&context, guard_native_handshake);
    }
}

}  // namespace

thread_local native_tls_context_scope* native_tls_context_scope::current_ = nullptr;

// Registers, once per process, the ex_data slots that carry the policy on
// every context, connection and BIO.
void register_tls_policy_indexes() {
    static const bool registered = [] {
        context_index = SSL_CTX_get_ex_new_index(
            0, nullptr, native_tls_context_scope::new_context, nullptr, free_context_policy);
        record_index = SSL_get_ex_new_index(0, nullptr, nullptr, copy_record_state, free_record_state);
        bio_index = BIO_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        if (context_index < 0 || record_index < 0 || bio_index < 0) {
            throw std::runtime_error("failed to register TLS policy storage");
        }
        return true;
    }();
    (void)registered;
}

native_tls_context_scope::native_tls_context_scope(std::string_view host, std::uint64_t record_limit)
    : host_(host),
      record_limit_(record_limit),
      previous_(current_) {
    // 构造通知只安装回调和 ex_data；不查询未初始化的 context 参数或 SSL 状态。
    register_tls_policy_indexes();
    current_ = host_.empty() ? nullptr : this;
}

native_tls_context_scope::~native_tls_context_scope() {
    current_ = previous_;
}

void native_tls_context_scope::rethrow_failure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

void native_tls_context_scope::new_context(void* parent, void*, CRYPTO_EX_DATA* storage, int index,
    long, void*) noexcept {
    if (!current_) {
        return;
    }
    auto* context = static_cast<SSL_CTX*>(parent);
    try {
        auto owner = std::make_unique<tls_context_policy>(*context, current_->host_, current_->record_limit_);
        if (CRYPTO_set_ex_data(storage, index, owner.get()) != 1) {
            throw std::bad_alloc();
        }
        install_context_callbacks(*context, owner.release());
    } catch (...) {
        current_->failure_ = std::current_exception();
        SSL_CTX_set_security_callback(context, deny_security);
        SSL_CTX_set_cert_verify_callback(context, deny_certificate, nullptr);
        SSL_CTX_set_msg_callback(context, block_messages);
    }
}

void validate_tls_record_limit(std::uint64_t limit) {
    if (limit == 0 || limit > (std::uint64_t{1} << 23)) {
        throw std::invalid_argument("TLS outgoing record limit must be in 1..2^23");
    }
}

void apply_tls_protocol_policy(SSL_CTX& context, std::uint64_t record_limit) {
    validate_tls_record_limit(record_limit);
    register_tls_policy_indexes();
    auto owner = std::make_unique<tls_context_policy>(context, std::string_view{}, record_limit);
    auto* previous = static_cast<tls_context_policy*>(SSL_CTX_get_ex_data(&context, context_index));
    if (SSL_CTX_set_ex_data(&context, context_index, owner.get()) != 1) {
        throw std::bad_alloc();
    }
    auto* policy = owner.release();
    delete previous;
    install_context_callbacks(context, policy);
    if (SSL_CTX_set_min_proto_version(&context, TLS1_1_VERSION) != 1 || SSL_CTX_set_max_proto_version(&context, 0) != 1) {
        throw std::runtime_error("failed to configure TLS versions");
    }
    SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_set_cipher_list(&context, tls_cipher_suites) != 1) {
        throw std::runtime_error("failed to configure TLS cipher suites");
    }
}

namespace {

[[nodiscard]] std::string_view origin_key(const http_target_origin& origin) noexcept {
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        return origin.host();
    }
    const auto address = origin.address();
    return {reinterpret_cast<const char*>(address.data()), address.size()};
}

}  // namespace

bool tls_served_certificate::covers(const http_target_origin& origin) noexcept {
    if (origin.action() != http_target_origin_action::verify_certificate) {
        return false;
    }
    const auto key = origin_key(origin);
    if (key.empty()) {
        return false;
    }
    const bool dns_name = origin.host_kind() == http_target_host_kind::dns_name;
    for (std::size_t i = 0; i < cached_; ++i) {
        const auto& entry = cache_[i];
        if (entry.kind_ != origin.host_kind() || entry.key_size_ != key.size()) {
            continue;
        }
        const std::string_view cached_key(key_storage_.data() + entry.key_offset_, entry.key_size_);
        if (dns_name ? http_ascii_equals_ignore_case(cached_key, key) : cached_key == key) {
            return entry.covered_;
        }
    }
    const bool covered = check(origin);
    if (key.size() <= cached_key_capacity) {
        if (cached_ == cache_size || key.size() > key_storage_capacity - key_used_) {
            cached_ = 0;
            key_used_ = 0;
        }
        auto& entry = cache_[cached_++];
        entry.key_offset_ = static_cast<std::uint8_t>(key_used_);
        entry.key_size_ = static_cast<std::uint8_t>(key.size());
        entry.kind_ = origin.host_kind();
        entry.covered_ = covered;
        std::copy_n(key.data(), key.size(), key_storage_.data() + key_used_);
        key_used_ = static_cast<std::uint16_t>(key_used_ + key.size());
    }
    return covered;
}

bool tls_served_certificate::check(const http_target_origin& origin) const noexcept {
    if (certificate_ == nullptr) {
        return false;
    }
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        const auto host = origin.host();
        return X509_check_host(certificate_, host.data(), host.size(),
                   X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) == 1;
    }
    const auto address = origin.address();
    return X509_check_ip(certificate_, address.data(), address.size(), 0) == 1;
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

struct ssl_context_deleter final {
    void operator()(SSL_CTX* context_value) const noexcept {
        SSL_CTX_free(context_value);
    }
};

struct x509_store_deleter final {
    void operator()(X509_STORE* store) const noexcept {
        X509_STORE_free(store);
    }
};

struct bio_deleter final {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};

struct x509_deleter final {
    void operator()(X509* certificate) const noexcept {
        X509_free(certificate);
    }
};

struct private_key_deleter final {
    void operator()(EVP_PKEY* private_key) const noexcept {
        EVP_PKEY_free(private_key);
    }
};

// A read-only BIO over caller-owned PEM text, which it never copies.
[[nodiscard]] std::unique_ptr<BIO, bio_deleter> open_pem_text(std::string_view text) {
    if (!std::in_range<int>(text.size())) {
        throw std::invalid_argument("TLS PEM text is too large");
    }
    ::ERR_clear_error();
    std::unique_ptr<BIO, bio_deleter> input(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())));
    if (input == nullptr) {
        throw_tls_credential_error("BIO_new_mem_buf");
    }
    return input;
}

// SSL_CTX_use_certificate_chain_file() over memory: the first certificate is
// the leaf, every later one joins its chain, and only the end of the text
// ends the chain.
void use_certificate_chain_pem(SSL_CTX& context, std::string_view text) {
    const auto input = open_pem_text(text);
    auto* const password_callback = SSL_CTX_get_default_passwd_cb(&context);
    auto* const password_userdata = SSL_CTX_get_default_passwd_cb_userdata(&context);
    const std::unique_ptr<X509, x509_deleter> leaf(
        PEM_read_bio_X509_AUX(input.get(), nullptr, password_callback, password_userdata));
    if (leaf == nullptr || SSL_CTX_use_certificate(&context, leaf.get()) != 1) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    for (;;) {
        std::unique_ptr<X509, x509_deleter> issuer(
            PEM_read_bio_X509(input.get(), nullptr, password_callback, password_userdata));
        if (issuer == nullptr) {
            break;
        }
        if (SSL_CTX_add0_chain_cert(&context, issuer.get()) != 1) {
            throw_tls_credential_error("use_certificate_chain_pem");
        }
        // The context owns it now.
        (void)issuer.release();
    }
    const auto error = ::ERR_peek_last_error();
    if (ERR_GET_LIB(error) != ERR_LIB_PEM || ERR_GET_REASON(error) != PEM_R_NO_START_LINE) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    ::ERR_clear_error();
}

void use_private_key_pem(SSL_CTX& context, std::string_view text) {
    const auto input = open_pem_text(text);
    const std::unique_ptr<EVP_PKEY, private_key_deleter> private_key(PEM_read_bio_PrivateKey(input.get(), nullptr,
        SSL_CTX_get_default_passwd_cb(&context), SSL_CTX_get_default_passwd_cb_userdata(&context)));
    if (private_key == nullptr || SSL_CTX_use_PrivateKey(&context, private_key.get()) != 1) {
        throw_tls_credential_error("use_private_key_pem");
    }
}

// Borrow the configured password only during synchronous credential loading.
// An absent password uses a caller provider, or an empty noninteractive default.
class tls_password_scope final {
public:
    tls_password_scope(SSL_CTX& context_value, std::string_view password) noexcept
        : context_(&context_value),
          password_(password),
          previous_callback_(SSL_CTX_get_default_passwd_cb(&context_value)),
          previous_userdata_(SSL_CTX_get_default_passwd_cb_userdata(&context_value)) {
        const bool default_source = previous_userdata_ == nullptr &&
                                    (previous_callback_ == nullptr || previous_callback_ == PEM_def_callback);
        if (!password.empty() || default_source) {
            SSL_CTX_set_default_passwd_cb(context_, &copy_password);
            SSL_CTX_set_default_passwd_cb_userdata(context_, this);
        }
    }

    ~tls_password_scope() {
        // The caller may own its userdata: Asio deletes its callback object.
        SSL_CTX_set_default_passwd_cb(context_, previous_callback_);
        SSL_CTX_set_default_passwd_cb_userdata(context_, previous_userdata_);
    }

    tls_password_scope(const tls_password_scope&) = delete;
    tls_password_scope& operator=(const tls_password_scope&) = delete;
    tls_password_scope(tls_password_scope&&) = delete;
    tls_password_scope& operator=(tls_password_scope&&) = delete;

private:
    static int copy_password(char* buffer, int size, int, void* userdata) noexcept {
        if (buffer == nullptr || size <= 0 || userdata == nullptr) {
            return -1;
        }
        const auto password = static_cast<const tls_password_scope*>(userdata)->password_;
        if (password.size() > static_cast<std::size_t>(size)) {
            return -1;
        }
        if (!password.empty()) {
            std::memcpy(buffer, password.data(), password.size());
        }
        return static_cast<int>(password.size());
    }

    SSL_CTX* context_;
    std::string_view password_;
    pem_password_cb* previous_callback_;
    void* previous_userdata_;
};

// OpenSSL's PEM loaders resolve the full chain and the (possibly encrypted)
// key into a scratch context, which keeps that one authoritative parser.
[[nodiscard]] std::unique_ptr<SSL_CTX, ssl_context_deleter> make_scratch_context() {
    ::ERR_clear_error();
    std::unique_ptr<SSL_CTX, ssl_context_deleter> scratch(SSL_CTX_new(TLS_method()));
    if (scratch == nullptr) {
        throw_tls_credential_error("SSL_CTX_new");
    }
    return scratch;
}

}  // namespace

void throw_tls_credential_error(const char* operation) {
    const auto code = ::ERR_get_error();
    const auto error = ERR_SYSTEM_ERROR(code)
                           ? std::error_code(ERR_GET_REASON(code), asio::error::get_system_category())
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category());
    throw asio::system_error(error, operation);
}

tls_certificate tls_certificate::load(const char* certificate_chain_file, const char* private_key_file,
    std::string_view private_key_password) {
    const auto scratch = make_scratch_context();
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        ::ERR_clear_error();
        if (::SSL_CTX_use_certificate_chain_file(scratch.get(), certificate_chain_file) != 1) {
            throw_tls_credential_error("use_certificate_chain_file");
        }
        ::ERR_clear_error();
        if (::SSL_CTX_use_PrivateKey_file(scratch.get(), private_key_file, SSL_FILETYPE_PEM) != 1) {
            throw_tls_credential_error("use_private_key_file");
        }
    }
    return adopt_loaded(*scratch);
}

tls_certificate tls_certificate::load_pem(std::string_view certificate_chain, std::string_view private_key,
    std::string_view private_key_password) {
    const auto scratch = make_scratch_context();
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        use_certificate_chain_pem(*scratch, certificate_chain);
        use_private_key_pem(*scratch, private_key);
    }
    return adopt_loaded(*scratch);
}

tls_certificate tls_certificate::adopt_loaded(SSL_CTX& scratch) {
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&scratch) != 1) {
        throw_tls_credential_error("check_private_key");
    }
    STACK_OF(X509)* chain = nullptr;
    if (SSL_CTX_get0_chain_certs(&scratch, &chain) != 1) {
        throw_tls_credential_error("get0_chain_certs");
    }
    return tls_certificate(SSL_CTX_get0_certificate(&scratch), chain, SSL_CTX_get0_privatekey(&scratch));
}

tls_certificate::tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key) {
    if (certificate == nullptr || private_key == nullptr) {
        throw std::invalid_argument("TLS certificate and private key must not be null");
    }
    if (chain != nullptr) {
        chain_ = X509_chain_up_ref(chain);
        if (chain_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    if (X509_up_ref(certificate) != 1) {
        OSSL_STACK_OF_X509_free(chain_);
        chain_ = nullptr;
        throw std::bad_alloc();
    }
    certificate_ = certificate;
    if (EVP_PKEY_up_ref(private_key) != 1) {
        X509_free(certificate_);
        certificate_ = nullptr;
        OSSL_STACK_OF_X509_free(chain_);
        chain_ = nullptr;
        throw std::bad_alloc();
    }
    private_key_ = private_key;
}

tls_certificate::tls_certificate(const tls_certificate& other)
    : tls_certificate(other.certificate_, other.chain_, other.private_key_) {}

tls_certificate::tls_certificate(tls_certificate&& other) noexcept
    : certificate_(std::exchange(other.certificate_, nullptr)),
      chain_(std::exchange(other.chain_, nullptr)),
      private_key_(std::exchange(other.private_key_, nullptr)) {}

tls_certificate& tls_certificate::operator=(tls_certificate&& other) noexcept {
    if (this != &other) {
        release();
        certificate_ = std::exchange(other.certificate_, nullptr);
        chain_ = std::exchange(other.chain_, nullptr);
        private_key_ = std::exchange(other.private_key_, nullptr);
    }
    return *this;
}

tls_certificate::~tls_certificate() {
    release();
}

void tls_certificate::release() noexcept {
    X509_free(certificate_);
    OSSL_STACK_OF_X509_free(chain_);
    EVP_PKEY_free(private_key_);
    certificate_ = nullptr;
    chain_ = nullptr;
    private_key_ = nullptr;
}

void tls_certificate::install(SSL_CTX& context) const {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate(&context, certificate_) != 1) {
        throw_tls_credential_error("use_certificate");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey(&context, private_key_) != 1) {
        throw_tls_credential_error("use_private_key");
    }
    ::ERR_clear_error();
    if (chain_ != nullptr && SSL_CTX_set1_chain(&context, chain_) != 1) {
        throw_tls_credential_error("set1_chain");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&context) != 1) {
        throw_tls_credential_error("check_private_key");
    }
}

bool tls_certificate::present(SSL& connection) const noexcept {
    // Override the whole slot: an absent chain_ clears any earlier chain.
    return ::SSL_use_cert_and_key(&connection, certificate_, private_key_, chain_, 1) == 1;
}

tls_trust_store tls_trust_store::load_file(const char* file) {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_credential_error("X509_STORE_new");
    }
    if (X509_STORE_load_file(store.get(), file) != 1) {
        throw_tls_credential_error("load_verify_file");
    }
    return tls_trust_store(store.release());
}

tls_trust_store tls_trust_store::load_default() {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_credential_error("X509_STORE_new");
    }
    if (X509_STORE_set_default_paths(store.get()) != 1) {
        throw_tls_credential_error("set_default_verify_paths");
    }
    return tls_trust_store(store.release());
}

tls_trust_store::tls_trust_store(const tls_trust_store& other)
    : store_(other.store_) {
    if (store_ != nullptr && X509_STORE_up_ref(store_) != 1) {
        store_ = nullptr;
        throw std::bad_alloc();
    }
}

tls_trust_store::tls_trust_store(tls_trust_store&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)) {}

tls_trust_store& tls_trust_store::operator=(const tls_trust_store& other) {
    if (this != &other) {
        *this = tls_trust_store(other);
    }
    return *this;
}

tls_trust_store& tls_trust_store::operator=(tls_trust_store&& other) noexcept {
    if (this != &other) {
        X509_STORE_free(store_);
        store_ = std::exchange(other.store_, nullptr);
    }
    return *this;
}

tls_trust_store::~tls_trust_store() {
    X509_STORE_free(store_);
}

void tls_trust_store::install(SSL_CTX& context) const noexcept {
    SSL_CTX_set1_cert_store(&context, store_);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

struct cleanse_identity final {
    tls_identity_config& identity_;
    ~cleanse_identity() {
        for (auto* secret : {&identity_.certificate_chain_pem_, &identity_.private_key_pem_,
                 &identity_.private_key_password_}) {
            if (!secret->empty()) {
                OPENSSL_cleanse(secret->data(), secret->size());
                secret->clear();
            }
        }
    }
};

// Never move a secret-bearing config between coroutine/lambda frames: an SSO
// move may leave bytes in its source. One stable, process-owned payload crosses
// the pool boundary, and every incoming/partially populated copy is cleansed.
struct owned_identity final {
    tls_identity_config value_;
    ~owned_identity() {
        const cleanse_identity cleanse{value_};
    }
};

using owned_identity_pointer = std::unique_ptr<owned_identity>;

void require_accepting_caller(const worker_ref& caller) {
    if (!caller.accepting()) {
        throw std::runtime_error("TLS certificate mutation caller is stopping");
    }
}

}  // namespace

struct tls_certificate_store::state final {
    detail::runtime_control_gate writer_;
    std::atomic<std::shared_ptr<const detail::tls_certificate_snapshot>> snapshot_{
        std::make_shared<const detail::tls_certificate_snapshot>()};

    [[nodiscard]] static task<void> insert_or_assign(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id,
        owned_identity_pointer identity) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto prepared = co_await run_blocking(caller,
            [current = owner->snapshot_.load(std::memory_order_acquire), id,
                identity = std::move(identity)]() mutable {
                auto certificate = std::make_shared<const detail::tls_certificate>(
                    detail::load_server_tls_certificate(identity->value_));
                identity.reset();
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->insert_or_assign(id, std::move(certificate));
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
    }

    [[nodiscard]] static task<bool> erase(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto current = owner->snapshot_.load(std::memory_order_acquire);
        if (!current->contains(id)) {
            co_return false;
        }
        // Copying an arbitrarily large immutable map is real preparation, not
        // a synchronous erase disguised by posting a closure to the caller.
        auto prepared = co_await run_blocking(caller,
            [current = std::move(current), id] {
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->erase(id);
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
        co_return true;
    }
};

tls_certificate_store::tls_certificate_store()
    : state_(std::make_shared<state>()) {}
tls_certificate_store::~tls_certificate_store() = default;

task<void> tls_certificate_store::insert_or_assign(
    worker_ref caller, tls_certificate_id id, tls_identity_config identity) {
    const cleanse_identity cleanse{identity};
    auto owner = state_;
    if (id == 0) {
        throw std::invalid_argument("TLS certificate ID zero is reserved");
    }
    auto owned = std::make_unique<owned_identity>();
    owned->value_.certificate_chain_file_ = std::move(identity.certificate_chain_file_);
    owned->value_.private_key_file_ = std::move(identity.private_key_file_);
    owned->value_.certificate_chain_pem_.assign(identity.certificate_chain_pem_);
    owned->value_.private_key_pem_.assign(identity.private_key_pem_);
    owned->value_.private_key_password_.assign(identity.private_key_password_);
    return state::insert_or_assign(std::move(owner), std::move(caller), id, std::move(owned));
}

task<bool> tls_certificate_store::erase(worker_ref caller, tls_certificate_id id) {
    return state::erase(state_, std::move(caller), id);
}

bool tls_certificate_store::contains(tls_certificate_id id) const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->contains(id);
}

std::size_t tls_certificate_store::size() const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->size();
}

bool tls_certificate_selection::offers_signature_algorithm(std::uint16_t algorithm) const noexcept {
    return std::ranges::find(signature_algorithms_, algorithm) != signature_algorithms_.end();
}

std::shared_ptr<const detail::tls_certificate_snapshot> detail::tls_certificate_store_access::snapshot(
    const tls_certificate_store& store) noexcept {
    return store.state_->snapshot_.load(std::memory_order_acquire);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] std::optional<tls_session_ticket_key> generate_ticket_key() noexcept {
    tls_session_ticket_key key;
    if (!fill_random_bytes(key.name_) || !fill_random_bytes(key.hmac_key_) || !fill_random_bytes(key.aes_key_)) {
        return std::nullopt;
    }
    return key;
}

// The ex_data slot that points every TLS context of a listener at its
// worker's key set. Allocated by the first install(), at startup.
[[nodiscard]] int ticket_key_set_index() noexcept {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

[[nodiscard]] bool set_ticket_mac_key(EVP_MAC_CTX& mac, const tls_session_ticket_key& key) noexcept {
    static char digest[] = "SHA256";
    const OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string(
            OSSL_MAC_PARAM_KEY, const_cast<unsigned char*>(key.hmac_key_.data()), key.hmac_key_.size()),
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end(),
    };
    return EVP_MAC_CTX_set_params(&mac, parameters) == 1;
}

[[nodiscard]] bool names_key(const std::optional<tls_session_ticket_key>& key, const unsigned char* name) noexcept {
    return key.has_value() && CRYPTO_memcmp(key->name_.data(), name, key->name_.size()) == 0;
}

}  // namespace

tls_session_ticket_key::~tls_session_ticket_key() {
    OPENSSL_cleanse(name_.data(), name_.size());
    OPENSSL_cleanse(hmac_key_.data(), hmac_key_.size());
    OPENSSL_cleanse(aes_key_.data(), aes_key_.size());
}

tls_session_ticket_key_ring::tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin)
    : origin_(origin),
      current_(generate_ticket_key()) {
    if (!current_.has_value()) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
}

std::uint64_t tls_session_ticket_key_ring::epoch_at(std::chrono::steady_clock::time_point now) const noexcept {
    if (now <= origin_) {
        return 0;
    }
    return static_cast<std::uint64_t>((now - origin_) / tls_session_lifetime);
}

std::chrono::steady_clock::time_point tls_session_ticket_key_ring::next_rotation(
    std::chrono::steady_clock::time_point now) const noexcept {
    return origin_ + (epoch_at(now) + 1) * std::chrono::duration_cast<std::chrono::steady_clock::duration>(tls_session_lifetime);
}

bool tls_session_ticket_key_ring::copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target) {
    std::optional<tls_session_ticket_key> retired_previous;
    std::optional<tls_session_ticket_key> retired_current;
    std::optional<tls_session_ticket_key> retired_target_current;
    std::optional<tls_session_ticket_key> retired_target_previous;
    bool has_current = false;
    {
        const std::lock_guard lock(mutex_);
        const auto epoch = epoch_at(now);
        if (epoch > epoch_) {
            // The previous key's validity ended with the old epoch; the current
            // key stays valid for decryption through exactly one more epoch.
            retired_previous = std::move(previous_);
            previous_.reset();
            if (epoch == epoch_ + 1) {
                previous_ = current_;
            }
            retired_current = std::move(current_);
            current_.reset();
            epoch_ = epoch;
        }
        if (!current_.has_value()) {
            current_ = generate_ticket_key();
        }
        retired_target_current = std::exchange(target.current_, current_);
        retired_target_previous = std::exchange(target.previous_, previous_);
        has_current = current_.has_value();
    }
    return has_current;
}

tls_session_ticket_key_set::tls_session_ticket_key_set()
    : aes_256_cbc_(EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr)) {
    if (aes_256_cbc_ == nullptr) {
        throw std::runtime_error("failed to fetch the TLS session ticket cipher");
    }
}

tls_session_ticket_key_set::~tls_session_ticket_key_set() {
    EVP_CIPHER_free(aes_256_cbc_);
}

void tls_session_ticket_key_set::install(SSL_CTX& context) {
    const int index = ticket_key_set_index();
    if (index < 0 || SSL_CTX_set_ex_data(&context, index, this) != 1 ||
        SSL_CTX_set_tlsext_ticket_key_evp_cb(&context, &tls_session_ticket_key_set::protect_ticket) != 1) {
        throw std::runtime_error("failed to install TLS session ticket keys");
    }
    (void)SSL_CTX_set_timeout(&context, static_cast<long>(tls_session_lifetime.count()));
}

// SSL_CTX_set_tlsext_ticket_key_evp_cb(3): returns 1 when the key is set up,
// 2 when a ticket decrypted with the previous key should be renewed, 0 when no
// ticket is issued or the key name is unknown (full handshake), -1 on error.
int tls_session_ticket_key_set::protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv,
    EVP_CIPHER_CTX* cipher, EVP_MAC_CTX* mac, int encrypt) noexcept {
    // OpenSSL calls the callback of the connection's initial context, but an
    // SNI switch may already have replaced the context; every context of the
    // listener points at the same set.
    const auto* keys = static_cast<const tls_session_ticket_key_set*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ticket_key_set_index()));
    if (keys == nullptr || cipher == nullptr || mac == nullptr) {
        return -1;
    }
    if (encrypt != 0) {
        if (!keys->current_.has_value()) {
            return 0;
        }
        const auto& key = *keys->current_;
        std::ranges::copy(key.name_, key_name);
        const int iv_size = EVP_CIPHER_get_iv_length(keys->aes_256_cbc_);
        if (iv_size <= 0 || iv_size > EVP_MAX_IV_LENGTH || RAND_bytes(iv, iv_size) != 1 ||
            EVP_EncryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1 ||
            !set_ticket_mac_key(*mac, key)) {
            return -1;
        }
        return 1;
    }
    const bool current = names_key(keys->current_, key_name);
    if (!current && !names_key(keys->previous_, key_name)) {
        return 0;
    }
    const auto& key = current ? *keys->current_ : *keys->previous_;
    if (!set_ticket_mac_key(*mac, key) ||
        EVP_DecryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1) {
        return -1;
    }
    return current ? 1 : 2;
}

}  // namespace ruvia::detail
