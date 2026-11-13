#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/http/http_hex.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/web/rate_limit.h"

namespace ruvia::detail {

using ::ruvia::rate_limit_key_buffer_bytes;

[[nodiscard]] inline std::string_view format_ipv4_octets(
    const std::uint8_t* bytes_value, std::span<char, 15> buffer) noexcept {
    char* cursor_value = buffer.data();
    char* const end = buffer.data() + buffer.size();
    for (std::size_t i = 0; i < 4; ++i) {
        if (i != 0) {
            *cursor_value++ = '.';
        }
        cursor_value = std::to_chars(cursor_value, end, bytes_value[i]).ptr;
    }
    return std::string_view(buffer.data(), static_cast<std::size_t>(cursor_value - buffer.data()));
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        return format_ipv4_octets(address.bytes().data(), std::span<char, 15>(buffer.data(), 15));
    }
    const auto raw = address.bytes();
    if (address.scope_id() != 0) {
        buffer[0] = 'v';
        buffer[1] = '6';
        buffer[2] = 's';
        buffer[3] = '/';
        http_encode_lower_hex(buffer.data() + 4, std::span<const unsigned char>(raw.data(), 16));
        buffer[36] = '/';
        const auto end = std::to_chars(
            buffer.data() + 37, buffer.data() + rate_limit_key_buffer_bytes, address.scope_id())
                             .ptr;
        return std::string_view(
            buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    }
    if (address.is_ipv4_mapped()) {
        return format_ipv4_octets(raw.data() + 12, std::span<char, 15>(buffer.data(), 15));
    }
    buffer[0] = 'v';
    buffer[1] = '6';
    buffer[2] = '/';
    http_encode_lower_hex(buffer.data() + 3, std::span<const unsigned char>(raw.data(), 8));
    return std::string_view(buffer.data(), 19);
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    const ip_address& address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    return rate_limit_key_for(address, std::span<char, rate_limit_key_buffer_bytes>(buffer));
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    std::string_view remote_address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    const auto parsed = ip_address::parse(remote_address);
    if (!parsed.has_value()) {
        return remote_address;
    }
    return rate_limit_key_for(*parsed, buffer);
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    std::string_view remote_address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    return rate_limit_key_for(remote_address, std::span<char, rate_limit_key_buffer_bytes>(buffer));
}

}  // namespace ruvia::detail
