#include "ruvia/web/Error.h"

#include "ruvia/web/Context.h"
#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"

#include "ruvia/core/memory/ProcessResource.h"
#include <exception>

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/web/detail/http/error/HttpErrorNormalize.h"
#include "ruvia/web/detail/http/error/HttpErrorResponse.h"

namespace ruvia {
namespace {

RUVIA_RESPONSE_MODEL(HttpValidationIssueResponseModel, RUVIA_REQUIRED_FIELD(field, ruvia::String), RUVIA_REQUIRED_FIELD(code, ruvia::String), RUVIA_REQUIRED_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(HttpErrorResponseModel, RUVIA_REQUIRED_FIELD(error, ruvia::String), RUVIA_REQUIRED_FIELD(code, ruvia::String), RUVIA_REQUIRED_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(details, ruvia::Array<HttpValidationIssueResponseModel>));

[[nodiscard]] std::pmr::string serializeErrorResponse(HttpErrorInfo error, std::pmr::memory_resource* resource) {
    HttpErrorResponseModel model({.resource = resource});
    model.set<"error">(error.statusText()).set<"code">(error.code()).set<"message">(error.message());
    if (!error.validationIssues().empty()) {
        auto& details = model.ensure<"details">();
        details.reserve(error.validationIssues().size());
        for (const auto& issue : error.validationIssues()) {
            details.emplace_back(ModelOptions{.resource = resource}).set<"field">(issue.field()).set<"code">(issue.code()).set<"message">(issue.message());
        }
    }
    return toJson(model, {.resource = resource});
}

[[nodiscard]] std::string_view defaultErrorCodeValue(HttpStatusCode status) noexcept {
    switch (status.value()) {
        case http_status::kBadRequest.value():
            return "bad_request";
        case http_status::kUnauthorized.value():
            return "unauthorized";
        case http_status::kForbidden.value():
            return "forbidden";
        case http_status::kNotFound.value():
            return "not_found";
        case http_status::kMethodNotAllowed.value():
            return "method_not_allowed";
        case http_status::kConflict.value():
            return "conflict";
        case http_status::kPreconditionFailed.value():
            return "precondition_failed";
        case http_status::kContentTooLarge.value():
            return "content_too_large";
        case http_status::kRangeNotSatisfiable.value():
            return "range_not_satisfiable";
        case http_status::kExpectationFailed.value():
            return "expectation_failed";
        case http_status::kUnprocessableContent.value():
            return "unprocessable_content";
        case http_status::kTooManyRequests.value():
            return "too_many_requests";
        case http_status::kRequestHeaderFieldsTooLarge.value():
            return "request_header_fields_too_large";
        case http_status::kInternalServerError.value():
            return "internal_error";
        case http_status::kNotImplemented.value():
            return "not_implemented";
        case http_status::kServiceUnavailable.value():
            return "service_unavailable";
        case http_status::kHttpVersionNotSupported.value():
            return "http_version_not_supported";
        default:
            return status.isServerError() ? "internal_error" : "bad_request";
    }
}

}  // namespace

HttpError::HttpError(HttpErrorInfoOptions options)
    : status_(options.status),
      statusText_(options.statusText.view(), detail::processResource()),
      code_(options.code.view(), detail::processResource()),
      message_(options.message.view(), detail::processResource()) {}

HttpError::HttpError(const HttpError& other)
    : status_(other.status_),
      statusText_(other.statusText_, detail::processResource()),
      code_(other.code_, detail::processResource()),
      message_(other.message_, detail::processResource()) {}

HttpError& HttpError::operator=(const HttpError& other) {
    if (this != &other) {
        status_ = other.status_;
        statusText_ = other.statusText_;
        code_ = other.code_;
        message_ = other.message_;
    }
    return *this;
}

const char* HttpError::what() const noexcept {
    return message_.c_str();
}

HttpErrorInfo HttpError::info() const& noexcept {
    return HttpErrorInfo({.status = status_, .code = code_, .message = message_, .statusText = statusText_});
}

std::string_view defaultErrorCode(HttpStatusCode status) noexcept {
    return defaultErrorCodeValue(status);
}

HttpResponse detail::makeDefaultErrorResponse(std::pmr::memory_resource* resource, HttpErrorInfo error) {
    error = normalizeHttpErrorInfo(error);

    HttpResponse response({.resource = resource});
    reserveResponseHeaders(response, 1);
    response.status(error.status());
    setResponseHeaderStableView(response, "Content-Type", "application/json");

    auto body = serializeErrorResponse(error, resource);
    setResponseBodyOwned(response, std::move(body));
    return response;
}

// Running the application's error handler, or the default response when there
// is none. A handler that throws is answered with the default response too:
// transport output stays deterministic and no exception detail reaches the
// client.
Task<HttpResponse> detail::invokeErrorHandler(Context& context, HttpErrorInfo error, HttpErrorHandlerRef handler) {
    error = normalizeHttpErrorInfo(error);

    if (handler != nullptr) {
        try {
            co_return co_await handler(context, error);
        } catch (const HttpError& nested) {
            co_return makeDefaultErrorResponse(context.resource(), nested.info());
        } catch (const std::exception&) {
            // The error handler itself threw; keep transport output deterministic
            // and avoid echoing exception detail to the client.
            co_return makeDefaultErrorResponse(context.resource(), HttpErrorInfo({.status = ruvia::http_status::kInternalServerError, .code = "error_handler_failed", .message = "error handler failed"}));
        } catch (...) {
            co_return makeDefaultErrorResponse(context.resource(), HttpErrorInfo({.status = ruvia::http_status::kInternalServerError, .code = "error_handler_failed", .message = "error handler failed"}));
        }
    }

    co_return makeDefaultErrorResponse(context.resource(), error);
}

}  // namespace ruvia
