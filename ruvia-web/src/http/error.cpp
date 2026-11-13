#include "ruvia/web/error.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/bytes.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "context/context_access.h"
#include "http/http_websocket_connection.h"
#include "http/inline_http_header_views.h"
#include "http/tls_protocol_policy.h"
#include "integration/operation_interrupt.h"
#include "router/route_table.h"
#include "server/http_server_options.h"
#include "server/multiplexed_stream_signal.h"

namespace ruvia {
namespace {

[[nodiscard]] http_error_info normalize_http_error_info(http_error_info error) noexcept {
    auto status = error.status();
    if (!status.is_error()) {
        status = http_status::internal_server_error;
    }
    auto status_text = error.status_text();
    if (status_text.empty() || !is_valid_http_header_value(status_text)) {
        status_text = http_reason_phrase(status);
        if (status_text.empty()) {
            // Application error presentation remains a Web concern. Do not
            // invent an HTTP/1 reason phrase for an extension status code.
            status_text = "HTTP Error";
        }
    }
    auto code = error.code();
    if (code.empty()) {
        code = default_error_code(status);
    }
    auto message = error.message();
    if (message.empty()) {
        message = status_text;
    }
    return http_error_info({.status_ = status,
        .code_ = code,
        .message_ = message,
        .status_text_ = status_text,
        .validation_issues_ = error.validation_issues()});
}

[[nodiscard]] std::string serialize_error_response(http_error_info error) {
    const auto phrase = http_reason_phrase(error.status());
    const auto title = phrase.empty() ? std::string_view("HTTP Error") : phrase;
    const auto issues = error.validation_issues();
    std::size_t reserve_size = 66U + detail::json_string_size_hint(title) +
                               detail::json_string_size_hint(error.message()) +
                               detail::json_string_size_hint(error.code());
    for (const auto& issue : issues) {
        reserve_size += 30U + detail::json_string_size_hint(issue.field()) +
                        detail::json_string_size_hint(issue.code()) +
                        detail::json_string_size_hint(issue.message());
    }
    std::string out;
    out.reserve(reserve_size);
    out.append("{\"type\":\"about:blank\",\"title\":");
    detail::append_json_string(out, title);
    out.append(",\"status\":");
    http_append_unsigned(out, error.status().value());
    out.append(",\"detail\":");
    detail::append_json_string(out, error.message());
    out.append(",\"code\":");
    detail::append_json_string(out, error.code());
    if (!issues.empty()) {
        out.append(",\"errors\":[");
        for (std::size_t i = 0; i < issues.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out.append("{\"field\":");
            detail::append_json_string(out, issues[i].field());
            out.append(",\"code\":");
            detail::append_json_string(out, issues[i].code());
            out.append(",\"message\":");
            detail::append_json_string(out, issues[i].message());
            out.push_back('}');
        }
        out.push_back(']');
    }
    out.push_back('}');
    return out;
}

// `error` is already normalized.
[[nodiscard]] http_response make_normalized_error_response(http_error_info error) {
    http_response response;
    response.status(error.status());
    response.header_stable_view("Content-Type", "application/problem+json");
    response.owned_body(serialize_error_response(error));
    return response;
}

}  // namespace

void validation_issue::swap(validation_issue& other) noexcept {
    field_.swap(other.field_);
    code_.swap(other.code_);
    message_.swap(other.message_);
}

validation_issue& validation_issue::operator=(const validation_issue& other) {
    if (this != &other) {
        validation_issue candidate(other);
        swap(candidate);
    }
    return *this;
}

validation_issue& validation_issue::operator=(validation_issue&& other) noexcept {
    if (this != &other) {
        swap(other);
        other.field_.clear();
        other.code_.clear();
        other.message_.clear();
    }
    return *this;
}

http_error::http_error(http_error_info_options options)
    : status_(options.status_),
      status_text_(options.status_text_.view()),
      code_(options.code_.view()),
      message_(options.message_.view()),
      validation_issues_(options.validation_issues_.begin(), options.validation_issues_.end()) {}

http_error::http_error(http_status_code status, std::string_view code, std::string_view message,
    std::string_view status_text, std::vector<validation_issue>&& issues)
    : status_(status),
      status_text_(status_text),
      code_(code),
      message_(message),
      validation_issues_(std::move(issues)) {}

http_error& http_error::operator=(const http_error& other) {
    if (this != &other) {
        std::vector<validation_issue> copied_issues(other.validation_issues_);
        std::string copied_status_text(other.status_text_);
        std::string copied_code(other.code_);
        std::string copied_message(other.message_);
        validation_issues_.swap(copied_issues);
        status_text_.swap(copied_status_text);
        code_.swap(copied_code);
        message_.swap(copied_message);
        status_ = other.status_;
    }
    return *this;
}
const char* http_error::what() const noexcept {
    return message_.c_str();
}

http_error_info http_error::info() const& noexcept {
    return http_error_info({.status_ = status_,
        .code_ = code_,
        .message_ = message_,
        .status_text_ = status_text_,
        .validation_issues_ = validation_issues_});
}

std::string_view default_error_code(http_status_code status) noexcept {
    switch (status.value()) {
        case http_status::bad_request.value():
            return "bad_request";
        case http_status::unauthorized.value():
            return "unauthorized";
        case http_status::forbidden.value():
            return "forbidden";
        case http_status::not_found.value():
            return "not_found";
        case http_status::method_not_allowed.value():
            return "method_not_allowed";
        case http_status::conflict.value():
            return "conflict";
        case http_status::precondition_failed.value():
            return "precondition_failed";
        case http_status::content_too_large.value():
            return "content_too_large";
        case http_status::range_not_satisfiable.value():
            return "range_not_satisfiable";
        case http_status::expectation_failed.value():
            return "expectation_failed";
        case http_status::unprocessable_content.value():
            return "unprocessable_content";
        case http_status::too_many_requests.value():
            return "too_many_requests";
        case http_status::request_header_fields_too_large.value():
            return "request_header_fields_too_large";
        case http_status::internal_server_error.value():
            return "internal_error";
        case http_status::not_implemented.value():
            return "not_implemented";
        case http_status::service_unavailable.value():
            return "service_unavailable";
        case http_status::http_version_not_supported.value():
            return "http_version_not_supported";
        default:
            return status.is_server_error() ? "internal_error" : "bad_request";
    }
}

std::string exception_text(const std::exception_ptr& exception) {
    if (exception == nullptr) {
        return {};
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::system_error& error) {
        return system_error_text(error);
    } catch (const std::exception& error) {
        return std::string(error.what());
    } catch (...) {
        return std::string("non-standard exception");
    }
}

http_response detail::make_default_error_response(http_error_info error) {
    return make_normalized_error_response(normalize_http_error_info(error));
}

http_response detail::make_route_not_found_response() {
    return make_default_error_response(
        http_error_info({.status_ = http_status::not_found, .message_ = "route not found"}));
}

// Running the application's error handler, or the default response when there
// is none. A handler that throws is answered with the default response too:
// transport output stays deterministic and no exception detail reaches the
// client.
task<http_response> detail::invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref handler) {
    error = normalize_http_error_info(error);

    if (handler != nullptr) {
        try {
            co_return co_await handler(context_value, error);
        } catch (const http_error& nested) {
            co_return make_default_error_response(nested.info());
        } catch (...) {
            // The error handler itself threw: nothing above it observes the
            // failure, so report it server-side, keep transport output
            // deterministic, and avoid echoing exception detail to the client.
            ruvia::report_unhandled_failure("error handler", std::current_exception());
            co_return make_default_error_response(
                http_error_info({.status_ = ruvia::http_status::internal_server_error,
                    .code_ = "error_handler_failed",
                    .message_ = "error handler failed"}));
        }
    }

    co_return make_normalized_error_response(error);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// One synchronous representation pass: apply CORS (`cors`, when set) and the
// listener's Alt-Svc, and run the compression policy up to its offload
// boundary, which only a worker with a blocking pool can cross.
[[nodiscard]] http_buffered_compression_step prepare_representation(const http_request& request,
    const http_response_coding_policy& policy, std::string_view alternative_service,
    const http_cors_policy* cors, http_response& response, const http_server_options& options,
    const worker_ref& worker) {
    if (cors != nullptr) {
        (void)cors->apply_response_headers(request, response);
    }
    apply_alternative_service(response, alternative_service);
    if (options.compression_.has_value()) {
        if (const auto* selection = policy.selection()) {
            return apply_response_compression(*selection, request.known_method(), response, *options.compression_,
                *options.codecs_, worker.blocking_pool() != nullptr);
        }
    }
    return {http_response_compression_status::not_applicable, false};
}

}  // namespace

