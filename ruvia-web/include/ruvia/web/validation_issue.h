#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/borrowed_text.h"

namespace ruvia {

inline constexpr std::size_t max_validation_issues = 64;
inline constexpr std::size_t max_validation_text_bytes = 1024;

class validation_error;
class validator;

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
        : validation_issue(detail::resolved_pmr_resource_tag{}, options.field_.view(),
              options.code_.view(), options.message_.view(),
              detail::pmr_resource_or_default(options.resource_)) {}

    validation_issue(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : field_(resource),
          code_(resource),
          message_(resource) {}

    validation_issue(detail::resolved_pmr_resource_tag, std::string_view field_name,
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

}  // namespace ruvia
