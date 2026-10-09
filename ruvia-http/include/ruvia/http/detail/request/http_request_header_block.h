#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>

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
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}
    http_request_header_block& operator=(http_request_header_block&& other) noexcept {
        if (this != &other) {
            release();
            fields_ = std::exchange(other.fields_, nullptr);
            kinds_ = std::exchange(other.kinds_, nullptr);
            resource_ = std::exchange(other.resource_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
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
    [[nodiscard]] const http_header_view& operator[](std::size_t index) const noexcept {
        return fields_[index];
    }
    [[nodiscard]] std::uint8_t kind_at(std::size_t index) const noexcept {
        return kinds_[index];
    }
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept {
        return {fields_, size_};
    }

    void reserve(std::size_t count, std::pmr::memory_resource* resource) {
        if (count > max_http_header_fields) {
            throw std::logic_error("request header block exceeds field limit");
        }
        if (count <= capacity_) {
            return;
        }
        auto* owner_value = resource == nullptr ? std::pmr::get_default_resource() : resource;
        auto* memory = static_cast<std::byte*>(owner_value->allocate(bytes_for(count), alignof(http_header_view)));
        auto* replacement = reinterpret_cast<http_header_view*>(memory);
        auto* replacement_kinds = reinterpret_cast<std::uint8_t*>(replacement + count);
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(replacement + i, fields_[i]);
            replacement_kinds[i] = kinds_[i];
        }
        const auto size = size_;
        release();
        fields_ = replacement;
        kinds_ = replacement_kinds;
        resource_ = owner_value;
        size_ = size;
        capacity_ = static_cast<std::uint8_t>(count);
    }

    void append(http_header_view field, std::uint8_t kind = 0) {
        if (size_ == max_http_header_fields) {
            throw std::logic_error("request header block is full");
        }
        if (size_ == capacity_) {
            reserve((std::min)(max_http_header_fields,
                        (std::max)(std::size_t{1}, std::size_t(capacity_) * 2)),
                resource_);
        }
        std::construct_at(fields_ + size_, field);
        kinds_[size_] = kind;
        ++size_;
    }

private:
    [[nodiscard]] static std::size_t bytes_for(std::size_t count) noexcept {
        return count * sizeof(http_header_view) + count;
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
    }

    static_assert(max_http_header_fields <= UINT8_MAX);
    http_header_view* fields_{nullptr};
    std::uint8_t* kinds_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    std::uint8_t size_{0};
    std::uint8_t capacity_{0};
};

}  // namespace ruvia::detail
