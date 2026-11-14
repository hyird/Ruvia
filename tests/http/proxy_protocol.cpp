#include "proxy_protocol.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"

namespace {

using ruvia::proxy_protocol_command;
using ruvia::proxy_protocol_header;
using ruvia::proxy_protocol_parse_error;
using ruvia::proxy_protocol_parser;
using ruvia::proxy_protocol_parser_options;
using ruvia::proxy_protocol_parser_state;
using ruvia::proxy_protocol_transport;
using ruvia::proxy_protocol_version;

constexpr std::string_view v1_tcp4 = "PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r\n";
constexpr std::string_view http_request_line = "GET / HTTP/1.1\r\n";

std::span<const std::byte> wire(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string text(std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

ruvia::ip_address ip(std::string_view literal) {
    return ruvia::ip_address::parse(literal).value();
}

ruvia::ip_endpoint endpoint(std::string_view literal, std::uint16_t port) {
    return ruvia::ip_endpoint(ip(literal), port);
}

std::string address_bytes(std::string_view literal) {
    const auto address = ip(literal);
    const auto bytes = address.bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string port_bytes(std::uint16_t port) {
    return {static_cast<char>(port >> 8), static_cast<char>(port & 0xFF)};
}

std::string v2_header(std::uint8_t version_command, std::uint8_t family_protocol, std::string_view block) {
    std::string header("\r\n\r\n\0\r\nQUIT\n", 12);
    header.push_back(static_cast<char>(version_command));
    header.push_back(static_cast<char>(family_protocol));
    header.append(port_bytes(static_cast<std::uint16_t>(block.size())));
    header.append(block);
    return header;
}

std::string tlv(std::uint8_t type, std::string_view value) {
    std::string bytes(1, static_cast<char>(type));
    bytes.append(port_bytes(static_cast<std::uint16_t>(value.size())));
    bytes.append(value);
    return bytes;
}

std::string inet_block() {
    return address_bytes("192.168.0.1") + address_bytes("10.0.0.2") + port_bytes(56324) + port_bytes(443);
}

std::string inet6_block() {
    return address_bytes("2001:db8::1") + address_bytes("2001:db8::2") + port_bytes(1234) + port_bytes(80);
}

// PROXY TCP over IPv4 with ALPN, AUTHORITY and an empty-valued custom TLV.
std::string v2_inet_with_tlvs() {
    return v2_header(0x21, 0x11, inet_block() + tlv(0x01, "h2") + tlv(0x02, "example.com") + tlv(0xE0, ""));
}

void expect_v1_tcp4(const proxy_protocol_header& header) {
    EXPECT_EQ(header.version(), proxy_protocol_version::v1);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
    EXPECT_EQ(header.source(), endpoint("192.168.0.1", 56324));
    EXPECT_EQ(header.destination(), endpoint("192.168.0.11", 443));
    EXPECT_TRUE(header.tlvs().empty());
}

void expect_v2_inet_with_tlvs(const proxy_protocol_header& header) {
    EXPECT_EQ(header.version(), proxy_protocol_version::v2);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
    EXPECT_EQ(header.source(), endpoint("192.168.0.1", 56324));
    EXPECT_EQ(header.destination(), endpoint("10.0.0.2", 443));
    const auto tlvs = header.tlvs();
    ASSERT_EQ(tlvs.size(), 3U);
    EXPECT_EQ(tlvs[0].type_, 0x01);
    EXPECT_EQ(text(tlvs[0].value_), "h2");
    EXPECT_EQ(tlvs[1].type_, 0x02);
    EXPECT_EQ(text(tlvs[1].value_), "example.com");
    EXPECT_EQ(tlvs[2].type_, 0xE0);
    EXPECT_TRUE(tlvs[2].value_.empty());
}

const proxy_protocol_header& parse_whole(proxy_protocol_parser& parser, std::string_view input) {
    const auto result = parser.parse(wire(input));
    EXPECT_TRUE(result.complete());
    EXPECT_FALSE(result.failure());
    EXPECT_EQ(result.consumed_bytes(), input.size());
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::finished);
    return parser.header();
}

// Feeds `chunks` in order; the last one must be rejected with `error`.
void expect_chunks_rejected(std::initializer_list<std::string_view> chunks, proxy_protocol_parse_error error, proxy_protocol_parser_options options = {}) {
    proxy_protocol_parser parser(options);
    std::size_t index = 0;
    for (const auto chunk : chunks) {
        const auto result = parser.parse(wire(chunk));
        EXPECT_FALSE(result.complete());
        if (++index < chunks.size()) {
            EXPECT_FALSE(result.failure());
            EXPECT_EQ(result.consumed_bytes(), chunk.size());
            continue;
        }
        EXPECT_EQ(result.failure(), error);
        EXPECT_EQ(result.consumed_bytes(), 0U);
    }
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::failed);
}

void expect_rejected(std::string_view input, proxy_protocol_parse_error error, proxy_protocol_parser_options options = {}) {
    SCOPED_TRACE(std::string(input));
    expect_chunks_rejected({input}, error, options);
}

template <typename check_type>
void expect_every_split_point(std::string_view input, check_type check) {
    for (std::size_t split = 0; split <= input.size(); ++split) {
        SCOPED_TRACE(split);
        proxy_protocol_parser parser;
        const auto first = parser.parse(wire(input.substr(0, split)));
        EXPECT_EQ(first.consumed_bytes(), split);
        if (split < input.size()) {
            EXPECT_FALSE(first.complete());
            const auto second = parser.parse(wire(input.substr(split)));
            EXPECT_TRUE(second.complete());
            EXPECT_EQ(second.consumed_bytes(), input.size() - split);
        } else {
            EXPECT_TRUE(first.complete());
        }
        ASSERT_EQ(parser.state(), proxy_protocol_parser_state::finished);
        check(parser.header());
    }
}

template <typename check_type>
void expect_byte_by_byte(std::string_view input, check_type check) {
    proxy_protocol_parser parser;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto result = parser.parse(wire(input.substr(index, 1)));
        EXPECT_EQ(result.consumed_bytes(), 1U);
        EXPECT_EQ(result.complete(), index + 1 == input.size());
    }
    ASSERT_EQ(parser.state(), proxy_protocol_parser_state::finished);
    check(parser.header());
}

}  // namespace

