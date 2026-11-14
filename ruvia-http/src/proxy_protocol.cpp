#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "borrowed_text.h"
#include "http_request.h"

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
