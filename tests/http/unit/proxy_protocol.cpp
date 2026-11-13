#include "ruvia/http/proxy_protocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/ip_address.h"

#include "test_exceptions.h"

namespace {

using ruvia::proxy_protocol_command;
using ruvia::proxy_protocol_error;
using ruvia::proxy_protocol_header;
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
    EXPECT_TRUE(result.complete_);
    EXPECT_EQ(result.consumed_bytes_, input.size());
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::finished);
    return parser.header();
}

// Feeds `chunks` in order; the last one must be rejected.
void expect_chunks_rejected(std::initializer_list<std::string_view> chunks, proxy_protocol_parser_options options = {}) {
    proxy_protocol_parser parser(options);
    std::size_t index = 0;
    for (const auto chunk : chunks) {
        if (++index < chunks.size()) {
            const auto result = parser.parse(wire(chunk));
            EXPECT_FALSE(result.complete_);
            EXPECT_EQ(result.consumed_bytes_, chunk.size());
            continue;
        }
        EXPECT_THROW((void)parser.parse(wire(chunk)), proxy_protocol_error);
    }
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::failed);
}

void expect_rejected(std::string_view input, proxy_protocol_parser_options options = {}) {
    SCOPED_TRACE(std::string(input));
    expect_chunks_rejected({input}, options);
}

template <typename check_type>
void expect_every_split_point(std::string_view input, check_type check) {
    for (std::size_t split = 0; split <= input.size(); ++split) {
        SCOPED_TRACE(split);
        proxy_protocol_parser parser;
        const auto first = parser.parse(wire(input.substr(0, split)));
        EXPECT_EQ(first.consumed_bytes_, split);
        if (split < input.size()) {
            EXPECT_FALSE(first.complete_);
            const auto second = parser.parse(wire(input.substr(split)));
            EXPECT_TRUE(second.complete_);
            EXPECT_EQ(second.consumed_bytes_, input.size() - split);
        } else {
            EXPECT_TRUE(first.complete_);
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
        EXPECT_EQ(result.consumed_bytes_, 1U);
        EXPECT_EQ(result.complete_, index + 1 == input.size());
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
    for (const std::string_view input :
        {std::string_view("PROXY UNKNOWN\r\n"), std::string_view("PROXY UNKNOWN ffff::1 ffff::2 65535 65535\r\n"),
            std::string_view("PROXY UNKNOWN anything \x01 goes\r\n")}) {
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
    for (const auto& input : {v2_header(0x20, 0x00, ""), v2_header(0x20, 0x11, inet_block() + tlv(0x01, "h2")),
             v2_header(0x20, 0x21, "short"), v2_header(0x20, 0x31, "")}) {
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
        EXPECT_TRUE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, v1_tcp4.size());
        expect_v1_tcp4(parser.header());
    }
    {
        // The CR of the CRLF ends one chunk; the LF and the payload follow.
        proxy_protocol_parser parser;
        const auto first = parser.parse(wire(v1_tcp4.substr(0, v1_tcp4.size() - 1)));
        EXPECT_FALSE(first.complete_);
        const auto rest = std::string("\n") + std::string(http_request_line);
        const auto second = parser.parse(wire(rest));
        EXPECT_TRUE(second.complete_);
        EXPECT_EQ(second.consumed_bytes_, 1U);
    }
    {
        proxy_protocol_parser parser;
        const auto header = v2_inet_with_tlvs();
        const auto input = header + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, header.size());
        expect_v2_inet_with_tlvs(parser.header());
    }
    {
        // A zero-length v2 header ends with its fixed part.
        proxy_protocol_parser parser;
        const auto input = v2_header(0x20, 0x00, "") + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, 16U);
    }
}

