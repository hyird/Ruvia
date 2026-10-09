#include <array>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http3_qpack.h"

#include "test_harness.h"

RUVIA_TEST(http3_qpack_static_table_uses_rfc_9204_indices) {
    const auto authority = ruvia::get_http3_qpack_static_entry(0);
    RUVIA_CHECK((authority.index() == 0));
    if ((authority.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(authority).name_, std::string_view(":authority"));
        RUVIA_CHECK(std::get<0>(authority).value_.empty());
    }
    const auto get = ruvia::get_http3_qpack_static_entry(17);
    RUVIA_CHECK((get.index() == 0));
    if ((get.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(get).name_, std::string_view(":method"));
        RUVIA_CHECK_EQ(std::get<0>(get).value_, std::string_view("GET"));
    }
    const auto last = ruvia::get_http3_qpack_static_entry(98);
    RUVIA_CHECK((last.index() == 0));
    if ((last.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(last).name_, std::string_view("x-frame-options"));
        RUVIA_CHECK_EQ(std::get<0>(last).value_, std::string_view("sameorigin"));
    }
    RUVIA_CHECK(std::get<1>(ruvia::get_http3_qpack_static_entry(99)) == ruvia::http3_qpack_error::invalid_index);
}

RUVIA_TEST(http3_qpack_static_table_matches_rfc_9204_critical_indices) {
    constexpr std::array<ruvia::http3_qpack_static_entry, 9> cache_and_content_entries{{
        {"cache-control", "max-age=0"},
        {"cache-control", "max-age=2592000"},
        {"cache-control", "max-age=604800"},
        {"cache-control", "no-cache"},
        {"cache-control", "no-store"},
        {"cache-control", "public, max-age=31536000"},
        {"content-encoding", "br"},
        {"content-encoding", "gzip"},
        {"content-type", "application/dns-message"},
    }};
    for (std::size_t offset = 0; offset < cache_and_content_entries.size(); ++offset) {
        const auto entry_value = ruvia::get_http3_qpack_static_entry(36 + offset);
        RUVIA_CHECK((entry_value.index() == 0));
        if ((entry_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(entry_value).name_, cache_and_content_entries[offset].name_);
            RUVIA_CHECK_EQ(std::get<0>(entry_value).value_, cache_and_content_entries[offset].value_);
        }
    }

    constexpr std::array<ruvia::http3_qpack_static_entry, 11> xss_status_and_accept_entries{{
        {"x-xss-protection", "1; mode=block"},
        {":status", "100"},
        {":status", "204"},
        {":status", "206"},
        {":status", "302"},
        {":status", "400"},
        {":status", "403"},
        {":status", "421"},
        {":status", "425"},
        {":status", "500"},
        {"accept-language", ""},
    }};
    for (std::size_t offset = 0; offset < xss_status_and_accept_entries.size(); ++offset) {
        const auto entry_value = ruvia::get_http3_qpack_static_entry(62 + offset);
        RUVIA_CHECK((entry_value.index() == 0));
        if ((entry_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(entry_value).name_, xss_status_and_accept_entries[offset].name_);
            RUVIA_CHECK_EQ(std::get<0>(entry_value).value_, xss_status_and_accept_entries[offset].value_);
        }
    }

    const auto last = ruvia::get_http3_qpack_static_entry(98);
    RUVIA_CHECK((last.index() == 0));
    if ((last.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(last).name_, std::string_view("x-frame-options"));
        RUVIA_CHECK_EQ(std::get<0>(last).value_, std::string_view("sameorigin"));
    }
}

RUVIA_TEST(http3_qpack_prefixed_integer_round_trips_large_values) {
    constexpr std::uint64_t value = 0x123456789abcdef0ULL;
    std::array<char, 16> wire{};
    const auto written = ruvia::encode_http3_qpack_integer(wire, 5, 0xe0, value);
    RUVIA_CHECK((written.index() == 0));
    if ((written.index() != 0)) {
        return;
    }
    const auto decoded = ruvia::decode_http3_qpack_integer(std::span<const char>(wire).first(std::get<0>(written)), 5);
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(decoded).value_, value);
        RUVIA_CHECK_EQ(std::get<0>(decoded).encoded_bytes_, std::get<0>(written));
    }
    const std::array<char, 1> truncated{static_cast<char>(0xff)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_qpack_integer(truncated, 5)) ==
                ruvia::http3_qpack_error::need_more_data);
}

RUVIA_TEST(http3_qpack_prefixed_integer_rejects_overflow_and_bad_prefixes) {
    const std::array<char, 11> overflow{
        static_cast<char>(0xff), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0x02)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_qpack_integer(overflow, 5)) ==
                ruvia::http3_qpack_error::integer_overflow);
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_qpack_integer(overflow, 0)) ==
                ruvia::http3_qpack_error::integer_overflow);
    std::array<char, 1> output{};
    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_qpack_integer(output, 9, 0, 1)) ==
                ruvia::http3_qpack_error::integer_overflow);
}

