#pragma once

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

#include "ruvia/web/Error.h"
#include "ruvia/web/Model.h"
#include "ruvia/web/ValidationTypes.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {

namespace detail {

template <typename T>
[[nodiscard]] std::string_view validationStringView(const T& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

}  // namespace detail

class ValidationError final : public std::exception {
public:
    using IssueList = std::pmr::vector<ValidationIssue>;

    explicit ValidationError(const IssueList& issues, HttpStatusCode statusCode = http_status::kBadRequest, std::string_view code = "validation_failed", std::string_view message = "request validation failed", std::pmr::memory_resource* resource = nullptr)
        : resource_(detail::pmrResourceOrDefault(resource)),
          issues_(resource_),
          statusCode_(statusCode),
          code_(code, resource_),
          message_(message, resource_) {
        issues_.reserve(issues.size());
        for (const auto& issue : issues) {
            issues_.push_back(ValidationIssue(issue.field(), issue.code(), issue.message(), resource_));
        }
    }

    explicit ValidationError(IssueList&& issues, HttpStatusCode statusCode = http_status::kBadRequest, std::string_view code = "validation_failed", std::string_view message = "request validation failed", std::pmr::memory_resource* resource = nullptr)
        : resource_(detail::pmrResourceOrDefault(resource)),
          issues_(std::move(issues), resource_),
          statusCode_(statusCode),
          code_(code, resource_),
          message_(message, resource_) {}

    [[nodiscard]] const char* what() const noexcept override {
        return message_.c_str();
    }

    [[nodiscard]] const IssueList& issues() const& noexcept {
        return issues_;
    }
    [[nodiscard]] const IssueList& issues() const&& = delete;

    [[nodiscard]] HttpErrorInfo info() const& noexcept {
        return HttpErrorInfo(statusCode_, code_, message_, {}, issues_);
    }
    [[nodiscard]] HttpErrorInfo info() const&& = delete;

private:
    std::pmr::memory_resource* resource_;
    IssueList issues_;
    HttpStatusCode statusCode_{http_status::kBadRequest};
    std::pmr::string code_;
    std::pmr::string message_;
};

class Validator final {
public:
    using IssueList = ValidationError::IssueList;

    explicit Validator(std::pmr::memory_resource* resource = nullptr)
        : resource_(detail::pmrResourceOrDefault(resource)),
          issues_(resource_) {}

    Validator& add(std::string_view field, std::string_view code, std::string_view message) & {
        issues_.push_back(ValidationIssue(field, code, message, resource_));
        return *this;
    }

    template <typename OptionalT>
    Validator& required(const OptionalT& value, std::string_view field, std::string_view message = "is required") & {
        if (!value) {
            add(field, "required", message);
        }
        return *this;
    }

    template <typename OptionalT>
    Validator& minLength(const OptionalT& value, std::string_view field, std::size_t min, std::string_view message = "is too short") & {
        if (value && detail::validationStringView(*value).size() < min) {
            add(field, "too_small", message);
        }
        return *this;
    }

    template <typename OptionalT>
    Validator& maxLength(const OptionalT& value, std::string_view field, std::size_t max, std::string_view message = "is too long") & {
        if (value && detail::validationStringView(*value).size() > max) {
            add(field, "too_big", message);
        }
        return *this;
    }

    template <typename OptionalT, typename MinT, typename MaxT>
    Validator& range(const OptionalT& value, std::string_view field, MinT min, MaxT max, std::string_view message = "is out of range") & {
        if (value) {
            if (*value < min) {
                add(field, "too_small", message);
            } else if (*value > max) {
                add(field, "too_big", message);
            }
        }
        return *this;
    }

    template <typename OptionalT>
    Validator& oneOf(const OptionalT& value, std::string_view field, std::initializer_list<std::string_view> allowed, std::string_view message = "is not allowed") & {
        if (!value) {
            return *this;
        }

        const auto actual = detail::validationStringView(*value);
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

    [[nodiscard]] const IssueList& issues() const& noexcept {
        return issues_;
    }
    [[nodiscard]] const IssueList& issues() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void throwIfInvalid(HttpStatusCode statusCode = http_status::kBadRequest, std::string_view code = "validation_failed", std::string_view message = "request validation failed") const& {
        if (!ok()) {
            throw ValidationError(issues_, statusCode, code, message, resource_);
        }
    }

    void throwIfInvalid(HttpStatusCode statusCode = http_status::kBadRequest, std::string_view code = "validation_failed", std::string_view message = "request validation failed") && {
        if (!ok()) {
            throw ValidationError(std::move(issues_), statusCode, code, message, resource_);
        }
    }

private:
    std::pmr::memory_resource* resource_;
    IssueList issues_;
};

}  // namespace ruvia

#define RUVIA_REQUIRED(message)        \
    ::ruvia::detail::model::Required { \
        message                        \
    }
// RUVIA_MIN / RUVIA_MAX constrain a field's magnitude. For a number field this
// is its value; for a string field it is the UTF-8 BYTE length, not the
// codepoint count -- a three-emoji string is 12 bytes -- so choose bounds with
// multibyte input in mind (for example a minimum-length rule on free text).
#define RUVIA_MIN(value, message)                \
    ::ruvia::detail::model::Min {                \
        static_cast<long double>(value), message \
    }
#define RUVIA_MAX(value, message)                \
    ::ruvia::detail::model::Max {                \
        static_cast<long double>(value), message \
    }
#define RUVIA_ONE_OF(message, ...)               \
    ::ruvia::detail::model::OneOf<__VA_ARGS__> { \
        message                                  \
    }
#define RUVIA_EMAIL(message)        \
    ::ruvia::detail::model::Email { \
        message                     \
    }
#define RUVIA_PATTERN(message, pattern)            \
    ::ruvia::detail::model::PatternRule<pattern> { \
        message                                    \
    }
#define RUVIA_REGEX(message, pattern)            \
    ::ruvia::detail::model::RegexRule<pattern> { \
        message                                  \
    }
#define RUVIA_CUSTOM(message, predicate) \
    ::ruvia::detail::model::Custom {     \
        message, predicate               \
    }
#define RUVIA_NESTED(validator_type) \
    ::ruvia::detail::model::Nested<validator_type> {}
#define RUVIA_EACH(validator_type) \
    ::ruvia::detail::model::Each<validator_type> {}

#define RUVIA_RULE(field, ...) (field, (#field), (::ruvia::detail::model::Rules{__VA_ARGS__}))
#define RUVIA_RULE_NAME(wire_name, field, ...) (field, (wire_name), (::ruvia::detail::model::Rules{__VA_ARGS__}))

#define RUVIA_VALIDATE_RULE_FIELD(T, x) RUVIA_VALIDATE_RULE_FIELD_I(RUVIA_VALIDATION_UNPAREN x)
#define RUVIA_VALIDATE_RULE_FIELD_I(...) RUVIA_VALIDATE_RULE_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_VALIDATE_RULE_FIELD_IMPL(field, wire, rules)                                                                  \
    {                                                                                                                       \
        ::std::pmr::string ruviaPath(validator.resource());                                                                 \
        ::ruvia::detail::model::appendPath(ruviaPath, prefix, ::std::string_view{wire});                                    \
        const auto& ruviaValue = body.template get<#field>();                                                              \
        rules.validate(::ruvia::detail::ModelValidationAccess::fieldState<#field>(body), ruviaValue, ruviaPath, validator); \
    }
