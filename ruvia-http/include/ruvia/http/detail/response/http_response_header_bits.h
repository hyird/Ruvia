#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

inline constexpr std::uint32_t response_header_content_length = 1U << 0;
inline constexpr std::uint32_t response_header_content_encoding = 1U << 1;
inline constexpr std::uint32_t response_header_content_type = 1U << 2;
inline constexpr std::uint32_t response_header_connection = 1U << 3;
inline constexpr std::uint32_t response_header_vary = 1U << 4;
inline constexpr std::uint32_t response_header_date = 1U << 5;
inline constexpr std::uint32_t response_header_server = 1U << 6;
inline constexpr std::uint32_t response_header_cache_control = 1U << 7;
inline constexpr std::uint32_t response_header_transfer_encoding = 1U << 8;
inline constexpr std::uint32_t response_header_allow = 1U << 9;
inline constexpr std::uint32_t response_header_access_control_allow_origin = 1U << 10;
inline constexpr std::uint32_t response_header_access_control_allow_credentials = 1U << 11;
inline constexpr std::uint32_t response_header_access_control_allow_methods = 1U << 12;
inline constexpr std::uint32_t response_header_access_control_allow_headers = 1U << 13;
inline constexpr std::uint32_t response_header_access_control_max_age = 1U << 14;
inline constexpr std::uint32_t response_header_access_control_expose_headers = 1U << 15;
inline constexpr std::uint32_t response_header_accept_ranges = 1U << 16;
inline constexpr std::uint32_t response_header_content_range = 1U << 17;
inline constexpr std::uint32_t response_header_etag = 1U << 18;
inline constexpr std::uint32_t response_header_last_modified = 1U << 19;
inline constexpr std::uint32_t response_header_location = 1U << 20;
inline constexpr std::uint32_t response_header_set_cookie = 1U << 21;
inline constexpr std::size_t response_known_header_count = 22;

[[nodiscard]] inline constexpr std::size_t response_known_header_slot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t known_mask = (1U << response_known_header_count) - 1U;
    if (bit == 0 || (bit & ~known_mask) != 0 || (bit & (bit - 1U)) != 0) {
        return response_known_header_count;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

[[nodiscard]] inline bool response_header_append_forbidden(std::uint32_t bit) noexcept {
    return bit == response_header_content_length || bit == response_header_content_type ||
           bit == response_header_date || bit == response_header_server ||
           bit == response_header_transfer_encoding || bit == response_header_allow ||
           bit == response_header_access_control_allow_origin ||
           bit == response_header_access_control_allow_credentials ||
           bit == response_header_access_control_max_age || bit == response_header_accept_ranges ||
           bit == response_header_content_range || bit == response_header_etag ||
           bit == response_header_last_modified || bit == response_header_location;
}

}  // namespace ruvia::detail
