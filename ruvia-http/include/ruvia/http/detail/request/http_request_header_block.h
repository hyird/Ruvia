#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

// Owns field descriptors and the parser's request_header_kind for each field.
// Kinds sit in the same allocation so Cookie/Accept scans do not re-classify
// names. Production parsers reserve the final size once after validating the
// head. Moving this block never allocates.
class http_request_header_block final {
public:
    http_request_header_block() noexcept = default;
    http_request_header_block(const http_request_header_block&) = delete;
    http_request_header_block& operator=(const http_request_header_block&) = delete;
    http_request_header_block(http_request_header_block&& other) noexcept
        : fields_(std::exchange(other.fields_, nullptr)),
          kinds_(std::exchange(other.kinds_, nullptr)),
          resource_(std::exchange(other.resource_, nullptr)),
          size_(std::exchange(other.size_, std::uint16_t{0})),
          capacity_(std::exchange(other.capacity_, std::uint16_t{0})),
          repeated_known_bits_(std::exchange(other.repeated_known_bits_, std::uint32_t{0})) {}
    http_request_header_block& operator=(http_request_header_block&& other) noexcept {
        if (this != &other) {
            release();
            fields_ = std::exchange(other.fields_, nullptr);
            kinds_ = std::exchange(other.kinds_, nullptr);
            resource_ = std::exchange(other.resource_, nullptr);
            size_ = std::exchange(other.size_, std::uint16_t{0});
            capacity_ = std::exchange(other.capacity_, std::uint16_t{0});
            repeated_known_bits_ = std::exchange(other.repeated_known_bits_, std::uint32_t{0});
        }
        return *this;
    }
    ~http_request_header_block() {
        release();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] const http_header_view& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_[index];
    }
    const http_header_view& operator[](std::size_t) const&& = delete;
    [[nodiscard]] request_header_kind kind_at(std::size_t index) const noexcept {
        return kinds_[index];
    }
    [[nodiscard]] std::span<const http_header_view> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return {fields_, size_};
    }
    std::span<const http_header_view> fields() const&& = delete;
    void mark_repeated_known_slot(std::size_t slot) noexcept {
        if (slot < 32U) {
            repeated_known_bits_ |= (std::uint32_t{1} << slot);
        }
    }
    [[nodiscard]] bool has_repeated_known_slot(std::size_t slot) const noexcept {
        return slot < 32U && (repeated_known_bits_ & (std::uint32_t{1} << slot)) != 0;
    }

    void reserve(std::size_t count, std::pmr::memory_resource* resource) {
        if (count > (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::logic_error("request header block exceeds field limit");
        }
        if (count <= capacity_) {
            return;
        }
        auto* owner_value = http_pmr_resource_or_default(resource);
        auto* memory = static_cast<std::byte*>(owner_value->allocate(bytes_for(count), alignof(http_header_view)));
        auto* replacement = reinterpret_cast<http_header_view*>(memory);
        auto* replacement_kinds = reinterpret_cast<request_header_kind*>(replacement + count);
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(replacement + i, fields_[i]);
            replacement_kinds[i] = kinds_[i];
        }
        const auto size = size_;
        const auto repeated_known_bits = repeated_known_bits_;
        release();
        fields_ = replacement;
        kinds_ = replacement_kinds;
        resource_ = owner_value;
        size_ = size;
        capacity_ = static_cast<std::uint16_t>(count);
        repeated_known_bits_ = repeated_known_bits;
    }

    void append(http_header_view field, request_header_kind kind) {
        if (size_ == (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::logic_error("request header block is full");
        }
        if (size_ == capacity_) {
            reserve((std::min)(static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()),
                        (std::max)(std::size_t{1}, std::size_t(capacity_) * 2)),
                resource_);
        }
        std::construct_at(fields_ + size_, field);
        kinds_[size_] = kind;
        ++size_;
    }

private:
    [[nodiscard]] static std::size_t bytes_for(std::size_t count) noexcept {
        return count * sizeof(http_header_view) + count * sizeof(request_header_kind);
    }

    void release() noexcept {
        if (fields_ != nullptr) {
            std::destroy_n(fields_, size_);
            resource_->deallocate(fields_, bytes_for(capacity_), alignof(http_header_view));
        }
        fields_ = nullptr;
        kinds_ = nullptr;
        resource_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        repeated_known_bits_ = 0;
    }

    http_header_view* fields_{nullptr};
    request_header_kind* kinds_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    std::uint16_t size_{0};
    std::uint16_t capacity_{0};
    std::uint32_t repeated_known_bits_{0};
};

}  // namespace ruvia::detail