http_response_preparation http_response_tail::prepare(
    http_response& response, const http_server_options& options, const worker_ref& worker) const {
    const auto& request = *request_;
    const auto first_pass = prepare_representation(
        request, coding_policy_, alternative_service_, cors(options), response, options, worker);
    if (!first_pass.offload_) {
        auto policy = coding_policy_;
        buffered_response_recovery recovery;
        const auto step = recovery.advance(policy, request, response, first_pass.status_);
        if (step.action_ == buffered_response_recovery_action::ready) {
            return http_response_preparation(prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), false});
        }
        return http_response_preparation(policy, recovery, step);
    }
    return http_response_preparation(coding_policy_);
}

task<prepared_application_response> http_response_tail::finish_prepare(const http_response_preparation& preparation,
    http_response& response, const http_server_options& options, const route_table& routes,
    const context_services& services) const {
    const auto& request = *request_;
    const auto& deferred = std::get<http_response_preparation::deferred_type>(preparation.state_);
    auto policy = deferred.policy_;
    auto recovery = deferred.recovery_;
    const auto* pending_step = deferred.pending_step_ ? &*deferred.pending_step_ : nullptr;
    http_buffered_compression_step pass{http_response_compression_status::not_applicable, true};
    for (;;) {
        buffered_response_recovery_step step;
        if (pending_step != nullptr) {
            step = *pending_step;
            pending_step = nullptr;
        } else {
            auto compression = pass.status_;
            if (pass.offload_) {
                compression = co_await compress_response_offloaded(
                    *policy.selection(), response, services.worker(), services.get_stop_token());
            }
            step = recovery.advance(policy, request, response, compression);
        }
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::handle_error) {
            response = co_await routes.handle_error(request, *step.error_, services);
        }
        pass = prepare_representation(
            request, policy, alternative_service_, cors(options), response, options, services.worker());
    }
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] bool is_csrf_token_header(std::string_view name) noexcept {
    for (const std::string_view prefix : {"sec-", "proxy-", "access-control-request-", "x-forwarded-"}) {
        if (name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix)) {
            return false;
        }
    }
    for (const std::string_view reserved : {
             "accept", "accept-language", "content-language", "content-type", "range",
             "accept-charset", "accept-encoding", "connection", "content-length",
             "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
             "origin", "permissions-policy", "referer", "set-cookie", "te",
             "trailer", "transfer-encoding", "upgrade", "via",
             "user-agent", "priority", "save-data", "device-memory", "dpr", "width",
             "viewport-width", "downlink", "ect", "rtt", "forwarded", "x-real-ip",
             "x-requested-with"}) {
        if (http_ascii_equals_ignore_case(name, reserved)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const csrf_protection_config& csrf_protection::validate(const csrf_protection_config& source_value) {
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!is_valid_http_header_name(source_value.header_name_)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    if (!is_csrf_token_header(source_value.header_name_)) {
        throw std::invalid_argument("CSRF token requires a script-supplied custom request field");
    }
    return source_value;
}

csrf_protection::csrf_protection()
    : csrf_protection(csrf_protection_config{}) {}

csrf_protection::csrf_protection(const csrf_protection_config& config)
    : cookie_name_(validate(config).cookie_name_),
      header_name_(config.header_name_) {}

task<void> csrf_protection::invoke(context& c, next& next_value) {
    const auto method = c.req().known_method();
    const bool safe = method == http_known_method::get || method == http_known_method::head ||
                      method == http_known_method::options;
    const auto cookie = c.req().cookie(cookie_name_);
    if (!safe) {
        const auto header_value = c.req().header(header_name_);
        if (!cookie || cookie->empty() || !header_value || header_value->empty() ||
            !detail::constant_time_bytes_equal(*cookie, *header_value)) {
            throw http_error({.status_ = ruvia::http_status::forbidden,
                .code_ = "csrf_token_mismatch",
                .message_ = "CSRF token missing or invalid"});
        }
    } else if (!cookie || cookie->empty()) {
        std::array<char, detail::secure_token_size> buffer;
        const auto token = detail::generate_secure_token(buffer);
        const auto connection = c.conn();
        const cookie_options options{
            .path_ = "/",
            .same_site_ = cookie_same_site::lax,
            .secure_ = connection.scheme() == http_scheme::https ? cookie_attribute_policy::emit
                                                                 : cookie_attribute_policy::omit,
        };
        c.policy().set_cookie({.name_ = cookie_name_, .value_ = token, .attributes_ = options});
    }
    co_await next_value();
}

}  // namespace ruvia

namespace ruvia {

namespace {

constexpr std::string_view content_security_policy_name = "Content-Security-Policy";
constexpr std::string_view referrer_policy_name = "Referrer-Policy";
constexpr std::string_view permissions_policy_name = "Permissions-Policy";

[[nodiscard]] bool emits_default_security_header(default_security_header_policy policy) {
    switch (policy) {
        case default_security_header_policy::emit_default:
            return true;
        case default_security_header_policy::omit:
            return false;
        default:
            throw std::invalid_argument("default security header policy is invalid");
    }
}

[[nodiscard]] bool replaces_existing_security_header(security_header_conflict_policy policy) {
    switch (policy) {
        case security_header_conflict_policy::preserve_existing:
            return false;
        case security_header_conflict_policy::replace_existing:
            return true;
        default:
            throw std::invalid_argument("security header conflict policy is invalid");
    }
}

}  // namespace

security_headers_middleware::security_headers_middleware()
    : security_headers_middleware(security_headers_config{}) {}

security_headers_middleware::security_headers_middleware(const security_headers_config& source_value)
    : emit_content_type_options_(emits_default_security_header(source_value.content_type_options_header_)),
      emit_frame_options_(emits_default_security_header(source_value.frame_options_header_)),
      emit_strict_transport_security_(
          emits_default_security_header(source_value.strict_transport_security_header_)),
      emit_disabled_xss_protection_([&] {
          switch (source_value.xss_protection_header_) {
              case xss_protection_header_policy::emit_disabled:
                  return true;
              case xss_protection_header_policy::omit:
                  return false;
              default:
                  throw std::invalid_argument("X-XSS-Protection header policy is invalid");
          }
      }()),
      replace_existing_(replaces_existing_security_header(source_value.existing_headers_)) {
    const auto validate_value = [](std::string_view value) {
        if (!value.empty() && !is_valid_http_header_value(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validate_value(source_value.content_security_policy_);
    validate_value(source_value.referrer_policy_);
    validate_value(source_value.permissions_policy_);
    for (const auto& header : source_value.custom_headers_) {
        if (!is_valid_http_header_name(header.name_)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!is_valid_http_header_value(header.value_)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    content_security_policy_.assign(source_value.content_security_policy_);
    referrer_policy_.assign(source_value.referrer_policy_);
    permissions_policy_.assign(source_value.permissions_policy_);
    custom_headers_.reserve(source_value.custom_headers_.size());
    for (const auto& header : source_value.custom_headers_) {
        custom_headers_.emplace_back(header.name_, header.value_);
    }
}

task<void> security_headers_middleware::invoke(context& context_value, next& next_value) {
    const bool secure_transport = context_value.conn().scheme() == http_scheme::https;
    const auto set_header = [&context_value, replace_existing = replace_existing_](
                                std::string_view name, std::string_view value, bool skip_empty) {
        if (skip_empty && value.empty()) {
            return;
        }
        detail::context_access::security_header(context_value, name, value, replace_existing);
    };

    if (emit_content_type_options_) {
        set_header("X-Content-Type-Options", "nosniff", true);
    }
    if (emit_frame_options_) {
        set_header("X-Frame-Options", "DENY", true);
    }
    if (emit_strict_transport_security_ && secure_transport) {
        set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (emit_disabled_xss_protection_) {
        set_header("X-XSS-Protection", "0", true);
    }

    set_header(content_security_policy_name, content_security_policy_, true);
    set_header(referrer_policy_name, referrer_policy_, true);
    set_header(permissions_policy_name, permissions_policy_, true);

    for (const auto& header : custom_headers_) {
        if (!secure_transport &&
            http_ascii_equals_ignore_case(header.name(), "Strict-Transport-Security")) {
            continue;
        }
        set_header(header.name(), header.value(), false);
    }
    co_await next_value();
}

}  // namespace ruvia

namespace {

constexpr const char* response_stream_lane_in_progress = "response stream output operation is already in progress";

// What an output operation of a response stream borrows from its writer.
struct response_stream_output final {
    void* target_;
    bool& active_;
    ruvia::detail::operation_interrupt_source& interrupts_;
    const ruvia::worker_ref& worker_;
    ruvia::context* context_;
};

template <typename operation_type>
auto run_response_stream_output(response_stream_output output, ruvia::operation_options operation,
    const char* operation_name, operation_type start) -> decltype(start()) {
    auto lease = ruvia::detail::claim_operation_lane(output.active_, response_stream_lane_in_progress);
    ruvia::detail::operation_interrupt interrupt(
        output.interrupts_, output.worker_, operation, output.context_, &ruvia::detail::context_access::reset_interrupted_request, operation_name);
    try {
        if constexpr (std::is_void_v<typename decltype(start())::value_type>) {
            co_await start();
            interrupt.throw_if_interrupted();
        } else {
            auto result = co_await start();
            interrupt.throw_if_interrupted();
            co_return result;
        }
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
}

}  // namespace

namespace ruvia {

sse_writer::sse_writer(const sse_writer& other) noexcept
    : writer_(other.writer_),
      registration_(other.registration_, this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept
    : writer_(std::exchange(other.writer_, nullptr)),
      registration_(std::move(other.registration_), this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept
    : writer_(writer.operation_scope_.active() ? &writer : nullptr),
      registration_(writer.operation_scope_, this, &sse_writer::expire_capability) {}

response_stream_writer& sse_writer::writer() const {
    registration_.require_active();
    return *writer_;
}

void sse_writer::expire_capability(void* target) noexcept {
    static_cast<sse_writer*>(target)->writer_ = nullptr;
}

http_tunnel& context::tunnel() const {
    const auto* output = response_output().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

websocket& context::websocket() const {
    const auto* output = response_output().get_websocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->get_websocket();
}

response_stream_writer& context::stream() {
    const auto* output = response_output().response_stream();
    if (output == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return output->writer();
}

sse_writer context::stream_sse() {
    auto& writer = stream();
    if (!writer.committed()) {
        set_stable_response_header("Content-Type", "text/event-stream");
        set_stable_response_header("Cache-Control", "no-cache");
    }
    return sse_writer(writer);
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(
    detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)> read, bool& read_active,
    operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read(std::move(operation));
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<view_type, std::string_view>) {
        co_return as_chars(*chunk);
    } else {
        co_return *chunk;
    }
}

task<void> write_transferred_chunk(response_stream_output output, task<void> (*write)(void*, std::string_view),
    std::string chunk, operation_options operation) {
    co_await run_response_stream_output(output, std::move(operation), "response stream write",
        [&] { return write(output.target_, chunk); });
}

task<void> end_owned(response_stream_output output, task<void> (*end)(void*, std::span<const http_header_view>),
    std::vector<http_header> fields, operation_options operation) {
    const detail::inline_http_header_views views(fields);
    co_await run_response_stream_output(output, std::move(operation), "response stream end",
        [&] { return end(output.target_, views.view()); });
}

task<timer_sleep_result> sleep_stopped_by_either(void* target,
    task<timer_sleep_result> (*sleep)(void*, std::chrono::milliseconds, const stop_token&),
    std::chrono::milliseconds duration, stop_token context_stop, stop_token stop) {
    const stop_group either(context_stop, stop);
    co_return co_await sleep(target, duration, either.token());
}

}  // namespace

task<std::optional<std::span<const std::byte>>> body_reader::read(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> body_reader::text(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_, std::move(operation)));
}

void response_stream_writer::require_output() const {
    require_active();
    if (worker_ == nullptr || context_ == nullptr) {
        throw std::logic_error("response stream is not bound to a request");
    }
}

task<stream_commit_result> response_stream_writer::commit(operation_options operation) & {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream commit",
            [target = target_, commit = commit_] { return commit(target); }));
}

task<stream_commit_result> sse_writer::commit(operation_options operation) & {
    return writer().commit(std::move(operation));
}

task<void> response_stream_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return write(as_chars(chunk), std::move(operation));
}

task<void> response_stream_writer::write(std::string_view chunk, operation_options operation) & {
    require_active();
    return write_owned(std::string(chunk), std::move(operation));
}

task<void> response_stream_writer::write_owned(std::string&& chunk, operation_options operation) {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_transferred_chunk({target_, output_active_, *interrupts_, *worker_, context_}, write_, std::move(chunk), std::move(operation)));
}

task<void> response_stream_writer::writeln(std::string_view chunk, operation_options operation) & {
    require_active();
    std::string owned;
    owned.reserve(chunk.size() + 1U);
    owned.append(chunk);
    owned.push_back('\n');
    return write_owned(std::move(owned), std::move(operation));
}

task<timer_sleep_result> response_stream_writer::sleep(timeout_duration duration, stop_token stop) & {
    require_active();
    if (!stop.stoppable()) {
        return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration.value(), stop_token_));
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, sleep_stopped_by_either(target_, sleep_, duration.value(), stop_token_, std::move(stop)));
}

task<void> response_stream_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    require_output();
    std::vector<http_header> owned_trailers_value;
    owned_trailers_value.reserve(trailers.size());
    for (const auto& header : trailers) {
        owned_trailers_value.emplace_back(header.name(), header.value());
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, end_owned({target_, output_active_, *interrupts_, *worker_, context_}, end_, std::move(owned_trailers_value), std::move(operation)));
}

task<timer_sleep_result> sse_writer::sleep(timeout_duration duration, stop_token stop) & {
    return writer().sleep(duration, std::move(stop));
}

task<void> sse_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    return writer().end(trailers, std::move(operation));
}

task<void> sse_writer::write(const sse_message& message, operation_options operation) & {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message);
    return stream_writer.write(std::move(frame), std::move(operation));
}

task<std::optional<multipart_stream_part>> multipart_reader::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, read_task(std::move(operation)));
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability(void* target) noexcept {
    auto& reader_value = *static_cast<multipart_reader*>(target);
    reader_value.operation_scope_.close();
    reader_value.body_reader_ = nullptr;
    reader_value.state_.emplace<expired_state_type>();
}