TEST(Ruvia, proxy_protocol_v1_tcp4_yields_endpoints) {
    proxy_protocol_parser parser;
    expect_v1_tcp4(parse_whole(parser, v1_tcp4));
}

TEST(Ruvia, proxy_protocol_v1_tcp6_yields_endpoints) {
    proxy_protocol_parser parser;
    const auto& header = parse_whole(parser, "PROXY TCP6 2001:db8::1 ::ffff:192.0.2.1 0 65535\r\n");
    EXPECT_EQ(header.version(), proxy_protocol_version::v1);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
    EXPECT_EQ(header.source(), endpoint("2001:db8::1", 0));
    EXPECT_EQ(header.destination(), endpoint("::ffff:192.0.2.1", 65535));
}

TEST(Ruvia, proxy_protocol_v1_unknown_keeps_real_endpoints) {
    for (const std::string_view input : {std::string_view("PROXY UNKNOWN\r\n"), std::string_view("PROXY UNKNOWN ffff::1 ffff::2 65535 65535\r\n"), std::string_view("PROXY UNKNOWN anything \x01 goes\r\n")}) {
        SCOPED_TRACE(std::string(input));
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, input);
        EXPECT_EQ(header.version(), proxy_protocol_version::v1);
        EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
        EXPECT_EQ(header.transport(), proxy_protocol_transport::unspecified);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        EXPECT_TRUE(header.tlvs().empty());
    }
}

TEST(Ruvia, proxy_protocol_v2_proxy_inet_exposes_tlvs_in_wire_order) {
    proxy_protocol_parser parser;
    const auto input = v2_inet_with_tlvs();
    expect_v2_inet_with_tlvs(parse_whole(parser, input));
}

TEST(Ruvia, proxy_protocol_v2_proxy_inet6_datagram_yields_endpoints) {
    proxy_protocol_parser parser;
    const auto input = v2_header(0x21, 0x22, inet6_block() + tlv(0x03, std::string("\x01\x02\x03\x04", 4)));
    const auto& header = parse_whole(parser, input);
    EXPECT_EQ(header.version(), proxy_protocol_version::v2);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::datagram);
    EXPECT_EQ(header.source(), endpoint("2001:db8::1", 1234));
    EXPECT_EQ(header.destination(), endpoint("2001:db8::2", 80));
    // CRC32C is exposed raw, not verified.
    ASSERT_EQ(header.tlvs().size(), 1U);
    EXPECT_EQ(header.tlvs()[0].type_, 0x03);
    EXPECT_EQ(text(header.tlvs()[0].value_), std::string("\x01\x02\x03\x04", 4));
}

