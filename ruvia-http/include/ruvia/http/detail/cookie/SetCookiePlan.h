#pragma once

#include "ruvia/http/Cookies.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

// Validates and fixes the exact Set-Cookie field-value shape before a runtime
// allocates its output buffer. The plan borrows name, value, path and domain
// until write() completes, so owning-string and CookieOptions temporaries are
// rejected at construction. Construction throws std::length_error if the wire
// length cannot be represented by std::size_t.
class SetCookiePlan final {
public:
    SetCookiePlan(std::string_view name, std::string_view value, const CookieOptions& options);

    template <typename Name, typename Value>
        requires(HttpTemporaryOwningCharString<Name> || HttpTemporaryOwningCharString<Value>)
    SetCookiePlan(Name&&, Value&&, const CookieOptions&) = delete;

    SetCookiePlan(std::string_view, std::string_view, CookieOptions&&) = delete;
    SetCookiePlan(std::string_view, std::string_view, const CookieOptions&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view wirePrefix() const noexcept {
        return prefixText_;
    }

    void write(char* output) const;

private:
    std::array<char, 32> expiresBuffer_{};
    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::string_view prefixText_;
    std::string_view priorityText_;
    std::string_view sameSiteText_;
    std::uint64_t maxAgeValue_{0};
    std::size_t expiresSize_{0};
    std::size_t maxAgeSize_{0};
    std::size_t size_{0};
    bool hasMaxAge_{false};
    bool httpOnly_{false};
    bool secure_{false};
    bool partitioned_{false};
};

}  // namespace ruvia::detail
