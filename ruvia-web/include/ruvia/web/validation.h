#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/model/rule/numeric_comparison.h"
#include "ruvia/web/error.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation_types.h"

namespace ruvia {

namespace detail {

[[nodiscard]] inline std::pmr::vector<validation_issue> clone_validation_issues(
    std::span<const validation_issue> issues, std::pmr::memory_resource* resource) {
    // Empty containers can allocate debug iterator proxies. Use a constructor
    // that can propagate failure while the original diagnostics remain intact.
    std::pmr::vector<validation_issue> copied(std::initializer_list<validation_issue>{}, resource);
    const auto count = std::min(issues.size(), max_validation_issues);
    copied.reserve(count);
    for (const auto& issue : issues.first(count)) {
        copied.push_back(validation_issue_access::copy(issue, resource));
    }
    return copied;
}

template <typename t_type>
[[nodiscard]] std::string_view validation_string_view(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

}  // namespace detail

struct validation_error_options final {
    http_status_code status_{http_status::bad_request};
    borrowed_text code_{"validation_failed"};
    borrowed_text message_{"request validation failed"};
};

class validation_error final : public std::exception {
public:
    using issue_list_type = std::pmr::vector<validation_issue>;

    explicit validation_error(const issue_list_type& issues, validation_error_options options = {})
        : issues_(detail::clone_validation_issues(issues, process_resource())),
          status_code_(options.status_),
          code_(options.code_.view(), process_resource()),
          message_(options.message_.view(), process_resource()) {}

    validation_error(const validation_error& other)
        : issues_(detail::clone_validation_issues(other.issues_, process_resource())),
          status_code_(other.status_code_),
          code_(other.code_, process_resource()),
          message_(other.message_, process_resource()) {}

    validation_error& operator=(const validation_error& other) {
        if (this == &other) {
            return *this;
        }

        auto copied_issues = detail::clone_validation_issues(other.issues_, process_resource());
        std::pmr::string copied_code(other.code_, process_resource());
        std::pmr::string copied_message(other.message_, process_resource());
        issues_.swap(copied_issues);
        code_.swap(copied_code);
        message_.swap(copied_message);
        status_code_ = other.status_code_;
        return *this;
    }

    validation_error(validation_error&& other) noexcept
        : issues_(std::move(other.issues_)),
          status_code_(other.status_code_),
          code_(std::move(other.code_)),
          message_(std::move(other.message_)) {}

    validation_error& operator=(validation_error&& other) {
        if (this == &other) {
            return *this;
        }

        issues_ = std::move(other.issues_);
        status_code_ = other.status_code_;
        code_ = std::move(other.code_);
        message_ = std::move(other.message_);
        return *this;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return message_.c_str();
    }

    [[nodiscard]] const issue_list_type& issues() const& noexcept {
        return issues_;
    }
    [[nodiscard]] const issue_list_type& issues() const&& = delete;

    [[nodiscard]] http_error_info info() const& noexcept {
        return http_error_info({.status_ = status_code_,
            .code_ = code_,
            .message_ = message_,
            .validation_issues_ = issues_});
    }
    [[nodiscard]] http_error_info info() const&& = delete;

private:
    issue_list_type issues_{process_resource()};
    http_status_code status_code_{http_status::bad_request};
    std::pmr::string code_{process_resource()};
    std::pmr::string message_{process_resource()};
};

// Diagnostics belong to the configured PMR resource. Copy and move construction
// retain the source resource, whose owner must outlive every resulting validator.
// Assignment retains the destination resource, including nested diagnostic text;
// rebinding may allocate, and allocation failure leaves both validators unchanged.
// Moves within the same resource do not allocate. Successful moves leave an
// empty source that can be reused with its original resource.
class validator final {
public:
    using issue_list_type = validation_error::issue_list_type;

    struct options_type final {
        std::pmr::memory_resource* resource_{nullptr};
    };

    validator()
        : validator(options_type{}) {}

    explicit validator(options_type options)
        : issues_(std::initializer_list<validation_issue>{}, pmr_resource_or_default(options.resource_)) {}

    validator(const validator& other)
        : issues_(detail::clone_validation_issues(other.issues_, other.resource())) {}

    validator& operator=(const validator& other) {
        if (this != &other) {
            auto copied = detail::clone_validation_issues(other.issues_, resource());
            issues_.swap(copied);
        }
        return *this;
    }

    validator(validator&& other) noexcept
        : issues_(std::move(other.issues_)) {
        other.issues_.clear();
    }