task<std::optional<multipart_stream_part>> multipart_reader::read_task(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, "multipart body read is already in progress");
    if (std::holds_alternative<finished_state_type>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<failed_state_type>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<receiving_state_type>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "multipart body read");
    }
    const detail::operation_steps steps(std::move(operation));
    read_guard_type read_guard_value(*this);
    try {
        for (;;) {
            auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
            if (const auto* part = result_value.part()) {
                read_guard_value.commit();
                co_return *part;
            }
            if (result_value.done() != nullptr) {
                while (co_await get_body_reader().read(steps.next("multipart body read"))) {
                }
                state_.emplace<finished_state_type>();
                read_guard_value.commit();
                co_return std::nullopt;
            }
            if (result_value.need_input() != nullptr) {
                auto chunk = co_await get_body_reader().read(steps.next("multipart body read"));
                if (!chunk) {
                    std::get<receiving_state_type>(state_).parser_.finish_input();
                } else {
                    std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
                }
                continue;
            }
            if (const auto* failure = result_value.failure()) {
                throw failure->protocol_error();
            }
            throw std::logic_error("unexpected multipart poll result");
        }
    } catch (const std::system_error& error) {
        // An interrupted body read fed the parser nothing; parsing resumes on
        // the next read.
        if (steps.interrupted(error)) {
            read_guard_value.commit();
        }
        throw;
    }
}

}  // namespace ruvia

