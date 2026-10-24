#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

namespace ruvia::detail {

// Both HTTP/1 parsers keep borrowed descriptors inline and spill only when the
// actual field count requires it. Views are rebound after moves, never cached.
template <typename field_type>
class http_header_descriptor_storage final {
    static_assert(std::is_trivially_copyable_v<field_type>);

public:
    explicit http_header_descriptor_storage(std::pmr::memory_resource* resource) noexcept
        : resource_(http_pmr_resource_or_default(resource)) {}

    http_header_descriptor_storage(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage& operator=(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage(http_header_descriptor_storage&& other) noexcept
        : resource_(other.resource_),
          overflow_(std::exchange(other.overflow_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, inline_capacity)) {
        if (overflow_ == nullptr) {
            std::copy_n(other.inline_.data(), size_, inline_.data());
        }
    }
    http_header_descriptor_storage& operator=(http_header_descriptor_storage&&) = delete;

    ~http_header_descriptor_storage() {
        if (overflow_ != nullptr) {
            std::pmr::polymorphic_allocator<field_type>(resource_).deallocate(overflow_, capacity_);
        }
    }

    void append(field_type field) {
        if (size_ == capacity_) {
            grow();
        }
        auto* destination = overflow_ != nullptr ? overflow_ : inline_.data();
        destination[size_++] = field;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::span<const field_type> view() const& noexcept {
        return {overflow_ != nullptr ? overflow_ : inline_.data(), size_};
    }
    std::span<const field_type> view() const&& = delete;

private:
    static constexpr std::size_t inline_capacity = 32;

    void grow() {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(field_type);
        if (capacity_ >= maximum) {
            throw std::length_error("too many HTTP header descriptors");
        }
        const auto next_capacity = capacity_ <= maximum / 2 ? capacity_ * 2 : maximum;
        std::pmr::polymorphic_allocator<field_type> allocator(resource_);
        auto* next = allocator.allocate(next_capacity);
        // Trivial descriptors cannot throw while copying. Publish only after
        // allocation succeeds, preserving the current fields on failure.
        const auto fields = view();
        std::uninitialized_copy_n(fields.data(), fields.size(), next);
        if (overflow_ != nullptr) {
            allocator.deallocate(overflow_, capacity_);
        }
        overflow_ = next;
        capacity_ = next_capacity;
    }

    // POD request slots stay uninitialized until appended. Every readable slot
    // is fully assigned before publication, without zeroing unused slots.
    std::array<field_type, inline_capacity> inline_;
    std::pmr::memory_resource* resource_;
    field_type* overflow_{nullptr};
    std::size_t size_{0};
    std::size_t capacity_{inline_capacity};
};

}  // namespace ruvia::detail
