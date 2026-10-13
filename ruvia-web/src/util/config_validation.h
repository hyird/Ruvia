#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

// A configured host whose port is a separate field: brackets and a lone colon
// (host:port) are rejected; an unbracketed IPv6 literal has several colons.
inline void ensure_config_host(std::string_view host, const char* empty_message, const char* invalid_message) {
    if (host.empty()) {
        throw std::invalid_argument(empty_message);
    }
    std::size_t colon_count = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' || byte == '[' || byte == ']') {
            throw std::invalid_argument(invalid_message);
        }
        colon_count += byte == ':' ? 1 : 0;
    }
    if (colon_count == 1) {
        throw std::invalid_argument(invalid_message);
    }
}

inline void ensure_positive_size(std::size_t value, const char* message) {
    if (value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_size(
    const std::optional<std::size_t>& value, const char* message) {
    if (value.has_value() && *value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_port(std::uint16_t port, const char* message) {
    if (port == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_optional_port(
    const std::optional<std::uint16_t>& port, const char* message) {
    if (port.has_value()) {
        ensure_non_zero_port(*port, message);
    }
}

template <typename rep_type, typename period_type>
void ensure_positive_duration(std::chrono::duration<rep_type, period_type> value, const char* message) {
    if (value.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

template <typename rep_type, typename period_type>
void ensure_positive_optional_duration(
    const std::optional<std::chrono::duration<rep_type, period_type>>& value, const char* message) {
    if (value.has_value() && value->count() <= 0) {
        throw std::invalid_argument(message);
    }
}

template <typename first_duration_type, typename... rest_durations_type>
void ensure_positive_optional_durations(
    const char* message, const first_duration_type& first, const rest_durations_type&... rest) {
    ensure_positive_optional_duration(first, message);
    (ensure_positive_optional_duration(rest, message), ...);
}

}  // namespace ruvia::detail