namespace {
ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target,
    ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*, ruvia::operation_options),
    bool& read_active, ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target, std::move(operation));
}

using websocket_write_type = ruvia::task<void> (*)(void*, ruvia::websocket_opcode, std::string_view, bool,
    ruvia::detail::websocket_write_extent, ruvia::operation_options);

// The final fragment releases the data lane of its fragmented message.
ruvia::task<void> write_websocket_payload(void* target, websocket_write_type write, ruvia::websocket_opcode opcode,
    std::string payload_value, bool& write_active, bool compress, ruvia::detail::websocket_write_extent extent,
    ruvia::operation_options operation, bool& fragmented_message_active) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress, extent, std::move(operation));
    if (extent == ruvia::detail::websocket_write_extent::final_fragment) {
        fragmented_message_active = false;
    }
}

// The read leases are claimed by the caller before this task exists: the
// payload borrows the connection's read storage, and no read may run, even one
// started while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target, websocket_write_type write,
    ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease,
    ruvia::detail::operation_lane_lease message_lease, bool& write_active, bool compress,
    ruvia::operation_options operation) {
    auto held_read = std::move(read_lease);
    auto held_message = std::move(message_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress, ruvia::detail::websocket_write_extent::message,
        std::move(operation));
}

ruvia::task<void> close_websocket_with_reason(void* target,
    ruvia::task<void> (*close)(void*, ruvia::websocket_close_options, ruvia::operation_options),
    ruvia::websocket_close_options options, std::string reason, ruvia::operation_options operation,
    bool& read_active, bool& write_active, const bool& message_send_active) {
    // The write lane also excludes another close.
    auto write_lease = ruvia::detail::claim_operation_lane(write_active, "websocket close cannot overlap an output operation");
    if (message_send_active) {
        throw std::logic_error("websocket close cannot overlap a message send");
    }
    // A pending read observes the peer Close itself; otherwise close reads,
    // and holds the read lane while it does.
    const ruvia::detail::operation_lane_lease read_lease(read_active);
    static_cast<void>(read_lease);
    options.reason_ = reason;
    co_await close(target, options, std::move(operation));
}

}  // namespace

