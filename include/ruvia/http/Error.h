#pragma once

#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia {

class Context;

struct HttpErrorInfo {
    std::uint16_t statusCode{500};
    std::string_view statusText{};
    std::string_view code{};
    std::string_view message{};
    std::string_view detailsJson{};
};

using HttpErrorHandler = Task<HttpResponse> (*)(Context&, HttpErrorInfo);

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

[[nodiscard]] Task<HttpResponse> makeErrorResponse(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection,
    HttpErrorHandler handler);

}  // namespace ruvia
