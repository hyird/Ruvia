#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "http_header.h"

namespace ruvia::detail {

// Ensures capacity for `required` elements before a mutation that must not
// reallocate. Growth at least doubles the current capacity because an exact
// reserve(n) (libc++) would turn a sequence of small appends into quadratic copying.
template <typename container_type>
void reserve_geometric(container_type& container, std::size_t required) {
    const auto capacity = container.capacity();
    if (required <= capacity) {
        return;
    }
    const auto max_size = container.max_size();
    if (required > max_size) {
        throw std::length_error("container capacity overflow");
    }
    const auto doubled = capacity > max_size / 2 ? max_size : capacity * 2;
    container.reserve((std::max)(required, doubled));
}

// Address intervals compare as unsigned integers: views into unrelated buffers
// are unrelated pointers, and subtraction avoids forming an overflowing end.
[[nodiscard]] inline bool http_text_overlaps(std::string_view left, std::string_view right) noexcept {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto left_address = reinterpret_cast<std::uintptr_t>(left.data());
    const auto right_address = reinterpret_cast<std::uintptr_t>(right.data());
    return left_address >= right_address ? left_address - right_address < right.size() : right_address - left_address < left.size();
}

// Whether `text` views bytes `storage` owns, its terminator included: growing or
// rewriting `storage` would destroy that input, so the caller owns a copy first.
[[nodiscard]] inline bool http_text_aliases_string(std::string_view text, const std::string& storage) noexcept {
    return http_text_overlaps(text, std::string_view(storage.data(), storage.capacity() + 1));
}

// Borrowed field descriptors stay inline until their reserved or appended count
// needs heap overflow. Views are rebound after moves, never cached.
template <typename field_type>
class http_header_descriptor_storage final {
    static_assert(std::is_trivially_copyable_v<field_type>);

public:
    http_header_descriptor_storage() noexcept = default;

    http_header_descriptor_storage(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage& operator=(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage(http_header_descriptor_storage&& other) noexcept :
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

// The first input byte occupies the low byte, independent of native byte order.
// The caller must provide at least eight readable bytes.
[[nodiscard]] inline std::uint64_t load_http_field_value_word(const char* data) noexcept {
    std::uint64_t bytes;
    std::memcpy(&bytes, data, sizeof(bytes));
    if constexpr (std::endian::native == std::endian::big) {
        bytes = 0;
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            bytes |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[index])) << (index * 8);
        }
    }
    return bytes;
}

// The first set high bit identifies the first invalid input byte. DEL detection
// can also mark later bytes, but never changes the position of its first match.
[[nodiscard]] inline std::uint64_t invalid_http_field_value_mask(std::uint64_t bytes_value) noexcept {
    constexpr std::uint64_t ones = 0x0101010101010101ULL;
    constexpr std::uint64_t high_bits = ones * 0x80U;
    constexpr std::uint64_t low_bits = ones * 0x7fU;
    const auto del = bytes_value ^ low_bits;
    // Setting each high bit prevents inter-byte borrow when testing < SP.
    // Exclude original high-bit bytes so obs-text remains valid.
    const auto controls = ~((bytes_value | high_bits) - ones * 0x20U) & ~bytes_value;
    const auto special = (controls | ((del - ones) & ~del)) & high_bits;
    if (special == 0) {
        return 0;
    }
    // This exact HTAB mask cannot carry between bytes or hide a DEL match.
    const auto tabs = bytes_value ^ (ones * 9U);
    const auto tab_bits = ~(((tabs & low_bits) + low_bits) | tabs | low_bits);
    return special & ~tab_bits;
}

[[nodiscard]] inline std::size_t http_field_value_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= sizeof(std::uint64_t)) {
        const auto bytes_value = load_http_field_value_word(value.data() + index);
        if (const auto invalid = invalid_http_field_value_mask(bytes_value); invalid != 0) {
            return index + static_cast<std::size_t>(std::countr_zero(invalid)) / 8;
        }
        index += sizeof(bytes_value);
    }
    while (index < value.size() && is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

enum class field_line_folding_policy { mime,
    http };

// A fold always starts with CRLF followed by SP/HTAB. Leave bare CR/LF and
// ordinary field separators visible to the protocol's field validator.
[[nodiscard]] inline std::size_t find_field_line_fold(std::string_view input, std::size_t start = 0) noexcept {
    auto position = input.find("\r\n", start);
    while (position != std::string_view::npos) {
        if (position + 2 < input.size() && is_http_ows_char(input[position + 2])) {
            return position;
        }
        position = input.find("\r\n", position + 2);
    }
    return std::string_view::npos;
}

// first_fold comes from find_field_line_fold. The caller owns storage through
// every use of the resulting views; input must not borrow that storage.
// MIME removes only CRLF. HTTP replaces the complete obs-fold, including its
// adjacent OWS/RWS, with SP before field values are interpreted (RFC 9112 5.2).
inline void copy_unfolded_field_lines(std::string_view input, std::size_t first_fold, field_line_folding_policy policy, std::string& storage) {
    storage.clear();
    storage.reserve(input.size());
    std::size_t segment_begin = 0;
    for (auto position = first_fold; position != std::string_view::npos;) {
        auto segment_end = position;
        auto next_segment = position + 2;
        if (policy == field_line_folding_policy::http) {
            while (segment_end > segment_begin && is_http_ows_char(input[segment_end - 1])) {
                --segment_end;
            }
            while (next_segment < input.size() && is_http_ows_char(input[next_segment])) {
                ++next_segment;
            }
        }
        storage.append(input.substr(segment_begin, segment_end - segment_begin));
        if (policy == field_line_folding_policy::http) {
            storage.push_back(' ');
        }
        segment_begin = next_segment;
        position = find_field_line_fold(input, next_segment);
    }
    storage.append(input.substr(segment_begin));
}

}  // namespace ruvia::detail