TEST(Ruvia, proxy_protocol_v2_local_ignores_address_block) {
    for (const auto& input : {v2_header(0x20, 0x00, ""), v2_header(0x20, 0x11, inet_block() + tlv(0x01, "h2")), v2_header(0x20, 0x21, "short"), v2_header(0x20, 0x31, "")}) {
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, input);
        EXPECT_EQ(header.version(), proxy_protocol_version::v2);
        EXPECT_EQ(header.command(), proxy_protocol_command::local);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        EXPECT_TRUE(header.tlvs().empty());
    }
}

TEST(Ruvia, proxy_protocol_v2_unspec_and_unix_keep_real_endpoints) {
    {
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, v2_header(0x21, 0x00, "opaque"));
        EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
        EXPECT_EQ(header.transport(), proxy_protocol_transport::unspecified);
        EXPECT_FALSE(header.source());
        EXPECT_TRUE(header.tlvs().empty());
    }
    {
        // An UNSPEC transport carries no address information to use.
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, v2_header(0x21, 0x10, inet_block() + tlv(0x02, "a")));
        EXPECT_EQ(header.transport(), proxy_protocol_transport::unspecified);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        ASSERT_EQ(header.tlvs().size(), 1U);
        EXPECT_EQ(text(header.tlvs()[0].value_), "a");
    }
    {
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, v2_header(0x21, 0x31, std::string(216, '\0') + tlv(0x05, "id")));
        EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        ASSERT_EQ(header.tlvs().size(), 1U);
        EXPECT_EQ(header.tlvs()[0].type_, 0x05);
        EXPECT_EQ(text(header.tlvs()[0].value_), "id");
    }
}

TEST(Ruvia, proxy_protocol_v1_every_split_point) {
    expect_every_split_point(v1_tcp4, expect_v1_tcp4);
}

TEST(Ruvia, proxy_protocol_v2_every_split_point) {
    const auto input = v2_inet_with_tlvs();
    expect_every_split_point(input, expect_v2_inet_with_tlvs);
}

TEST(Ruvia, proxy_protocol_byte_by_byte) {
    expect_byte_by_byte(v1_tcp4, expect_v1_tcp4);
    const auto input = v2_inet_with_tlvs();
    expect_byte_by_byte(input, expect_v2_inet_with_tlvs);
}

TEST(Ruvia, proxy_protocol_consumed_bytes_exclude_payload) {
    {
        proxy_protocol_parser parser;
        const auto input = std::string(v1_tcp4) + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete());
        EXPECT_EQ(result.consumed_bytes(), v1_tcp4.size());
        expect_v1_tcp4(parser.header());
    }
    {
        // The CR of the CRLF ends one chunk; the LF and the payload follow.
        proxy_protocol_parser parser;
        const auto first = parser.parse(wire(v1_tcp4.substr(0, v1_tcp4.size() - 1)));
        EXPECT_FALSE(first.complete());
        const auto rest = std::string("\n") + std::string(http_request_line);
        const auto second = parser.parse(wire(rest));
        EXPECT_TRUE(second.complete());
        EXPECT_EQ(second.consumed_bytes(), 1U);
    }
    {
        proxy_protocol_parser parser;
        const auto header = v2_inet_with_tlvs();
        const auto input = header + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete());
        EXPECT_EQ(result.consumed_bytes(), header.size());
        expect_v2_inet_with_tlvs(parser.header());
    }
    {
        // A zero-length v2 header ends with its fixed part.
        proxy_protocol_parser parser;
        const auto input = v2_header(0x20, 0x00, "") + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete());
        EXPECT_EQ(result.consumed_bytes(), 16U);
    }
}

TEST(Ruvia, proxy_protocol_rejects_invalid_signature_immediately) {
    for (const std::string_view input : {
             std::string_view("G"),
             std::string_view("\0", 1),
             std::string_view("PROXX"),
             std::string_view("proxy "),
             std::string_view("\r\n\r\n\x01", 5),
             std::string_view("\r\n\r\n\0\r\nQUIT\r", 12),
             http_request_line,
         }) {
        expect_rejected(input, proxy_protocol_parse_error::invalid_signature);
    }
    // A signature prefix is not rejected while it may still complete.
    for (const std::string_view prefix : {std::string_view("PROX"), std::string_view("\r\n\r\n\0\r\nQ", 8)}) {
        proxy_protocol_parser parser;
        const auto result = parser.parse(wire(prefix));
        EXPECT_FALSE(result.complete());
        EXPECT_FALSE(result.failure());
        EXPECT_EQ(result.consumed_bytes(), prefix.size());
        EXPECT_EQ(parser.state(), proxy_protocol_parser_state::active);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_overlong_line) {
    constexpr std::string_view prefix = "PROXY UNKNOWN ";
    const auto longest = std::string(prefix) + std::string(107 - prefix.size() - 2, 'a') + "\r\n";
    ASSERT_EQ(longest.size(), 107U);
    proxy_protocol_parser parser;
    EXPECT_EQ(parse_whole(parser, longest).transport(), proxy_protocol_transport::unspecified);

    expect_rejected(std::string(prefix) + std::string(107 - prefix.size() - 1, 'a') + "\r\n", proxy_protocol_parse_error::v1_line_too_long);
    // 107 bytes without CRLF reject without waiting for more input.
    expect_rejected(std::string(prefix) + std::string(107 - prefix.size(), 'a'), proxy_protocol_parse_error::v1_line_too_long);
    // A CR as the 107th byte leaves no room for its LF.
    expect_rejected(std::string(prefix) + std::string(107 - prefix.size() - 1, 'a') + "\r", proxy_protocol_parse_error::v1_line_too_long);
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_port) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 65536 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 123456 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 -1 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 +1 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 http 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 0x1BB\r\n"),
         }) {
        expect_rejected(input, proxy_protocol_parse_error::v1_invalid_port);
    }
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324\r\n", proxy_protocol_parse_error::v1_invalid_fields);
}

