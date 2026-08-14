#pragma once

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/ValidationIssue.h"

#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace ruvia {

namespace detail {

template <typename Range>
concept HttpTemporaryOwningValidationIssueRange =
    !std::is_lvalue_reference_v<Range&&> &&
    std::ranges::contiguous_range<Range> &&
    !std::ranges::borrowed_range<Range> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, ValidationIssue>;

}  // namespace detail

// Non-owning Web application error metadata used by Context and custom error
// handlers. Every borrowed value must outlive this view; basic_string rvalues
// and temporary owning validation-issue ranges are rejected. The JSON error
// envelope is a framework product concern, not an HTTP protocol primitive, so
// this type belongs to ruvia-web.
class HttpErrorInfo final {
public:
    constexpr HttpErrorInfo(HttpStatusCode status = http_status::kInternalServerError, std::string_view code = {}, std::string_view message = {}, std::string_view statusText = {}, std::span<const ValidationIssue> validationIssues = {}) noexcept
        : status_(status),
          statusText_(statusText),
          code_(code),
          message_(message),
          validationIssues_(validationIssues) {}

    template <detail::HttpTemporaryOwningCharString String>
    HttpErrorInfo(HttpStatusCode, String&&, std::string_view = {}, std::string_view = {}, std::span<const ValidationIssue> = {}) = delete;

    template <detail::HttpTemporaryOwningCharString String>
    HttpErrorInfo(HttpStatusCode, std::string_view, String&&, std::string_view = {}, std::span<const ValidationIssue> = {}) = delete;

    template <detail::HttpTemporaryOwningCharString String>
    HttpErrorInfo(HttpStatusCode, std::string_view, std::string_view, String&&, std::span<const ValidationIssue> = {}) = delete;

    template <detail::HttpTemporaryOwningValidationIssueRange Issues>
    HttpErrorInfo(HttpStatusCode, std::string_view, std::string_view, std::string_view, Issues&&) = delete;

    [[nodiscard]] constexpr HttpStatusCode status() const noexcept {
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

    [[nodiscard]] constexpr std::span<const ValidationIssue> validationIssues() const noexcept {
        return validationIssues_;
    }

private:
    HttpStatusCode status_{http_status::kInternalServerError};
    std::string_view statusText_{};
    std::string_view code_{};
    std::string_view message_{};
    std::span<const ValidationIssue> validationIssues_{};
};

class HttpError final : public std::exception {
public:
    HttpError(HttpStatusCode status, std::string_view code, std::string_view message, std::string_view statusText = {});
    HttpError(const HttpError& other);
    HttpError& operator=(const HttpError& other);
    HttpError(HttpError&&) noexcept = default;
    HttpError& operator=(HttpError&&) noexcept = default;

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] HttpErrorInfo info() const& noexcept;
    [[nodiscard]] HttpErrorInfo info() const&& = delete;

private:
    HttpStatusCode status_{http_status::kInternalServerError};
    std::pmr::string statusText_;
    std::pmr::string code_;
    std::pmr::string message_;
};

[[nodiscard]] std::string_view defaultErrorCode(HttpStatusCode status) noexcept;

}  // namespace ruvia
