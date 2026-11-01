#include "ruvia/core/socket.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

namespace {

// The lexical shape is checked here because the platform parsers behind
// asio::ip::make_address are not strict validators: Windows accepts
// `a.b.c.d:port`, `[v6]:port`, and fills in missing IPv4 octets. The address
// text must therefore already be a complete literal before it reaches asio.

[[nodiscard]] constexpr bool is_decimal_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr bool is_hex_digit(char value) noexcept {
    return is_decimal_digit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

// Exactly four decimal octets, each 1-3 digits without leading zeros and at
// most 255. Returns the bytes in network order on success.
[[nodiscard]] std::optional<asio::ip::address_v4::bytes_type> parse_ipv4_literal(std::string_view text) noexcept {
    asio::ip::address_v4::bytes_type bytes{};
    std::size_t position = 0;
    for (std::size_t octet = 0; octet < bytes.size(); ++octet) {
        if (octet != 0) {
            if (position >= text.size() || text[position] != '.') {
                return std::nullopt;
            }
            ++position;
        }
        std::size_t digits = 0;
        unsigned value = 0;
        while (position < text.size() && is_decimal_digit(text[position])) {
            if (digits == 3 || (digits == 1 && value == 0)) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<unsigned>(text[position] - '0');
            ++digits;
            ++position;
        }
        if (digits == 0 || value > 255) {
            return std::nullopt;
        }
        bytes[octet] = static_cast<unsigned char>(value);
    }
    if (position != text.size()) {
        return std::nullopt;
    }
    return bytes;
}

// Hex digits, ':' and '.' (for an embedded IPv4 tail), optionally followed by
// one '%' and a non-empty zone that contains no further '%'; nothing else.
[[nodiscard]] bool is_ipv6_literal_shape(std::string_view text) noexcept {
    const auto zone = text.find('%');
    const auto address = text.substr(0, zone);
    if (address.find(':') == std::string_view::npos) {
        return false;
    }
    for (const char value : address) {
        if (!is_hex_digit(value) && value != ':' && value != '.') {
            return false;
        }
    }
    if (address.find('.') != std::string_view::npos) {
        const auto last_colon = address.rfind(':');
        if (last_colon == std::string_view::npos || !parse_ipv4_literal(address.substr(last_colon + 1)).has_value()) {
            return false;
        }
    }
    if (zone == std::string_view::npos) {
        return true;
    }
    const auto zone_name = text.substr(zone + 1);
    if (zone_name.empty()) {
        return false;
    }
    for (const unsigned char ch : zone_name) {
        if (ch <= 0x20 || ch >= 0x7f || ch == '%' || ch == ':' || ch == '[' || ch == ']') {
            return false;
        }
    }
    if (zone_name.front() == '+' || zone_name.front() == '-') {
        return false;
    }
    if (is_decimal_digit(zone_name.front())) {
        if (zone_name.front() == '0') {
            return false;
        }
        std::uint64_t scope_value = 0;
        for (const char ch : zone_name) {
            if (!is_decimal_digit(ch)) {
                return false;
            }
            scope_value = scope_value * 10 + static_cast<std::uint64_t>(ch - '0');
            if (scope_value > std::numeric_limits<asio::ip::scope_id_type>::max()) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// Consumes the complete view, including checking for embedded NUL. Common
// address text uses stack storage; unusually long scope names use temporary PMR
// storage that is released before returning the independent address value.
std::optional<asio::ip::address> parse_ip_address(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    if (const auto v4 = parse_ipv4_literal(text)) {
        return asio::ip::address(asio::ip::address_v4(*v4));
    }
    if (!is_ipv6_literal_shape(text)) {
        return std::nullopt;
    }
    std::error_code error;
    std::array<char, 64> terminated;
    const auto address = [&] {
        if (text.size() < terminated.size()) {
            std::memcpy(terminated.data(), text.data(), text.size());
            terminated[text.size()] = '\0';
            return asio::ip::make_address_v6(terminated.data(), error);
        }
        const std::pmr::string owned(text, process_resource());
        return asio::ip::make_address_v6(owned.c_str(), error);
    }();
    if (error || (text.find('%') != std::string_view::npos && address.scope_id() == 0)) {
        return std::nullopt;
    }
    return asio::ip::address(address);
}

}  // namespace ruvia
