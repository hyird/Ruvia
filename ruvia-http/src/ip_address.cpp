#include "ip_address.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "decimal_text.h"
#include "http_uri_syntax.h"

namespace ruvia {
namespace {

[[nodiscard]] std::optional<std::uint32_t> parse_zone(std::string_view text) noexcept {
    if (text.empty() || text.front() == '0') {
        return std::nullopt;
    }
    return parse_integer<std::uint32_t>(text);
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

bool ip_address::is_loopback() const noexcept {
    if (family_ == ip_address_family::ipv4 || is_ipv4_mapped()) {
        return bytes_[family_ == ip_address_family::ipv4 ? 0 : 12] == 127;
    }
    return std::ranges::all_of(std::span(bytes_).first<15>(), [](auto byte) {
        return byte == 0;
    }) &&
        bytes_[15] == 1;
}

bool ip_address::is_multicast() const noexcept {
    return family_ == ip_address_family::ipv4 || is_ipv4_mapped() ? (bytes_[family_ == ip_address_family::ipv4 ? 0 : 12] & 0xf0) == 0xe0 : bytes_[0] == 0xff;
}

bool ip_address::is_link_local() const noexcept {
    if (family_ == ip_address_family::ipv4 || is_ipv4_mapped()) {
        const auto* bytes = bytes_.data() + (family_ == ip_address_family::ipv4 ? 0 : 12);
        return bytes[0] == 169 && bytes[1] == 254;
    }
    return bytes_[0] == 0xfe && (bytes_[1] & 0xc0) == 0x80;
}

bool ip_address::is_public_unicast() const noexcept {
    if (scope_id_ != 0) {
        return false;
    }
    const bool mapped = is_ipv4_mapped();
    const auto* b = bytes_.data() + (mapped ? 12 : 0);
    if (family_ == ip_address_family::ipv4 || mapped) {
        if (b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 || (b[0] == 100 && (b[1] & 0xc0) == 64) || (b[0] == 169 && b[1] == 254) || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168) || (b[0] == 198 && (b[1] & 0xfe) == 18)) {
            return false;
        }
        if (b[0] == 192 && b[1] == 0 && b[2] == 0) {
            return b[3] == 9 || b[3] == 10;
        }
        return !(b[0] == 192 && b[1] == 0 && b[2] == 2) && !(b[0] == 192 && b[1] == 88 && b[2] == 99) && !(b[0] == 198 && b[1] == 51 && b[2] == 100) && !(b[0] == 203 && b[1] == 0 && b[2] == 113);
    }
    return (b[0] & 0xe0) == 0x20 && !(b[0] == 0x20 && b[1] == 0x01 && b[2] < 2) && !(b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) && !(b[0] == 0x20 && b[1] == 0x02) && !(b[0] == 0x3f && b[1] == 0xfe) && !(b[0] == 0x3f && b[1] == 0xff && (b[2] & 0xf0) == 0);
}

std::string_view ip_address::format_to(std::span<char, max_text_size> output) const noexcept {
    char* const end = output.data() + output.size();
    char* cursor = output.data();
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
    return {output.data(), static_cast<std::size_t>(cursor - output.data())};
}

std::string ip_address::to_string() const {
    std::array<char, max_text_size> buffer;
    return std::string(format_to(buffer));
}

std::optional<ip_cidr> ip_cidr::parse(std::string_view text) noexcept {
    const auto slash = text.find('/');
    const auto address_text = slash == std::string_view::npos ? text : text.substr(0, slash);
    const auto parsed = ip_address::parse(address_text);
    if (!parsed || address_text.find('%') != std::string_view::npos) {
        return std::nullopt;
    }
    const bool was_v4 = parsed->family() == ip_address_family::ipv4;
    const unsigned max_bits = was_v4 ? 32U : 128U;
    unsigned bits = max_bits;
    if (slash != std::string_view::npos) {
        const auto prefix_text = text.substr(slash + 1);
        const auto prefix = prefix_text.size() > 3 ? std::nullopt : parse_integer<unsigned>(prefix_text);
        if (!prefix || *prefix > max_bits) {
            return std::nullopt;
        }
        bits = *prefix;
    }
    ip_cidr result;
    const auto bytes = parsed->bytes();
    if (was_v4) {
        result.network_[10] = 0xff;
        result.network_[11] = 0xff;
    }
    std::copy(bytes.begin(), bytes.end(), result.network_.begin() + (was_v4 ? 12 : 0));
    const unsigned mapped_bits = was_v4 ? bits + 96U : bits;
    for (unsigned index = mapped_bits / 8U; index < result.network_.size(); ++index) {
        const auto host_mask = static_cast<std::uint8_t>(index == mapped_bits / 8U && mapped_bits % 8U != 0 ? 0xffU >> (mapped_bits % 8U) : 0xffU);
        if ((result.network_[index] & host_mask) != 0) {
            return std::nullopt;
        }
    }
    result.prefix_bits_ = static_cast<std::uint8_t>(bits);
    result.family_ = parsed->family();
    return result;
}

ip_address ip_cidr::network() const noexcept {
    return family_ == ip_address_family::ipv4 ? ip_address::ipv4({network_[12], network_[13], network_[14], network_[15]}) : ip_address::ipv6(network_);
}

ip_address ip_cidr::last_address() const noexcept {
    auto bytes = network_;
    const unsigned bits = family_ == ip_address_family::ipv4 ? prefix_bits_ + 96U : prefix_bits_;
    for (unsigned index = bits / 8U; index < bytes.size(); ++index) {
        bytes[index] |= static_cast<std::uint8_t>(index == bits / 8U && bits % 8U != 0 ? 0xffU >> (bits % 8U) : 0xffU);
    }
    return family_ == ip_address_family::ipv4 ? ip_address::ipv4({bytes[12], bytes[13], bytes[14], bytes[15]}) : ip_address::ipv6(bytes);
}

bool ip_cidr::contains(const ip_address& address) const noexcept {
    if (family_ == ip_address_family::ipv4 && address.scope_id() != 0) {
        return false;
    }
    const auto peer = address.bytes();
    const bool ipv4 = address.family() == ip_address_family::ipv4;
    const auto peer_byte = [&peer, ipv4](std::size_t index) noexcept -> std::uint8_t {
        if (!ipv4) {
            return peer[index];
        }
        return index >= 12 ? peer[index - 12] : static_cast<std::uint8_t>(index >= 10 ? 0xff : 0);
    };
    const unsigned bits = family_ == ip_address_family::ipv4 ? prefix_bits_ + 96U : prefix_bits_;
    const std::size_t full_bytes = bits / 8U;
    const std::uint8_t rem_bits = bits % 8U;
    for (std::size_t index = 0; index < full_bytes; ++index) {
        if (network_[index] != peer_byte(index)) {
            return false;
        }
    }
    if (rem_bits == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xffU << (8U - rem_bits));
    return (network_[full_bytes] & mask) == (peer_byte(full_bytes) & mask);
}

bool ip_cidr::contains(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && contains(*parsed);
}

}  // namespace ruvia
