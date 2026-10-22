#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ruvia {

enum class http_transfer_coding : std::uint8_t { gzip,
    deflate };

// Parsing is bounded to prevent attacker-controlled unbounded state growth.
inline constexpr std::size_t max_transfer_codings = 8;

// A bounded inline sequence. It owns no memory resource, so a parsed framing
// plan may outlive the per-request storage its head was parsed from.
class http_transfer_codings final {
public:
    void push_back(http_transfer_coding coding) {
        if (size_ == max_transfer_codings) {
            throw std::length_error("too many transfer codings");
        }
        data_[size_++] = coding;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] http_transfer_coding* data() noexcept {
        return data_.data();
    }
    [[nodiscard]] const http_transfer_coding* data() const noexcept {
        return data_.data();
    }
    [[nodiscard]] http_transfer_coding* begin() noexcept {
        return data_.data();
    }
    [[nodiscard]] const http_transfer_coding* begin() const noexcept {
        return data_.data();
    }
    [[nodiscard]] http_transfer_coding* end() noexcept {
        return data_.data() + size_;
    }
    [[nodiscard]] const http_transfer_coding* end() const noexcept {
        return data_.data() + size_;
    }
    [[nodiscard]] http_transfer_coding& operator[](std::size_t index) noexcept {
        return data_[index];
    }
    [[nodiscard]] const http_transfer_coding& operator[](std::size_t index) const noexcept {
        return data_[index];
    }

private:
    std::array<http_transfer_coding, max_transfer_codings> data_{};
    std::size_t size_{0};
};
}  // namespace ruvia