namespace ruvia {
namespace {
detail::websocket_state& require_websocket(const std::shared_ptr<detail::websocket_state>& state) {
    if (!state) {
        throw std::logic_error("websocket owner was moved");
    }
    state->require_active();
    return *state;
}
void retire_websocket(std::shared_ptr<detail::websocket_state> state) noexcept {
    if (!state) {
        return;
    }
    if (state->owner_) {
        state->abort();
    }
    state->operation_scope_.expire();
    if (auto owner = std::exchange(state->owner_, nullptr)) {
        if (state->retire_) {
            state->retire_(owner.get());
        }
    }
}
}  // namespace
websocket::websocket(std::shared_ptr<detail::websocket_state> state) noexcept
    : state_(std::move(state)) {}
websocket::websocket(websocket&&) noexcept = default;
websocket& websocket::operator=(websocket&& other) noexcept {
    if (this != &other) {
        retire_websocket(std::exchange(state_, std::move(other.state_)));
    }
    return *this;
}
websocket::~websocket() {
    retire_websocket(std::move(state_));
}
task<std::optional<websocket_message>> websocket::read(operation_options operation) & {
    return require_websocket(state_).read(std::move(operation));
}
task<void> websocket::text(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).text(payload, options, std::move(operation));
}
task<void> websocket::binary(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::binary(std::span<const std::byte> payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::ping(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).ping(payload, std::move(operation));
}
task<void> websocket::pong(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).pong(payload, std::move(operation));
}
task<void> websocket::write_owned(websocket_opcode opcode, std::string&& payload, bool compress, operation_options operation) {
    return require_websocket(state_).write(opcode, std::move(payload), compress, std::move(operation));
}
task<void> websocket::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).send(std::move(message), options, std::move(operation));
}
task<void> websocket::close(websocket_close_options options, operation_options operation) & {
    return require_websocket(state_).close(options, std::move(operation));
}
void websocket::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
websocket_message_writer websocket::begin_text(websocket_send_options options) & {
    return require_websocket(state_).begin_text(options);
}
websocket_message_writer websocket::begin_binary(websocket_send_options options) & {
    return require_websocket(state_).begin_binary(options);
}
std::string_view websocket::subprotocol() const& noexcept {
    return state_ ? state_->subprotocol() : std::string_view{};
}
bool websocket::connected() const noexcept {
    if (!state_) {
        return false;
    }
    if (!state_->operation_scope_.active()) {
        return false;
    }
    state_->require_active();
    return state_->protocol_(state_->target_).liveness_mode() == websocket_liveness_mode::open;
}
const worker_ref& websocket::worker() const& {
    return *require_websocket(state_).worker_;
}
std::optional<std::uint16_t> websocket::peer_close_code() const noexcept {
    return state_ ? state_->peer_close_code() : std::nullopt;
}
std::string_view websocket::peer_close_reason() const& noexcept {
    return state_ ? state_->peer_close_reason() : std::string_view{};
}
websocket_termination websocket::termination() const noexcept {
    return state_ ? state_->termination() : websocket_termination::none;
}
std::uint64_t websocket::bytes_received() const noexcept {
    return state_ ? state_->bytes_received() : 0;
}
std::uint64_t websocket::bytes_sent() const noexcept {
    return state_ ? state_->bytes_sent() : 0;
}
task<std::optional<websocket_message>> detail::websocket_state::read(operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (read_timeout_) {
        operation = override_operation_options({.timeout_ = read_timeout_->value()}, std::move(operation));
    }
    return make_scoped_operation(operation_scope_, read_websocket(target_, read_, read_active_, std::move(operation)));
}

