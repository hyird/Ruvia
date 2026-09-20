#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpHeader.h"

namespace ruvia::detail {

// Owns only field descriptors, never their text. Production parsers reserve
// the final size once in request/stream PMR storage after validating the head.
// Unlike a general container, moving this block never allocates even in debug.
class HttpRequestHeaderBlock final {
public:
    HttpRequestHeaderBlock() noexcept = default;
    HttpRequestHeaderBlock(const HttpRequestHeaderBlock&) = delete;
    HttpRequestHeaderBlock& operator=(const HttpRequestHeaderBlock&) = delete;
    HttpRequestHeaderBlock(HttpRequestHeaderBlock&& other) noexcept
        : fields_(std::exchange(other.fields_, nullptr)),
          resource_(std::exchange(other.resource_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}
    HttpRequestHeaderBlock& operator=(HttpRequestHeaderBlock&& other) noexcept {
        if (this != &other) {
            release();
            fields_ = std::exchange(other.fields_, nullptr);
            resource_ = std::exchange(other.resource_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }
    ~HttpRequestHeaderBlock() {
        release();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] const HttpHeaderView& operator[](std::size_t index) const noexcept {
        return fields_[index];
    }
    [[nodiscard]] std::span<const HttpHeaderView> fields() const noexcept {
        return {fields_, size_};
    }

    void reserve(std::size_t count, std::pmr::memory_resource* resource) {
        if (count > kMaxHttpHeaderFields) {
            throw std::logic_error("request header block exceeds field limit");
        }
        if (count <= capacity_) {
            return;
        }
        auto* owner = resource == nullptr ? std::pmr::get_default_resource() : resource;
        auto* replacement = static_cast<HttpHeaderView*>(owner->allocate(count * sizeof(HttpHeaderView), alignof(HttpHeaderView)));
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(replacement + i, fields_[i]);
        }
        const auto size = size_;
        release();
        fields_ = replacement;
        resource_ = owner;
        size_ = size;
        capacity_ = static_cast<std::uint8_t>(count);
    }

    void append(HttpHeaderView field) {
        if (size_ == kMaxHttpHeaderFields) {
            throw std::logic_error("request header block is full");
        }
        if (size_ == capacity_) {
            reserve(std::min(kMaxHttpHeaderFields, std::max(std::size_t{1}, std::size_t(capacity_) * 2)), resource_);
        }
        std::construct_at(fields_ + size_, field);
        ++size_;
    }

private:
    void release() noexcept {
        if (fields_ != nullptr) {
            std::destroy_n(fields_, size_);
            resource_->deallocate(fields_, std::size_t(capacity_) * sizeof(HttpHeaderView), alignof(HttpHeaderView));
        }
        fields_ = nullptr;
        resource_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

    static_assert(kMaxHttpHeaderFields <= UINT8_MAX);
    HttpHeaderView* fields_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    std::uint8_t size_{0};
    std::uint8_t capacity_{0};
};

}  // namespace ruvia::detail
