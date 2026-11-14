#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"

#include "http_connection_fields.h"

namespace ruvia {
namespace {

constexpr std::string_view v1_signature = "PROXY ";
// Spec §2.1: the longest line, "PROXY UNKNOWN" with two IPv6 addresses and
// two ports, including CRLF.
constexpr std::size_t v1_max_line_bytes = 107;
constexpr std::array<std::uint8_t, 12> v2_signature{0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};
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

[[nodiscard]] ip_endpoint load_v2_endpoint(std::span<const std::byte> address, std::span<const std::byte> port, ip_address_family family) noexcept {
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

proxy_protocol_parser::proxy_protocol_parser(proxy_protocol_parser_options options) :
    max_v2_header_bytes_(options.max_v2_header_bytes_) {
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
        const auto expected = header_.version_ == proxy_protocol_version::v1 ? static_cast<std::uint8_t>(v1_signature[index]) : v2_signature[index];
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
    const auto stop = std::find_if(chunk.begin(), chunk.end(), [](std::byte value) {
        return value == std::byte{'\r'} || value == std::byte{'\n'};
    });
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

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> to_mapped_ipv6_bytes(const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        const auto v4 = address.bytes();
        return std::array<std::uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, v4[0], v4[1], v4[2], v4[3]};
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
        const auto [end, err] = std::from_chars(prefix_text.data(), prefix_text.data() + prefix_text.size(), bits);
        if (err != std::errc{} || end != prefix_text.data() + prefix_text.size() || bits > max_bits) {
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

namespace ruvia {

http_connect_udp_target::http_connect_udp_target(std::string host, port_number port) noexcept :
    host_(std::move(host)),
    port_(port.value()) {}

namespace {
constexpr std::string_view masque_udp_prefix{"/.well-known/masque/udp/"};
bool valid_host(std::string_view host) {
    return !host.empty() && host.find('%') == std::string_view::npos && (host.find(':') != std::string_view::npos ? detail::parse_ipv6_address(host).has_value() : detail::is_valid_reg_name(host));
}
// RFC 9297 section 3.2: these statuses cannot start the Capsule Protocol.
constexpr bool forbids_capsule_protocol(std::uint16_t status) noexcept {
    return status == 204 || status == 205 || status == 206;
}
[[nodiscard]] bool is_http_or_https_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}
enum class handshake_message_role : std::uint8_t {
    request_recipient,
    request_sender,
    response_recipient,
    response_sender,
};
enum class capsule_protocol_state : std::uint8_t {
    absent,
    valid_true,
    invalid,
};
struct handshake_fields {
    capsule_protocol_state capsule_protocol_{capsule_protocol_state::absent};
    bool upgrade_{false}, upgrade_field_{false}, connection_{false}, connection_field_{false};
    bool host_{false}, forbidden_{false}, http2_forbidden_{false};
    std::string_view host_value_{};
};
template <typename header_type>
std::variant<handshake_fields, http_connect_udp_error> scan_handshake_fields(std::span<const header_type> headers, handshake_message_role role, http_connect_udp_error syntax_error) noexcept {
    const auto connection_role = role == handshake_message_role::request_recipient ? detail::http_connection_field_role::request_recipient : (role == handshake_message_role::response_recipient ? detail::http_connection_field_role::response_recipient : detail::http_connection_field_role::sender);
    const auto list_role = (role == handshake_message_role::request_recipient || role == handshake_message_role::response_recipient) ? detail::http_field_list_role::recipient : detail::http_field_list_role::sender;
    handshake_fields found;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp_offered = false;
    for (const auto& field : headers) {
        if (!is_valid_http_header_name(field.name()) || !is_valid_http_header_value(field.value())) {
            return syntax_error;
        }
        if (detail::is_forbidden_http_binary_connection_field(field.name())) {
            found.http2_forbidden_ = true;
        } else if (http_ascii_equals_ignore_case(field.name(), "te")) {
            if (role == handshake_message_role::response_recipient || role == handshake_message_role::response_sender || !detail::is_valid_binary_http_te_field_value(field.value(), list_role)) {
                found.http2_forbidden_ = true;
            }
        }
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            // RFC 9297 section 3.4: recipients handle a non-Boolean value as if the
            // field were absent; ?0 has the same meaning as absence. Only senders
            // require a single ?1 Capsule-Protocol field.
            if (list_role == detail::http_field_list_role::sender) {
                const auto value = parse_http_capsule_protocol(field.value());
                found.capsule_protocol_ = (found.capsule_protocol_ == capsule_protocol_state::absent && value == std::optional<bool>(true)) ? capsule_protocol_state::valid_true : capsule_protocol_state::invalid;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            found.upgrade_field_ = true;
            if (upgrade_protocols.parse_field(field.value(), list_role, [&](const detail::http_upgrade_protocol& protocol) noexcept {
                const bool is_connect_udp = protocol.version_.empty() && http_ascii_equals_ignore_case(protocol.name_, "connect-udp");
                if (role == handshake_message_role::request_recipient) {
                    if (is_connect_udp) {
                        connect_udp_offered = true;
                    }
                    return true;
                }
                if (!is_connect_udp || (role != handshake_message_role::response_recipient && connect_udp_offered)) {
                    return false;
                }
                connect_udp_offered = true;
                return true;
            }) != detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            found.connection_field_ = true;
            if (connection_options.parse_field(field.value(), connection_role) != detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
            found.connection_ = connection_options.contains(detail::http_connection_option::upgrade);
        } else if (http_ascii_equals_ignore_case(field.name(), "host")) {
            const auto trimmed_host = http_trim_ows(field.value());
            if (found.host_ || !parse_http_authority(trimmed_host).has_value()) {
                return syntax_error;
            }
            found.host_ = true;
            found.host_value_ = trimmed_host;
        } else if (detail::http_field_name_in(field.name(), detail::http_connect_forbidden_fields)) {
            // RFC 9297 Section 3.2: the Capsule Protocol MUST NOT be used with
            // messages carrying Content-Length, Content-Type, or Transfer-Encoding
            // (the list also covers Content-Encoding and Trailer); a receiver that
            // observes one treats the message as malformed.
            found.forbidden_ = true;
        }
    }
    found.upgrade_ = upgrade_protocols.has_protocol() && connect_udp_offered;
    if (found.upgrade_field_ && !found.upgrade_) {
        return syntax_error;
    }
    return found;
}
template <typename header_type>
std::optional<http_connect_udp_error> validate_connect_udp_response(http_protocol_version version, std::uint16_t status, std::span<const header_type> headers) noexcept {
    if (version == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_response;
    }
    const bool extended_connect = version != http_protocol_version::http11;
    const auto parsed_value = scan_handshake_fields(headers, handshake_message_role::response_recipient, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    // The connect-udp token, not Capsule-Protocol, selects the Capsule Protocol
    // (RFC 9298 section 3); RFC 9297 section 3.2 makes 204/205/206 malformed.
    if (found.forbidden_ || (extended_connect ? status < 200 || status >= 300 || forbids_capsule_protocol(status) || found.upgrade_field_ || found.connection_field_ || found.http2_forbidden_ : status != 101 || !found.upgrade_ || !found.connection_)) {
        return http_connect_udp_error::invalid_response;
    }
    return std::nullopt;
}
}  // namespace
std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target) {
    if (!valid_host(target.host_) || target.port_ == 0) {
        return http_connect_udp_error::invalid_target;
    }
    std::size_t encoded_host_size = 0;
    for (const unsigned char c : target.host_.view()) {
        encoded_host_size += detail::is_unreserved_byte(c) ? 1U : 3U;
    }
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = http_format_unsigned(port_buffer, target.port_.value());
    std::string result_value;
    result_value.reserve(masque_udp_prefix.size() + encoded_host_size + 1U + port_digits.size() + 1U);
    result_value.append(masque_udp_prefix);
    for (const unsigned char c : target.host_.view()) {
        if (detail::is_unreserved_byte(c)) {
            result_value.push_back(static_cast<char>(c));
        } else {
            http_append_percent_byte(result_value, c);
        }
    }
    result_value.push_back('/');
    result_value.append(port_digits);
    result_value.push_back('/');
    return result_value;
}
std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path) {
    if (!path.starts_with(masque_udp_prefix) || path.size() <= masque_udp_prefix.size() || !path.ends_with('/')) {
        return http_connect_udp_error::invalid_path;
    }
    path.remove_prefix(masque_udp_prefix.size());
    path.remove_suffix(1);
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || path.find('/', slash + 1) != std::string_view::npos) {
        return http_connect_udp_error::invalid_path;
    }
    const auto raw_host = path.substr(0, slash);
    // Colons in IPv6 addresses must be escaped in this URI-template variable.
    if (raw_host.find(':') != std::string_view::npos) {
        return http_connect_udp_error::invalid_target;
    }
    const auto port = detail::parse_port_value(path.substr(slash + 1));
    if ((port.index() != 0) || !std::get<0>(port)) {
        return http_connect_udp_error::invalid_target;
    }
    std::string host;
    const auto decoded_host = decode_url_component(raw_host, host);
    if (!decoded_host || !valid_host(*decoded_host)) {
        return http_connect_udp_error::invalid_target;
    }
    if (decoded_host->data() == raw_host.data()) {
        host.assign(raw_host);
    }
    return http_connect_udp_target{std::move(host), std::get<0>(port)};
}
namespace {
std::optional<http_connect_udp_error> validate_request(http_connect_udp_request_view request, bool sending) noexcept {
    const auto authority = parse_http_authority(request.authority_);
    if (!authority || authority->host().empty() || !is_valid_http_origin_form_target(request.path_)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto parsed_value = scan_handshake_fields(request.headers_.view(), sending ? handshake_message_role::request_sender : handshake_message_role::request_recipient, http_connect_udp_error::invalid_request);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (sending && found.capsule_protocol_ != capsule_protocol_state::valid_true) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.forbidden_ || request.authority_.empty() || request.path_.empty()) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_request;
    }
    if (found.host_) {
        const auto default_port = detail::http_uri_scheme_default_port(request.scheme_);
        if (!http_authorities_equal(request.authority_, found.host_value_, default_port)) {
            return http_connect_udp_error::invalid_request;
        }
    }
    if (request.version_ != http_protocol_version::http11) {
        if (request.method_.view() != "CONNECT" || request.protocol_.view() != "connect-udp" || !is_http_or_https_scheme(request.scheme_) || found.connection_field_ || found.upgrade_field_ || found.http2_forbidden_) {
            return http_connect_udp_error::invalid_request;
        }
    } else if (request.method_.view() != "GET" || (!request.scheme_.empty() && !is_http_or_https_scheme(request.scheme_)) || !found.upgrade_ || !found.connection_ || !found.host_) {
        return http_connect_udp_error::invalid_request;
    }
    return std::nullopt;
}
}  // namespace
std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept {
    return validate_request(request, false);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(http_connect_udp_request_view request) noexcept {
    return validate_request(request, true);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(http_protocol_version version, std::uint16_t status, std::span<const http_header> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 || request.known_method() != http_known_method::get) {
        return false;
    }
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp = false;
    const bool valid = detail::http_request_access::visit_known_headers(request, detail::http_field_kind::upgrade, [&](std::string_view value) noexcept {
        return upgrade_protocols.parse_field(value, detail::http_field_list_role::recipient, [&connect_udp](const detail::http_upgrade_protocol& protocol) noexcept {
            if (protocol.version_.empty() && http_ascii_equals_ignore_case(protocol.name_, "connect-udp")) {
                connect_udp = true;
            }
            return true;
        }) == detail::http_field_list_parse_status::ok;
    });
    return valid && upgrade_protocols.has_protocol() && connect_udp;
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept {
    if (request.target_form() == http_request_target_form::absolute && !detail::is_valid_uri_component(request.query_string(), true, true)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto path = request.target_form() == http_request_target_form::absolute ? request.path() : request.target();
    return validate_http_connect_udp_request({.version_ = request.protocol_version(), .method_ = request.method(), .protocol_ = request.protocol(), .scheme_ = request.scheme(), .authority_ = request.authority().empty() ? detail::http_request_access::known_header(request, detail::http_field_kind::host) : request.authority(), .path_ = path, .headers_ = request.headers()});
}
std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version) {
    if (version == http_protocol_version::http10 || !response.status().is_successful() || forbids_capsule_protocol(response.status().value()) || response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    // The response's own fields go through the one CONNECT-UDP handshake walk;
    // the protocol, not the application, generates the Upgrade pair.
    const auto parsed_value = scan_handshake_fields(std::span(response.headers().begin(), response.headers().size()), handshake_message_role::response_sender, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (found.upgrade_field_ || found.connection_field_ || found.forbidden_ || (version != http_protocol_version::http11 && found.http2_forbidden_)) {
        return http_connect_udp_error::invalid_response;
    }
    // As the sender, the planner only lets a single ?1 through.
    if (found.capsule_protocol_ == capsule_protocol_state::invalid) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.capsule_protocol_ == capsule_protocol_state::absent) {
        response.header_stable_view("Capsule-Protocol", "?1");
    }
    if (version == http_protocol_version::http11) {
        response.status_code_ = http_status::switching_protocols;
        response.header_stable_view(detail::http_connection_field_name, detail::http_connection_upgrade_option);
        response.header_stable_view("Upgrade", "connect-udp");
    }
    return response;
}
http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response) {
    if (response.status() != http_status::switching_protocols || response.file_body() || !response.body_bytes().empty() || validate_connect_udp_response(http_protocol_version::http11, response.status().value(), std::span(response.headers().begin(), response.headers().size()))) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT-UDP response head");
    }
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, response.status()), http1_request_connection_plan::http11_close());
}
}  // namespace ruvia

namespace ruvia {
namespace {
// CONNECT-UDP context ID 0 (RFC 9298 section 5) as its one-byte variable-length integer.
constexpr std::string_view udp_payload_context_id{"\0", 1};
}  // namespace
// One DATAGRAM capsule (RFC 9297 section 3.5) whose value is `context_prefix`
// followed by the borrowed payload.
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::plan_datagram_capsule(std::span<const char> payload_value, std::string_view context_prefix) noexcept {
    if (payload_value.size() > detail::quic_varint_max - context_prefix.size()) {
        return http_datagram_error::payload_too_large;
    }
    http_datagram_write_plan plan;
    plan.payload_ = payload_value;
    const auto header = encode_http_capsule_header(plan.prefix_, http_datagram_capsule_type, context_prefix.size() + payload_value.size());
    if (header.index() != 0) {
        return http_datagram_error::payload_too_large;
    }
    plan.prefix_size_ = std::get<0>(header);
    std::ranges::copy(context_prefix, plan.prefix_.data() + plan.prefix_size_);
    plan.prefix_size_ += context_prefix.size();
    return plan;
}
std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept {
    detail::http_structured_parser parser{detail::http_structured_text_input{value}};
    parser.spaces();
    detail::http_structured_item item;
    if (!parser.item(item) || !parser.parameters()) {
        return std::nullopt;
    }
    // RFC 8941 4.2: only SP surrounds a top-level Item; OWS (with HTAB)
    // belongs between List and Dictionary members.
    parser.spaces();
    if (!parser.empty() || item.kind_ != detail::http_structured_item::kind_type::boolean) {
        return std::nullopt;
    }
    return item.boolean_;
}

std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept {
    auto id = detail::decode_quic_varint(input);
    if (id.index() != 0) {
        return std::nullopt;
    }
    return http_udp_datagram_view{std::get<0>(id).value_, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return detail::encode_quic_varint(output, context_id);
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > detail::quic_varint_max || length > detail::quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto type_size = detail::quic_varint_size(type);
    const auto length_size = detail::quic_varint_size(length);
    if (output.size() < type_size + length_size) {
        return http_capsule_codec_error::output_too_small;
    }
    (void)detail::encode_quic_varint(output, type);
    (void)detail::encode_quic_varint(output.subspan(type_size), length);
    return type_size + length_size;
}
http_capsule_decoder::http_capsule_decoder(http_capsule_config config) :
    config_(config) {
    if (config.max_capsule_length_ > detail::quic_varint_max) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
http_capsule_status http_capsule_decoder::feed(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, false, retention).status_;
}
http_capsule_feed_result http_capsule_decoder::feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, true, retention);
}
http_capsule_feed_result http_capsule_decoder::feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, bool one, http_capsule_retention retention) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (status_ != http_capsule_status::need_more_data) {
        return {.status_ = status_};
    }
    struct feeding_guard final {
        bool& value_;
        explicit feeding_guard(bool& value) noexcept :
            value_(value) {
            value_ = true;
        }
        ~feeding_guard() {
            value_ = false;
        }
    } feeding_scope(feeding_);
    const auto initial_size = input.size();
    bool completed{};
    const auto limit_reached = [&] {
        status_ = http_capsule_status::limit;
        return http_capsule_feed_result{.status_ = status_, .consumed_bytes_ = initial_size - input.size()};
    };
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[header_size_++] = input.front();
                input = input.subspan(1);
                if (type_size_ == 0) {
                    auto type = detail::decode_quic_varint(std::span<const char>(header_).first(header_size_));
                    if ((type.index() != 0)) {
                        continue;
                    }
                    type_ = std::get<0>(type).value_;
                    type_size_ = std::get<0>(type).encoded_bytes_;
                }
                auto length = detail::decode_quic_varint(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
                if ((length.index() != 0)) {
                    continue;
                }
                remaining_ = std::get<0>(length).value_;
                header_size_ = 0;
                type_size_ = 0;
                const bool kept_type = retention == http_capsule_retention::all || type_ == http_datagram_capsule_type;
                if (!kept_type) {
                    // RFC 9297 section 3.2: drop and skip over it, at any length.
                    mode_ = payload_mode::skip;
                } else if (retention == http_capsule_retention::udp_payloads && remaining_ != 0) {
                    // RFC 9298 section 4: an unknown Context ID drops the
                    // datagram, so the limit waits for the Context ID.
                    mode_ = payload_mode::context_id;
                    context_id_size_ = 0;
                } else if (remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                } else {
                    mode_ = payload_mode::deliver;
                }
                payload_ = remaining_ != 0;
                if (!payload_ && kept_type) {
                    if (callback_value) {
                        callback_value(context_value, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else if (mode_ == payload_mode::skip) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
            } else if (mode_ == payload_mode::context_id) {
                context_id_[context_id_size_++] = input.front();
                input = input.subspan(1);
                --remaining_;
                const auto context_id = detail::decode_quic_varint(std::span<const char>(context_id_).first(context_id_size_));
                if (context_id.index() != 0 && remaining_ != 0) {
                    continue;
                }
                payload_ = remaining_ != 0;
                if (context_id.index() == 0 && std::get<0>(context_id).value_ != 0) {
                    mode_ = payload_mode::skip;
                    continue;
                }
                // Context ID 0, or a value too short to hold its Context ID.
                if (context_id_size_ + remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                }
                mode_ = payload_mode::deliver;
                if (callback_value) {
                    callback_value(context_value, {type_, std::span<const char>(context_id_).first(context_id_size_), !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else {
                auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                auto part = input.first(count);
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
                if (callback_value) {
                    callback_value(context_value, {type_, part, !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            }
        }
        if (fin && input.empty()) {
            status_ = payload_ || header_size_ ? http_capsule_status::truncated : http_capsule_status::end;
        }
        return {.status_ = status_, .consumed_bytes_ = initial_size - input.size(), .capsule_complete_ = completed};
    } catch (...) {
        status_ = http_capsule_status::truncated;
        throw;
    }
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    return plan_datagram_capsule(payload_value, {});
}
std::optional<std::span<const char>> http_datagram_session::receive_datagram(std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    return input;
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_udp_datagram(std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (payload_value.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return plan_datagram_capsule(payload_value, udp_payload_context_id);
}
std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> http_datagram_session::receive_udp_datagram(std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    const auto udp = decode_http_udp_datagram(input);
    if (!udp.has_value()) {
        return http_datagram_error::malformed;
    }
    if (udp->context_id_ != 0) {
        return std::nullopt;
    }
    if (udp->payload_.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return *udp;
}
}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t value, unsigned int shift) noexcept {
    return (value << shift) | (value >> (32U - shift));
}

[[nodiscard]] constexpr std::uint32_t rotr32(std::uint32_t value, unsigned int shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

[[nodiscard]] std::array<char, 32> md5_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
    static constexpr std::array<unsigned int, 64> s_table = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::uint32_t a0 = 0x67452301U;
    std::uint32_t b0 = 0xefcdab89U;
    std::uint32_t c0 = 0x98badcfeU;
    std::uint32_t d0 = 0x10325476U;

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8U) | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16U) | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24U);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl32(a + f + k_table[i] + m[g], s_table[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 8 + i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    std::array<std::uint8_t, 16> digest{};
    const std::array<std::uint32_t, 4> words = {a0, b0, c0, d0};
    for (std::size_t i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(words[i] & 0xffU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, 32> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i * 2] = hex_digits[(digest[i] >> 4U) & 0x0fU];
        out[i * 2 + 1] = hex_digits[digest[i] & 0x0fU];
    }
    return out;
}

[[nodiscard]] std::array<char, 64> sha256_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) | static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + ch + k_table[i] + w[i];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 1 - i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, 64> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            const auto byte = static_cast<std::uint8_t>((state[i] >> ((3U - b) * 8U)) & 0xffU);
            const std::size_t pos = (i * 4 + b) * 2;
            out[pos] = hex_digits[(byte >> 4U) & 0x0fU];
            out[pos + 1] = hex_digits[byte & 0x0fU];
        }
    }
    return out;
}

template <typename int_type>
void append_decimal_list(std::string& out, std::span<const int_type> items) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        http_append_unsigned(out, static_cast<std::uint64_t>(items[index]));
    }
}

void append_hex_u16(std::string& out, std::uint16_t value) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    out.push_back(hex_digits[(value >> 12U) & 0x0fU]);
    out.push_back(hex_digits[(value >> 8U) & 0x0fU]);
    out.push_back(hex_digits[(value >> 4U) & 0x0fU]);
    out.push_back(hex_digits[value & 0x0fU]);
}

[[nodiscard]] std::string_view ja4_version_code(std::uint16_t version) noexcept {
    switch (version) {
        case 0x0304U:
            return "13";
        case 0x0303U:
            return "12";
        case 0x0302U:
            return "11";
        case 0x0301U:
            return "10";
        case 0x0300U:
            return "s3";
        case 0x0002U:
            return "s2";
        case 0xfeffU:
            return "d1";
        case 0xfefdU:
            return "d2";
        case 0xfefcU:
            return "d3";
        default:
            return "00";
    }
}

[[nodiscard]] constexpr bool is_ascii_alnum(char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

}  // namespace

std::uint16_t tls_client_hello_info::version() const noexcept {
    if (!supported_versions_.empty()) {
        std::uint16_t highest = 0;
        for (const auto ver : supported_versions_) {
            if (ver > highest) {
                highest = ver;
            }
        }
        return highest;
    }
    return legacy_version_;
}

std::string tls_client_hello_info::ja3_raw() const {
    std::string out;
    http_append_unsigned(out, legacy_version_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back(',');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    return out;
}

std::string tls_client_hello_info::ja3() const {
    const auto raw = ja3_raw();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

std::string tls_client_hello_info::ja4(bool quic) const {
    std::string out;
    out.reserve(36);

    out.push_back(quic ? 'q' : 't');
    out.append(ja4_version_code(version()));

    const bool has_domain_sni = !server_name_.empty() && !ip_address::parse(server_name_).has_value();
    out.push_back(has_domain_sni ? 'd' : 'i');

    const auto append_two_digits = [&out](std::size_t count) {
        const auto clamped = (std::min)(std::size_t{99}, count);
        out.push_back(static_cast<char>('0' + (clamped / 10U)));
        out.push_back(static_cast<char>('0' + (clamped % 10U)));
    };
    append_two_digits(cipher_suites_.size());
    append_two_digits(extensions_.size());

    if (alpn_.empty() || alpn_.front().empty()) {
        out.append("00");
    } else {
        const std::string_view first_alpn = alpn_.front();
        const char first_char = first_alpn.front();
        const char last_char = first_alpn.back();
        if (is_ascii_alnum(first_char) && is_ascii_alnum(last_char)) {
            out.push_back(first_char);
            out.push_back(last_char);
        } else {
            static constexpr char hex_digits[] = "0123456789abcdef";
            const auto first_byte = static_cast<unsigned char>(first_char);
            out.push_back(hex_digits[(first_byte >> 4U) & 0x0fU]);
            out.push_back(hex_digits[first_byte & 0x0fU]);
        }
    }
    out.push_back('_');

    if (cipher_suites_.empty()) {
        out.append("000000000000");
    } else {
        std::vector<std::uint16_t> sorted_ciphers(cipher_suites_);
        std::ranges::sort(sorted_ciphers);
        std::string cipher_list;
        cipher_list.reserve(sorted_ciphers.size() * 5U);
        for (std::size_t index = 0; index < sorted_ciphers.size(); ++index) {
            if (index != 0) {
                cipher_list.push_back(',');
            }
            append_hex_u16(cipher_list, sorted_ciphers[index]);
        }
        const auto hash_b = sha256_hex_lower(cipher_list);
        out.append(hash_b.data(), 12U);
    }
    out.push_back('_');

    std::vector<std::uint16_t> filtered_exts;
    filtered_exts.reserve(extensions_.size());
    for (const auto ext : extensions_) {
        if (ext != 0x0000U && ext != 0x0010U) {
            filtered_exts.push_back(ext);
        }
    }
    std::ranges::sort(filtered_exts);

    if (filtered_exts.empty() && signature_algorithms_.empty()) {
        out.append("000000000000");
    } else {
        std::string ext_sig_list;
        for (std::size_t index = 0; index < filtered_exts.size(); ++index) {
            if (index != 0) {
                ext_sig_list.push_back(',');
            }
            append_hex_u16(ext_sig_list, filtered_exts[index]);
        }
        if (!signature_algorithms_.empty()) {
            ext_sig_list.push_back('_');
            for (std::size_t index = 0; index < signature_algorithms_.size(); ++index) {
                if (index != 0) {
                    ext_sig_list.push_back(',');
                }
                append_hex_u16(ext_sig_list, signature_algorithms_[index]);
            }
        }
        const auto hash_c = sha256_hex_lower(ext_sig_list);
        out.append(hash_c.data(), 12U);
    }

    return out;
}

std::string tls_client_hello_info::peetprint() const {
    std::string out;
    if (!supported_versions_.empty()) {
        append_decimal_list<std::uint16_t>(out, supported_versions_);
    } else {
        http_append_unsigned(out, legacy_version_);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back('|');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    out.push_back('|');
    for (std::size_t index = 0; index < alpn_.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        out.append(alpn_[index]);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, signature_algorithms_);
    return out;
}

std::string tls_client_hello_info::peetprint_hash() const {
    const auto raw = peetprint();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

tls_client_hello_fingerprints tls_client_hello_info::fingerprints(bool quic) const {
    tls_client_hello_fingerprints result;
    result.ja3_raw_ = ja3_raw();
    const auto ja3_md5 = md5_hex_lower(result.ja3_raw_);
    result.ja3_.assign(ja3_md5.data(), ja3_md5.size());
    result.ja4_ = ja4(quic);
    result.peetprint_ = peetprint();
    const auto peet_md5 = md5_hex_lower(result.peetprint_);
    result.peetprint_hash_.assign(peet_md5.data(), peet_md5.size());
    return result;
}

std::optional<tls_client_hello_info> parse_tls_client_hello(std::span<const std::byte> message) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(message.data());
    std::size_t size = message.size();

    // Unwrap optional 5-byte TLS record header (ContentType::handshake = 0x16).
    if (size >= 9 && data[0] == 0x16U && data[1] == 0x03U) {
        const std::size_t record_len = (static_cast<std::size_t>(data[3]) << 8U) | static_cast<std::size_t>(data[4]);
        if (record_len == size - 5U && data[5] == 0x01U) {
            data += 5U;
            size -= 5U;
        }
    }

    // Handshake message header: msg_type == 1 (ClientHello) + 3-byte length.
    if (size < 4 || data[0] != 0x01U) {
        return std::nullopt;
    }
    const std::size_t body_len = (static_cast<std::size_t>(data[1]) << 16U) | (static_cast<std::size_t>(data[2]) << 8U) | static_cast<std::size_t>(data[3]);
    if (body_len != size - 4U) {
        return std::nullopt;
    }

    const std::uint8_t* const body = data + 4U;
    std::size_t cursor = 0;
    const auto read_u8 = [&](std::uint8_t& out) noexcept {
        if (cursor + 1U > body_len) {
            return false;
        }
        out = body[cursor++];
        return true;
    };
    const auto read_u16 = [&](std::uint16_t& out) noexcept {
        if (cursor + 2U > body_len) {
            return false;
        }
        out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(body[cursor]) << 8U) | body[cursor + 1U]);
        cursor += 2U;
        return true;
    };

    tls_client_hello_info info;
    if (!read_u16(info.legacy_version_)) {
        return std::nullopt;
    }
    // 32-byte ClientHello random.
    if (cursor + 32U > body_len) {
        return std::nullopt;
    }
    cursor += 32U;

    // Session ID (0..32 bytes).
    std::uint8_t session_id_len = 0;
    if (!read_u8(session_id_len) || session_id_len > 32U || cursor + session_id_len > body_len) {
        return std::nullopt;
    }
    cursor += session_id_len;

    // Cipher suites (2-byte byte length, non-empty, even).
    std::uint16_t cipher_bytes = 0;
    if (!read_u16(cipher_bytes) || cipher_bytes < 2U || (cipher_bytes % 2U) != 0 || cursor + cipher_bytes > body_len) {
        return std::nullopt;
    }
    const std::size_t cipher_count = cipher_bytes / 2U;
    info.cipher_suites_.reserve(cipher_count);
    for (std::size_t i = 0; i < cipher_count; ++i) {
        std::uint16_t suite = 0;
        (void)read_u16(suite);
        if (!is_tls_grease_value(suite)) {
            info.cipher_suites_.push_back(suite);
        }
    }

    // Compression methods (1-byte length, non-empty).
    std::uint8_t comp_len = 0;
    if (!read_u8(comp_len) || comp_len == 0 || cursor + comp_len > body_len) {
        return std::nullopt;
    }
    cursor += comp_len;

    if (cursor == body_len) {
        return info;
    }

    std::uint16_t extensions_len = 0;
    if (!read_u16(extensions_len) || cursor + extensions_len != body_len) {
        return std::nullopt;
    }

    while (cursor < body_len) {
        std::uint16_t ext_type = 0;
        std::uint16_t ext_len = 0;
        if (!read_u16(ext_type) || !read_u16(ext_len) || cursor + ext_len > body_len) {
            return std::nullopt;
        }
        const std::uint8_t* const ext_data = body + cursor;
        cursor += ext_len;

        if (is_tls_grease_value(ext_type)) {
            continue;
        }
        info.extensions_.push_back(ext_type);

        switch (ext_type) {
            case 0x0000U: {  // server_name
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    if (pos + 3U > ext_len) {
                        return std::nullopt;
                    }
                    const std::uint8_t name_type = ext_data[pos];
                    const std::size_t name_len = (static_cast<std::size_t>(ext_data[pos + 1U]) << 8U) | ext_data[pos + 2U];
                    pos += 3U;
                    if (name_len == 0 || pos + name_len > ext_len) {
                        return std::nullopt;
                    }
                    if (name_type == 0 && info.server_name_.empty()) {
                        info.server_name_.assign(reinterpret_cast<const char*>(ext_data + pos), name_len);
                    }
                    pos += name_len;
                }
                break;
            }
            case 0x000aU: {  // supported_groups
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto group = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(group)) {
                        info.supported_groups_.push_back(group);
                    }
                }
                break;
            }
            case 0x000bU: {  // ec_point_formats
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len == 0 || list_len != ext_len - 1U) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; ++pos) {
                    info.ec_point_formats_.push_back(ext_data[pos]);
                }
                break;
            }
            case 0x000dU: {  // signature_algorithms
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto scheme = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(scheme)) {
                        info.signature_algorithms_.push_back(scheme);
                    }
                }
                break;
            }
            case 0x0010U: {  // application_layer_protocol_negotiation
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len == 0 || list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    const std::size_t proto_len = ext_data[pos++];
                    if (proto_len == 0 || pos + proto_len > ext_len) {
                        return std::nullopt;
                    }
                    const bool grease_alpn = proto_len == 2U && is_tls_grease_value(static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]));
                    if (!grease_alpn) {
                        info.alpn_.emplace_back(reinterpret_cast<const char*>(ext_data + pos), proto_len);
                    }
                    pos += proto_len;
                }
                break;
            }
            case 0x002bU: {  // supported_versions
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len < 2U || list_len != ext_len - 1U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; pos += 2U) {
                    const auto ver = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(ver)) {
                        info.supported_versions_.push_back(ver);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return info;
}