TEST(Ruvia, proxy_protocol_rejects_invalid_signature_immediately) {
    expect_rejected("G");
    expect_rejected(std::string_view("\0", 1));
    expect_rejected("PROXX");
    expect_rejected("proxy ");
    expect_rejected(std::string_view("\r\n\r\n\x01", 5));
    expect_rejected(std::string_view("\r\n\r\n\0\r\nQUIT\r", 12));
    expect_rejected(http_request_line);
    // A signature prefix is not rejected while it may still complete.
    for (const std::string_view prefix : {std::string_view("PROX"), std::string_view("\r\n\r\n\0\r\nQ", 8)}) {
        proxy_protocol_parser parser;
        const auto result = parser.parse(wire(prefix));
        EXPECT_FALSE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, prefix.size());
        EXPECT_EQ(parser.state(), proxy_protocol_parser_state::active);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_overlong_line) {
    constexpr std::string_view prefix = "PROXY UNKNOWN ";
    const auto longest = std::string(prefix) + std::string(107 - prefix.size() - 2, 'a') + "\r\n";
    ASSERT_EQ(longest.size(), 107U);
    proxy_protocol_parser parser;
    EXPECT_EQ(parse_whole(parser, longest).transport(), proxy_protocol_transport::unspecified);

    expect_rejected(std::string(prefix) + std::string(107 - prefix.size() - 1, 'a') + "\r\n");
    // 107 bytes without CRLF reject without waiting for more input.
    expect_rejected(std::string(prefix) + std::string(107 - prefix.size(), 'a'));
    // A CR as the 107th byte leaves no room for its LF.
    expect_rejected(std::string(prefix) + std::string(107 - prefix.size() - 1, 'a') + "\r");
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_port) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 65536 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 123456 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 -1 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 +1 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 http 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 0x1BB\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324\r\n"),
         }) {
        expect_rejected(input);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_port_leading_zero) {
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 056324 443\r\n");
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 00\r\n");
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
        expect_rejected(input);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_bare_line_endings) {
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\n");
    expect_rejected("PROXY UNKNOWN\n");
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\rX");
    expect_chunks_rejected({"PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r", "X"});
    expect_rejected("PROXY UNKNOWN\r\r\n");
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_syntax) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4  192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1  192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443 \r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443 1\r\n"),
             std::string_view("PROXY TCP5 192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY tcp4 192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4\r\n"),
             std::string_view("PROXY UNKNOWNX\r\n"),
             std::string_view("PROXY \r\n"),
         }) {
        expect_rejected(input);
    }
}

TEST(Ruvia, proxy_protocol_v2_rejects_invalid_fixed_fields_immediately) {
    const auto signature = std::string("\r\n\r\n\0\r\nQUIT\n", 12);
    // Version nibble other than 2.
    expect_rejected(signature + "\x11");
    expect_rejected(signature + "\x31");
    // Unassigned command.
    expect_rejected(signature + "\x22");
    expect_rejected(signature + "\x2F");
    // Unspecified address family.
    expect_rejected(signature + "\x21\x41");
    expect_rejected(signature + "\x21\xF1");
    // Unspecified transport protocol.
    expect_rejected(signature + "\x21\x13");
    expect_rejected(signature + "\x20\x0F");
}

TEST(Ruvia, proxy_protocol_v2_rejects_over_limit_length) {
    const proxy_protocol_parser_options options{.max_v2_header_bytes_ = 64};
    // Rejected once the length field arrives, before any block byte.
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string(37, 'x')).substr(0, 16), options);
    expect_rejected(v2_header(0x20, 0x00, std::string(0xFFFF, 'x')).substr(0, 16));

    proxy_protocol_parser parser(options);
    const auto at_limit = v2_header(0x21, 0x11, inet_block() + tlv(0xE0, std::string(33, 'x')));
    ASSERT_EQ(at_limit.size(), 64U);
    const auto& header = parse_whole(parser, at_limit);
    ASSERT_EQ(header.tlvs().size(), 1U);
    EXPECT_EQ(header.tlvs()[0].value_.size(), 33U);
}

