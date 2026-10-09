#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace ruvia {

enum class http_transfer_coding : std::uint8_t { gzip,
    deflate };

// Parsing is bounded to prevent attacker-controlled unbounded state growth.
inline constexpr std::size_t max_transfer_codings = 8;

// A bounded PMR sequence whose construction and moves never allocate debug
// iterator proxies. Copies retain the source resource; assignments retain the
// destination resource. The resource must outlive the sequence and its copies.
class transfer_coding_sequence final {
public:
    explicit transfer_coding_sequence(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    transfer_coding_sequence(const transfer_coding_sequence& other)
        : transfer_coding_sequence(other.resource_) {
        copy_from(other);
    }

    transfer_coding_sequence(transfer_coding_sequence&& other) noexcept
        : resource_(other.resource_),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}

    transfer_coding_sequence& operator=(const transfer_coding_sequence& other) {
        if (this != &other) {
            copy_from(other);
        }
        return *this;
    }

    transfer_coding_sequence& operator=(transfer_coding_sequence&& other) {
        if (this != &other) {
            if (resource_->is_equal(*other.resource_)) {
                release();
                data_ = std::exchange(other.data_, nullptr);
                size_ = std::exchange(other.size_, 0);
            } else {
                copy_from(other);
                other.release();
            }
        }
        return *this;
    }

    ~transfer_coding_sequence() noexcept {
        release();
    }

    void push_back(http_transfer_coding coding) {
        if (size_ == max_transfer_codings) {
            throw std::length_error("too many transfer codings");
        }
        ensure_storage();
        data_[size_++] = coding;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] http_transfer_coding* data() noexcept {
        return data_;
    }
    [[nodiscard]] const http_transfer_coding* data() const noexcept {
        return data_;
    }
    [[nodiscard]] http_transfer_coding* begin() noexcept {
        return data_;
    }
    [[nodiscard]] const http_transfer_coding* begin() const noexcept {
        return data_;
    }
    [[nodiscard]] http_transfer_coding* end() noexcept {
        return size_ == 0 ? data_ : data_ + size_;
    }
    [[nodiscard]] const http_transfer_coding* end() const noexcept {
        return size_ == 0 ? data_ : data_ + size_;
    }
    [[nodiscard]] http_transfer_coding& operator[](std::size_t index) noexcept {
        return data_[index];
    }
    [[nodiscard]] const http_transfer_coding& operator[](std::size_t index) const noexcept {
        return data_[index];
    }
    [[nodiscard]] std::pmr::polymorphic_allocator<http_transfer_coding> get_allocator() const noexcept {
        return {resource_};
    }

private:
    void ensure_storage() {
        if (data_ == nullptr) {
            data_ = get_allocator().allocate(max_transfer_codings);
        }
    }

    void copy_from(const transfer_coding_sequence& other) {
        if (!other.empty()) {
            ensure_storage();
            std::copy_n(other.data_, other.size_, data_);
        }
        size_ = other.size_;
    }

    void release() noexcept {
        if (data_ != nullptr) {
            get_allocator().deallocate(data_, max_transfer_codings);
        }
        data_ = nullptr;
        size_ = 0;
    }

    std::pmr::memory_resource* resource_;
    http_transfer_coding* data_{nullptr};
    std::size_t size_{0};
};

struct http_transfer_codings final {
    explicit http_transfer_codings(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : values_(resource) {}

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

    transfer_coding_sequence values_;
};

}  // namespace ruvia