std::optional<tls_client_hello_info> parse_tls_client_hello(std::string_view message) {
    return parse_tls_client_hello(std::span<const std::byte>(reinterpret_cast<const std::byte*>(message.data()), message.size()));
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::span<const std::byte> message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::string_view message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

}  // namespace ruvia

namespace ruvia::detail {

// RFC 6265bis section 5.6 storage limits. The sender rejects anything the
// receiving parser would truncate or drop, so set_cookie can find and replace
// cookies it emitted earlier by re-parsing them.
inline constexpr std::size_t max_cookie_name_value_bytes = 4096;
inline constexpr std::size_t max_cookie_attribute_value_bytes = 1024;

[[nodiscard]] inline bool is_valid_cookie_attribute(std::string_view value) noexcept {
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_cookie_domain(std::string_view value) noexcept {
    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    return value.empty() || is_valid_dns_domain_name(value);
}

[[nodiscard]] inline std::string_view cookie_priority_token(cookie_priority priority) noexcept {
    switch (priority) {
        case cookie_priority::low:
            return "Low";
        case cookie_priority::medium:
            return "Medium";
        case cookie_priority::high:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_same_site_token(cookie_same_site same_site) noexcept {
    switch (same_site) {
        case cookie_same_site::strict:
            return "Strict";
        case cookie_same_site::lax:
            return "Lax";
        case cookie_same_site::none:
            return "None";
    }
    return {};
}

[[nodiscard]] inline bool cookie_attribute_emitted(cookie_attribute_policy policy) {
    switch (policy) {
        case cookie_attribute_policy::omit:
            return false;
        case cookie_attribute_policy::emit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

// `secure` and `partitioned` are resolved attribute policies; the plan also
// checks Max-Age before converting it to unsigned storage.
inline void validate_cookie(std::string_view name, std::string_view value, const cookie_options& options, bool secure, bool partitioned) {
    if (!is_valid_http_header_name(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    const auto prefix_size = options.prefix_ ? ::ruvia::http_cookie_prefix_text(*options.prefix_).size() : 0;
    if (name.size() > max_cookie_name_value_bytes - prefix_size || value.size() > max_cookie_name_value_bytes - prefix_size - name.size()) {
        throw std::invalid_argument("cookie name and value exceed 4096 bytes");
    }
    // Receivers trim OWS around Path, so edge spaces would not round-trip and
    // trailing SP would end the Set-Cookie field value in whitespace.
    const std::string_view path = options.path_;
    if (!is_valid_cookie_attribute(path) || path.size() > max_cookie_attribute_value_bytes || http_trim_ows(path).size() != path.size()) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!is_valid_cookie_domain(options.domain_)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.expires_.has_value()) {
        using namespace std::chrono;
        constexpr auto minimum = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
        const auto expires = floor<seconds>(options.expires_->time_since_epoch());
        if (expires < minimum) {
            throw std::invalid_argument("cookie Expires must not precede year 1601");
        }
    }
    if (options.priority_ && cookie_priority_token(*options.priority_).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.same_site_ && cookie_same_site_token(*options.same_site_).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.same_site_ == cookie_same_site::none && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix_ && ::ruvia::http_cookie_prefix_text(*options.prefix_).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const auto prefix = options.prefix_ ? options.prefix_ : ::ruvia::cookie_name_prefix(name);
    if (prefix && !::ruvia::cookie_prefix_requirements_met(*prefix, secure, options.path_, options.domain_)) {
        throw std::invalid_argument(*prefix == cookie_prefix::host ? "__Host- cookie requires Secure, Path=/, and no Domain" : "__Secure- cookie requires Secure");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

set_cookie_plan::set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options) :
    name_(name),
    value_(value),
    path_(options.path_),
    domain_(options.domain_),
    prefix_text_(options.prefix_ ? http_cookie_prefix_text(*options.prefix_) : std::string_view{}),
    priority_text_(options.priority_ ? detail::cookie_priority_token(*options.priority_) : std::string_view{}),
    same_site_text_(options.same_site_ ? detail::cookie_same_site_token(*options.same_site_) : std::string_view{}),
    http_only_(detail::cookie_attribute_emitted(options.http_only_)),
    secure_(detail::cookie_attribute_emitted(options.secure_)),
    partitioned_(detail::cookie_attribute_emitted(options.partitioned_)) {
    // max-age-av starts with a non-zero digit (RFC 6265 section 4.1.1);
    // an immediate expiry is written as the past deletion Expires instead.
    const bool expire_now = options.max_age_.has_value() && options.max_age_->count() == 0;
    if (expire_now || options.expires_.has_value()) {
        const auto expires = expire_now ? cookie_deletion_expires : *options.expires_;
        const auto seconds = std::chrono::floor<std::chrono::seconds>(expires.time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = format_http_date(static_cast<std::time_t>(seconds));
        if (!date) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expires_size_ = date->size();
        std::memcpy(expires_buffer_.data(), date->data(), expires_size_);
    }
    if (options.max_age_.has_value() && options.max_age_->count() != 0) {
        if (options.max_age_->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        max_age_value_ = static_cast<std::uint64_t>(options.max_age_->count());
    }

    const auto add_size = [this](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size_) {
            throw std::length_error("Set-Cookie value is too large");
        }
        size_ += amount;
    };

    // Compute the complete wire length before validating the borrowed views.
    // This keeps a hostile oversized view from reaching a grammar scan and
    // prevents the later uninitialized response-header write from receiving a
    // wrapped length.
    add_size(prefix_text_.size());
    add_size(name_.size());
    add_size(1);
    add_size(value_.size());
    if (!path_.empty()) {
        add_size(std::string_view("; Path=").size());
        add_size(path_.size());
    }
    if (!domain_.empty()) {
        add_size(std::string_view("; Domain=").size());
        add_size(domain_.size());
    }
    if (max_age_value_ != 0) {
        add_size(std::string_view("; Max-Age=").size());
        add_size(http_unsigned_decimal_size(max_age_value_));
    }
    if (expires_size_ != 0) {
        add_size(std::string_view("; Expires=").size());
        add_size(expires_size_);
    }
    if (http_only_) {
        add_size(std::string_view("; HttpOnly").size());
    }
    if (secure_) {
        add_size(std::string_view("; Secure").size());
    }
    if (!same_site_text_.empty()) {
        add_size(std::string_view("; SameSite=").size());
        add_size(same_site_text_.size());
    }
    if (!priority_text_.empty()) {
        add_size(std::string_view("; Priority=").size());
        add_size(priority_text_.size());
    }
    if (partitioned_) {
        add_size(std::string_view("; Partitioned").size());
    }

    detail::validate_cookie(name, value, options, secure_, partitioned_);
}

char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value) {
    const std::string_view written(out, cookie_request_pair_size(name, value));
    const bool name_overlaps = detail::http_text_overlaps(name, written);
    const bool value_overlaps = detail::http_text_overlaps(value, written);
    if (name_overlaps || value_overlaps) {
        // Snapshot only bytes that writing the pair could overwrite.
        const auto name_bytes = name_overlaps ? name.size() : 0;
        const auto value_bytes = value_overlaps ? value.size() : 0;
        std::array<char, 256> stack_snapshot;
        if (value_bytes <= stack_snapshot.size() && name_bytes <= stack_snapshot.size() - value_bytes) {
            if (name_overlaps) {
                std::memcpy(stack_snapshot.data(), name.data(), name_bytes);
            }
            if (value_overlaps) {
                std::memcpy(stack_snapshot.data() + name_bytes, value.data(), value_bytes);
            }
            return write_cookie_request_pair(out, name_overlaps ? std::string_view(stack_snapshot.data(), name_bytes) : name, value_overlaps ? std::string_view(stack_snapshot.data() + name_bytes, value_bytes) : value);
        }
        std::string snapshot;
        snapshot.reserve(name_bytes + value_bytes);
        if (name_overlaps) {
            snapshot.append(name);
        }
        if (value_overlaps) {
            snapshot.append(value);
        }
        return write_cookie_request_pair(out, name_overlaps ? std::string_view(snapshot.data(), name_bytes) : name, value_overlaps ? std::string_view(snapshot.data() + name_bytes, value_bytes) : value);
    }
    detail::http_raw_wire_sink sink(out);
    sink.append(name);
    sink.append('=');
    sink.append(value);
    return sink.position();
}

void set_cookie_plan::write(char* cursor_value) const {
    (void)write_fields(cursor_value);
}

set_cookie_plan::written_fields set_cookie_plan::write_fields(char* cursor_value) const noexcept {
    written_fields fields;
    detail::http_raw_wire_sink sink(cursor_value);
    const auto* const name_begin = sink.position();
    sink.append(prefix_text_);
    sink.append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(sink.position() - name_begin)};
    sink.append('=');
    sink.append(value_);
    if (!path_.empty()) {
        sink.append("; Path=");
        fields.path_ = {sink.position(), path_.size()};
        sink.append(path_);
    }
    if (!domain_.empty()) {
        sink.append("; Domain=");
        fields.domain_ = {sink.position(), domain_.size()};
        sink.append(domain_);
    }
    if (max_age_value_ != 0) {
        // The constructor sized this slice with http_unsigned_decimal_size().
        sink.append("; Max-Age=");
        sink.append_unsigned(max_age_value_);
    }
    if (expires_size_ != 0) {
        sink.append("; Expires=");
        sink.append(std::string_view(expires_buffer_.data(), expires_size_));
    }
    if (http_only_) {
        sink.append("; HttpOnly");
    }
    if (secure_) {
        sink.append("; Secure");
    }
    if (!same_site_text_.empty()) {
        sink.append("; SameSite=");
        sink.append(same_site_text_);
    }
    if (!priority_text_.empty()) {
        sink.append("; Priority=");
        sink.append(priority_text_);
    }
    if (partitioned_) {
        sink.append("; Partitioned");
    }
    return fields;
}

}  // namespace ruvia

namespace ruvia {

bool is_valid_cookie_value(std::string_view value) noexcept {
    // cookie-value = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE ); the quotes
    // are part of the value (RFC 6265 section 4.1.1).
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept {
    const auto starts_with_ignore_case = [name](std::string_view prefix) noexcept {
        return name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix);
    };
    if (starts_with_ignore_case("__Host-")) {
        return cookie_prefix::host;
    }
    if (starts_with_ignore_case("__Secure-")) {
        return cookie_prefix::secure;
    }
    return std::nullopt;
}

bool cookie_prefix_requirements_met(cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return secure;
        case cookie_prefix::host:
            return secure && path == "/" && domain.empty();
    }
    return false;
}

std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return "__Secure-";
        case cookie_prefix::host:
            return "__Host-";
    }
    return {};
}

namespace {

[[nodiscard]] bool is_cookie_ip_host(std::string_view host) noexcept {
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        return true;
    }
    return ip_address::parse(host).has_value();
}

[[nodiscard]] std::chrono::system_clock::time_point compute_cookie_expiration(std::chrono::system_clock::time_point now, std::int64_t max_age_seconds) noexcept {
    using clock_type = std::chrono::system_clock;
    const auto available = std::chrono::duration_cast<std::chrono::seconds>(clock_type::time_point::max() - now);
    if (std::chrono::seconds(max_age_seconds) >= available) {
        return clock_type::time_point::max();
    }
    return now + std::chrono::duration_cast<clock_type::duration>(std::chrono::seconds(max_age_seconds));
}

[[nodiscard]] bool satisfies_cookie_name_prefix(const http_set_cookie_view& cookie, bool secure) noexcept {
    const auto prefix = cookie_name_prefix(cookie.name());
    return !prefix || cookie_prefix_requirements_met(*prefix, secure, cookie.path(), cookie.domain());
}

}  // namespace

http_cookie_jar::http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options) :
    http_cookie_jar(origin.scheme(), origin.host(), options) {}

http_cookie_jar::http_cookie_jar(http_scheme scheme, std::string_view host, http_cookie_jar_options options) :
    host_(host),
    scheme_(scheme),
    max_cookies_(options.max_cookies_),
    max_cookie_bytes_(options.max_cookie_bytes_) {
    if (!host_.empty() && host_.front() == '[' && host_.back() == ']') {
        host_.erase(host_.size() - 1, 1);
        host_.erase(0, 1);
    }
}

bool http_cookie_jar::cookie_domain_matches(std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) {
        return true;
    }
    if (http_ascii_equals_ignore_case(host, domain)) {
        return true;
    }
    if (is_cookie_ip_host(host)) {
        return false;
    }
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' && http_ascii_equals_ignore_case(host.substr(host.size() - domain.size()), domain);
}

bool http_cookie_jar::cookie_path_matches(std::string_view request_path, std::string_view cookie_path) noexcept {
    if (cookie_path.empty() || cookie_path == "/") {
        return !request_path.empty() && request_path.front() == '/';
    }
    if (!request_path.starts_with(cookie_path)) {
        return false;
    }
    return request_path.size() == cookie_path.size() || cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
}

std::string_view http_cookie_jar::request_path_only(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view http_cookie_jar::default_cookie_path(std::string_view target) noexcept {
    const auto path = request_path_only(target);
    if (path.empty() || path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/") : path.substr(0, slash);
}

std::size_t http_cookie_jar::entry_storage_bytes(std::string_view name, std::string_view value, std::string_view path, std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > (std::numeric_limits<std::size_t>::max)() - total) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        total += field.size();
    }
    return total;
}

bool http_cookie_jar::has_capacity(std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept {
    if (adding && cookies_.size() >= max_cookies_) {
        return false;
    }
    const auto retained_bytes = cookie_bytes_ - replaced_bytes;
    return replacement_bytes <= max_cookie_bytes_ - std::min(retained_bytes, max_cookie_bytes_);
}

void http_cookie_jar::reserve_cookie_slot() {
    if (cookies_.size() < cookies_.capacity()) {
        return;
    }
    const auto remaining = max_cookies_ - cookies_.size();
    const auto growth = std::min(remaining, std::max(std::size_t{4}, cookies_.size()));
    cookies_.reserve(cookies_.size() + growth);
}

void http_cookie_jar::recompute_next_expiry() noexcept {
    next_expiry_.reset();
    for (const auto& cookie : cookies_) {
        if (cookie.expires_.has_value() && (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    }
}

void http_cookie_jar::discard_expired(std::chrono::system_clock::time_point now) {
    std::optional<std::chrono::system_clock::time_point> earliest_expiry;
    std::erase_if(cookies_, [this, now, &earliest_expiry](const stored_cookie& cookie) {
        if (!cookie.expires_.has_value()) {
            return false;
        }
        if (*cookie.expires_ > now) {
            if (!earliest_expiry.has_value() || *cookie.expires_ < *earliest_expiry) {
                earliest_expiry = *cookie.expires_;
            }
            return false;
        }
        cookie_bytes_ -= entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        return true;
    });
    next_expiry_ = earliest_expiry;
}

void http_cookie_jar::clear() noexcept {
    cookies_.clear();
    next_expiry_.reset();
    cookie_bytes_ = 0;
}

void http_cookie_jar::add_configured_cookie(std::string_view name, std::string_view value) {
    cookies_.emplace_back(name, value);
    cookie_bytes_ += entry_storage_bytes(name, value, "/", {});
}

std::optional<http_cookie_jar::received_cookie> http_cookie_jar::accept_set_cookie_view(std::string_view request_target, std::string_view set_cookie_value, std::chrono::system_clock::time_point now) const {
    const auto parsed = parse_set_cookie(set_cookie_value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const bool secure = parsed->has(http_set_cookie_attribute::secure);
    const bool partitioned = parsed->has(http_set_cookie_attribute::partitioned);
    if ((secure && scheme_ != http_scheme::https) || !cookie_domain_matches(host_, parsed->domain()) || (parsed->same_site_none() && !secure) || (partitioned && !secure) || !satisfies_cookie_name_prefix(*parsed, secure)) {
        return std::nullopt;
    }

    received_cookie cookie{
        .name_ = parsed->name(),
        .value_ = parsed->value(),
        .path_ = parsed->path().empty() ? default_cookie_path(request_target) : parsed->path(),
        .domain_ = parsed->domain(),
        .expires_ = std::nullopt,
        .secure_ = secure,
        .partitioned_ = partitioned,
    };
    if (const auto max_age_seconds = parsed->max_age_seconds()) {
        cookie.expired_ = *max_age_seconds <= 0;
        if (!cookie.expired_) {
            cookie.expires_ = compute_cookie_expiration(now, *max_age_seconds);
        }
    } else if (const auto expires_at = parsed->expires()) {
        cookie.expired_ = *expires_at <= std::chrono::system_clock::to_time_t(now);
        if (!cookie.expired_) {
            const auto expiration_limit = std::chrono::system_clock::to_time_t(compute_cookie_expiration(now, max_cookie_age_seconds));
            cookie.expires_ = std::chrono::system_clock::from_time_t(std::min(*expires_at, expiration_limit));
        }
    }
    return cookie;
}

bool http_cookie_jar::store_received_cookie(const received_cookie& cookie) {
    const auto identity_domain = [this](std::string_view domain) {
        return domain.empty() ? std::string_view(host_) : domain;
    };
    const auto cookie_domain = identity_domain(cookie.domain_);
    const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& stored) {
        return !stored.configured_ && stored.name_ == cookie.name_ && stored.path_ == cookie.path_ && stored.partitioned_ == cookie.partitioned_ && http_ascii_equals_ignore_case(identity_domain(stored.domain_), cookie_domain);
    });
    const auto replaced_bytes = match == cookies_.end() ? 0 : entry_storage_bytes(match->name_, match->value_, match->path_, match->domain_);
    if (cookie.expired_) {
        if (match != cookies_.end()) {
            const bool removed_earliest = match->expires_.has_value() && next_expiry_ == match->expires_;
            cookie_bytes_ -= replaced_bytes;
            cookies_.erase(match);
            if (removed_earliest) {
                recompute_next_expiry();
            }
        }
        return true;
    }
    const auto replacement_bytes = entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
    if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
        return false;
    }

    if (match == cookies_.end()) {
        stored_cookie replacement(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        replacement.expires_ = cookie.expires_;
        replacement.secure_ = cookie.secure_;
        replacement.partitioned_ = cookie.partitioned_;
        replacement.configured_ = false;
        const auto insertion = std::ranges::find_if(cookies_, [&cookie](const stored_cookie& stored) {
            return stored.path_.size() < cookie.path_.size();
        });
        const auto insertion_index = insertion - cookies_.begin();
        reserve_cookie_slot();
        cookies_.emplace(cookies_.begin() + insertion_index, std::move(replacement));
        if (cookie.expires_.has_value() && (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    } else {
        const auto old_expires = match->expires_;
        std::optional<std::string> replacement_value;
        if (match->value_ != cookie.value_ && cookie.value_.size() > match->value_.capacity()) {
            replacement_value.emplace(cookie.value_);
        }
        std::optional<std::string> replacement_domain;
        if (match->domain_ != cookie.domain_ && cookie.domain_.size() > match->domain_.capacity()) {
            replacement_domain.emplace(cookie.domain_);
        }
        if (replacement_value.has_value()) {
            match->value_.swap(*replacement_value);
        } else if (match->value_ != cookie.value_) {
            match->value_.assign(cookie.value_);
        }
        if (replacement_domain.has_value()) {
            match->domain_.swap(*replacement_domain);
        } else if (match->domain_ != cookie.domain_) {
            match->domain_.assign(cookie.domain_);
        }
        match->expires_ = cookie.expires_;
        match->secure_ = cookie.secure_;
        match->partitioned_ = cookie.partitioned_;
        if (cookie.expires_.has_value() && (!next_expiry_.has_value() || *cookie.expires_ <= *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        } else if (old_expires.has_value() && next_expiry_ == old_expires) {
            recompute_next_expiry();
        }
    }
    cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
    return true;
}

bool http_cookie_jar::absorb_set_cookie(std::string_view request_target, std::string_view set_cookie_value, std::chrono::system_clock::time_point now) {
    if (!cookies_.empty() && next_expiry_.has_value() && now >= *next_expiry_) {
        discard_expired(now);
    }
    const auto accepted = accept_set_cookie_view(request_target, set_cookie_value, now);
    if (!accepted.has_value()) {
        return false;
    }
    return store_received_cookie(*accepted);
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target, std::span<const http_header> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target, std::span<const http_header_view> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

std::string http_cookie_jar::cookie_header(std::string_view request_target, bool include_received, std::chrono::system_clock::time_point now) {
    std::string output;
    visit_matching_cookies(request_target, include_received, now, [&output](std::string_view name, std::string_view value) {
        append_cookie_request_pair(output, name, value);
    });
    return output;
}

}  // namespace ruvia

namespace ruvia {
namespace {

std::pair<std::string_view, std::string_view> split_attribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {http_trim_ows(value), {}};
    }
    return {http_trim_ows(value.substr(0, equals)), http_trim_ows(value.substr(equals + 1))};
}

std::optional<std::int64_t> parse_max_age_seconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t seconds = 0;
    const auto [parsed_value, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    // Even overflow must consume the complete attribute before saturation.
    if (parsed_value != value.data() + value.size()) {
        return std::nullopt;
    }
    if (error == std::errc::result_out_of_range) {
        return value.front() == '-' ? std::numeric_limits<std::int64_t>::min() : max_cookie_age_seconds;
    }
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return seconds > max_cookie_age_seconds ? max_cookie_age_seconds : seconds;
}

bool contains_rejected_received_cookie_control(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x08 || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

bool is_cookie_date_delimiter(unsigned char byte) noexcept {
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) || (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
    std::size_t digits = 0;
    int result_value = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        if (digits == maximum_digits) {
            return std::nullopt;
        }
        result_value = result_value * 10 + (value[digits] - '0');
        ++digits;
    }
    if (digits < minimum_digits) {
        return std::nullopt;
    }
    value.remove_prefix(digits);
    return result_value;
}

struct cookie_date_time final {
    int hour_;
    int minute_;
    int second_;
};

std::optional<cookie_date_time> parse_cookie_date_time(std::string_view value) noexcept {
    const auto hour = consume_cookie_date_digits(value, 1, 2);
    if (!hour || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto minute = consume_cookie_date_digits(value, 1, 2);
    if (!minute || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto second = consume_cookie_date_digits(value, 1, 2);
    if (!second) {
        return std::nullopt;
    }
    return cookie_date_time{*hour, *minute, *second};
}

std::optional<int> parse_cookie_date_month(std::string_view value) noexcept {
    if (value.size() < 3) {
        return std::nullopt;
    }
    value = value.substr(0, 3);
    for (std::size_t index = 0; index < detail::http_month_names.size(); ++index) {
        if (http_ascii_equals_ignore_case(value, detail::http_month_names[index])) {
            return static_cast<int>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> parse_cookie_date(std::string_view value) noexcept {
    std::optional<cookie_date_time> time;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t cursor_value = 0;
    while (cursor_value < value.size()) {
        while (cursor_value < value.size() && is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        const auto begin = cursor_value;
        while (cursor_value < value.size() && !is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        if (begin == cursor_value) {
            continue;
        }
        auto token = value.substr(begin, cursor_value - begin);
        if (!time) {
            time = parse_cookie_date_time(token);
            if (time) {
                continue;
            }
        }
        if (!day) {
            day = consume_cookie_date_digits(token, 1, 2);
            if (day) {
                continue;
            }
        }
        if (!month) {
            month = parse_cookie_date_month(token);
            if (month) {
                continue;
            }
        }
        if (!year) {
            year = consume_cookie_date_digits(token, 2, 4);
        }
    }

    if (!time || !day || !month || !year) {
        return std::nullopt;
    }
    if (*year >= 70 && *year <= 99) {
        *year += 1900;
    } else if (*year <= 69) {
        *year += 2000;
    }
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour_ > 23 || time->minute_ > 59 || time->second_ > 59) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, *month, *day, time->hour_, time->minute_, time->second_);
}

}  // namespace

std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept {
    const auto first_end = value.find(';');
    const auto cookie_pair = value.substr(0, first_end);
    // RFC 6265 section 5.2 steps 2 and 5: a pair without '=' or with an empty
    // name makes the user agent ignore the whole set-cookie-string.
    const auto equals = cookie_pair.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto name = http_trim_ows(cookie_pair.substr(0, equals));
    const auto cookie_value = http_trim_ows(cookie_pair.substr(equals + 1));
    if (name.empty() || name.size() > detail::max_cookie_name_value_bytes || cookie_value.size() > detail::max_cookie_name_value_bytes - name.size() || contains_rejected_received_cookie_control(name) || contains_rejected_received_cookie_control(cookie_value)) {
        return std::nullopt;
    }

    http_set_cookie_view result(name, cookie_value);
    std::size_t offset = first_end == std::string_view::npos ? value.size() : first_end + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = split_attribute(value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (contains_rejected_received_cookie_control(attribute) || contains_rejected_received_cookie_control(argument)) {
            return std::nullopt;
        }
        if (argument.size() <= detail::max_cookie_attribute_value_bytes) {
            switch (attribute.size()) {
                case 4U:
                    if (http_ascii_equals_ignore_case(attribute, "Path")) {
                        // RFC 6265 section 5.2.4: a value that is empty or does not start
                        // with '/' selects the default-path; the last Path attribute wins.
                        result.path_ = argument.empty() || argument.front() != '/' ? std::string_view{} : argument;
                    }
                    break;
                case 6U:
                    if (http_ascii_equals_ignore_case(attribute, "Domain")) {
                        // RFC 6265 section 5.2.3: an empty value ignores this cookie-av; a
                        // leading '.' is dropped, so "Domain=." yields a host-only cookie.
                        // Domain-match against the request host is the storage step's job.
                        if (!argument.empty()) {
                            result.domain_ = argument.front() == '.' ? argument.substr(1) : argument;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Secure")) {
                        result.set(http_set_cookie_attribute::secure);
                    }
                    break;
                case 7U:
                    if (http_ascii_equals_ignore_case(attribute, "Expires")) {
                        if (const auto expires = parse_cookie_date(argument)) {
                            result.expires_ = expires;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Max-Age")) {
                        if (const auto seconds = parse_max_age_seconds(argument)) {
                            result.max_age_seconds_ = seconds;
                        }
                    }
                    break;
                case 8U:
                    if (http_ascii_equals_ignore_case(attribute, "SameSite")) {
                        if (http_ascii_equals_ignore_case(argument, "Strict")) {
                            result.same_site_ = cookie_same_site::strict;
                        } else if (http_ascii_equals_ignore_case(argument, "Lax")) {
                            result.same_site_ = cookie_same_site::lax;
                        } else if (http_ascii_equals_ignore_case(argument, "None")) {
                            result.same_site_ = cookie_same_site::none;
                        } else {
                            result.same_site_.reset();
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "HttpOnly")) {
                        result.set(http_set_cookie_attribute::http_only);
                    }
                    break;
                case 11U:
                    if (http_ascii_equals_ignore_case(attribute, "Partitioned")) {
                        result.set(http_set_cookie_attribute::partitioned);
                    }
                    break;
                default:
                    break;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] bool is_valid_cors_origin_value(std::string_view origin) noexcept {
    return origin == "null" || is_valid_http_serialized_origin(origin);
}

[[nodiscard]] std::string to_lower_copy(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
    }
    return result;
}

[[nodiscard]] std::string join_tokens(std::span<const std::string> items) {
    std::string output;
    if (items.empty()) {
        return output;
    }
    std::size_t total = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            total += 2U;
        }
        total += items[index].size();
    }
    output.reserve(total);
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        output.append(items[index]);
    }
    return output;
}

[[nodiscard]] bool vary_value_contains_token(std::string_view vary_value, std::string_view target_token) noexcept {
    bool found = false;
    detail::http_visit_comma_separated_tokens(vary_value, [&](std::string_view token) noexcept {
        if (token == "*" || http_ascii_equals_ignore_case(token, target_token)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

[[nodiscard]] bool headers_have_vary_token(std::span<const http_header> headers, std::string_view token) noexcept {
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Vary") && vary_value_contains_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool is_http_cors_controlled_response_header(std::string_view name) noexcept {
    switch (name.size()) {
        case 22U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Max-Age");
        case 27U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Origin");
        case 28U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Methods") || http_ascii_equals_ignore_case(name, "Access-Control-Allow-Headers");
        case 29U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Expose-Headers");
        case 32U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Credentials");
        default:
            return false;
    }
}

void merge_http_vary_tokens(std::vector<http_header>& headers, std::span<const std::string_view> tokens) {
    std::vector<std::string_view> missing;
    for (const auto token : tokens) {
        const auto trimmed = http_trim_ows(token);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed == "*") {
            if (!headers_have_vary_token(headers, "*")) {
                std::erase_if(headers, [](const http_header& field) noexcept {
                    return http_ascii_equals_ignore_case(field.name(), "Vary");
                });
                headers.emplace_back("Vary", "*");
            }
            return;
        }
        if (!is_valid_http_header_name(trimmed)) {
            throw std::invalid_argument("Vary token must be a valid HTTP field name");
        }
        if (headers_have_vary_token(headers, trimmed)) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(missing, [trimmed](std::string_view current) noexcept {
            return http_ascii_equals_ignore_case(current, trimmed);
        });
        if (!duplicate) {
            missing.push_back(trimmed);
        }
    }
    if (missing.empty()) {
        return;
    }

    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!http_ascii_equals_ignore_case(headers[index].name(), "Vary")) {
            continue;
        }
        std::string combined;
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "Vary")) {
                continue;
            }
            const auto trimmed = http_trim_ows(field.value());
            if (trimmed.empty()) {
                continue;
            }
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(trimmed);
        }
        for (const auto token : missing) {
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(token);
        }
        headers[index] = http_header("Vary", combined);
        for (std::size_t rest = headers.size(); rest > index + 1; --rest) {
            if (http_ascii_equals_ignore_case(headers[rest - 1].name(), "Vary")) {
                headers.erase(headers.begin() + static_cast<std::ptrdiff_t>(rest - 1));
            }
        }
        return;
    }

    std::string value;
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index != 0) {
            value.append(", ");
        }
        value.append(missing[index]);
    }
    headers.emplace_back("Vary", value);
}

void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token) {
    merge_http_vary_tokens(headers, std::span<const std::string_view>(&token, 1));
}

http_cors_policy::http_cors_policy(const http_cors_policy_config& config) :
    allow_any_origin_(config.allow_any_origin_),
    reflect_request_headers_(config.reflect_request_headers_),
    allow_credentials_(config.allow_credentials_) {
    const auto add_suffix = [this](std::string_view pattern) {
        if (pattern.empty()) {
            throw std::invalid_argument("CORS origin suffix must not be empty");
        }
        suffix_rule rule;
        auto rest = pattern;
        if (const auto scheme_pos = rest.find("://"); scheme_pos != std::string_view::npos) {
            const auto scheme = rest.substr(0, scheme_pos);
            if (!http_ascii_equals_ignore_case(scheme, "http") && !http_ascii_equals_ignore_case(scheme, "https")) {
                throw std::invalid_argument("CORS origin suffix scheme must be http or https");
            }
            rule.scheme_ = to_lower_copy(scheme);
            rest = rest.substr(scheme_pos + 3U);
        }
        if (rest.starts_with("*.")) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(2U);
        } else if (rest.starts_with('.')) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(1U);
        }
        const auto authority = parse_http_authority(rest);
        if (!authority || authority->host().empty() || authority->port_kind() == http_authority_port_kind::empty || authority->host().starts_with('[') || is_valid_http_ipv4_literal(authority->host()) || authority->host().front() == '.' || authority->host().back() == '.') {
            throw std::invalid_argument("CORS origin suffix must name a valid domain");
        }
        rule.host_suffix_ = to_lower_copy(authority->host());
        rule.port_ = authority->port();
        suffix_rules_.push_back(std::move(rule));
    };

    for (const auto& origin_entry : config.allowed_origins_) {
        const std::string_view view(origin_entry);
        if (view == "*") {
            allow_any_origin_ = true;
            continue;
        }
        if (view.starts_with("*.") || view.starts_with('.') || view.find("://*.") != std::string_view::npos || view.find("://.") != std::string_view::npos) {
            add_suffix(view);
            continue;
        }
        if (!is_valid_cors_origin_value(view)) {
            throw std::invalid_argument("CORS allowed origin must be a WHATWG serialized origin");
        }
        exact_origins_.emplace_back(view);
    }

    for (const auto& suffix_entry : config.allowed_origin_suffixes_) {
        add_suffix(suffix_entry);
    }
    if (exact_origins_.empty() && suffix_rules_.empty()) {
        allow_any_origin_ = true;
    }
    for (const auto& method : config.allowed_methods_) {
        const std::string_view view(method);
        if (view == "*") {
            allow_any_method_ = true;
            continue;
        }
        if (!is_valid_http_method_token(view)) {
            throw std::invalid_argument("CORS allowed method must be a valid HTTP method token");
        }
        allowed_methods_.emplace_back(view);
    }
    if (allowed_methods_.empty()) {
        allow_any_method_ = true;
    }
    joined_allowed_methods_ = join_tokens(allowed_methods_);

    for (const auto& header_name : config.allowed_headers_) {
        const std::string_view view(header_name);
        if (view == "*") {
            reflect_request_headers_ = true;
            continue;
        }
        if (!is_valid_http_header_name(view)) {
            throw std::invalid_argument("CORS allowed header must be a valid HTTP field name");
        }
        allowed_headers_.emplace_back(view);
    }
    if (allowed_headers_.empty()) {
        reflect_request_headers_ = true;
    }
    joined_allowed_headers_ = join_tokens(allowed_headers_);
    for (const auto& expose_name : config.expose_headers_) {
        if (!is_valid_http_header_name(expose_name)) {
            throw std::invalid_argument("CORS exposed header must be a valid HTTP field name");
        }
    }
    joined_expose_headers_ = join_tokens(config.expose_headers_);

    if (config.max_age_.has_value()) {
        if (config.max_age_->count() < 0) {
            throw std::invalid_argument("CORS max age must not be negative");
        }
        std::array<char, http_max_unsigned_decimal_size> buffer{};
        const auto formatted = http_format_unsigned(buffer, static_cast<std::uint64_t>(config.max_age_->count()));
        max_age_.assign(formatted);
    }
}

bool http_cors_policy::allows_origin(std::string_view origin) const noexcept {
    if (!is_valid_cors_origin_value(origin)) {
        return false;
    }
    if (allow_any_origin_) {
        return true;
    }
    if (origin == "null") {
        return std::ranges::any_of(exact_origins_, [](std::string_view configured) noexcept {
            return configured == "null";
        });
    }

    const auto scheme_sep = origin.find("://");
    if (scheme_sep == std::string_view::npos) {
        return false;
    }
    const auto scheme = origin.substr(0, scheme_sep);
    const auto authority_text = origin.substr(scheme_sep + 3U);

    for (const auto& configured : exact_origins_) {
        if (configured == "null" || configured.size() != origin.size()) {
            continue;
        }
        const auto conf_sep = configured.find("://");
        if (conf_sep != scheme_sep || !http_ascii_equals_ignore_case(scheme, std::string_view(configured).substr(0, conf_sep))) {
            continue;
        }
        if (http_ascii_equals_ignore_case(authority_text, std::string_view(configured).substr(conf_sep + 3U))) {
            return true;
        }
    }

    if (suffix_rules_.empty()) {
        return false;
    }
    const auto authority = parse_http_authority(authority_text);
    if (!authority || authority->host().empty() || authority->host().starts_with('[') || is_valid_http_ipv4_literal(authority->host())) {
        return false;
    }
    const auto host = authority->host();
    const auto port = authority->port();

    for (const auto& rule : suffix_rules_) {
        if (!rule.scheme_.empty() && !http_ascii_equals_ignore_case(scheme, rule.scheme_)) {
            continue;
        }
        if (rule.port_.has_value() && port != rule.port_) {
            continue;
        }
        const std::string_view suffix(rule.host_suffix_);
        if (!rule.subdomain_only_ && http_ascii_equals_ignore_case(host, suffix)) {
            return true;
        }
        if (host.size() > suffix.size() && host[host.size() - suffix.size() - 1U] == '.' && http_ascii_equals_ignore_case(host.substr(host.size() - suffix.size()), suffix)) {
            return true;
        }
    }
    return false;
}

bool http_cors_policy::allows_method(std::string_view method) const noexcept {
    if (!is_valid_http_method_token(method)) {
        return false;
    }
    if (allow_any_method_) {
        return true;
    }
    return std::ranges::any_of(allowed_methods_, [method](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, method);
    });
}

bool http_cors_policy::allows_request_header(std::string_view name) const noexcept {
    if (!is_valid_http_header_name(name)) {
        return false;
    }
    if (reflect_request_headers_) {
        return true;
    }
    return std::ranges::any_of(allowed_headers_, [name](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, name);
    });
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(http_cors_preflight_input input) const {
    const auto method = input.method_.view();
    const auto origin = input.origin_.view();
    const auto requested_method = http_trim_ows(input.access_control_request_method_.view());
    const auto requested_headers = input.access_control_request_headers_.view();

    if (method != "OPTIONS" || origin.empty() || !allows_origin(origin) || requested_method.empty() || !allows_method(requested_method)) {
        return http_cors_preflight_result();
    }

    std::string reflected_headers;
    if (!http_trim_ows(requested_headers).empty()) {
        http_field_name_list names(requested_headers);
        bool saw_name = false;
        while (const auto header_name = names.next()) {
            saw_name = true;
            if (!allows_request_header(*header_name)) {
                return http_cors_preflight_result();
            }
            if (reflect_request_headers_) {
                if (!reflected_headers.empty()) {
                    reflected_headers.append(", ");
                }
                reflected_headers.append(*header_name);
            }
        }
        if (!saw_name || !names.valid()) {
            return http_cors_preflight_result();
        }
    }

    std::vector<http_header> headers;
    headers.reserve(6);
    const std::string_view allow_origin = (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    const std::string_view allow_methods = !joined_allowed_methods_.empty() ? std::string_view(joined_allowed_methods_) : requested_method;
    headers.emplace_back("Access-Control-Allow-Methods", allow_methods);
    if (reflect_request_headers_) {
        if (!reflected_headers.empty()) {
            headers.emplace_back("Access-Control-Allow-Headers", reflected_headers);
        }
    } else if (!joined_allowed_headers_.empty()) {
        headers.emplace_back("Access-Control-Allow-Headers", joined_allowed_headers_);
    }
    if (!max_age_.empty()) {
        headers.emplace_back("Access-Control-Max-Age", max_age_);
    }
    headers.emplace_back("Vary", reflect_request_headers_ ? "Origin, Access-Control-Request-Method, Access-Control-Request-Headers" : "Origin, Access-Control-Request-Method");

    return http_cors_preflight_result(true, std::move(headers));
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(const http_request& request) const {
    const auto origin = request.header("Origin");
    const auto requested_method = request.header("Access-Control-Request-Method");
    if (!origin.has_value() || !requested_method.has_value()) {
        return http_cors_preflight_result();
    }
    std::string combined_headers;
    for (const auto& field : request.headers()) {
        if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        if (http_trim_ows(field.value()).empty()) {
            return http_cors_preflight_result();
        }
        if (!combined_headers.empty()) {
            combined_headers.append(", ");
        }
        combined_headers.append(field.value());
    }
    return evaluate_preflight({
        .method_ = request.method(),
        .origin_ = *origin,
        .access_control_request_method_ = *requested_method,
        .access_control_request_headers_ = combined_headers,
    });
}

bool http_cors_policy::rewrite_response_headers(borrowed_text request_origin, std::vector<http_header>& response_headers) const {
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return is_http_cors_controlled_response_header(field.name());
    });

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        merge_http_vary_token(response_headers, "Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin = (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.emplace_back("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(borrowed_text request_origin, http_response_headers& response_headers) const {
    response_headers.remove_header("Access-Control-Allow-Origin");
    response_headers.remove_header("Access-Control-Allow-Credentials");
    response_headers.remove_header("Access-Control-Expose-Headers");
    response_headers.remove_header("Access-Control-Max-Age");
    response_headers.remove_header("Access-Control-Allow-Methods");
    response_headers.remove_header("Access-Control-Allow-Headers");

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        response_headers.add_vary_token("Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin = (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.header("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.header_stable_view("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(borrowed_text request_origin, http_response& response) const {
    auto staged = response.headers().clone(4);
    const bool granted = rewrite_response_headers(request_origin, staged);
    response.commit_headers(std::move(staged));
    return granted;
}

bool http_cors_policy::apply_response_headers(const http_request& request, http_response& response) const {
    const bool options = request.known_method() == http_known_method::options;
    if (options) {
        response.add_vary_token("Origin");
        response.add_vary_token("Access-Control-Request-Method");
        if (reflect_request_headers_) {
            response.add_vary_token("Access-Control-Request-Headers");
        }
    } else if (!allow_any_origin_ || allow_credentials_) {
        response.add_vary_token("Origin");
    }

    const auto origin = request.header("Origin");
    if (origin.has_value() && !allows_origin(*origin)) {
        return false;
    }
    const auto requested_method = options ? request.header("Access-Control-Request-Method") : std::nullopt;
    if (requested_method.has_value() && !allows_method(http_trim_ows(*requested_method))) {
        return false;
    }
    const bool preflight = options && origin.has_value() && requested_method.has_value();
    const bool reflect_headers = preflight && reflect_request_headers_ && !response.header("Access-Control-Allow-Headers").has_value();
    std::string reflected_headers;
    if (options && request.header("Access-Control-Request-Headers").has_value()) {
        std::size_t request_header_names_size = 0;
        for (const auto& field : request.headers()) {
            if (field.name().size() != 30U || !http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
                continue;
            }
            http_field_name_list names(field.value());
            bool saw_name = false;
            while (const auto header_name = names.next()) {
                saw_name = true;
                if (!allows_request_header(*header_name)) {
                    return false;
                }
                const auto separator_size = request_header_names_size == 0 ? std::size_t{0} : std::size_t{2};
                const auto maximum = (std::numeric_limits<std::size_t>::max)();
                if (header_name->size() > maximum - separator_size || request_header_names_size > maximum - separator_size - header_name->size()) {
                    return false;
                }
                request_header_names_size += separator_size + header_name->size();
                if (reflect_headers) {
                    if (!reflected_headers.empty()) {
                        reflected_headers.append(", ");
                    }
                    reflected_headers.append(*header_name);
                }
            }
            if (!saw_name || !names.valid()) {
                return false;
            }
        }
    }

    if (!response.header("Access-Control-Allow-Origin").has_value()) {
        if (allow_any_origin_ && !allow_credentials_) {
            response.header_stable_view("Access-Control-Allow-Origin", "*");
        } else if (origin.has_value()) {
            response.header("Access-Control-Allow-Origin", *origin);
        } else if (exact_origins_.size() == 1 && suffix_rules_.empty()) {
            response.header("Access-Control-Allow-Origin", exact_origins_.front());
        }
    }
    if (allow_credentials_ && !response.header("Access-Control-Allow-Credentials").has_value()) {
        response.header_stable_view("Access-Control-Allow-Credentials", "true");
    }

    if (preflight) {
        if (!response.header("Access-Control-Allow-Methods").has_value()) {
            if (!joined_allowed_methods_.empty()) {
                response.header("Access-Control-Allow-Methods", joined_allowed_methods_);
            } else if (const auto allow = response.header("Allow"); allow.has_value() && !allow->empty()) {
                response.header("Access-Control-Allow-Methods", *allow);
            } else {
                response.header("Access-Control-Allow-Methods", http_trim_ows(*requested_method));
            }
        }
        if (!response.header("Access-Control-Allow-Headers").has_value()) {
            if (!reflect_request_headers_ && !joined_allowed_headers_.empty()) {
                response.header("Access-Control-Allow-Headers", joined_allowed_headers_);
            } else if (reflect_request_headers_ && !reflected_headers.empty()) {
                response.header("Access-Control-Allow-Headers", reflected_headers);
            }
        }
        if (!max_age_.empty() && !response.header("Access-Control-Max-Age").has_value()) {
            response.header("Access-Control-Max-Age", max_age_);
        }
        return true;
    }

    if (!joined_expose_headers_.empty() && !response.header("Access-Control-Expose-Headers").has_value()) {
        response.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

}  // namespace ruvia

namespace ruvia {
namespace {

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
[[nodiscard]] std::string_view extract_forwarded_node_address(std::string_view node_value) noexcept {
    node_value = http_trim_quoted_field_value(http_trim_ows(node_value));
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        if (close == std::string_view::npos) {
            return {};
        }
        const auto rest = node_value.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') {
            return {};
        }
        return node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos && node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::optional<http_scheme> parse_forwarded_http_scheme(std::string_view token) noexcept {
    token = http_trim_quoted_field_value(http_trim_ows(token));
    if (http_ascii_equals_ignore_case(token, "http")) {
        return http_scheme::http;
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return http_scheme::https;
    }
    return std::nullopt;
}

struct forwarded_hop final {
    std::optional<ip_address> address_{};
    std::optional<http_scheme> scheme_{};
};

[[nodiscard]] std::optional<ip_address> parse_hop_address(std::string_view node_value) noexcept {
    const auto address_text = extract_forwarded_node_address(node_value);
    if (address_text.empty()) {
        return std::nullopt;
    }
    const auto parsed = ip_address::parse(address_text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->normalized_ipv4();
}

[[nodiscard]] forwarded_hop parse_rfc7239_element(std::string_view element) noexcept {
    forwarded_hop hop{};
    bool saw_for = false;
    bool saw_proto = false;
    http_visit_semicolon_parameters_quoted_field(element, [&](std::string_view name, std::string_view value) noexcept {
        if (http_ascii_equals_ignore_case(name, "for")) {
            if (saw_for) {
                hop.address_.reset();
            } else {
                saw_for = true;
                hop.address_ = parse_hop_address(value);
            }
        } else if (http_ascii_equals_ignore_case(name, "proto")) {
            if (saw_proto) {
                hop.scheme_.reset();
            } else {
                saw_proto = true;
                hop.scheme_ = parse_forwarded_http_scheme(value);
            }
        }
        return true;
    });
    return hop;
}

class forwarding_chain final {
public:
    void append(const http_client_ip_extractor& extractor, forwarded_hop hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const bool trusted = hop.address_.has_value() && extractor.is_trusted_proxy(*hop.address_);
        if (trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = hop.address_.has_value();
            has_client_hop_ = true;
            nearest_trusted_.reset();
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    [[nodiscard]] forwarded_hop client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return has_client_hop_ ? client_index_ : 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_hop leftmost_{};
    forwarded_hop client_hop_{};
    std::optional<ip_address> nearest_trusted_{};
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

template <typename header_span_type>
[[nodiscard]] std::optional<http_client_ip_result> resolve_from_header(const http_client_ip_extractor& extractor, const ip_address& normalized_peer, std::string_view header_name, http_forwarded_header_kind kind, header_span_type headers) noexcept {
    if (kind == http_forwarded_header_kind::single_ip) {
        std::string_view single_value{};
        std::size_t line_count = 0;
        for (const auto& field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), header_name)) {
                single_value = field.value();
                ++line_count;
            }
        }
        if (line_count != 1) {
            return std::nullopt;
        }
        const auto trimmed = http_trim_ows(single_value);
        if (trimmed.empty() || trimmed.find(',') != std::string_view::npos) {
            return std::nullopt;
        }
        const auto parsed = parse_hop_address(trimmed);
        if (!parsed.has_value() || extractor.is_trusted_proxy(*parsed)) {
            return std::nullopt;
        }
        return http_client_ip_result{.address_ = *parsed, .from_forwarded_header_ = true, .scheme_ = std::nullopt};
    }

    forwarding_chain chain;
    for (const auto& field : headers) {
        if (!http_ascii_equals_ignore_case(field.name(), header_name)) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(field.value(), [&](std::string_view item) noexcept {
            if (item.empty()) {
                return true;
            }
            if (kind == http_forwarded_header_kind::rfc7239_forwarded) {
                chain.append(extractor, parse_rfc7239_element(item));
            } else {
                chain.append(extractor, {.address_ = parse_hop_address(item), .scheme_ = std::nullopt});
            }
            return true;
        });
    }
    if (chain.size() == 0) {
        return std::nullopt;
    }
    auto resolved = chain.client();
    if (kind == http_forwarded_header_kind::comma_separated_list && extractor.trust_x_forwarded_proto() && http_ascii_equals_ignore_case(header_name, "X-Forwarded-For")) {
        std::size_t proto_count = 0;
        std::optional<http_scheme> matched_scheme{};
        const auto target_index = chain.client_index();
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "X-Forwarded-Proto")) {
                continue;
            }
            http_visit_comma_separated_quoted_field_items(field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                if (proto_count == target_index) {
                    matched_scheme = parse_forwarded_http_scheme(item);
                }
                ++proto_count;
                return true;
            });
        }
        if (proto_count == chain.size()) {
            resolved.scheme_ = matched_scheme;
        }
    }
    if (!resolved.address_.has_value() && !resolved.scheme_.has_value()) {
        return std::nullopt;
    }
    return http_client_ip_result{
        .address_ = resolved.address_.value_or(normalized_peer),
        .from_forwarded_header_ = true,
        .scheme_ = resolved.scheme_,
    };
}

[[nodiscard]] http_forwarded_header_kind resolve_header_kind(std::string_view name, http_forwarded_header_kind configured) noexcept {
    if (configured != http_forwarded_header_kind::auto_detect) {
        return configured;
    }
    if (http_ascii_equals_ignore_case(name, "Forwarded")) {
        return http_forwarded_header_kind::rfc7239_forwarded;
    }
    if (http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
        return http_forwarded_header_kind::comma_separated_list;
    }
    return http_forwarded_header_kind::single_ip;
}
}  // namespace

http_client_ip_extractor::http_client_ip_extractor(const http_client_ip_config& config) :
    enabled_(config.enabled_),
    trust_x_forwarded_proto_(config.trust_x_forwarded_proto_) {
    trusted_cidrs_.reserve(config.trusted_proxies_.size());
    for (const auto& cidr_text : config.trusted_proxies_) {
        const auto parsed = ip_cidr::parse(cidr_text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("trusted proxy entry must be a valid IP address or CIDR");
        }
        trusted_cidrs_.push_back(*parsed);
    }

    if (config.forwarding_headers_.empty()) {
        compiled_header xff;
        xff.name_.assign("X-Forwarded-For");
        xff.kind_ = http_forwarded_header_kind::comma_separated_list;
        headers_.push_back(std::move(xff));

        compiled_header fwd;
        fwd.name_.assign("Forwarded");
        fwd.kind_ = http_forwarded_header_kind::rfc7239_forwarded;
        headers_.push_back(std::move(fwd));
        return;
    }

    headers_.reserve(config.forwarding_headers_.size());
    for (const auto& spec : config.forwarding_headers_) {
        if (!is_valid_http_header_name(spec.name_)) {
            throw std::invalid_argument("forwarding header name must be a valid HTTP field name");
        }
        compiled_header entry;
        entry.name_.assign(spec.name_);
        entry.kind_ = resolve_header_kind(spec.name_, spec.kind_);
        headers_.push_back(std::move(entry));
    }
}

bool http_client_ip_extractor::is_trusted_proxy(const ip_address& address) const noexcept {
    return std::ranges::any_of(trusted_cidrs_, [&address](const ip_cidr& cidr) noexcept {
        return cidr.contains(address);
    });
}

bool http_client_ip_extractor::is_trusted_proxy(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && is_trusted_proxy(*parsed);
}

http_client_ip_result http_client_ip_extractor::resolve(const ip_address& peer_ip, std::span<const http_header> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(*this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header_view& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(*this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(const ip_address& peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(std::string_view peer_ip, std::span<const http_header> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(std::string_view peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

}  // namespace ruvia