TEST(Ruvia, proxy_protocol_v2_rejects_short_address_block) {
    expect_rejected(v2_header(0x21, 0x11, inet_block().substr(0, 11)).substr(0, 16));
    expect_rejected(v2_header(0x21, 0x21, inet6_block().substr(0, 35)).substr(0, 16));
    expect_rejected(v2_header(0x21, 0x31, std::string(215, '\0')).substr(0, 16));
    expect_rejected(v2_header(0x21, 0x12, ""));
}

TEST(Ruvia, proxy_protocol_v2_rejects_tlv_overrun) {
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string("\x01\x00\x05"
                                                                     "ab",
                                                             5)));
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string("\x01\x00", 2)));
    expect_rejected(v2_header(0x21, 0x11, inet_block() + tlv(0x01, "h2") + std::string("\x02", 1)));
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
    EXPECT_THROW((void)failed.parse(wire("X")), proxy_protocol_error);
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
    EXPECT_THROW((void)parser.parse(wire("PROXY TCP4 1.2.3.4\n")), proxy_protocol_error);
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

static_assert(std::is_trivially_copyable_v<ruvia::ip_address>);
static_assert(std::is_trivially_copyable_v<ruvia::ip_endpoint>);
static_assert(ruvia::ip_address{}.family() == ruvia::ip_address_family::ipv4);
static_assert(ruvia::ip_endpoint(ruvia::ip_address{}, 443).port() == 443);

TEST(RuviaHttp, ip_address_owns_network_order_bytes_and_endpoint_values) {
    std::array<std::uint8_t, 4> bytes{192, 0, 2, 1};
    const auto address = ruvia::ip_address::ipv4(bytes);
    bytes[0] = 127;
    EXPECT_EQ(address.family(), ruvia::ip_address_family::ipv4);
    ASSERT_EQ(address.bytes().size(), 4);
    EXPECT_EQ(address.bytes()[0], 192);
    EXPECT_EQ(address.bytes()[3], 1);
    EXPECT_EQ(address.scope_id(), 0);
    EXPECT_EQ(address.to_string(), "192.0.2.1");
    const ruvia::ip_endpoint endpoint(address, 65535);
    EXPECT_EQ(endpoint.address(), address);
    EXPECT_EQ(endpoint.port(), 65535);
    EXPECT_EQ(endpoint, ruvia::ip_endpoint(address, 65535));
    EXPECT_NE(endpoint, ruvia::ip_endpoint(address, 0));
    EXPECT_EQ(ruvia::ip_endpoint{}.port(), 0);
    EXPECT_EQ(ruvia::ip_address{}.to_string(), "0.0.0.0");
}

TEST(RuviaHttp, ip_address_formats_rfc5952_and_round_trips) {
    struct example final {
        std::string_view input;
        std::string_view canonical;
    };
    for (const auto& entry : std::array{
             example{"0.0.0.0", "0.0.0.0"},
             example{"255.255.255.255", "255.255.255.255"},
             example{"0:0:0:0:0:0:0:0", "::"},
             example{"0:0:0:0:0:0:0:1", "::1"},
             example{"2001:0DB8:0000:0000:0001:0000:0000:0001", "2001:db8::1:0:0:1"},
             example{"2001:db8:1:2:3:4:0:0", "2001:db8:1:2:3:4::"},
             example{"2001:db8:0:1:2:3:4:5", "2001:db8:0:1:2:3:4:5"},
             example{"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295"},
             example{"::ffff:c000:201", "::ffff:192.0.2.1"},
             example{"fe80::1%12", "fe80::1%12"}}) {
        SCOPED_TRACE(entry.input);
        const auto address = ruvia::ip_address::parse(entry.input);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->to_string(), entry.canonical);
        EXPECT_EQ(ruvia::ip_address::parse(address->to_string()), address);
    }
}

