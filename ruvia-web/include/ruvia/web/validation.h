#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/error.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation_types.h"

namespace ruvia {

namespace detail {

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
        : issues_(copy_issues(issues)),
          status_code_(options.status_),
          code_(options.code_.view(), detail::process_resource()),
          message_(options.message_.view(), detail::process_resource()) {}

    explicit validation_error(issue_list_type&& issues, validation_error_options options = {})
        : validation_error(static_cast<const issue_list_type&>(issues), options) {}

    validation_error(const validation_error& other)
        : issues_(copy_issues(other.issues_)),
          status_code_(other.status_code_),
          code_(other.code_, detail::process_resource()),
          message_(other.message_, detail::process_resource()) {}

    validation_error& operator=(const validation_error& other) {
        if (this == &other) {
            return *this;
        }

        auto copied_issues = copy_issues(other.issues_);
        std::pmr::string copied_code(other.code_, detail::process_resource());
        std::pmr::string copied_message(other.message_, detail::process_resource());
        issues_ = std::move(copied_issues);
        code_ = std::move(copied_code);
        message_ = std::move(copied_message);
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
    [[nodiscard]] static issue_list_type copy_issues(const issue_list_type& issues) {
        issue_list_type copied(detail::process_resource());
        const auto count = std::min(issues.size(), max_validation_issues);
        copied.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& issue = issues[index];
            copied.push_back(detail::validation_issue_access::copy(
                issue, detail::process_resource()));
        }
        return copied;
    }

    issue_list_type issues_{detail::process_resource()};
    http_status_code status_code_{http_status::bad_request};
    std::pmr::string code_{detail::process_resource()};
    std::pmr::string message_{detail::process_resource()};
};

class validator final {
public:
    using issue_list_type = validation_error::issue_list_type;

    struct options_type final {
        std::pmr::memory_resource* resource_{nullptr};
    };

    validator()
        : validator(options_type{}) {}

    explicit validator(options_type options)
        : resource_(detail::pmr_resource_or_default(options.resource_)),
          issues_(resource_) {}

    [[nodiscard]] bool full() const noexcept {
        return issues_.size() >= max_validation_issues;
    }

    validator& add(std::string_view field, std::string_view code, std::string_view message) & {
        if (full()) {
            return *this;
        }
        issues_.push_back(validation_issue(
            {.field_ = field, .code_ = code, .message_ = message, .resource_ = resource_}));
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
            if (*value < min_value) {
                add(field, "too_small", message);
            } else if (*value > max_value) {
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
        return resource_;
    }

    void throw_if_invalid(validation_error_options options = {}) const& {
        if (!ok()) {
            throw validation_error(issues_, options);
        }
    }

    void throw_if_invalid(validation_error_options options = {}) && {
        if (!ok()) {
            throw validation_error(std::move(issues_), options);
        }
    }

private:
    std::pmr::memory_resource* resource_;
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
#define RUVIA_PATTERN(message, pattern) \
    ::ruvia::detail::model::pattern_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_REGEX(message, pattern) \
    ::ruvia::detail::model::regex_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_CUSTOM(message, predicate) \
    ::ruvia::detail::model::custom<predicate, ::ruvia::fixed_string{message}>
