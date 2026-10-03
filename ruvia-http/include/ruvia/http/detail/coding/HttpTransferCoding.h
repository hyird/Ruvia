#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace ruvia {

enum class HttpTransferCoding : std::uint8_t { kGzip,
    kDeflate };

// Parsing is bounded to prevent attacker-controlled unbounded state growth.
inline constexpr std::size_t kMaxTransferCodings = 8;

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

    void push_back(HttpTransferCoding coding) {
        if (size_ == kMaxTransferCodings) {
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
    [[nodiscard]] HttpTransferCoding* data() noexcept {
        return data_;
    }
    [[nodiscard]] const HttpTransferCoding* data() const noexcept {
        return data_;
    }
    [[nodiscard]] HttpTransferCoding* begin() noexcept {
        return data_;
    }
    [[nodiscard]] const HttpTransferCoding* begin() const noexcept {
        return data_;
    }
    [[nodiscard]] HttpTransferCoding* end() noexcept {
        return size_ == 0 ? data_ : data_ + size_;
    }
    [[nodiscard]] const HttpTransferCoding* end() const noexcept {
        return size_ == 0 ? data_ : data_ + size_;
    }
    [[nodiscard]] HttpTransferCoding& operator[](std::size_t index) noexcept {
        return data_[index];
    }
    [[nodiscard]] const HttpTransferCoding& operator[](std::size_t index) const noexcept {
        return data_[index];
    }
    [[nodiscard]] std::pmr::polymorphic_allocator<HttpTransferCoding> get_allocator() const noexcept {
        return {resource_};
    }

private:
    void ensure_storage() {
        if (data_ == nullptr) {
            data_ = get_allocator().allocate(kMaxTransferCodings);
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
            get_allocator().deallocate(data_, kMaxTransferCodings);
        }
        data_ = nullptr;
        size_ = 0;
    }

    std::pmr::memory_resource* resource_;
    HttpTransferCoding* data_{nullptr};
    std::size_t size_{0};
};

struct HttpTransferCodings final {
    explicit HttpTransferCodings(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : values(resource) {}

    [[nodiscard]] bool empty() const noexcept {
        return values.empty();
    }

    transfer_coding_sequence values;
};

}  // namespace ruvia