TEST(Ruvia, proxy_protocol_v1_rejects_port_leading_zero) {
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 056324 443\r\n", proxy_protocol_parse_error::v1_invalid_port);
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 00\r\n", proxy_protocol_parse_error::v1_invalid_port);
    proxy_protocol_parser parser;
    const auto& header = parse_whole(parser, "PROXY TCP4 192.168.0.1 192.168.0.11 0 0\r\n");
    EXPECT_EQ(header.source(), endpoint("192.168.0.1", 0));
    EXPECT_EQ(header.destination(), endpoint("192.168.0.11", 0));
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_address) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4 2001:db8::1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP6 2001:db8::1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP6 fe80::1%1 2001:db8::2 1 2\r\n"),
             std::string_view("PROXY TCP6 [2001:db8::1] 2001:db8::2 1 2\r\n"),
             std::string_view("PROXY TCP4 192.168.000.1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP4 192.168.0 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP4 256.0.0.1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP4 localhost 192.168.0.11 1 2\r\n"),
         }) {
        expect_rejected(input, proxy_protocol_parse_error::v1_invalid_address);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_bare_line_endings) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\n"),
             std::string_view("PROXY UNKNOWN\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\rX"),
             std::string_view("PROXY UNKNOWN\r\r\n"),
         }) {
        expect_rejected(input, proxy_protocol_parse_error::v1_invalid_line_ending);
    }
    expect_chunks_rejected({"PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r", "X"}, proxy_protocol_parse_error::v1_invalid_line_ending);
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_syntax) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4  192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1  192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443 \r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443 1\r\n"),
         }) {
        expect_rejected(input, proxy_protocol_parse_error::v1_invalid_fields);
    }
    for (const std::string_view input : {
             std::string_view("PROXY TCP5 192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY tcp4 192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4\r\n"),
             std::string_view("PROXY UNKNOWNX\r\n"),
             std::string_view("PROXY \r\n"),
         }) {
        expect_rejected(input, proxy_protocol_parse_error::v1_unsupported_protocol);
    }
}

TEST(Ruvia, proxy_protocol_v2_rejects_invalid_fixed_fields_immediately) {
    const auto signature = std::string("\r\n\r\n\0\r\nQUIT\n", 12);
    // Version nibble other than 2.
    expect_rejected(signature + "\x11", proxy_protocol_parse_error::v2_unsupported_version);
    expect_rejected(signature + "\x31", proxy_protocol_parse_error::v2_unsupported_version);
    expect_rejected(signature + "\x22", proxy_protocol_parse_error::v2_unassigned_command);
    expect_rejected(signature + "\x2F", proxy_protocol_parse_error::v2_unassigned_command);
    expect_rejected(signature + "\x21\x41", proxy_protocol_parse_error::v2_unassigned_address_family);
    expect_rejected(signature + "\x21\xF1", proxy_protocol_parse_error::v2_unassigned_address_family);
    expect_rejected(signature + "\x21\x13", proxy_protocol_parse_error::v2_unassigned_transport);
    expect_rejected(signature + "\x20\x0F", proxy_protocol_parse_error::v2_unassigned_transport);
}

