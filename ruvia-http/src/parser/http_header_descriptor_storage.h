#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace ruvia::detail {

// Borrowed field descriptors stay inline until their reserved or appended count
// needs heap overflow. Views are rebound after moves, never cached.
template <typename field_type>
class http_header_descriptor_storage final {
    static_assert(std::is_trivially_copyable_v<field_type>);

public:
    http_header_descriptor_storage() noexcept = default;

    http_header_descriptor_storage(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage& operator=(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage(http_header_descriptor_storage&& other) noexcept
        : overflow_(std::exchange(other.overflow_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, inline_capacity)) {
        if (overflow_ == nullptr) {
            std::copy_n(other.inline_.data(), size_, inline_.data());
        }
    }
    http_header_descriptor_storage& operator=(http_header_descriptor_storage&&) = delete;

    ~http_header_descriptor_storage() {
        if (overflow_ != nullptr) {
            std::allocator<field_type>{}.deallocate(overflow_, capacity_);
        }
    }

    void reserve(std::size_t requested_capacity) {
        if (requested_capacity <= capacity_) {
            return;
        }
        reallocate(requested_capacity);
    }

    void append(field_type field) {
        if (size_ == capacity_) {
            grow();
        }
        auto* destination = overflow_ != nullptr ? overflow_ : inline_.data();
        destination[size_++] = field;
    }

    void clear() noexcept {
        size_ = 0;
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
        reallocate(next_capacity);
    }

    void reallocate(std::size_t next_capacity) {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(field_type);
        if (next_capacity > maximum) {
            throw std::length_error("too many HTTP header descriptors");
        }
        std::allocator<field_type> allocator;
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
    field_type* overflow_{nullptr};
    std::size_t size_{0};
    std::size_t capacity_{inline_capacity};
};

}  // namespace ruvia::detail
