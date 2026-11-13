#include "ruvia/web/error.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/context.h"
#include "ruvia/web/csrf.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"
#include "ruvia/web/security_headers.h"
#include "ruvia/web/validation.h"

#include "auth/constant_time.h"
#include "context/context_access.h"
#include "context/context_services.h"
#include "http/http_error_response.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"
#include "tls/tls_random.h"

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