TEST(Ruvia, proxy_protocol_v2_rejects_over_limit_length) {
    const proxy_protocol_parser_options options{.max_v2_header_bytes_ = 64};
    // Rejected once the length field arrives, before any block byte.
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string(37, 'x')).substr(0, 16), proxy_protocol_parse_error::v2_header_too_large, options);
    expect_rejected(v2_header(0x20, 0x00, std::string(0xFFFF, 'x')).substr(0, 16), proxy_protocol_parse_error::v2_header_too_large);

    proxy_protocol_parser parser(options);
    const auto at_limit = v2_header(0x21, 0x11, inet_block() + tlv(0xE0, std::string(33, 'x')));
    ASSERT_EQ(at_limit.size(), 64U);
    const auto& header = parse_whole(parser, at_limit);
    ASSERT_EQ(header.tlvs().size(), 1U);
    EXPECT_EQ(header.tlvs()[0].value_.size(), 33U);
}

TEST(Ruvia, proxy_protocol_v2_rejects_short_address_block) {
    expect_rejected(v2_header(0x21, 0x11, inet_block().substr(0, 11)).substr(0, 16), proxy_protocol_parse_error::v2_short_address_block);
    expect_rejected(v2_header(0x21, 0x21, inet6_block().substr(0, 35)).substr(0, 16), proxy_protocol_parse_error::v2_short_address_block);
    expect_rejected(v2_header(0x21, 0x31, std::string(215, '\0')).substr(0, 16), proxy_protocol_parse_error::v2_short_address_block);
    expect_rejected(v2_header(0x21, 0x12, ""), proxy_protocol_parse_error::v2_short_address_block);
}

TEST(Ruvia, proxy_protocol_v2_rejects_tlv_overrun) {
    expect_rejected(v2_header(0x21, 0x11,
                        inet_block() +
                            std::string("\x01\x00\x05"
                                        "ab",
                                5)),
        proxy_protocol_parse_error::v2_tlv_overrun);
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string("\x01\x00", 2)), proxy_protocol_parse_error::v2_tlv_overrun);
    expect_rejected(v2_header(0x21, 0x11, inet_block() + tlv(0x01, "h2") + std::string("\x02", 1)), proxy_protocol_parse_error::v2_tlv_overrun);
}

TEST(Ruvia, proxy_protocol_options_reject_small_v2_limit) {
    EXPECT_THROW((void)proxy_protocol_parser(proxy_protocol_parser_options{.max_v2_header_bytes_ = 51}), std::invalid_argument);
    EXPECT_NO_THROW((void)proxy_protocol_parser(proxy_protocol_parser_options{.max_v2_header_bytes_ = 52}));
}

TEST(Ruvia, proxy_protocol_parse_after_finished_or_failed_throws_logic_error) {
    proxy_protocol_parser parser;
    EXPECT_THROW((void)parser.header(), std::logic_error);
    (void)parse_whole(parser, v1_tcp4);
    EXPECT_THROW((void)parser.parse(wire(http_request_line)), std::logic_error);
    EXPECT_THROW((void)parser.parse({}), std::logic_error);
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::finished);

    proxy_protocol_parser failed;
    EXPECT_EQ(failed.parse(wire("X")).failure(), proxy_protocol_parse_error::invalid_signature);
    EXPECT_THROW((void)failed.parse(wire(v1_tcp4)), std::logic_error);
    EXPECT_THROW((void)failed.header(), std::logic_error);
    EXPECT_EQ(failed.state(), proxy_protocol_parser_state::failed);
}

TEST(Ruvia, proxy_protocol_reset_reuses_parser) {
    proxy_protocol_parser parser;
    const auto v2 = v2_inet_with_tlvs();
    expect_v2_inet_with_tlvs(parse_whole(parser, v2));
    parser.reset();
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::active);
    expect_v1_tcp4(parse_whole(parser, v1_tcp4));

    parser.reset();
    EXPECT_EQ(parser.parse(wire("PROXY TCP4 1.2.3.4\n")).failure(), proxy_protocol_parse_error::v1_invalid_line_ending);
    parser.reset();
    expect_v2_inet_with_tlvs(parse_whole(parser, v2));

    // A reset in the middle of a header starts over.
    parser.reset();
    (void)parser.parse(wire(v2.substr(0, 20)));
    parser.reset();
    expect_v1_tcp4(parse_whole(parser, v1_tcp4));
}

TEST(Ruvia, proxy_protocol_moved_parser_keeps_header_views) {
    proxy_protocol_parser parser;
    const auto input = v2_inet_with_tlvs();
    (void)parse_whole(parser, input);
    const auto tlvs = parser.header().tlvs();
    const proxy_protocol_parser moved(std::move(parser));
    EXPECT_EQ(moved.state(), proxy_protocol_parser_state::finished);
    expect_v2_inet_with_tlvs(moved.header());
    EXPECT_EQ(moved.header().tlvs().data(), tlvs.data());
}
