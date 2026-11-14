#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "borrowed_text.h"
#include "callback_ref.h"
#include "http_response.h"
#include "http_status.h"
#include "move_only_function.h"
#include "task.h"

namespace ruvia {

class context;

class validator;

inline constexpr std::size_t max_validation_text_bytes = 1024;

namespace detail {
struct validation_issue_access;

[[nodiscard]] inline std::string_view bounded_validation_text(std::string_view value) noexcept {
    auto count = std::min(value.size(), max_validation_text_bytes);
    // Keep a UTF-8 prefix complete when the last code point crosses the bound.
    while (count < value.size() && count > 0 && (static_cast<unsigned char>(value[count]) & 0xc0U) == 0x80U) {
        --count;
    }
    return value.substr(0, count);
}
}  // namespace detail

struct validation_issue_options final {
    borrowed_text field_{};
    borrowed_text code_{};
    borrowed_text message_{};
};

class validation_issue final {
public:
    validation_issue(const validation_issue&) = default;
    validation_issue(validation_issue&&) noexcept = default;
    validation_issue& operator=(const validation_issue& other);
    validation_issue& operator=(validation_issue&& other) noexcept;

    [[nodiscard]] std::string_view field() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_;
    }
    [[nodiscard]] std::string_view field() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

private:
    friend class validator;
    friend struct detail::validation_issue_access;

    void swap(validation_issue& other) noexcept;

    explicit validation_issue(validation_issue_options options) :
        field_(detail::bounded_validation_text(options.field_.view())),
        code_(detail::bounded_validation_text(options.code_.view())),
        message_(detail::bounded_validation_text(options.message_.view())) {}

    std::string field_;
    std::string code_;
    std::string message_;
};
using borrowed_validation_issues = borrowed_span<validation_issue>;

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
    constexpr explicit http_error_info(http_error_info_options options = {}) noexcept :
        status_(options.status_),
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

// The owning Web application error: status, code, message, status text and
// validation issues are deep-copied from the options.
class http_error final : public std::exception {
public:
    explicit http_error(http_error_info_options options);
    http_error(const http_error&) = default;
    http_error& operator=(const http_error& other);
    http_error(http_error&&) noexcept = default;
    http_error& operator=(http_error&&) noexcept = default;
    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] http_error_info info() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] http_error_info info() const&& = delete;

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view status_text() const& noexcept RUVIA_LIFETIMEBOUND {
        return status_text_;
    }
    [[nodiscard]] std::string_view status_text() const&& = delete;

    [[nodiscard]] std::string_view code() const& noexcept RUVIA_LIFETIMEBOUND {
        return code_;
    }
    [[nodiscard]] std::string_view code() const&& = delete;

    [[nodiscard]] std::string_view message() const& noexcept RUVIA_LIFETIMEBOUND {
        return message_;
    }
    [[nodiscard]] std::string_view message() const&& = delete;

    [[nodiscard]] std::span<const validation_issue> validation_issues() const& noexcept RUVIA_LIFETIMEBOUND {
        return validation_issues_;
    }
    [[nodiscard]] std::span<const validation_issue> validation_issues() const&& = delete;

private:
    friend class validator;
    http_error(http_status_code status, std::string_view code, std::string_view message, std::string_view status_text, std::vector<validation_issue>&& issues);
    http_status_code status_{http_status::internal_server_error};
    std::string status_text_;
    std::string code_;
    std::string message_;
    std::vector<validation_issue> validation_issues_;
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
using http_error_handler_ref = callback_ref<task<http_response>(context&, http_error_info)>;
using http_not_found_handler_ref = callback_ref<task<http_response>(context&)>;
}  // namespace detail

// Answers a request that failed. Accepts a plain function -- on_error(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
// 空函数指针构造空处理器；未配置时不得调用。所有权与退役语义同 move_only_function。
using http_error_handler = move_only_function<task<http_response>(context&, http_error_info)>;

// Answers a request that matched no route.
// 空函数指针与目标析构重入的契约同 http_error_handler。
using http_not_found_handler = move_only_function<task<http_response>(context&)>;

struct scoped_error_handler_options final {
    borrowed_text prefix_{};
    http_error_handler handler_{nullptr};
};

struct scoped_not_found_handler_options final {
    borrowed_text prefix_{};
    http_not_found_handler handler_{nullptr};
};

}  // namespace ruvia
