#pragma once

#include "ruvia/http/BorrowedText.h"
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

class BorrowedValidationIssues final {
public:
    constexpr BorrowedValidationIssues() noexcept = default;

    constexpr BorrowedValidationIssues(std::span<const ValidationIssue> issues) noexcept
        : issues_(issues) {}

    template <typename Range>
        requires std::is_lvalue_reference_v<Range&&> &&
        std::ranges::contiguous_range<Range> &&
        std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, ValidationIssue>
    constexpr BorrowedValidationIssues(Range&& issues) noexcept
        : issues_(std::ranges::data(issues), std::ranges::size(issues)) {}

    template <detail::HttpTemporaryOwningValidationIssueRange Issues>
    BorrowedValidationIssues(Issues&&) = delete;

    [[nodiscard]] constexpr std::span<const ValidationIssue> view() const noexcept {
        return issues_;
    }

    [[nodiscard]] constexpr operator std::span<const ValidationIssue>() const noexcept {
        return issues_;
    }

private:
    std::span<const ValidationIssue> issues_{};
};

struct HttpErrorInfoOptions final {
    HttpStatusCode status{http_status::kInternalServerError};
    BorrowedText code;
    BorrowedText message;
    BorrowedText statusText;
    BorrowedValidationIssues validationIssues;
};

// Non-owning Web application error metadata used by Context and custom error
// handlers. Every borrowed value must outlive this view; basic_string rvalues
// and temporary owning validation-issue ranges are rejected. The JSON error
// envelope is a framework product concern, not an HTTP protocol primitive, so
// this type belongs to ruvia-web.
class HttpErrorInfo final {
public:
    constexpr explicit HttpErrorInfo(HttpErrorInfoOptions options = {}) noexcept
        : status_(options.status),
          statusText_(options.statusText.view()),
          code_(options.code.view()),
          message_(options.message.view()),
          validationIssues_(options.validationIssues.view()) {}

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
    explicit HttpError(HttpErrorInfoOptions options);
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
