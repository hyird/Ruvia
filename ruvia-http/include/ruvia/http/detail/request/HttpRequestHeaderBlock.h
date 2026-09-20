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

// Owns field descriptors and the parser's RequestHeaderKind for each field.
// Kinds sit in the same allocation so Cookie/Accept scans do not re-classify
// names. Production parsers reserve the final size once after validating the
// head. Moving this block never allocates.
class HttpRequestHeaderBlock final {
public:
    HttpRequestHeaderBlock() noexcept = default;
    HttpRequestHeaderBlock(const HttpRequestHeaderBlock&) = delete;
    HttpRequestHeaderBlock& operator=(const HttpRequestHeaderBlock&) = delete;
    HttpRequestHeaderBlock(HttpRequestHeaderBlock&& other) noexcept
        : fields_(std::exchange(other.fields_, nullptr)),
          kinds_(std::exchange(other.kinds_, nullptr)),
          resource_(std::exchange(other.resource_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}
    HttpRequestHeaderBlock& operator=(HttpRequestHeaderBlock&& other) noexcept {
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
    [[nodiscard]] std::uint8_t kindAt(std::size_t index) const noexcept {
        return kinds_[index];
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
        auto* memory = static_cast<std::byte*>(owner->allocate(bytesFor(count), alignof(HttpHeaderView)));
        auto* replacement = reinterpret_cast<HttpHeaderView*>(memory);
        auto* replacementKinds = reinterpret_cast<std::uint8_t*>(replacement + count);
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(replacement + i, fields_[i]);
            replacementKinds[i] = kinds_[i];
        }
        const auto size = size_;
        release();
        fields_ = replacement;
        kinds_ = replacementKinds;
        resource_ = owner;
        size_ = size;
        capacity_ = static_cast<std::uint8_t>(count);
    }

    void append(HttpHeaderView field, std::uint8_t kind = 0) {
        if (size_ == kMaxHttpHeaderFields) {
            throw std::logic_error("request header block is full");
        }
        if (size_ == capacity_) {
            reserve(std::min(kMaxHttpHeaderFields,
                        std::max(std::size_t{1}, std::size_t(capacity_) * 2)),
                resource_);
        }
        std::construct_at(fields_ + size_, field);
        kinds_[size_] = kind;
        ++size_;
    }

private:
    [[nodiscard]] static std::size_t bytesFor(std::size_t count) noexcept {
        return count * sizeof(HttpHeaderView) + count;
    }

    void release() noexcept {
        if (fields_ != nullptr) {
            std::destroy_n(fields_, size_);
            resource_->deallocate(fields_, bytesFor(capacity_), alignof(HttpHeaderView));
        }
        fields_ = nullptr;
        kinds_ = nullptr;
        resource_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

    static_assert(kMaxHttpHeaderFields <= UINT8_MAX);
    HttpHeaderView* fields_{nullptr};
    std::uint8_t* kinds_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    std::uint8_t size_{0};
    std::uint8_t capacity_{0};
};

}  // namespace ruvia::detail