TEST(RuviaHttp, ip_address_preserves_ipv6_family_scope_and_mapped_identity) {
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 0xfe;
    bytes[1] = 0x80;
    bytes[15] = 1;
    const auto address = ruvia::ip_address::ipv6(bytes, 12);
    EXPECT_EQ(address.family(), ruvia::ip_address_family::ipv6);
    EXPECT_EQ(address.bytes().size(), 16);
    EXPECT_EQ(address.scope_id(), 12);
    EXPECT_EQ(address.to_string(), "fe80::1%12");
    EXPECT_NE(address, ruvia::ip_address::ipv6(bytes, 13));
    EXPECT_NE(ruvia::ip_address::parse("192.0.2.1"), ruvia::ip_address::parse("::ffff:192.0.2.1"));
}

TEST(RuviaHttp, ip_address_rejects_nonliteral_and_malformed_input) {
    for (const std::string_view text : {"", "localhost", "1", "192.0.2", "01.2.3.4", "192.0.2.256",
             "192.0.2.1:80", "192.0.2.1%1", "[::1]", "[::1]:80", " ::1", "::1 ",
             "1::2::3", "1:2:3:4:5:6:7:8:9", "::ffff:01.2.3.4", "fe80::1%",
             "fe80::1%eth0", "fe80::1%-1", "fe80::1%+1", "fe80::1%4294967296", "fe80::1%1%2"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(ruvia::ip_address::parse(text).has_value());
    }
    EXPECT_FALSE(ruvia::ip_address::parse(std::string_view("::1\0suffix", 10)).has_value());
    constexpr std::string_view bounded = "192.0.2.1suffix";
    EXPECT_TRUE(ruvia::ip_address::parse(bounded.substr(0, 9)).has_value());
    EXPECT_FALSE(ruvia::ip_address::parse(bounded).has_value());
}

TEST(RuviaHttp, ip_address_formats_maximal_scoped_ipv6) {
    const auto address = ruvia::ip_address::parse("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->to_string(), "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
}

TEST(Ruvia, port_number_rejects_wide_inputs_without_changing_an_existing_origin) {
    const auto rejects = [](auto input) {
        try {
            (void)ruvia::http_origin_view::https({.host_ = "example.test", .port_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 7> invalid{
        std::numeric_limits<std::int64_t>::min(), -65093, -1,
        65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid) {
        EXPECT_TRUE(rejects(input));
        EXPECT_TRUE(!ruvia::port_number::try_from_value(input).has_value());
        ruvia::http_origin_options options{.host_ = "example.test", .port_ = 8443};
        bool assignment_rejected = false;
        try {
            options.port_ = input;
        } catch (const std::invalid_argument&) {
            assignment_rejected = true;
        }
        EXPECT_TRUE(assignment_rejected);
        const auto retained = ruvia::http_origin_view::https(options);
        EXPECT_EQ(ruvia::make_http_origin_authority(retained), "example.test:8443");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rejects(maximum));
    EXPECT_TRUE(!ruvia::port_number::try_from_value(maximum).has_value());
}

TEST(Ruvia, port_number_preserves_zero_maximum_and_scheme_default_authorities) {
    struct port_case final {
        std::uint64_t input_;
        std::string_view authority_;
    };
    for (const auto entry : std::array{
             port_case{0, "[::1]:0"},
             port_case{1, "[::1]:1"},
             port_case{80, "[::1]:80"},
             port_case{443, "[::1]"},
             port_case{65535, "[::1]:65535"}}) {
        const auto origin = ruvia::http_origin_view::https({.host_ = "[::1]", .port_ = entry.input_});
        EXPECT_EQ(ruvia::make_http_origin_authority(origin), entry.authority_);
    }
    const auto absent = ruvia::http_origin_view::http({.host_ = "example.test"});
    const auto zero = ruvia::http_origin_view::http({.host_ = "example.test", .port_ = 0});
    EXPECT_EQ(ruvia::make_http_origin_authority(absent), "example.test");
    EXPECT_EQ(ruvia::make_http_origin_authority(zero), "example.test:0");
}

TEST(Ruvia, port_number_rejects_invalid_authority_defaults_before_origin_comparison) {
    const auto authority = ruvia::parse_http_authority("example.test");
    EXPECT_TRUE(authority.has_value());
    if (!authority) {
        return;
    }
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    for (const auto source : std::array<std::int64_t, 7>{
             std::numeric_limits<std::int64_t>::min(), -65093, -1,
             65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_TRUE(rejects([&] { (void)authority->effective_port(source); }));
        EXPECT_TRUE(rejects([&] {
            (void)ruvia::http_authorities_equal("example.test", "example.test:443", source);
        }));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rejects([&] { (void)authority->effective_port(maximum); }));
    EXPECT_TRUE(rejects([&] {
        (void)ruvia::http_authorities_equal("example.test", "example.test:443", maximum);
    }));
    EXPECT_TRUE(ruvia::http_authorities_equal("example.test", "EXAMPLE.test:443", 443));
    EXPECT_TRUE(!ruvia::http_authorities_equal("example.test", "example.test:443", 80));
}

TEST(Ruvia, port_number_preserves_absent_empty_and_explicit_authority_port_rules) {
    const auto absent = ruvia::parse_http_authority("[::1]");
    const auto empty = ruvia::parse_http_authority("[::1]:");
    const auto explicit_port = ruvia::parse_http_authority("[::1]:8443");
    EXPECT_TRUE(absent && empty && explicit_port);
    if (!absent || !empty || !explicit_port) {
        return;
    }
    for (const auto source : std::array<std::uint64_t, 5>{0, 1, 80, 443, 65535}) {
        EXPECT_TRUE(absent->effective_port(source) == source);
        EXPECT_TRUE(empty->effective_port(source) == source);
        EXPECT_TRUE(explicit_port->effective_port(source) == 8443);
        EXPECT_TRUE(ruvia::http_authorities_equal("[::1]", "[::1]:", source));
    }
    EXPECT_TRUE(!ruvia::http_authorities_equal("[::1]", "[::1]:0", 0));
    EXPECT_TRUE(ruvia::http_authorities_equal("[::1]:", "[::1]:65535", 65535));
    EXPECT_TRUE(!ruvia::http_authorities_equal("[::1]:8443", "[::1]:443", 443));
}

namespace {

void write_text(std::span<char> target, std::string_view text) {
    std::memcpy(target.data(), text.data(), text.size());
}

enum class reallocation_operation { reserve,
    shrink,
    append,
    append_alias };

}  // namespace

TEST(Ruvia, io_buffer_publishes_only_committed_bytes) {
    {
        ruvia::http_io_buffer buffer;
        EXPECT_TRUE(buffer.empty());
        auto space = buffer.prepare(16);
        EXPECT_EQ(space.size(), std::size_t{16});
        EXPECT_TRUE(buffer.empty());
        write_text(space, "hello");
        buffer.commit(5);
        EXPECT_EQ(buffer.view(), "hello");
        EXPECT_TRUE(buffer.capacity() >= std::size_t{16});
        EXPECT_TRUE(ruvia::test::throws_on([&] { buffer.commit(buffer.capacity()); }));
        EXPECT_EQ(buffer.view(), "hello");
    }
}

TEST(Ruvia, io_buffer_grows_geometrically_and_keeps_readable_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.append("abc");
    EXPECT_EQ(buffer.capacity(), std::size_t{3});
    write_text(buffer.prepare(1), "d");
    buffer.commit(1);
    EXPECT_EQ(buffer.capacity(), std::size_t{6});
    EXPECT_EQ(buffer.view(), "abcd");
    const std::string large(100, 'x');
    buffer.append(large);
    EXPECT_EQ(buffer.view(), "abcd" + large);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)buffer.prepare(static_cast<std::size_t>(-1)); }));
    EXPECT_EQ(buffer.view(), "abcd" + large);
}

TEST(Ruvia, io_buffer_append_accepts_its_own_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdef");
    // Fits the tail.
    buffer.append(buffer.view().substr(0, 2));
    EXPECT_EQ(buffer.view(), "abcdefab");
    // Needs new storage while the source lives in the old one.
    buffer.append(buffer.view());
    EXPECT_EQ(buffer.view(), "abcdefababcdefab");
    // Needs the room of the consumed prefix while the source lies inside it.
    ruvia::http_io_buffer window;
    window.reserve(8);
    window.append("abcdefgh");
    const auto consumed = window.view();
    window.consume(6);
    window.append(consumed.substr(0, 4));
    EXPECT_EQ(window.view(), "ghabcd");
    EXPECT_EQ(window.capacity(), std::size_t{8});
}

TEST(Ruvia, io_buffer_small_reads_preserve_the_sliding_window) {
    for (const bool prepared : {false, true}) {
        {
            ruvia::http_io_buffer buffer;
            std::string expected(64, '\0');
            for (std::size_t index = 0; index < expected.size(); ++index) {
                expected[index] = static_cast<char>(index);
            }
            buffer.append(expected);
            for (std::size_t index = 0; index < 1024; ++index) {
                buffer.consume(1);
                expected.erase(0, 1);
                const char byte = static_cast<char>(index);
                if (prepared) {
                    buffer.prepare(1)[0] = byte;
                    buffer.commit(1);
                } else {
                    buffer.append(std::string_view(&byte, 1));
                }
                expected.push_back(byte);
                EXPECT_EQ(buffer.view(), std::string_view(expected));
            }
            // A self-append can grow or compact; it must read the old window.
            buffer.consume(32);
            expected.erase(0, 32);
            const auto copied = expected;
            buffer.append(buffer.view());
            expected.append(copied);
            EXPECT_EQ(buffer.view(), std::string_view(expected));
        }
    }
}

TEST(Ruvia, io_buffer_reader_prepares_when_no_spare_is_available) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdefgh");
    buffer.consume(1);
    auto space = buffer.prepare_spare();
    if (space.empty()) {
        space = buffer.prepare(1);
    }
    ASSERT_FALSE(space.empty());
    space[0] = 'i';
    buffer.commit(1);
    EXPECT_EQ(buffer.view(), "bcdefghi");
}

TEST(Ruvia, http_priority_structured_dictionary_types_duplicates_and_parameters) {
    const auto parsed_value = ruvia::parse_http_priority("u=5;note=\"x\", i, extra=(token :YWJj:);p=?0");
    EXPECT_TRUE((parsed_value.index() == 0));
    EXPECT_EQ(*std::get<0>(parsed_value).urgency_, 5);
    EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
    const auto defaults = ruvia::parse_http_priority("u=8, i=42, u=2, u=\"ignored\"");
    EXPECT_TRUE((defaults.index() == 0) && !std::get<0>(defaults).urgency_ && !std::get<0>(defaults).incremental_);
    EXPECT_EQ(std::get<0>(defaults).request_priority().urgency_, 3);
    const auto partial = ruvia::parse_http_priority("u=0");
    EXPECT_TRUE((partial.index() == 0) && std::get<0>(partial).urgency_ == 0 && !std::get<0>(partial).incremental_);
    for (const auto invalid : {"u=", "u=2,", "U=2", "i=?2", "u=1.1234", "u=(x\t y)", "x=\"bad\\q\""}) {
        EXPECT_TRUE((ruvia::parse_http_priority(invalid).index() != 0));
    }
}
TEST(Ruvia, http_priority_byte_sequences_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"u=2, i;bytes=:YQ=:", "u=2, i;bytes=:YQ==:", "u=2, i;bytes=:YQ:",
             "u=2, i;bytes=:YWJj:", "u=2, i;bytes=:YWJ:", "u=2, i;bytes=:YWJ=:", "u=2, i;bytes=:YR==:",
             "u=2, i, x=:YQ=:", "u=2, i, x=(:YQ=:)", "u=2, i;bytes=::"}) {
        const auto parsed_value = ruvia::parse_http_priority(value);
        EXPECT_TRUE((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            EXPECT_TRUE(std::get<0>(parsed_value).urgency_ == 2);
            EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
        }
    }
    for (const auto value : {"u=2, i;bytes=:Y:", "u=2, i;bytes=:Y=:", "u=2, i;bytes=:YWJj=:",
             "u=2, i;bytes=:YWJ==:", "u=2, i;bytes=:YQ===:", "u=2, i;bytes=:Y=Q:",
             "u=2, i;bytes=:YQ$:", "u=2, i;bytes=:YQ\n:", "u=2, i;bytes=:YQ"}) {
        EXPECT_TRUE((ruvia::parse_http_priority(value).index() != 0));
    }
}

TEST(Ruvia, http_priority_repeated_headers_combine_members_and_replace_invalid_values) {
    const std::array headers{ruvia::http_header_view{"Priority", "u=2, i"},
        ruvia::http_header_view{"X-Other", "u=0"},
        ruvia::http_header_view{"pRIORITY", "u=\"ignored\""}};
    const auto parsed_value = ruvia::parse_http_priority(headers);
    EXPECT_TRUE((parsed_value.index() == 0) && !std::get<0>(parsed_value).urgency_ && std::get<0>(parsed_value).incremental_ == true);
    const std::array malformed{ruvia::http_header_view{"priority", "u=2"},
        ruvia::http_header_view{"priority", "i=?2"}};
    EXPECT_TRUE((ruvia::parse_http_priority(malformed).index() != 0));
    const std::array empty_repeated{ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"priority", "u=1"}};
    EXPECT_TRUE((ruvia::parse_http_priority(empty_repeated).index() != 0));
    EXPECT_TRUE(std::get<0>(ruvia::parse_http_priority(std::span<const ruvia::http_header_view>{})).request_priority().urgency_ == 3);
}