task<void> detail::websocket_state::text(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::text, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::binary, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::span<const std::byte> payload_value, websocket_send_options options, operation_options operation) & {
    return binary(as_chars(payload_value), options, std::move(operation));
}

task<void> detail::websocket_state::pong(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::pong, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::ping(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::ping, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    auto read_lease = detail::claim_operation_lane(read_active_, "websocket message send cannot overlap a read");
    auto message_lease = detail::claim_operation_lane(message_send_active_, "websocket message send is already pending");
    return make_scoped_operation(operation_scope_,
        write_websocket_message(target_, write_, message, std::move(read_lease), std::move(message_lease),
            write_active_, options.compress_, std::move(operation)));
}

task<void> detail::websocket_state::close(websocket_close_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    std::string owned(options.reason_.view());
    return make_scoped_operation(operation_scope_,
        close_websocket_with_reason(target_, close_, options, std::move(owned), std::move(operation),
            read_active_, write_active_, message_send_active_));
}

void detail::websocket_state::abort() & noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return;
    }
    abort_(target_);
}

std::optional<std::uint16_t> detail::websocket_state::peer_close_code() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return std::nullopt;
    }
    return peer_close_code_(target_);
}

std::string_view detail::websocket_state::peer_close_reason() const& noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return {};
    }
    return peer_close_reason_(target_);
}

