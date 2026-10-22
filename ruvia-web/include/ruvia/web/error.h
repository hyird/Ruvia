#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/detail/callback.h"
#include "ruvia/web/detail/callback_ref.h"

namespace ruvia {

class context;
class validation_error;
class validator;

inline constexpr std::size_t max_validation_issues = 64;
inline constexpr std::size_t max_validation_text_bytes = 1024;

namespace detail {
struct validation_issue_access;

[[nodiscard]] inline std::string_view bounded_validation_text(std::string_view value) noexcept {
    auto count = std::min(value.size(), max_validation_text_bytes);
    // Keep a UTF-8 prefix complete when the last code point crosses the bound.
    while (count < value.size() && count > 0 &&
           (static_cast<unsigned char>(value[count]) & 0xc0U) == 0x80U) {
        --count;
    }
    return value.substr(0, count);
}
}  // namespace detail

struct validation_issue_options final {
    borrowed_text field_{};
    borrowed_text code_{};
    borrowed_text message_{};
    std::pmr::memory_resource* resource_{nullptr};
};

class validation_issue final {
public:
    [[nodiscard]] std::string_view field() const& noexcept {
        return field_;
    }
    [[nodiscard]] std::string_view field() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

private:
    friend class validation_error;
    friend class validator;
    friend struct detail::validation_issue_access;

    explicit validation_issue(validation_issue_options options)
        : validation_issue(resolved_pmr_resource_tag{}, options.field_.view(),
              options.code_.view(), options.message_.view(),
              pmr_resource_or_default(options.resource_)) {}

    validation_issue(resolved_pmr_resource_tag, std::string_view field_name,
        std::string_view code_value, std::string_view message_value,
        std::pmr::memory_resource* resource)
        : field_(detail::bounded_validation_text(field_name), resource),
          code_(detail::bounded_validation_text(code_value), resource),
          message_(detail::bounded_validation_text(message_value), resource) {}

    std::pmr::string field_;
    std::pmr::string code_;
    std::pmr::string message_;
};

namespace detail {

struct validation_issue_access final {
    [[nodiscard]] static validation_issue copy(
        const validation_issue& issue, std::pmr::memory_resource* resource) {
        return validation_issue({.field_ = issue.field(),
            .code_ = issue.code(),
            .message_ = issue.message(),
            .resource_ = resource});
    }
};

}  // namespace detail
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

// UTF-8 text describing any exception, for logging a failure such as the one
// application::run rethrows. On Windows, what() of a std::system_error in the
// system or Asio category carries the OS message in the ANSI code page; that
// message is replaced with its UTF-8 form. Any other std::exception yields
// what(), a non-standard exception a fixed description, and null an empty
// string.
[[nodiscard]] std::string exception_text(const std::exception_ptr& exception);

namespace detail {
using http_error_handler_ref_type = callback_ref_type<task<http_response>(context&, http_error_info)>;
using http_not_found_handler_ref_type = callback_ref_type<task<http_response>(context&)>;
}  // namespace detail

// Answers a request that failed. Accepts a plain function -- on_error(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
using http_error_handler_type = detail::callback<task<http_response>(context&, http_error_info)>;

// Answers a request that matched no route.
using http_not_found_handler_type = detail::callback<task<http_response>(context&)>;

struct scoped_error_handler_options final {
    std::string prefix_{};
    http_error_handler_type handler_{nullptr};
};

struct scoped_not_found_handler_options final {
    std::string prefix_{};
    http_not_found_handler_type handler_{nullptr};
};

static_assert(sizeof(http_error_handler_type) == 5 * sizeof(void*));
static_assert(sizeof(http_not_found_handler_type) == 5 * sizeof(void*));

}  // namespace ruvia