    validator& operator=(validator&& other) {
        if (this != &other) {
            if (resource() == other.resource()) {
                issues_.swap(other.issues_);
            } else {
                auto copied = detail::clone_validation_issues(other.issues_, resource());
                issues_.swap(copied);
            }
            other.issues_.clear();
        }
        return *this;
    }

    ~validator() = default;

    [[nodiscard]] bool full() const noexcept {
        return issues_.size() >= max_validation_issues;
    }

    validator& add(std::string_view field, std::string_view code, std::string_view message) & {
        if (full()) {
            return *this;
        }
        issues_.push_back(validation_issue(
            {.field_ = field, .code_ = code, .message_ = message, .resource_ = resource()}));
        return *this;
    }

    template <typename optional_t_type>
    validator& required(const optional_t_type& value, std::string_view field,
        std::string_view message = "is required") & {
        if (!value) {
            add(field, "required", message);
        }
        return *this;
    }

    template <typename optional_t_type>
    validator& min_length(const optional_t_type& value, std::string_view field, std::size_t min_value,
        std::string_view message = "is too short") & {
        if (value && detail::validation_string_view(*value).size() < min_value) {
            add(field, "too_small", message);
        }
        return *this;
    }

    template <typename optional_t_type>
    validator& max_length(const optional_t_type& value, std::string_view field, std::size_t max_value,
        std::string_view message = "is too long") & {
        if (value && detail::validation_string_view(*value).size() > max_value) {
            add(field, "too_big", message);
        }
        return *this;
    }

    template <typename optional_t_type, typename min_t_type, typename max_t_type>
    validator& range(const optional_t_type& value, std::string_view field, min_t_type min_value, max_t_type max_value,
        std::string_view message = "is out of range") & {
        if (value) {
            if (detail::validation_less(*value, min_value)) {
                add(field, "too_small", message);
            } else if (detail::validation_greater(*value, max_value)) {
                add(field, "too_big", message);
            }
        }
        return *this;
    }

    template <typename optional_t_type>
    validator& one_of(const optional_t_type& value, std::string_view field,
        std::initializer_list<std::string_view> allowed,
        std::string_view message = "is not allowed") & {
        if (!value) {
            return *this;
        }

        const auto actual = detail::validation_string_view(*value);
        for (const auto option : allowed) {
            if (actual == option) {
                return *this;
            }
        }

        add(field, "one_of", message);
        return *this;
    }

    [[nodiscard]] bool ok() const noexcept {
        return issues_.empty();
    }

    [[nodiscard]] const issue_list_type& issues() const& noexcept {
        return issues_;
    }
    [[nodiscard]] const issue_list_type& issues() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return issues_.get_allocator().resource();
    }

    void throw_if_invalid(validation_error_options options = {}) const& {
        if (!ok()) {
            throw validation_error(issues_, options);
        }
    }

private:
    issue_list_type issues_;
};

}  // namespace ruvia

// RUVIA_MIN / RUVIA_MAX constrain a field's magnitude. For a number field this
// is its value; for a string field it is the UTF-8 BYTE length, not the
// codepoint count -- a three-emoji string is 12 bytes -- so choose bounds with
// multibyte input in mind (for example a minimum-length rule on free text).
#define RUVIA_MIN(value, message) \
    ::ruvia::detail::model::min<value, ::ruvia::fixed_string{message}>
#define RUVIA_MAX(value, message) \
    ::ruvia::detail::model::max<value, ::ruvia::fixed_string{message}>
#define RUVIA_ONE_OF(message, ...) \
    ::ruvia::detail::model::one_of<::ruvia::fixed_string{message}, __VA_ARGS__>
#define RUVIA_EMAIL(message) ::ruvia::detail::model::email<::ruvia::fixed_string{message}>
// Both pattern rules use anchored (^...$) byte patterns: literals, '.', positive
// or negative character classes, literal ranges, \d/\w/\s, and '*', '+', '?'.
// Escaped literals may be range endpoints; descending ranges and range endpoints
// such as \d are rejected at compile time. A trailing class '-' is a literal.
// Patterns are limited to 256 bytes and bounded matching work; RUVIA_REGEX also
// limits input to 4096 bytes. Exceeding a match limit fails validation. Use
// RUVIA_CUSTOM for matching syntax outside this dialect.
#define RUVIA_PATTERN(message, pattern) \
    ::ruvia::detail::model::pattern_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_REGEX(message, pattern) \
    ::ruvia::detail::model::regex_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_CUSTOM(message, predicate) \
    ::ruvia::detail::model::custom<predicate, ::ruvia::fixed_string{message}>
