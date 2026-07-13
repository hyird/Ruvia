#pragma once

#include "ruvia/http/Cookies.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// Validates and fixes the exact Set-Cookie field-value shape before a runtime
// allocates its output buffer. The plan borrows name, value, path and domain
// until write() completes.
class SetCookiePlan final {
public:
    SetCookiePlan(
        std::string_view name,
        std::string_view value,
        const CookieOptions& options);

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
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
