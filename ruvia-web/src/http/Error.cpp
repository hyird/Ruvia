#include "ruvia/web/Error.h"

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/web/detail/http/HttpErrorNormalize.h"
#include "ruvia/web/detail/http/HttpErrorResponse.h"
#include "ruvia/web/detail/json/JsonEscape.h"

namespace ruvia {
namespace {

void appendErrorBody(std::pmr::string& body, HttpErrorInfo error) {
    std::size_t size =
        std::string_view("{\"error\":").size() +
        detail::jsonStringSizeHint(error.statusText()) +
        std::string_view(",\"code\":").size() +
        detail::jsonStringSizeHint(error.code()) +
        std::string_view(",\"message\":").size() +
        detail::jsonStringSizeHint(error.message()) +
        1;
    if (!error.detailsJson().empty()) {
        size += std::string_view(",\"details\":").size() + error.detailsJson().size();
    }
    body.reserve(size);

    body.append("{\"error\":");
    detail::appendJsonString(body, error.statusText());
    body.append(",\"code\":");
    detail::appendJsonString(body, error.code());
    body.append(",\"message\":");
    detail::appendJsonString(body, error.message());
    if (!error.detailsJson().empty()) {
        body.append(",\"details\":");
        body.append(error.detailsJson().data(), error.detailsJson().size());
    }
    body.push_back('}');
}

struct DefaultErrorPresentation final {
    std::string_view code;
    std::string_view body;
};

[[nodiscard]] DefaultErrorPresentation defaultErrorPresentation(
    HttpStatusCode status) noexcept {
    switch (status.value()) {
        case http_status::kBadRequest.value():
            return {
                "bad_request",
                "{\"error\":\"Bad Request\",\"code\":\"bad_request\","
                "\"message\":\"Bad Request\"}"};
        case http_status::kUnauthorized.value():
            return {
                "unauthorized",
                "{\"error\":\"Unauthorized\",\"code\":\"unauthorized\","
                "\"message\":\"Unauthorized\"}"};
        case http_status::kForbidden.value():
            return {
                "forbidden",
                "{\"error\":\"Forbidden\",\"code\":\"forbidden\","
                "\"message\":\"Forbidden\"}"};
        case http_status::kNotFound.value():
            return {
                "not_found",
                "{\"error\":\"Not Found\",\"code\":\"not_found\","
                "\"message\":\"Not Found\"}"};
        case http_status::kMethodNotAllowed.value():
            return {
                "method_not_allowed",
                "{\"error\":\"Method Not Allowed\","
                "\"code\":\"method_not_allowed\","
                "\"message\":\"Method Not Allowed\"}"};
        case http_status::kConflict.value():
            return {
                "conflict",
                "{\"error\":\"Conflict\",\"code\":\"conflict\","
                "\"message\":\"Conflict\"}"};
        case http_status::kPreconditionFailed.value():
            return {
                "precondition_failed",
                "{\"error\":\"Precondition Failed\","
                "\"code\":\"precondition_failed\","
                "\"message\":\"Precondition Failed\"}"};
        case http_status::kContentTooLarge.value():
            return {
                "content_too_large",
                "{\"error\":\"Content Too Large\","
                "\"code\":\"content_too_large\","
                "\"message\":\"Content Too Large\"}"};
        case http_status::kRangeNotSatisfiable.value():
            return {
                "range_not_satisfiable",
                "{\"error\":\"Range Not Satisfiable\","
                "\"code\":\"range_not_satisfiable\","
                "\"message\":\"Range Not Satisfiable\"}"};
        case http_status::kExpectationFailed.value():
            return {
                "expectation_failed",
                "{\"error\":\"Expectation Failed\","
                "\"code\":\"expectation_failed\","
                "\"message\":\"Expectation Failed\"}"};
        case http_status::kUnprocessableContent.value():
            return {
                "unprocessable_content",
                "{\"error\":\"Unprocessable Content\","
                "\"code\":\"unprocessable_content\","
                "\"message\":\"Unprocessable Content\"}"};
        case http_status::kTooManyRequests.value():
            return {
                "too_many_requests",
                "{\"error\":\"Too Many Requests\","
                "\"code\":\"too_many_requests\","
                "\"message\":\"Too Many Requests\"}"};
        case http_status::kRequestHeaderFieldsTooLarge.value():
            return {
                "request_header_fields_too_large",
                "{\"error\":\"Request Header Fields Too Large\","
                "\"code\":\"request_header_fields_too_large\","
                "\"message\":\"Request Header Fields Too Large\"}"};
        case http_status::kInternalServerError.value():
            return {
                "internal_error",
                "{\"error\":\"Internal Server Error\","
                "\"code\":\"internal_error\","
                "\"message\":\"Internal Server Error\"}"};
        case http_status::kNotImplemented.value():
            return {
                "not_implemented",
                "{\"error\":\"Not Implemented\","
                "\"code\":\"not_implemented\","
                "\"message\":\"Not Implemented\"}"};
        case http_status::kServiceUnavailable.value():
            return {
                "service_unavailable",
                "{\"error\":\"Service Unavailable\","
                "\"code\":\"service_unavailable\","
                "\"message\":\"Service Unavailable\"}"};
        case http_status::kHttpVersionNotSupported.value():
            return {
                "http_version_not_supported",
                "{\"error\":\"HTTP Version Not Supported\","
                "\"code\":\"http_version_not_supported\","
                "\"message\":\"HTTP Version Not Supported\"}"};
        default: return {
            status.isServerError() ? "internal_error" : "bad_request", {}};
    }
}

[[nodiscard]] bool isDefaultErrorBodyCandidate(HttpErrorInfo error) noexcept {
    return error.detailsJson().empty() &&
        !httpReasonPhrase(error.status()).empty() &&
        error.statusText() == httpReasonPhrase(error.status()) &&
        error.code() == defaultErrorCode(error.status()) &&
        error.message() == error.statusText();
}

}  // namespace

HttpError::HttpError(
    HttpStatusCode status,
    std::string_view code,
    std::string_view message,
    std::string_view statusText)
    : status_(status),
      statusText_(statusText, detail::processResource()),
      code_(code, detail::processResource()),
      message_(message, detail::processResource()) {}

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

HttpErrorInfo HttpError::info() const & noexcept {
    return HttpErrorInfo(status_, code_, message_, statusText_);
}

std::string_view defaultErrorCode(HttpStatusCode status) noexcept {
    return defaultErrorPresentation(status).code;
}

HttpResponse detail::makeDefaultErrorResponse(
    std::pmr::memory_resource* resource,
    HttpErrorInfo error) {
    error = normalizeHttpErrorInfo(error);

    HttpResponse response(resource);
    reserveResponseHeaders(response, 1);
    response.status(error.status());
    setResponseHeaderStableView(response, "Content-Type", "application/json");

    if (isDefaultErrorBodyCandidate(error)) {
        if (const auto body = defaultErrorPresentation(error.status()).body;
            !body.empty()) {
            setResponseBodyStaticView(response, body);
            return response;
        }
    }

    std::pmr::string body(resource);
    appendErrorBody(body, error);
    setResponseBodyOwned(response, std::move(body));
    return response;
}

}  // namespace ruvia
