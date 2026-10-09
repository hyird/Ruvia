#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

// http_response_header stores both lengths in uint32_t and releases the one
// name/value allocation using their sum. Never let a public or runtime helper
// construct a descriptor whose wire/storage lengths cannot be represented;
// truncating either field would make the response header view disagree with
// the allocation and could turn a large application value into malformed wire
// bytes or an out-of-bounds read during emission.
[[nodiscard]] inline constexpr bool response_header_storage_size_fits(
    std::size_t name_size, std::size_t value_size) noexcept {
    constexpr auto max_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    return name_size <= max_size && value_size <= max_size && value_size <= max_size - name_size;
}

inline void validate_response_header_storage_size(std::size_t name_size, std::size_t value_size) {
    if (!response_header_storage_size_fits(name_size, value_size)) {
        throw std::length_error("HTTP response header is too large");
    }
}

[[nodiscard]] inline bool response_header_storage_overlaps(
    const http_response_header& header_value, std::string_view value) noexcept {
    const auto name = header_value.name();
    if (value.empty() || name.data() == nullptr) {
        return false;
    }
    const auto storage_begin = reinterpret_cast<std::uintptr_t>(name.data());
    const auto storage_end = storage_begin + name.size() + header_value.value().size();
    const auto value_begin = reinterpret_cast<std::uintptr_t>(value.data());
    const auto value_end = value_begin + value.size();
    return value_begin < storage_end && storage_begin < value_end;
}

struct http_response_header_access final {
    [[nodiscard]] static constexpr http_response_header make(const char* bytes_value, std::uint32_t name_size,
        std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
        http_response_header header_value{};
        header_value.bytes_ = bytes_value;
        header_value.name_size_ = name_size;
        header_value.value_size_ = value_size;
        header_value.known_bit_ = known_bit;
        header_value.owned_ = owned;
        header_value.append_ = false;
        return header_value;
    }

    [[nodiscard]] static char* value_begin(http_response_header& header_value) noexcept {
        if (header_value.bytes_ == nullptr) {
            return nullptr;
        }
        return const_cast<char*>(header_value.bytes_) + header_value.name_size_;
    }

    [[nodiscard]] static char* value_end(http_response_header& header_value) noexcept {
        auto* const begin = value_begin(header_value);
        return begin == nullptr ? nullptr : begin + header_value.value_size_;
    }

    [[nodiscard]] static std::uint32_t known_bit(const http_response_header& header_value) noexcept {
        return header_value.known_bit_;
    }

    [[nodiscard]] static bool append(const http_response_header& header_value) noexcept {
        return header_value.append_;
    }

    static void set_append(http_response_header& header_value, bool value) noexcept {
        header_value.append_ = value;
    }
};

[[nodiscard]] inline constexpr http_response_header make_response_header(const char* bytes_value,
    std::uint32_t name_size, std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
    return http_response_header_access::make(bytes_value, name_size, value_size, known_bit, owned);
}

[[nodiscard]] inline char* response_header_value_begin(http_response_header& header_value) noexcept {
    return http_response_header_access::value_begin(header_value);
}

[[nodiscard]] inline char* response_header_value_end(http_response_header& header_value) noexcept {
    return http_response_header_access::value_end(header_value);
}

[[nodiscard]] inline std::uint32_t response_header_known_bit(
    const http_response_header& header_value) noexcept {
    return http_response_header_access::known_bit(header_value);
}

[[nodiscard]] inline bool response_header_append(const http_response_header& header_value) noexcept {
    return http_response_header_access::append(header_value);
}

inline void set_response_header_append(http_response_header& header_value, bool value) noexcept {
    http_response_header_access::set_append(header_value, value);
}

}  // namespace ruvia::detail
