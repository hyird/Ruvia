#include "context.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

#include "app.h"
#include "blocking_pool.h"
#include "borrowed_text.h"
#include "byte_view.h"
#include "connection_lifecycle.h"
#include "context_access.h"
#include "crypto.h"
#include "db_session.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_cache.h"
#include "http_client.h"
#include "http_client_pool.h"
#include "http_content_codec.h"
#include "http_content_coding.h"
#include "http_content_negotiation.h"
#include "http_cookie.h"
#include "http_header.h"
#include "http_multipart.h"
#include "http_representation.h"
#include "http_request.h"
#include "http_request_admission.h"
#include "http_request_body_failure.h"
#include "http_request_target.h"
#include "http_response.h"
#include "inline_storage.h"
#include "middleware.h"
#include "model.h"
#include "redis.h"
#include "response_coding_policy.h"
#include "route_table.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byte_body_view(std::span<const std::byte> body) noexcept {
    return body.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
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

std::string context::url_for(std::string_view pattern, std::span<const std::string_view> values) const {
    if (services().routes() == nullptr) {
        throw std::logic_error("url_for requires a route table bound to this context");
    }
    return services().routes()->url_for(pattern, values);
}

std::string context::url_for(std::string_view pattern, std::initializer_list<std::string_view> values) const {
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
    const auto handler = routes == nullptr ? detail::http_not_found_handler_ref{nullptr} : routes->not_found_handler_for(request_.path());
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

void context::apply_response_state(http_response& response, std::optional<http_status_code> status_code) {
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

void write_cookie(http_response& response, std::string_view name, std::string_view value, const cookie_options& options) {
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
    const auto prefix = options.attributes_.prefix_ ? std::string_view(ruvia::http_cookie_prefix_text(*options.attributes_.prefix_)) : std::string_view{};
    const detail::inline_text_concat<128> wire_name({prefix, name});
    char signature[detail::cookie_signature_size];
    detail::write_cookie_signature(signature, secret, wire_name.view(), value);
    const detail::inline_text_concat<256> signed_value({value, ".", std::string_view(signature, sizeof(signature))});
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
            return detail::context_access::make_static_json_response(context_value, R"({"status":"ready"})");
        case readiness_state::unavailable:
            break;
        default:
            throw std::invalid_argument("readiness state is invalid");
    }

    context_value.draft().status(http_status::service_unavailable);
    const auto reason = options.unavailable_reason_.view();
    if (reason.empty()) {
        return detail::context_access::make_static_json_response(context_value, R"({"status":"not_ready"})");
    }

    static constexpr std::string_view prefix = R"({"status":"not_ready","reason":)";
    std::string body;
    body.reserve(prefix.size() + detail::json_string_size_hint(reason) + 1U);
    body.append(prefix);
    detail::append_json_string(body, reason);
    body.push_back('}');
    return detail::context_access::make_owned_json_response(context_value, body);
}
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
task<void> http_interim_response_output::inform(const http_interim_response_head& response, operation_options operation, context& context_value) {
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::vector<http_header> owned;
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.emplace_back(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(scope_, write_owned(response.status(), std::move(owned), std::move(operation), context_value));
}
task<void> http_interim_response_output::write_owned(http_status_code status, std::vector<http_header> fields_value, operation_options operation, context& context_value) {
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    const detail::inline_http_header_views<16> active_views(fields_value);
    const http_interim_response_head head(status, active_views.view());
    operation_interrupt interrupt(interrupts_, context_value.worker(), operation, &context_value, &context_access::reset_interrupted_request, "interim response write");
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
    if (!is_valid_http_method_token(options.method_) || classify_http_method(options.method_) == http_known_method::connect || !is_valid_http_origin_form_target(options.target_)) {
        throw std::invalid_argument("dispatch requires a valid method and origin-form target");
    }
    if (options.body_.size() > services().max_decoded_body_bytes()) {
        throw std::invalid_argument("subrequest body exceeds the configured limit");
    }
    if (options.method_.size() > max_subrequest_header_bytes || options.target_.size() > max_subrequest_header_bytes - options.method_.size()) {
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
        if (header.name().size() > max_subrequest_header_bytes - header_bytes || header.value().size() > max_subrequest_header_bytes - header_bytes - header.name().size()) {
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
    return ::ruvia::make_scoped_operation(operation_scope_, dispatch_task(std::move(method), std::move(target), std::move(body), std::move(headers), std::move(options.stop_token_)));
}

task<dispatch_response> context::dispatch_task(std::string method, std::string target, std::string body_storage, std::vector<http_header> headers, ::ruvia::stop_token stop_token_value) {
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
            return make_parsed_http_request(method, target, active_headers.view(), std::as_bytes(std::span(body.data(), body.size())));
        } catch (const http_protocol_error&) {
            throw std::invalid_argument("invalid subrequest");
        }
    }();
    const auto& routes = *services().routes();
    const auto resolution = routes.resolve(request);
    if (const auto* resolved = resolution.resolved()) {
        const auto& endpoint = resolved->route().endpoint();
        if (endpoint.buffered() == nullptr || endpoint.request_body_mode() != detail::request_body_mode::buffered) {
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
    if (detail::http_response_needs_not_acceptable(admission.tail().coding_policy(), request, response) && !response.status().is_error()) {
        response = co_await routes.respond_error(request, http_error_info(detail::http_not_acceptable_coding_error()), admission.services());
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

context::context(const http_request& request, const detail::context_services& services) :
    context(request, {}, nullptr, nullptr, 0, 0, services) {}

context::context(const http_request& request, std::string_view route_path, const std::string_view* param_names, const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope, const detail::context_services& services) :
    request_(request),
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
    return services_value.client_registries().db(operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

db_session context::db(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().db(alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session context::redis() const {
    const auto& services_value = services();
    return services_value.client_registries().redis(operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

redis_session context::redis(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().redis(alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
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
    const auto forwarded = worker_services_->trusted_proxies_->resolve(resolved.remote().address(), request);
    if (forwarded.from_forwarded_header()) {
        resolved.apply_forwarded(forwarded.address(), forwarded.scheme());
    }
    return resolved;
}

namespace detail {

// A media-type mismatch is the client speaking the wrong format at a valid
// endpoint: RFC 9110 15.5.16 assigns that 415, distinct from the 400 a
// malformed body of the RIGHT type earns below.
[[noreturn]] void throw_invalid_json_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type, .code_ = "unsupported_media_type", .message_ = "request body must be application/json"});
}

[[noreturn]] void throw_invalid_json_body() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid json body"});
}

[[noreturn]] void throw_invalid_form_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type, .code_ = "unsupported_media_type", .message_ = "request body must be application/x-www-form-urlencoded"});
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
    static const request_name_value_list empty = detail::request_name_value_list_access::make();
    return empty;
}

// Unencoded names and values borrow the request query string; decoded ones
// live in the request arena. Form decoding keeps an invalid '%' literally.
void ensure_request_query(const http_request& request, detail::context_request_storage& request_storage) {
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
        const auto size = *decode_url_bytes(input, out, url_decode_mode::form);
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
[[nodiscard]] std::size_t count_encoded_route_params(const std::string_view* param_values, std::size_t param_count, detail::context_request_storage& request_storage) {
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
void ensure_route_params(const std::string_view* param_names, const std::string_view* param_values, std::size_t param_count, detail::context_request_storage& request_storage) {
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
            const auto size = decode_url_bytes(decoded_value, out, param_names[i] == "*" ? url_decode_mode::path : url_decode_mode::percent);
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
    const auto boundary = parse_multipart_boundary(request.header("Content-Type").value_or(std::string_view{}));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (boundary.not_applicable() != nullptr) {
        throw http_error({.status_ = http_status::unsupported_media_type, .code_ = "unsupported_media_type", .message_ = "request body must be multipart/form-data"});
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

[[nodiscard]] task<std::variant<std::string, http_content_decode_error>> decode_request_content(const worker_ref& worker_value, const stop_token& stop_token_value, std::span<const http_content_coding> codings, std::string_view raw, http_content_decode_options options) {
    if (worker_value.blocking_pool() != nullptr && options.max_decoded_bytes_ > inline_request_decode_bytes) {
        if (raw.size() <= inline_request_decode_bytes) {
            auto inline_result = decode_http_content(codings, raw, {.max_decoded_bytes_ = inline_request_decode_bytes});
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
        auto offloaded = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value}, [owned_codings, coding_count, owned_raw = std::move(owned_raw), limit = options.max_decoded_bytes_]() {
            return decode_http_content(std::span<const http_content_coding>(owned_codings.data(), coding_count), owned_raw, {.max_decoded_bytes_ = limit});
        });
        // A codec failure on the pool is rethrown here; pool saturation or
        // shutdown throws blocking_operation_rejected (mapped to 503).
        co_return std::move(offloaded).take_value();
    }
    co_return decode_http_content(codings, raw, options);
}

// Rejects an invalid or unsupported Content-Encoding; the result owns the codings span.
[[nodiscard]] http_content_coding_field_result require_decodable_request_coding(const http_request& request, detail::unsupported_request_content_coding::read_type read) {
    auto parsed_coding = request_content_coding(request);
    if (const auto* invalid = parsed_coding.invalid()) {
        throw http_protocol_error(invalid->status(), "invalid_content_encoding", "invalid request Content-Encoding");
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
    const auto parsed_coding = require_decodable_request_coding(request, detail::unsupported_request_content_coding::read_type::decoded);
    const auto codings = parsed_coding.codings();
    if (codings.empty()) {
        detail::context_access::mark_body_identity(context_value);
        co_return raw;
    }
    auto decode_result = co_await decode_request_content(services.worker(), context_value.stop_token(), codings, raw, {.max_decoded_bytes_ = services.max_decoded_request_body_bytes()});
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
    auto parsed = parse_multipart_body(request_body, {.boundary_ = boundary});
    if (const auto* failure = parsed.failure()) {
        throw failure->protocol_error();
    }
    co_return std::move(parsed).take_parts();
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
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->fields() : std::span<const http_header>{};
}

std::optional<std::string_view> context_request::trailer(std::string_view name) const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->field(name) : std::nullopt;
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

request_field_lookup context_request::unique_cookie(std::string_view name) const {
    if (const auto& cache = context_->request_storage().cookies_) {
        return cache->unique(name);
    }
    // Scan every Cookie field in place; materializing cookie_fields() would allocate.
    request_field_lookup result;
    for (const auto& header_field : context_->request_.headers()) {
        if (header_field.name().size() != 6U || !http_ascii_equals_ignore_case(header_field.name(), "Cookie")) {
            continue;
        }
        http_visit_cookie_pairs(header_field.value(), [name, &result](std::string_view key, std::string_view value) noexcept {
            if (key != name) {
                return true;
            }
            if (result.status_ != unique_lookup_status::absent) {
                result = {unique_lookup_status::repeated, {}};
                return false;
            }
            result = {unique_lookup_status::found, value};
            return true;
        });
        if (result.status_ == unique_lookup_status::repeated) {
            break;
        }
    }
    return result;
}

request_field_lookup context_request::unique_header(std::string_view name) const {
    return header_fields().unique(name);
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
        if (header_field.name().size() != 6U || !http_ascii_equals_ignore_case(header_field.name(), "Cookie")) {
            continue;
        }
        if (!cookies) {
            cookies.emplace(detail::request_name_value_list_access::make());
        }
        http_visit_cookie_pairs(header_field.value(), [&cookies](std::string_view key, std::string_view value) {
            detail::request_name_value_list_access::push_back(*cookies, http_header_view{key, value});
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

std::optional<std::string_view> context_request::negotiate(negotiable_field field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const bool media_type = field == negotiable_field::media_type;
    const bool encoding = field == negotiable_field::encoding;
    const auto header_name = negotiation_header_name(field);
    // Only Accept-Language needs prefix matching in the generic token parser.
    const auto token_mode = field == negotiable_field::language ? http_accept_token_match_mode::language_prefix : http_accept_token_match_mode::exact;

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
                if (http_ascii_equals_ignore_case(header_field.name(), header_name) && !header_field.value().empty()) {
                    accumulate(header_field.value());
                }
            }
        }
        const auto quality = encoding ? encoding_quality.quality(http_ascii_equals_ignore_case(offered, "identity")) : match.quality();
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

std::optional<std::string_view> context_request::signed_cookie(signed_cookie_lookup_options options) const {
    const auto name = options.name_.view();
    const auto secret = options.secret_.view();
    // A repeated name is ambiguous about which value was signed, so it fails
    // verification instead of resolving one occurrence.
    const auto stored = unique_cookie(name);
    if (stored.status_ != unique_lookup_status::found || stored.value_.size() <= detail::cookie_signature_size) {
        return std::nullopt;
    }
    const auto value_size = stored.value_.size() - detail::cookie_signature_size - 1;
    if (stored.value_[value_size] != '.') {
        return std::nullopt;
    }
    const auto value = stored.value_.substr(0, value_size);
    const auto signature = stored.value_.substr(value_size + 1);
    char expected[detail::cookie_signature_size];
    detail::write_cookie_signature(expected, secret, name, value);
    if (!detail::constant_time_bytes_equal(signature, std::string_view(expected, sizeof(expected)))) {
        return std::nullopt;
    }
    return value;
}

task<std::vector<multipart_part>> context_request::multipart(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_multipart(*context_, std::move(operation)));
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
    const auto parsed_coding = require_decodable_request_coding(context_->request_, detail::unsupported_request_content_coding::read_type::streaming);
    if (std::ranges::any_of(parsed_coding.codings(), [](http_content_coding coding) {
        return coding != http_content_coding::identity;
    })) {
        throw detail::unsupported_request_content_coding(detail::unsupported_request_content_coding::read_type::streaming);
    }
    return ::ruvia::multipart_reader(body_reader(), {.boundary_ = boundary});
}
}  // namespace ruvia