TEST(Ruvia, http_priority_repeated_fields_match_their_comma_joined_dictionary) {
    const std::array headers{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"X-Other", "ignored"},
        ruvia::http_header_view{"priority", "beta\", u=1, i"}};
    const auto combined = ruvia::parse_http_priority("extra=\"alpha, beta\", u=1, i");
    EXPECT_TRUE((combined.index() == 0) && std::get<0>(combined).urgency_ == 1 && std::get<0>(combined).incremental_ == true);
    const auto parsed_value = ruvia::parse_http_priority(headers);
    EXPECT_TRUE((parsed_value.index() == 0));
    if (parsed_value.index() == 0) {
        EXPECT_TRUE(std::get<0>(parsed_value).urgency_ == 1);
        EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
    }

    const std::array parameter_headers{ruvia::http_header_view{"priority", "i;note=\"a"},
        ruvia::http_header_view{"priority", "b\", u=0"}};
    const auto parameters = ruvia::parse_http_priority(parameter_headers);
    EXPECT_TRUE((parameters.index() == 0));
    if (parameters.index() == 0) {
        EXPECT_TRUE(std::get<0>(parameters).urgency_ == 0);
        EXPECT_TRUE(std::get<0>(parameters).incremental_ == true);
    }
}

TEST(Ruvia, http_priority_repeated_fields_preserve_separators_and_empty_values) {
    const std::array with_empty{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"pRIORITY", "beta\", u=1, i"}};
    const auto parsed_value = ruvia::parse_http_priority(with_empty);
    EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).urgency_ == 1 && std::get<0>(parsed_value).incremental_ == true);

    const std::array tab_separated{ruvia::http_header_view{"priority", "u=0"},
        ruvia::http_header_view{"priority", "\ti"}};
    const auto tab = ruvia::parse_http_priority(tab_separated);
    EXPECT_TRUE((tab.index() == 0) && std::get<0>(tab).urgency_ == 0 && std::get<0>(tab).incremental_ == true);

    const std::array fragments{
        std::array<std::string_view, 2>{"u=1", ""},
        std::array<std::string_view, 2>{"u=1", "\t"},
        std::array<std::string_view, 2>{"u=1, i=?", "1"},
        std::array<std::string_view, 2>{"u=1, i, x=:YQ", ":"},
        std::array<std::string_view, 2>{"u=1", "2, i"},
        std::array<std::string_view, 2>{"x=\"a\\", "\"b\", u=1, i"},
    };
    for (const auto& values : fragments) {
        const std::array headers{ruvia::http_header_view{"priority", values[0]},
            ruvia::http_header_view{"priority", values[1]}};
        EXPECT_TRUE(ruvia::parse_http_priority(headers).index() != 0);
    }
}

