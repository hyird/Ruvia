#pragma once

#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/HttpTypes.h"

namespace ruvia {

class Context;
class HttpResponse;
template <typename T>
class Task;

class HttpErrorInfo final {
public:
    constexpr HttpErrorInfo(
        std::uint16_t status = 500,
        std::string_view code = {},
        std::string_view message = {},
        std::string_view statusText = {},
        std::string_view detailsJson = {}) noexcept
        : status_(status),
          statusText_(statusText),
          code_(code),
          message_(message),
          detailsJson_(detailsJson) {}

    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::string_view statusText() const noexcept {
        return statusText_;
    }

    [[nodiscard]] constexpr std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] constexpr std::string_view message() const noexcept {
        return message_;
    }

    [[nodiscard]] constexpr std::string_view detailsJson() const noexcept {
        return detailsJson_;
    }

private:
    std::uint16_t status_{500};
    std::string_view statusText_{};
    std::string_view code_{};
    std::string_view message_{};
    std::string_view detailsJson_{};
};

// DELIBERATE BOUNDARY ARTIFACT: hook types for the web framework's error /
// not-found handlers. They live next to HttpErrorInfo so the error surface is one
// public header, and they only require Context/HttpResponse forward declarations --
// ruvia::http itself never defines Context nor invokes these; only ruvia::web
// (Router::setErrorHandler / dispatch) does. A non-web product leaves them null.
using HttpErrorHandler = Task<HttpResponse> (*)(Context&, HttpErrorInfo);
using HttpNotFoundHandler = Task<HttpResponse> (*)(Context&);

class HttpError final : public std::exception {
public:
    HttpError(
        std::uint16_t statusCode,
        std::string_view code,
        std::string_view message,
        std::string_view statusText = {});

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] HttpErrorInfo info() const noexcept;

private:
    std::uint16_t statusCode_{500};
    std::pmr::string statusText_;
    std::pmr::string code_;
    std::pmr::string message_;
};

[[nodiscard]] std::string_view defaultStatusText(std::uint16_t statusCode) noexcept;
[[nodiscard]] std::string_view defaultErrorCode(std::uint16_t statusCode) noexcept;

[[nodiscard]] HttpResponse makeErrorResponse(
    std::pmr::memory_resource* resource,
    HttpErrorInfo error,
    bool closeConnection = false);

#ifdef RUVIA_ENABLE_WEB
[[nodiscard]] Task<HttpResponse> makeErrorResponse(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection,
    HttpErrorHandler handler);
#endif

}  // namespace ruvia
