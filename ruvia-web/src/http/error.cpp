#include "ruvia/web/error.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/context.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "http/http_error_response.h"

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
