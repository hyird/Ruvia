#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/BorrowedText.h"

namespace ruvia {

inline constexpr std::size_t max_validation_issues = 64;
inline constexpr std::size_t max_validation_text_bytes = 1024;

class ValidationError;
class Validator;

namespace detail {
struct ValidationIssueAccess;

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

struct ValidationIssueOptions final {
    BorrowedText field{};
    BorrowedText code{};
    BorrowedText message{};
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
        : ValidationIssue(detail::ResolvedPmrResourceTag{}, options.field.view(),
              options.code.view(), options.message.view(),
              detail::pmrResourceOrDefault(options.resource)) {}

    ValidationIssue(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : field_(resource),
          code_(resource),
          message_(resource) {}

    ValidationIssue(detail::ResolvedPmrResourceTag, std::string_view fieldName,
        std::string_view codeValue, std::string_view messageValue,
        std::pmr::memory_resource* resource)
        : field_(detail::bounded_validation_text(fieldName), resource),
          code_(detail::bounded_validation_text(codeValue), resource),
          message_(detail::bounded_validation_text(messageValue), resource) {}

    std::pmr::string field_;
    std::pmr::string code_;
    std::pmr::string message_;
};

namespace detail {

struct ValidationIssueAccess final {
    [[nodiscard]] static ValidationIssue copy(
        const ValidationIssue& issue, std::pmr::memory_resource* resource) {
        return ValidationIssue({.field = issue.field(),
            .code = issue.code(),
            .message = issue.message(),
            .resource = resource});
    }
};

}  // namespace detail

}  // namespace ruvia
