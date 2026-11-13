#include "ruvia/http/proxy_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/ip_address.h"

#include "parser/http_uri_grammar.h"

namespace ruvia {
namespace {

constexpr std::string_view v1_signature = "PROXY ";
// Spec §2.1: the longest line, "PROXY UNKNOWN" with two IPv6 addresses and
// two ports, including CRLF.
constexpr std::size_t v1_max_line_bytes = 107;
constexpr std::array<std::uint8_t, 12> v2_signature{
    0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};
constexpr std::size_t v2_fixed_bytes = 16;
constexpr std::size_t v2_inet_block_bytes = 12;
constexpr std::size_t v2_inet6_block_bytes = 36;
constexpr std::size_t v2_unix_block_bytes = 216;
constexpr std::size_t v2_tlv_header_bytes = 3;

constexpr std::uint8_t af_unspec = 0x0;
constexpr std::uint8_t af_inet = 0x1;
constexpr std::uint8_t af_inet6 = 0x2;
constexpr std::uint8_t af_unix = 0x3;

[[noreturn]] void reject(const char* reason) {
    throw proxy_protocol_error(reason);
}

[[nodiscard]] constexpr std::uint8_t byte_value(std::byte value) noexcept {
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] constexpr std::uint16_t load_big_endian_16(std::span<const std::byte> bytes) noexcept {
    return static_cast<std::uint16_t>((byte_value(bytes[0]) << 8) | byte_value(bytes[1]));
}

// Spec §2.2: the address block that precedes TLVs; AF_UNSPEC has none whose
// size is known.
[[nodiscard]] constexpr std::size_t v2_address_block_bytes(std::uint8_t family) noexcept {
    switch (family) {
        case af_inet:
            return v2_inet_block_bytes;
        case af_inet6:
            return v2_inet6_block_bytes;
        case af_unix:
            return v2_unix_block_bytes;
        default:
            return 0;
    }
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Spec §2.1: canonical address text of the announced family, without a zone.
[[nodiscard]] std::optional<ip_address> parse_v1_address(std::string_view text, ip_address_family family) noexcept {
    if (text.find('%') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto address = ip_address::parse(text);
    if (!address || address->family() != family) {
        return std::nullopt;
    }
    return address;
}

// Spec §2.1: decimal 0..65535 without heading zeroes.
[[nodiscard]] std::optional<std::uint16_t> parse_v1_port(std::string_view text) noexcept {
    if (text.empty() || text.size() > 5 || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    if (value > 0xFFFF) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// Splits "<src> <dst> <sport> <dport>" at single spaces; an empty field or a
// field count other than four rejects.
[[nodiscard]] std::optional<std::array<std::string_view, 4>> split_v1_fields(std::string_view text) noexcept {
    std::array<std::string_view, 4> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto space = text.find(' ');
        const bool last = i + 1 == fields.size();
        if (last != (space == std::string_view::npos)) {
            return std::nullopt;
        }
        fields[i] = text.substr(0, space);
        if (fields[i].empty()) {
            return std::nullopt;
        }
        if (!last) {
            text.remove_prefix(space + 1);
        }
    }
    return fields;
}

[[nodiscard]] ip_endpoint load_v2_endpoint(
    std::span<const std::byte> address, std::span<const std::byte> port, ip_address_family family) noexcept {
    if (family == ip_address_family::ipv4) {
        std::array<std::uint8_t, 4> bytes{};
        std::transform(address.begin(), address.end(), bytes.begin(), byte_value);
        return ip_endpoint(ip_address::ipv4(bytes), load_big_endian_16(port));
    }
    std::array<std::uint8_t, 16> bytes{};
    std::transform(address.begin(), address.end(), bytes.begin(), byte_value);
    return ip_endpoint(ip_address::ipv6(bytes), load_big_endian_16(port));
}

}  // namespace

proxy_protocol_parser::proxy_protocol_parser(proxy_protocol_parser_options options)
    : max_v2_header_bytes_(options.max_v2_header_bytes_) {
    if (max_v2_header_bytes_ < v2_fixed_bytes + v2_inet6_block_bytes) {
        throw std::invalid_argument("PROXY protocol v2 header limit cannot hold an AF_INET6 address block");
    }
}

proxy_protocol_parse_result proxy_protocol_parser::parse(std::span<const std::byte> input) & {
    if (state_ != proxy_protocol_parser_state::active) {
        throw std::logic_error("PROXY protocol parser is not active");
    }
    try {
        return parse_bytes(input);
    } catch (...) {
        // Fail closed: the caller cannot know how much of a rejected or
        // unbufferable input was taken.
        state_ = proxy_protocol_parser_state::failed;
        throw;
    }
}

const proxy_protocol_header& proxy_protocol_parser::header() const& {
    if (state_ != proxy_protocol_parser_state::finished) {
        throw std::logic_error("PROXY protocol header is not complete");
    }
    return header_;
}

void proxy_protocol_parser::reset() noexcept {
    buffer_.clear();
    tlvs_.clear();
    header_ = proxy_protocol_header();
    v2_header_size_ = 0;
    v2_family_ = 0;
    phase_ = parse_phase::signature;
    state_ = proxy_protocol_parser_state::active;
}

proxy_protocol_parse_result proxy_protocol_parser::parse_bytes(std::span<const std::byte> input) {
    std::size_t position = 0;
    while (position < input.size() && state_ == proxy_protocol_parser_state::active) {
        switch (phase_) {
            case parse_phase::signature:
                position = consume_signature(input, position);
                break;
            case parse_phase::v1_line:
                position = consume_v1_line(input, position);
                break;
            case parse_phase::v2_fixed:
                position = consume_v2_fixed(input, position);
                break;
            case parse_phase::v2_block:
                position = consume_v2_block(input, position);
                break;
        }
    }
    return {.complete_ = state_ == proxy_protocol_parser_state::finished, .consumed_bytes_ = position};
}

// The first byte selects the only signature it can begin ('P' or CR), so each
// later byte either continues that signature or rules out both.
std::size_t proxy_protocol_parser::consume_signature(std::span<const std::byte> input, std::size_t position) {
    const auto value = byte_value(input[position]);
    const auto index = buffer_.size();
    if (index == 0) {
        if (value == static_cast<std::uint8_t>(v1_signature.front())) {
            header_.version_ = proxy_protocol_version::v1;
        } else if (value == v2_signature.front()) {
            header_.version_ = proxy_protocol_version::v2;
        } else {
            reject("input does not begin a PROXY protocol signature");
        }
    } else {
        const auto expected = header_.version_ == proxy_protocol_version::v1
                                  ? static_cast<std::uint8_t>(v1_signature[index])
                                  : v2_signature[index];
        if (value != expected) {
            reject("input does not begin a PROXY protocol signature");
        }
    }
    buffer_.push_back(input[position]);
    if (header_.version_ == proxy_protocol_version::v1 && buffer_.size() == v1_signature.size()) {
        phase_ = parse_phase::v1_line;
    } else if (header_.version_ == proxy_protocol_version::v2 && buffer_.size() == v2_signature.size()) {
        phase_ = parse_phase::v2_fixed;
    }
    return position + 1;
}

// Spec §2.1: the line ends at the first CRLF within 107 bytes; a lone CR or LF
// rejects. Bytes before the end are only searched for CR and LF here; the
// fields are decoded once, after the CRLF arrives.
std::size_t proxy_protocol_parser::consume_v1_line(std::span<const std::byte> input, std::size_t position) {
    // The signature ends in a space, so a buffered trailing CR is a line's CR
    // whose LF did not arrive in the previous input.
    if (buffer_.back() == std::byte{'\r'}) {
        if (input[position] != std::byte{'\n'}) {
            reject("PROXY protocol v1 CR is not followed by LF");
        }
        buffer_.push_back(input[position]);
        finish_v1();
        return position + 1;
    }
    const auto chunk = input.subspan(position, std::min(input.size() - position, v1_max_line_bytes - buffer_.size()));
    const auto stop = std::find_if(chunk.begin(), chunk.end(),
        [](std::byte value) { return value == std::byte{'\r'} || value == std::byte{'\n'}; });
    if (stop != chunk.end() && *stop == std::byte{'\n'}) {
        reject("PROXY protocol v1 line ends in a bare LF");
    }
    const auto taken = static_cast<std::size_t>(stop - chunk.begin()) + (stop == chunk.end() ? 0 : 1);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(taken));
    // Full without CRLF, or a CR that leaves no room for its LF.
    if (buffer_.size() >= v1_max_line_bytes) {
        reject("PROXY protocol v1 line exceeds 107 bytes");
    }
    return position + taken;
}

std::size_t proxy_protocol_parser::consume_v2_fixed(std::span<const std::byte> input, std::size_t position) {
    const auto value = byte_value(input[position]);
    switch (buffer_.size()) {
        case 12:
            // Spec §2.2: the receiver must only accept version 2 and must drop
            // unassigned commands.
            if ((value >> 4) != 0x2) {
                reject("PROXY protocol v2 version is not 2");
            }
            switch (value & 0x0F) {
                case 0x0:
                    header_.command_ = proxy_protocol_command::local;
                    break;
                case 0x1:
                    header_.command_ = proxy_protocol_command::proxy;
                    break;
                default:
                    reject("PROXY protocol v2 command is unassigned");
            }
            break;
        case 13:
            v2_family_ = static_cast<std::uint8_t>(value >> 4);
            if (v2_family_ > af_unix) {
                reject("PROXY protocol v2 address family is unspecified");
            }
            switch (value & 0x0F) {
                case 0x0:
                    header_.transport_ = proxy_protocol_transport::unspecified;
                    break;
                case 0x1:
                    header_.transport_ = proxy_protocol_transport::stream;
                    break;
                case 0x2:
                    header_.transport_ = proxy_protocol_transport::datagram;
                    break;
                default:
                    reject("PROXY protocol v2 transport protocol is unspecified");
            }
            break;
        default:
            break;
    }
    buffer_.push_back(input[position]);
    if (buffer_.size() < v2_fixed_bytes) {
        return position + 1;
    }
    const std::size_t length = load_big_endian_16(std::span<const std::byte>(buffer_).subspan(14, 2));
    if (length > max_v2_header_bytes_ - v2_fixed_bytes) {
        reject("PROXY protocol v2 header exceeds the configured limit");
    }
    if (header_.command_ == proxy_protocol_command::proxy && length < v2_address_block_bytes(v2_family_)) {
        reject("PROXY protocol v2 length does not cover the address block");
    }
    v2_header_size_ = v2_fixed_bytes + length;
    phase_ = parse_phase::v2_block;
    if (length == 0) {
        finish_v2();
    }
    return position + 1;
}

// Spec §2.2: the receiver must not parse an address before the whole block
// has arrived.
std::size_t proxy_protocol_parser::consume_v2_block(std::span<const std::byte> input, std::size_t position) {
    const auto taken = std::min(input.size() - position, v2_header_size_ - buffer_.size());
    const auto chunk = input.subspan(position, taken);
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    if (buffer_.size() == v2_header_size_) {
        finish_v2();
    }
    return position + taken;
}

void proxy_protocol_parser::finish_v1() {
    // Without the signature and the CRLF.
    const auto line = as_text(std::span<const std::byte>(buffer_)).substr(v1_signature.size(), buffer_.size() - v1_signature.size() - 2);
    header_.command_ = proxy_protocol_command::proxy;
    constexpr std::string_view unknown = "UNKNOWN";
    if (line.starts_with(unknown) && (line.size() == unknown.size() || line[unknown.size()] == ' ')) {
        // Spec §2.1: anything after UNKNOWN is ignored and the real endpoints
        // are used.
        header_.transport_ = proxy_protocol_transport::unspecified;
        state_ = proxy_protocol_parser_state::finished;
        return;
    }
    const bool tcp4 = line.starts_with("TCP4 ");
    if (!tcp4 && !line.starts_with("TCP6 ")) {
        reject("PROXY protocol v1 protocol is not TCP4, TCP6 or UNKNOWN");
    }
    const auto family = tcp4 ? ip_address_family::ipv4 : ip_address_family::ipv6;
    const auto fields = split_v1_fields(line.substr(5));
    if (!fields) {
        reject("PROXY protocol v1 line does not have four single-space separated fields");
    }
    const auto source_address = parse_v1_address((*fields)[0], family);
    const auto destination_address = parse_v1_address((*fields)[1], family);
    if (!source_address || !destination_address) {
        reject("PROXY protocol v1 address is not canonical text of the announced family");
    }
    const auto source_port = parse_v1_port((*fields)[2]);
    const auto destination_port = parse_v1_port((*fields)[3]);
    if (!source_port || !destination_port) {
        reject("PROXY protocol v1 port is not a decimal 0..65535 without heading zeroes");
    }
    header_.transport_ = proxy_protocol_transport::stream;
    header_.source_ = ip_endpoint(*source_address, *source_port);
    header_.destination_ = ip_endpoint(*destination_address, *destination_port);
    state_ = proxy_protocol_parser_state::finished;
}

void proxy_protocol_parser::finish_v2() {
    // Spec §2.2: LOCAL discards the block, family included. With AF_UNSPEC the
    // address length is unknown, so no TLV boundary can be located.
    if (header_.command_ == proxy_protocol_command::local || v2_family_ == af_unspec) {
        state_ = proxy_protocol_parser_state::finished;
        return;
    }
    const auto block = std::span<const std::byte>(buffer_).subspan(v2_fixed_bytes);
    if (header_.transport_ != proxy_protocol_transport::unspecified && v2_family_ != af_unix) {
        const auto family = v2_family_ == af_inet ? ip_address_family::ipv4 : ip_address_family::ipv6;
        const std::size_t address_bytes = family == ip_address_family::ipv4 ? 4 : 16;
        const auto ports = block.subspan(2 * address_bytes, 4);
        header_.source_ = load_v2_endpoint(block.subspan(0, address_bytes), ports.subspan(0, 2), family);
        header_.destination_ = load_v2_endpoint(block.subspan(address_bytes, address_bytes), ports.subspan(2, 2), family);
    }
    // Spec §2.2.3: PP2_TYPE_CRC32C verification is a MAY; every TLV, that one
    // included, is exposed raw.
    auto rest = block.subspan(v2_address_block_bytes(v2_family_));
    while (!rest.empty()) {
        if (rest.size() < v2_tlv_header_bytes) {
            reject("PROXY protocol v2 TLV header overruns the header");
        }
        const std::size_t value_bytes = load_big_endian_16(rest.subspan(1, 2));
        if (value_bytes > rest.size() - v2_tlv_header_bytes) {
            reject("PROXY protocol v2 TLV value overruns the header");
        }
        tlvs_.push_back({.type_ = byte_value(rest[0]), .value_ = rest.subspan(v2_tlv_header_bytes, value_bytes)});
        rest = rest.subspan(v2_tlv_header_bytes + value_bytes);
    }
    header_.tlvs_ = tlvs_;
    state_ = proxy_protocol_parser_state::finished;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] std::optional<std::uint32_t> parse_zone(std::string_view text) noexcept {
    if (text.empty() || text.front() == '0') {
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

std::string ip_address::to_string() const {
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
    return std::string(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()));
}

namespace {

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> to_mapped_ipv6_bytes(
    const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        const auto v4 = address.bytes();
        return std::array<std::uint8_t, 16>{
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, v4[0], v4[1], v4[2], v4[3]};
    }
    // Scoped IPv6 literals compare by their 128-bit address prefix in CIDR blocks.
    std::array<std::uint8_t, 16> v6{};
    const auto raw = address.bytes();
    std::copy_n(raw.data(), 16, v6.data());
    return v6;
}

}  // namespace

std::optional<ip_cidr> ip_cidr::parse(std::string_view text) noexcept {
    const auto slash = text.find('/');
    const auto address_text = slash == std::string_view::npos ? text : text.substr(0, slash);
    const auto parsed = ip_address::parse(address_text);
    if (!parsed) {
        return std::nullopt;
    }
    const auto mapped = to_mapped_ipv6_bytes(*parsed);
    if (!mapped) {
        return std::nullopt;
    }
    const bool was_v4 = parsed->family() == ip_address_family::ipv4;
    const unsigned max_bits = was_v4 ? 32U : 128U;
    unsigned bits = max_bits;
    if (slash != std::string_view::npos) {
        const auto prefix_text = text.substr(slash + 1);
        if (prefix_text.empty() || prefix_text.size() > 3) {
            return std::nullopt;
        }
        const auto [end, err] =
            std::from_chars(prefix_text.data(), prefix_text.data() + prefix_text.size(), bits);
        if (err != std::errc{} || end != prefix_text.data() + prefix_text.size() ||
            bits > max_bits) {
            return std::nullopt;
        }
    }
    ip_cidr result;
    result.network_ = *mapped;
    result.prefix_bits_ = static_cast<std::uint8_t>(was_v4 ? bits + 96U : bits);
    return result;
}

bool ip_cidr::contains(const ip_address& address) const noexcept {
    const auto peer = to_mapped_ipv6_bytes(address);
    if (!peer) {
        return false;
    }
    const std::size_t full_bytes = prefix_bits_ / 8U;
    const std::uint8_t rem_bits = prefix_bits_ % 8U;
    if (!std::equal(network_.begin(), network_.begin() + full_bytes, peer->begin())) {
        return false;
    }
    if (rem_bits == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xffU << (8U - rem_bits));
    return (network_[full_bytes] & mask) == ((*peer)[full_bytes] & mask);
}

bool ip_cidr::contains(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && contains(*parsed);
}

}  // namespace ruvia
