#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// RFC 9113 §6.5.2 / RFC 9114 §4.2.2: decoded fields, including
// pseudo-fields, cost their name and value lengths plus 32 bytes.
// Rejected additions leave the accumulated size unchanged.
class binary_field_section_size final {
public:
    explicit constexpr binary_field_section_size(std::uint64_t limit) noexcept
        : limit_(limit) {}

    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::uint64_t field_metadata_bytes = 32;
        const auto remaining = limit_ - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes ||
            value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        bytes_ += name.size() + value.size() + field_metadata_bytes;
        return true;
    }

private:
    std::uint64_t limit_;
    std::uint64_t bytes_{0};
};

}  // namespace ruvia::detail
