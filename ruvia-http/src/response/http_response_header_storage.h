#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/util/http_address_overlap.h"
#include "ruvia/http/http_ascii.h"
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

// The one field-identity rule: known fields match by bit, unknown fields by
// case-insensitive name among unknown fields only.
[[nodiscard]] inline bool response_header_matches(
    const http_response_header& header_value, std::string_view key, std::uint32_t known_bit) noexcept {
    const auto header_known_bit = http_response_header_access::known_bit(header_value);
    return known_bit != 0 ? header_known_bit == known_bit
                          : header_known_bit == 0 && http_ascii_equals_ignore_case(header_value.name(), key);
}

[[nodiscard]] inline bool response_header_storage_overlaps(
    const http_response_header& header_value, std::string_view value) noexcept {
    const auto name = header_value.name();
    if (name.data() == nullptr) {
        return false;
    }
    return http_text_overlaps(
        {name.data(), name.size() + header_value.value().size()}, value);
}

[[nodiscard]] inline bool response_header_views_contiguous(
    std::string_view name, std::string_view value) noexcept {
    if (name.data() == nullptr) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    if (value.data() == nullptr) {
        return false;
    }
    return reinterpret_cast<std::uintptr_t>(value.data()) ==
           reinterpret_cast<std::uintptr_t>(name.data() + name.size());
}

}  // namespace ruvia::detail