RUVIA_TEST(http3_qpack_string_literals_support_raw_and_shared_huffman) {
    std::array<char, 32> wire{};
    const auto written = ruvia::encode_http3_qpack_string(wire, "hello");
    RUVIA_CHECK((written.index() == 0));
    std::pmr::string decoded;
    if ((written.index() == 0)) {
        const auto consumed = ruvia::decode_http3_qpack_string(
            std::span<const char>(wire).first(std::get<0>(written)), decoded);
        RUVIA_CHECK((consumed.index() == 0));
        RUVIA_CHECK_EQ(decoded, std::string_view("hello"));
        if ((consumed.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(consumed), std::get<0>(written));
        }
    }

    constexpr std::array<char, 13> huffman{
        static_cast<char>(0x8c), static_cast<char>(0xf1), static_cast<char>(0xe3),
        static_cast<char>(0xc2), static_cast<char>(0xe5), static_cast<char>(0xf2),
        static_cast<char>(0x3a), static_cast<char>(0x6b), static_cast<char>(0xa0),
        static_cast<char>(0xab), static_cast<char>(0x90), static_cast<char>(0xf4),
        static_cast<char>(0xff)};
    const auto consumed = ruvia::decode_http3_qpack_string(huffman, decoded);
    RUVIA_CHECK((consumed.index() == 0));
    RUVIA_CHECK_EQ(decoded, std::string_view("www.example.com"));
    if ((consumed.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(consumed), huffman.size());
    }

    auto invalid_padding = huffman;
    invalid_padding.back() = static_cast<char>(0xfe);
    decoded = "stale";
    const auto bad_padding = ruvia::decode_http3_qpack_string(invalid_padding, decoded);
    RUVIA_CHECK(!(bad_padding.index() == 0));
    if ((bad_padding.index() != 0)) {
        RUVIA_CHECK(std::get<1>(bad_padding) == ruvia::http3_qpack_error::invalid_huffman);
    }
    RUVIA_CHECK(decoded.empty());

    constexpr std::array<char, 5> eos{
        static_cast<char>(0x84), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xfc)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_qpack_string(eos, decoded)) ==
                ruvia::http3_qpack_error::invalid_huffman);

    const std::array<char, 1> short_length{static_cast<char>(0x82)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_qpack_string(short_length, decoded)) ==
                ruvia::http3_qpack_error::need_more_data);
    std::array<char, 2> short_output{};
    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_qpack_string(short_output, "hello")) ==
                ruvia::http3_qpack_error::output_too_small);
    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_qpack_integer(short_output, 7, 0, 1000)) ==
                ruvia::http3_qpack_error::output_too_small);
}

RUVIA_TEST(http3_qpack_huffman_strings_support_each_prefix_width_and_output_reuse) {
    // RFC 7541 C.4.1, reused by QPACK with the representation's own prefix width.
    constexpr std::array<char, 12> payload_value{
        '\xf1', '\xe3', '\xc2', '\xe5', '\xf2', '\x3a',
        '\x6b', '\xa0', '\xab', '\x90', '\xf4', '\xff'};
    std::pmr::string decoded;
    for (std::uint8_t prefix_bits = 1; prefix_bits <= 7; ++prefix_bits) {
        std::array<char, 16> prefix{};
        const auto written = ruvia::encode_http3_qpack_integer(
            prefix, prefix_bits, static_cast<std::uint8_t>(1U << prefix_bits), payload_value.size());
        RUVIA_CHECK((written.index() == 0));
        if ((written.index() != 0)) {
            continue;
        }
        std::string encoded(prefix.data(), std::get<0>(written));
        encoded.append(payload_value.data(), payload_value.size());
        decoded = "old output";
        const auto consumed = ruvia::decode_http3_qpack_string(encoded, prefix_bits, decoded);
        RUVIA_CHECK((consumed.index() == 0));
        if (consumed.index() == 0) {
            RUVIA_CHECK_EQ(std::get<0>(consumed), encoded.size());
        }
        RUVIA_CHECK_EQ(decoded, std::string_view("www.example.com"));

        encoded.back() = '\xfe';
        const auto invalid = ruvia::decode_http3_qpack_string(encoded, prefix_bits, decoded);
        RUVIA_CHECK(!(invalid.index() == 0));
        if ((invalid.index() != 0)) {
            RUVIA_CHECK(std::get<1>(invalid) == ruvia::http3_qpack_error::invalid_huffman);
        }
        RUVIA_CHECK(decoded.empty());

        decoded = "old output";
        const std::array<char, 1> empty{static_cast<char>(1U << prefix_bits)};
        const auto consumed_empty = ruvia::decode_http3_qpack_string(empty, prefix_bits, decoded);
        RUVIA_CHECK((consumed_empty.index() == 0));
        RUVIA_CHECK(decoded.empty());
    }
}
