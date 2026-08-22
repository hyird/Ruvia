#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/BorrowedText.h"

namespace ruvia {

class ValidationError;
class Validator;

namespace detail {
struct ValidationIssueAccess;
}

struct ValidationIssueOptions final {
    BorrowedText field;
    BorrowedText code;
    BorrowedText message;
    std::pmr::memory_resource* resource{nullptr};
};

class ValidationIssue final {
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
    friend class ValidationError;
    friend class Validator;
    friend struct detail::ValidationIssueAccess;

    explicit ValidationIssue(ValidationIssueOptions options)
        : ValidationIssue(detail::ResolvedPmrResourceTag{}, options.field.view(), options.code.view(), options.message.view(), detail::pmrResourceOrDefault(options.resource)) {}

    ValidationIssue(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : field_(resource),
          code_(resource),
          message_(resource) {}

    ValidationIssue(detail::ResolvedPmrResourceTag, std::string_view fieldName, std::string_view codeValue, std::string_view messageValue, std::pmr::memory_resource* resource)
        : field_(fieldName, resource),
          code_(codeValue, resource),
          message_(messageValue, resource) {}

    std::pmr::string field_;
    std::pmr::string code_;
    std::pmr::string message_;
};

namespace detail {

struct ValidationIssueAccess final {
    [[nodiscard]] static ValidationIssue copy(const ValidationIssue& issue, std::pmr::memory_resource* resource) {
        return ValidationIssue({.field = issue.field(), .code = issue.code(), .message = issue.message(), .resource = resource});
    }
};

}  // namespace detail

}  // namespace ruvia
