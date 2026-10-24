#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <type_traits>
#include <vector>

#include "ruvia/http/detail/util/pmr_resource.h"

namespace ruvia::detail {

// Both HTTP/1 parsers keep borrowed descriptors inline and spill only when the
// actual field count requires it. Views are rebound after moves, never cached.
template <typename field_type>
class http_header_descriptor_storage final {
    static_assert(std::is_trivially_copyable_v<field_type>);

public:
    explicit http_header_descriptor_storage(std::pmr::memory_resource* resource) noexcept
        : overflow_(http_pmr_resource_or_default(resource)) {}

    http_header_descriptor_storage(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage& operator=(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage(http_header_descriptor_storage&&) noexcept = default;
    http_header_descriptor_storage& operator=(http_header_descriptor_storage&&) = delete;

    void append(field_type field) {
        if (overflow_.empty()) {
            if (inline_size_ < inline_capacity) {
                inline_[inline_size_++] = field;
                return;
            }
            // Allocation is the only throwing step before the inline fields
            // have all been copied; failure preserves the existing sequence.
            overflow_.reserve(inline_capacity * 2);
            for (const auto& inline_field : inline_) {
                overflow_.push_back(inline_field);
            }
        }
        overflow_.push_back(field);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return overflow_.empty() ? inline_size_ : overflow_.size();
    }

    [[nodiscard]] std::span<const field_type> view() const& noexcept {
        if (!overflow_.empty()) {
            return overflow_;
        }
        return {inline_.data(), inline_size_};
    }
    std::span<const field_type> view() const&& = delete;

private:
    static constexpr std::size_t inline_capacity = 32;

    // POD request slots stay uninitialized until appended. Every readable slot
    // is fully assigned before publication, without zeroing unused slots.
    std::array<field_type, inline_capacity> inline_;
    std::size_t inline_size_{0};
    std::pmr::vector<field_type> overflow_;
};

}  // namespace ruvia::detail
