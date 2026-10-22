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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/model/rule/numeric_comparison.h"
#include "ruvia/web/detail/model/rule/rules.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/error.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"

namespace ruvia {

enum class validation_target : std::uint8_t { json,
    form,
    query,
    param,
    header,
    cookie };
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

// How validator::throw_if_invalid presents its issues: the http_error it throws
// carries these values together with the collected validation issues.
struct validation_failure_options final {
    http_status_code status_{http_status::bad_request};
    borrowed_text code_{"validation_failed"};
    borrowed_text message_{"request validation failed"};
};

// Diagnostics belong to the configured PMR resource. Copy and move construction
// retain the source resource, whose owner must outlive every resulting validator.
// Assignment retains the destination resource, including nested diagnostic text;
// rebinding may allocate, and allocation failure leaves both validators unchanged.
// Moves within the same resource do not allocate. Successful moves leave an
// empty source that can be reused with its original resource.
class validator final {
public:
    using issue_list_type = std::pmr::vector<validation_issue>;

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

    template <typename optional_type>
    validator& required(const optional_type& value, std::string_view field,
        std::string_view message = "is required") & {
        if (!value) {
            add(field, "required", message);
        }
        return *this;
    }

    template <typename optional_type>
    validator& min_length(const optional_type& value, std::string_view field, std::size_t min_value,
        std::string_view message = "is too short") & {
        if (value && detail::validation_string_view(*value).size() < min_value) {
            add(field, "too_small", message);
        }
        return *this;
    }

    template <typename optional_type>
    validator& max_length(const optional_type& value, std::string_view field, std::size_t max_value,
        std::string_view message = "is too long") & {
        if (value && detail::validation_string_view(*value).size() > max_value) {
            add(field, "too_big", message);
        }
        return *this;
    }

    template <typename optional_type, typename min_type, typename max_type>
    validator& range(const optional_type& value, std::string_view field, min_type min_value, max_type max_value,
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

    template <typename optional_type>
    validator& one_of(const optional_type& value, std::string_view field,
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

    // Throws an http_error owning copies of the issues in the process resource,
    // so the exception outlives this validator and its resource.
    void throw_if_invalid(validation_failure_options options = {}) const& {
        if (!ok()) {
            throw http_error({.status_ = options.status_,
                .code_ = options.code_,
                .message_ = options.message_,
                .validation_issues_ = issues_});
        }
    }

private:
    issue_list_type issues_;
};

template <typename model_type>
    requires detail::is_model<model_type>
void validate_model(const model_type& model, validator& out) {
    detail::model_validation_access::validate_model(model, out);
}

template <typename t_type>
    requires detail::is_model<t_type>
[[nodiscard]] t_type from_form(std::string_view body, model_parse_options options = {}) {
    auto* const resource = pmr_resource_or_default(options.resource_);
    auto parsed_value = detail::model_parse_access::parse_form_owned<t_type>(body, resource);
    if (!parsed_value) {
        throw std::invalid_argument("invalid form value");
    }
    validator validator_value({.resource_ = resource});
    validate_model(*parsed_value, validator_value);
    validator_value.throw_if_invalid();
    return std::move(*parsed_value);
}

// Owns parsed data in options.resource. Supports models, Ruvia scalar values,
// strings, bytes and arrays of these types. Model types also evaluate their
// declared field validation rules.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] t_type from_json(std::string_view body, model_parse_options options = {}) {
    auto* const resource = pmr_resource_or_default(options.resource_);
    auto parsed_value = detail::parse_json_document<t_type>(
        body, resource, detail::model_string_storage::owned, options);
    if (!parsed_value) {
        throw std::invalid_argument("invalid JSON value");
    }
    if constexpr (detail::model_validation_access::contains_model<t_type>()) {
        validator validator_value({.resource_ = resource});
        detail::model_validation_access::validate_value(*parsed_value, validator_value);
        validator_value.throw_if_invalid();
    }
    return std::move(*parsed_value);
}

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
// Pattern rules use anchored (^...$) byte patterns: literals, '.', positive
// or negative character classes, literal ranges, \d/\w/\s, and '*', '+', '?'.
// Escaped literals may be range endpoints; descending ranges and range endpoints
// such as \d are rejected at compile time. A trailing class '-' is a literal.
// Patterns are limited to 256 bytes and bounded matching work. Exceeding a
// match limit fails validation. Use RUVIA_CUSTOM for matching syntax outside
// this dialect.
#define RUVIA_PATTERN(message, pattern) \
    ::ruvia::detail::model::pattern_rule<pattern, ::ruvia::fixed_string{message}>
#define RUVIA_CUSTOM(message, predicate) \
    ::ruvia::detail::model::custom<predicate, ::ruvia::fixed_string{message}>

namespace ruvia {
class context;

namespace detail {

// One validating middleware per request source: parses the model from `target`,
// runs its declared rules, binds it for the handler, then continues the chain.
template <validation_target target, typename body_type>
class model_validation_middleware final {
public:
    static_assert(is_model<body_type>, "request validation middleware requires a RUVIA_MODEL");
    static_assert(target != validation_target::header ||
                      model::unique_model_wire_names_ignore_ascii_case(model::model_access::schema<body_type>()),
        "header_model field names must be unique ignoring ASCII case");
    using ruvia_validation_body_type = body_type;

    void validate(const body_type& body, validator& validator_value) const {
        validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value);
};

}  // namespace detail

template <typename body_type>
using json_body = detail::model_validation_middleware<validation_target::json, body_type>;

template <typename body_type>
using form_body = detail::model_validation_middleware<validation_target::form, body_type>;

template <typename body_type>
using query_model = detail::model_validation_middleware<validation_target::query, body_type>;

template <typename body_type>
using path_model = detail::model_validation_middleware<validation_target::param, body_type>;

template <typename body_type>
using header_model = detail::model_validation_middleware<validation_target::header, body_type>;

template <typename body_type>
using cookie_model = detail::model_validation_middleware<validation_target::cookie, body_type>;
}  // namespace ruvia