TEST(Ruvia, http_priority_http2_frame_roundtrips_and_output_bounds) {
    std::array<char, 64> bytes_value{};
    auto h2 = ruvia::encode_http2_priority_update(bytes_value, 5, {.urgency_ = 1, .incremental_ = true});
    EXPECT_TRUE((h2.index() == 0));
    auto decoded = ruvia::decode_http2_priority_update(std::span(bytes_value).subspan(9, std::get<0>(h2) - 9));
    EXPECT_TRUE((decoded.index() == 0) && std::get<0>(decoded).element_id_ == 5 && !std::get<0>(decoded).push_ && std::get<0>(decoded).fields_.urgency_ == 1);
    EXPECT_TRUE((ruvia::encode_http_priority(std::span(bytes_value).first(1), {.urgency_ = 2}).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_priority_update(bytes_value, 0, {}).index() != 0));
}

TEST(Ruvia, http_priority_rejects_wide_local_urgency_without_replacing_an_existing_signal) {
    std::array<char, 64> bytes{};
    const auto rejects = [&](auto input) {
        try {
            (void)ruvia::encode_http_priority(bytes, {.urgency_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    for (const auto input : std::array<std::int64_t, 7>{
             std::numeric_limits<std::int64_t>::min(), -256, -1, 8,
             256, 259, std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_TRUE(rejects(input));
    }
    EXPECT_TRUE(rejects(std::numeric_limits<std::uint64_t>::max()));

    ruvia::http_priority_fields fields{.urgency_ = 2, .incremental_ = true};
    bool rejected = false;
    try {
        fields.urgency_ = 259;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    const auto encoded = ruvia::encode_http_priority(bytes, fields);
    EXPECT_TRUE(encoded.index() == 0);
    if (const auto* count = std::get_if<std::size_t>(&encoded)) {
        const auto reparsed = ruvia::parse_http_priority(std::string_view(bytes.data(), *count));
        EXPECT_TRUE(reparsed.index() == 0);
        if (const auto* retained = std::get_if<ruvia::http_priority_fields>(&reparsed)) {
            EXPECT_TRUE(retained->urgency_ == 2);
            EXPECT_TRUE(retained->incremental_ == true);
        }
    }
}

TEST(Ruvia, http_priority_ignores_out_of_range_peer_urgency_instead_of_narrowing_it) {
    // RFC 9218 section 4 requires ignoring successfully parsed values outside
    // the urgency domain, including one that replaces a valid earlier member.
    for (const auto value : {"u=-256", "u=-1", "u=8", "u=256", "u=259", "u=2, u=256"}) {
        const auto parsed = ruvia::parse_http_priority(value);
        EXPECT_TRUE(parsed.index() == 0);
        if (const auto* fields = std::get_if<ruvia::http_priority_fields>(&parsed)) {
            EXPECT_TRUE(!fields->urgency_.has_value());
            EXPECT_TRUE(fields->request_priority().urgency_ == 3);
        }
    }
    const auto maximum = ruvia::parse_http_priority("u=7, i=?0");
    EXPECT_TRUE(maximum.index() == 0);
    if (const auto* fields = std::get_if<ruvia::http_priority_fields>(&maximum)) {
        EXPECT_TRUE(fields->urgency_ == 7);
        EXPECT_TRUE(fields->incremental_ == false);
    }
}
