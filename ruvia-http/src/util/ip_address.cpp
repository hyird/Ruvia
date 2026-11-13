#include "ruvia/http/ip_address.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "parser/http_uri_grammar.h"

namespace ruvia {
namespace {

[[nodiscard]] std::optional<std::uint32_t> parse_zone(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] char* append_ipv4(char* cursor, char* end, const std::uint8_t* bytes) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        if (i != 0) {
            *cursor++ = '.';
        }
        cursor = std::to_chars(cursor, end, bytes[i]).ptr;
    }
    return cursor;
}

}  // namespace

std::optional<ip_address> ip_address::parse(std::string_view text) noexcept {
    if (text.find(':') == std::string_view::npos) {
        const auto octets = detail::parse_ipv4_address(text);
        if (!octets) {
            return std::nullopt;
        }
        return ip_address::ipv4(*octets);
    }
    std::uint32_t scope_id = 0;
    const auto zone = text.find('%');
    if (zone != std::string_view::npos) {
        const auto parsed_zone = parse_zone(text.substr(zone + 1));
        if (!parsed_zone) {
            return std::nullopt;
        }
        scope_id = *parsed_zone;
        text = text.substr(0, zone);
    }
    const auto groups = detail::parse_ipv6_address(text);
    if (!groups) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < groups->size(); ++i) {
        bytes[2 * i] = static_cast<std::uint8_t>((*groups)[i] >> 8);
        bytes[2 * i + 1] = static_cast<std::uint8_t>((*groups)[i] & 0xffU);
    }
    return ip_address::ipv6(bytes, scope_id);
}

std::pmr::string ip_address::to_string(std::pmr::memory_resource* resource) const {
    // "ffff:" * 7 + "ffff" + "%" + 10 zone digits.
    std::array<char, 56> buffer;
    char* const end = buffer.data() + buffer.size();
    char* cursor = buffer.data();
    if (family_ == ip_address_family::ipv4) {
        cursor = append_ipv4(cursor, end, bytes_.data());
    } else {
        bool mapped = bytes_[10] == 0xffU && bytes_[11] == 0xffU;
        for (std::size_t i = 0; mapped && i < 10; ++i) {
            mapped = bytes_[i] == 0;
        }
        if (mapped) {
            const std::string_view prefix = "::ffff:";
            cursor = std::copy(prefix.begin(), prefix.end(), cursor);
            cursor = append_ipv4(cursor, end, bytes_.data() + 12);
        } else {
            std::array<std::uint16_t, 8> groups{};
            for (std::size_t i = 0; i < groups.size(); ++i) {
                groups[i] = static_cast<std::uint16_t>(bytes_[2 * i] << 8 | bytes_[2 * i + 1]);
            }
            // RFC 5952 §4.2: compress the first longest run of two or more
            // zero groups.
            std::size_t best_begin = groups.size();
            std::size_t best_size = 1;
            for (std::size_t i = 0; i < groups.size();) {
                if (groups[i] != 0) {
                    ++i;
                    continue;
                }
                std::size_t run_end = i;
                while (run_end < groups.size() && groups[run_end] == 0) {
                    ++run_end;
                }
                if (run_end - i > best_size) {
                    best_begin = i;
                    best_size = run_end - i;
                }
                i = run_end;
            }
            for (std::size_t i = 0; i < groups.size();) {
                if (i == best_begin) {
                    *cursor++ = ':';
                    *cursor++ = ':';
                    i += best_size;
                    continue;
                }
                if (i != 0 && i != best_begin + best_size) {
                    *cursor++ = ':';
                }
                cursor = std::to_chars(cursor, end, groups[i], 16).ptr;
                ++i;
            }
        }
        if (scope_id_ != 0) {
            *cursor++ = '%';
            cursor = std::to_chars(cursor, end, scope_id_).ptr;
        }
    }
    return std::pmr::string(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()),
        resource == nullptr ? std::pmr::get_default_resource() : resource);
}

}  // namespace ruvia
