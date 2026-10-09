#include "ruvia/web/error.h"

#include <algorithm>
#include <exception>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/context.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "http/http_error_normalize.h"
#include "http/http_error_response.h"

namespace ruvia {
namespace {

RUVIA_MODEL(http_validation_issue_response_model, RUVIA_REQUIRED_FIELD(field, ruvia::string),
    RUVIA_REQUIRED_FIELD(code, ruvia::string), RUVIA_REQUIRED_FIELD(message, ruvia::string));

RUVIA_MODEL(http_error_response_model,
    RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(title, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::int32),
    RUVIA_REQUIRED_FIELD(detail, ruvia::string),
    RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<http_validation_issue_response_model>));

[[nodiscard]] std::pmr::string serialize_error_response(
    http_error_info error, std::pmr::memory_resource* resource) {
    http_error_response_model model({.resource_ = resource});
    const auto phrase = http_reason_phrase(error.status());
    model.set<"type">("about:blank")
        .set<"title">(phrase.empty() ? std::string_view("HTTP Error") : phrase)
        .set<"status">(error.status().value())
        .set<"detail">(error.message())
        .set<"code">(error.code());
    if (!error.validation_issues().empty()) {
        auto& details = model.ensure<"errors">();
        const auto issues = error.validation_issues().first(
            std::min(error.validation_issues().size(), max_validation_issues));
        details.reserve(issues.size());
        for (const auto& issue : issues) {
            details.emplace_back(model_options{.resource_ = resource})
                .set<"field">(issue.field())
                .set<"code">(issue.code())
                .set<"message">(issue.message());
        }
    }
    return to_json(model, {.resource_ = resource});
}

[[nodiscard]] std::string_view default_error_code_value(http_status_code status) noexcept {
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

}  // namespace

http_error::http_error(http_error_info_options options)
    : status_(options.status_),
      status_text_(options.status_text_.view(), detail::process_resource()),
      code_(options.code_.view(), detail::process_resource()),
      message_(options.message_.view(), detail::process_resource()) {}

http_error::http_error(const http_error& other)
    : status_(other.status_),
      status_text_(other.status_text_, detail::process_resource()),
      code_(other.code_, detail::process_resource()),
      message_(other.message_, detail::process_resource()) {}

http_error& http_error::operator=(const http_error& other) {
    if (this != &other) {
        status_ = other.status_;
        status_text_ = other.status_text_;
        code_ = other.code_;
        message_ = other.message_;
    }
    return *this;
}

const char* http_error::what() const noexcept {
    return message_.c_str();
}

http_error_info http_error::info() const& noexcept {
    return http_error_info(
        {.status_ = status_, .code_ = code_, .message_ = message_, .status_text_ = status_text_});
}

std::string_view default_error_code(http_status_code status) noexcept {
    return default_error_code_value(status);
}

http_response detail::make_default_error_response(
    std::pmr::memory_resource* resource, http_error_info error) {
    error = normalize_http_error_info(error);

    http_response response({.resource_ = resource});
    response.status(error.status());
    response.header("Content-Type", "application/problem+json");

    auto body = serialize_error_response(error, resource);
    response.owned_body(std::move(body));
    return response;
}

// Running the application's error handler, or the default response when there
// is none. A handler that throws is answered with the default response too:
// transport output stays deterministic and no exception detail reaches the
// client.
task<http_response> detail::invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref_type handler) {
    error = normalize_http_error_info(error);

    if (handler != nullptr) {
        try {
            co_return co_await handler(context_value, error);
        } catch (const http_error& nested) {
            co_return make_default_error_response(context_value.arena(), nested.info());
        } catch (const std::exception&) {
            // The error handler itself threw; keep transport output deterministic
            // and avoid echoing exception detail to the client.
            co_return make_default_error_response(context_value.arena(),
                http_error_info({.status_ = ruvia::http_status::internal_server_error,
                    .code_ = "error_handler_failed",
                    .message_ = "error handler failed"}));
        } catch (...) {
            co_return make_default_error_response(context_value.arena(),
                http_error_info({.status_ = ruvia::http_status::internal_server_error,
                    .code_ = "error_handler_failed",
                    .message_ = "error handler failed"}));
        }
    }

    co_return make_default_error_response(context_value.arena(), error);
}

}  // namespace ruvia
