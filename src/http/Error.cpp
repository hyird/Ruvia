#include "ruvia/http/Error.h"

#include "ruvia/http/Context.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/JsonUtils.h"

namespace ruvia {
namespace {

[[nodiscard]] HttpErrorInfo normalizeError(HttpErrorInfo error) noexcept {
    if (error.statusText.empty()) {
        error.statusText = defaultStatusText(error.statusCode);
    }
    if (error.code.empty()) {
        error.code = defaultErrorCode(error.statusCode);
    }
    if (error.message.empty()) {
        error.message = error.statusText;
    }
    return error;
}

void appendErrorBody(std::pmr::string& body, HttpErrorInfo error) {
    body.append("{\"error\":");
    detail::appendJsonString(body, error.statusText);
    body.append(",\"code\":");
    detail::appendJsonString(body, error.code);
    body.append(",\"message\":");
    detail::appendJsonString(body, error.message);
    if (!error.detailsJson.empty()) {
        body.append(",\"details\":");
        body.append(error.detailsJson.data(), error.detailsJson.size());
    }
    body.push_back('}');
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
    return HttpErrorInfo{
        .statusCode = statusCode_,
        .statusText = statusText_,
        .code = code_,
        .message = message_};
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
    response.reserveHeaders(closeConnection ? 2 : 1);
    response.setStatus(error.statusCode, error.statusText);
    response.setHeader("Content-Type", "application/json; charset=utf-8");
    if (closeConnection) {
        response.setHeader("Connection", "close");
    }

    std::pmr::string body(resource);
    appendErrorBody(body, error);
    response.setBody(std::move(body));
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
                response.setHeader("Connection", "close");
            }
            co_return response;
        } catch (const HttpError& nested) {
            co_return makeErrorResponse(context.resource(), nested.info(), closeConnection);
        } catch (const std::exception& nested) {
            co_return makeErrorResponse(
                context.resource(),
                HttpErrorInfo{
                    .statusCode = 500,
                    .code = "error_handler_failed",
                    .message = nested.what()},
                closeConnection);
        } catch (...) {
            co_return makeErrorResponse(
                context.resource(),
                HttpErrorInfo{
                    .statusCode = 500,
                    .code = "error_handler_failed",
                    .message = "error handler failed"},
                closeConnection);
        }
    }

    co_return makeErrorResponse(context.resource(), error, closeConnection);
}

}  // namespace ruvia
