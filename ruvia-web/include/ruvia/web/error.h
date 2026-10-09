#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/validation_issue.h"

namespace ruvia {

namespace detail {

template <typename range_type>
concept http_temporary_owning_validation_issue_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, validation_issue>;

}  // namespace detail

class borrowed_validation_issues final {
public:
    constexpr borrowed_validation_issues() noexcept = default;

    constexpr borrowed_validation_issues(std::span<const validation_issue> issues) noexcept
        : issues_(issues) {}

    template <typename range_type>
        requires std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
                 std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, validation_issue>
    constexpr borrowed_validation_issues(range_type&& issues) noexcept
        : issues_(std::ranges::data(issues), std::ranges::size(issues)) {}

    template <detail::http_temporary_owning_validation_issue_range issues_type>
    borrowed_validation_issues(issues_type&&) = delete;

    [[nodiscard]] constexpr std::span<const validation_issue> view() const noexcept {
        return issues_;
    }

    [[nodiscard]] constexpr operator std::span<const validation_issue>() const noexcept {
        return issues_;
    }

private:
    std::span<const validation_issue> issues_{};
};

struct http_error_info_options final {
    http_status_code status_{http_status::internal_server_error};
    borrowed_text code_{};
    borrowed_text message_{};
    borrowed_text status_text_{};
    borrowed_validation_issues validation_issues_{};
};

// Non-owning Web application error metadata used by context and custom error
// handlers. Every borrowed value must outlive this view; basic_string rvalues
// and temporary owning validation-issue ranges are rejected. The JSON error
// envelope is a framework product concern, not an HTTP protocol primitive, so
// this type belongs to ruvia-web.
class http_error_info final {
public:
    constexpr explicit http_error_info(http_error_info_options options = {}) noexcept
        : status_(options.status_),
          status_text_(options.status_text_.view()),
          code_(options.code_.view()),
          message_(options.message_.view()),
          validation_issues_(options.validation_issues_.view()) {}

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::string_view status_text() const noexcept {
        return status_text_;
    }

    [[nodiscard]] constexpr std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] constexpr std::string_view message() const noexcept {
        return message_;
    }

    [[nodiscard]] constexpr std::span<const validation_issue> validation_issues() const noexcept {
        return validation_issues_;
    }

private:
    http_status_code status_{http_status::internal_server_error};
    std::string_view status_text_{};
    std::string_view code_{};
    std::string_view message_{};
    std::span<const validation_issue> validation_issues_{};
};

class http_error final : public std::exception {
public:
    explicit http_error(http_error_info_options options);
    http_error(const http_error& other);
    http_error& operator=(const http_error& other);
    http_error(http_error&&) noexcept = default;
    http_error& operator=(http_error&&) noexcept = default;

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] http_error_info info() const& noexcept;
    [[nodiscard]] http_error_info info() const&& = delete;

private:
    http_status_code status_{http_status::internal_server_error};
    std::pmr::string status_text_;
    std::pmr::string code_;
    std::pmr::string message_;
};

[[nodiscard]] std::string_view default_error_code(http_status_code status) noexcept;

}  // namespace ruvia
