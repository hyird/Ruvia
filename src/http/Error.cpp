#include "ruvia/http/Error.h"

#include "HttpResponseBodyAccess.h"
#include "HttpResponseHeaderState.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/JsonUtils.h"

namespace ruvia {
namespace {

[[nodiscard]] HttpErrorInfo normalizeError(HttpErrorInfo error) noexcept {
    auto statusText = error.statusText();
    auto code = error.code();
    auto message = error.message();
    if (statusText.empty()) {
        statusText = defaultStatusText(error.status());
    }
    if (code.empty()) {
        code = defaultErrorCode(error.status());
    }
    if (message.empty()) {
        message = statusText;
    }
    return HttpErrorInfo(error.status(), code, message, statusText, error.detailsJson());
}

void appendErrorBody(std::pmr::string& body, HttpErrorInfo error) {
    std::size_t size =
        std::string_view("{\"error\":").size() + detail::jsonStringSizeHint(error.statusText()) +
        std::string_view(",\"code\":").size() + detail::jsonStringSizeHint(error.code()) +
        std::string_view(",\"message\":").size() + detail::jsonStringSizeHint(error.message()) +
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

[[nodiscard]] std::string_view defaultErrorBody(std::uint16_t statusCode) noexcept {
    switch (statusCode) {
        case 400:
            return "{\"error\":\"Bad Request\",\"code\":\"bad_request\",\"message\":\"Bad Request\"}";
        case 401:
            return "{\"error\":\"Unauthorized\",\"code\":\"unauthorized\",\"message\":\"Unauthorized\"}";
        case 403:
            return "{\"error\":\"Forbidden\",\"code\":\"forbidden\",\"message\":\"Forbidden\"}";
        case 404:
            return "{\"error\":\"Not Found\",\"code\":\"not_found\",\"message\":\"Not Found\"}";
        case 405:
            return "{\"error\":\"Method Not Allowed\",\"code\":\"method_not_allowed\",\"message\":\"Method Not Allowed\"}";
        case 409:
            return "{\"error\":\"Conflict\",\"code\":\"conflict\",\"message\":\"Conflict\"}";
        case 412:
            return "{\"error\":\"Precondition Failed\",\"code\":\"precondition_failed\",\"message\":\"Precondition Failed\"}";
        case 413:
            return "{\"error\":\"Payload Too Large\",\"code\":\"payload_too_large\",\"message\":\"Payload Too Large\"}";
        case 416:
            return "{\"error\":\"Range Not Satisfiable\",\"code\":\"range_not_satisfiable\",\"message\":\"Range Not Satisfiable\"}";
        case 417:
            return "{\"error\":\"Expectation Failed\",\"code\":\"expectation_failed\",\"message\":\"Expectation Failed\"}";
        case 418:
            return "{\"error\":\"I'm a Teapot\",\"code\":\"teapot\",\"message\":\"I'm a Teapot\"}";
        case 422:
            return "{\"error\":\"Unprocessable Entity\",\"code\":\"unprocessable_entity\",\"message\":\"Unprocessable Entity\"}";
        case 429:
            return "{\"error\":\"Too Many Requests\",\"code\":\"too_many_requests\",\"message\":\"Too Many Requests\"}";
        case 431:
            return "{\"error\":\"Request Header Fields Too Large\",\"code\":\"request_header_fields_too_large\",\"message\":\"Request Header Fields Too Large\"}";
        case 500:
            return "{\"error\":\"Internal Server Error\",\"code\":\"internal_error\",\"message\":\"Internal Server Error\"}";
        case 501:
            return "{\"error\":\"Not Implemented\",\"code\":\"not_implemented\",\"message\":\"Not Implemented\"}";
        case 503:
            return "{\"error\":\"Service Unavailable\",\"code\":\"service_unavailable\",\"message\":\"Service Unavailable\"}";
        case 505:
            return "{\"error\":\"HTTP Version Not Supported\",\"code\":\"http_version_not_supported\",\"message\":\"HTTP Version Not Supported\"}";
        default:
            return {};
    }
}

[[nodiscard]] bool isDefaultErrorBodyCandidate(HttpErrorInfo error) noexcept {
    return error.detailsJson().empty() &&
        error.statusText() == defaultStatusText(error.status()) &&
        error.code() == defaultErrorCode(error.status()) &&
        error.message() == error.statusText();
}

}  // namespace

HttpError::HttpError(
    std::uint16_t statusCode,
    std::string_view code,
    std::string_view message,
    std::string_view statusText)
    : statusCode_(statusCode),
      statusText_(statusText, std::pmr::get_default_resource()),
      code_(code, std::pmr::get_default_resource()),
      message_(message, std::pmr::get_default_resource()) {}

const char* HttpError::what() const noexcept {
    return message_.c_str();
}

HttpErrorInfo HttpError::info() const noexcept {
    return HttpErrorInfo(statusCode_, code_, message_, statusText_);
}

std::string_view defaultStatusText(std::uint16_t statusCode) noexcept {
    return detail::httpDefaultStatusText(statusCode);
}

std::string_view defaultErrorCode(std::uint16_t statusCode) noexcept {
    switch (statusCode) {
        case 400:
            return "bad_request";
        case 401:
            return "unauthorized";
        case 403:
            return "forbidden";
        case 404:
            return "not_found";
        case 405:
            return "method_not_allowed";
        case 409:
            return "conflict";
        case 412:
            return "precondition_failed";
        case 413:
            return "payload_too_large";
        case 416:
            return "range_not_satisfiable";
        case 417:
            return "expectation_failed";
        case 418:
            return "teapot";
        case 422:
            return "unprocessable_entity";
        case 429:
            return "too_many_requests";
        case 431:
            return "request_header_fields_too_large";
        case 500:
            return "internal_error";
        case 501:
            return "not_implemented";
        case 503:
            return "service_unavailable";
        case 505:
            return "http_version_not_supported";
        default:
            return statusCode >= 500 ? "internal_error" : "bad_request";
    }
}

HttpResponse makeErrorResponse(
    std::pmr::memory_resource* resource,
    HttpErrorInfo error,
    bool closeConnection) {
    error = normalizeError(error);

    HttpResponse response(resource);
    detail::reserveResponseHeaders(response, closeConnection ? 2 : 1);
    response.status(error.status(), error.statusText());
    detail::setResponseHeaderStableView(response, "Content-Type", "application/json");
    if (closeConnection) {
        detail::setResponseHeaderStableView(response, "Connection", "close");
    }

    if (isDefaultErrorBodyCandidate(error)) {
        if (const auto body = defaultErrorBody(error.status()); !body.empty()) {
            detail::setResponseBodyStaticView(response, body);
            return response;
        }
    }

    std::pmr::string body(resource);
    appendErrorBody(body, error);
    detail::setResponseBodyOwned(response, std::move(body));
    return response;
}

Task<HttpResponse> makeErrorResponse(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection,
    HttpErrorHandler handler) {
    error = normalizeError(error);

    if (handler != nullptr) {
        try {
            auto response = co_await handler(context, error);
            if (closeConnection) {
                detail::setResponseHeaderStableView(response, "Connection", "close");
            }
            co_return response;
        } catch (const HttpError& nested) {
            co_return makeErrorResponse(context.resource(), nested.info(), closeConnection);
        } catch (const std::exception& nested) {
            co_return makeErrorResponse(
                context.resource(),
                HttpErrorInfo(500, "error_handler_failed", nested.what()),
                closeConnection);
        } catch (...) {
            co_return makeErrorResponse(
                context.resource(),
                HttpErrorInfo(500, "error_handler_failed", "error handler failed"),
                closeConnection);
        }
    }

    co_return makeErrorResponse(context.resource(), error, closeConnection);
}

}  // namespace ruvia