websocket_termination detail::websocket_state::termination() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return websocket_termination::none;
    }
    return protocol_(target_).termination();
}

std::uint64_t detail::websocket_state::bytes_received() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_received();
}

std::uint64_t detail::websocket_state::bytes_sent() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_sent();
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string_view payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    return write(opcode, std::string(payload_value), compress, std::move(operation), extent);
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string&& payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    if (extent == detail::websocket_write_extent::message && fragmented_message_active_ &&
        (opcode == websocket_opcode::text || opcode == websocket_opcode::binary)) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    operation = override_operation_options(defaults_, std::move(operation));
    return make_scoped_operation(operation_scope_,
        write_websocket_payload(target_, write_, opcode, std::move(payload_value), write_active_, compress, extent,
            std::move(operation), fragmented_message_active_));
}

websocket_message_writer detail::websocket_state::begin_text(websocket_send_options options) & {
    return begin_message(websocket_opcode::text, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_binary(websocket_send_options options) & {
    return begin_message(websocket_opcode::binary, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_message(websocket_opcode opcode, bool compress) {
    require_active();
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is already open");
    }
    fragmented_message_active_ = true;
    ++fragmented_message_sequence_;
    return websocket_message_writer(*this, opcode, compress);
}

websocket_message_writer::websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept
    : owner_(&owner),
      message_id_(owner.fragmented_message_sequence_),
      opcode_(opcode),
      compress_(compress),
      registration_(owner.operation_scope_, this, &websocket_message_writer::expire_capability) {}

websocket_message_writer::websocket_message_writer(websocket_message_writer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      message_id_(other.message_id_),
      opcode_(other.opcode_),
      compress_(other.compress_),
      registration_(std::move(other.registration_), this) {}

websocket_message_writer::~websocket_message_writer() {
    abort();
}

void websocket_message_writer::expire_capability(void* target) noexcept {
    static_cast<websocket_message_writer*>(target)->owner_ = nullptr;
}

bool websocket_message_writer::message_open() const noexcept {
    return owner_ != nullptr && owner_->fragmented_message_active_ &&
           owner_->fragmented_message_sequence_ == message_id_;
}

void websocket_message_writer::abort() & noexcept {
    if (!message_open()) {
        owner_ = nullptr;
        return;
    }
    // The peer cannot tell where an unfinished message ends.
    auto* owner = std::exchange(owner_, nullptr);
    owner->fragmented_message_active_ = false;
    owner->abort();
}

task<void> websocket_message_writer::write(std::string_view chunk, operation_options operation) & {
    return submit(chunk, false, std::move(operation));
}

task<void> websocket_message_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return submit(as_chars(chunk), false, std::move(operation));
}

task<void> websocket_message_writer::finish(std::string_view last_chunk, operation_options operation) & {
    return submit(last_chunk, true, std::move(operation));
}

task<void> websocket_message_writer::finish(std::span<const std::byte> last_chunk, operation_options operation) & {
    return submit(as_chars(last_chunk), true, std::move(operation));
}

task<void> websocket_message_writer::submit(std::string_view chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, chunk, compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

task<void> websocket_message_writer::submit_owned(
    std::string&& chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, std::move(chunk), compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

}  // namespace ruvia
