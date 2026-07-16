#pragma once

#include "ruvia/web/detail/BorrowedView.h"

#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia {

// Non-owning Web application error metadata used by Context and custom error
// handlers. Every text field must outlive this view; basic_string rvalues are
// rejected at each position. The JSON error envelope is a framework product
// concern, not an HTTP protocol primitive, so this type belongs to ruvia-web.
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

    template <detail::RvalueCharBasicString String>
    HttpErrorInfo(
        std::uint16_t,
        String&&,
        std::string_view = {},
        std::string_view = {},
        std::string_view = {}) = delete;

    template <detail::RvalueCharBasicString String>
    HttpErrorInfo(
        std::uint16_t,
        std::string_view,
        String&&,
        std::string_view = {},
        std::string_view = {}) = delete;

    template <detail::RvalueCharBasicString String>
    HttpErrorInfo(
        std::uint16_t,
        std::string_view,
        std::string_view,
        String&&,
        std::string_view = {}) = delete;

    template <detail::RvalueCharBasicString String>
    HttpErrorInfo(
        std::uint16_t,
        std::string_view,
        std::string_view,
        std::string_view,
        String&&) = delete;

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

class HttpError final : public std::exception {
public:
    HttpError(
        std::uint16_t status,
        std::string_view code,
        std::string_view message,
        std::string_view statusText = {});
    HttpError(const HttpError& other);
    HttpError& operator=(const HttpError& other);
    HttpError(HttpError&&) noexcept = default;
    HttpError& operator=(HttpError&&) noexcept = default;

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] HttpErrorInfo info() const & noexcept;
    [[nodiscard]] HttpErrorInfo info() const && = delete;

private:
    std::uint16_t status_{500};
    std::pmr::string statusText_;
    std::pmr::string code_;
    std::pmr::string message_;
};

[[nodiscard]] std::string_view defaultErrorCode(std::uint16_t status) noexcept;

}  // namespace ruvia
