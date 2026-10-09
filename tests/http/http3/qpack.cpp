#include <array>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/Http3Qpack.h"

#include "test_harness.h"

RUVIA_TEST(http3_qpack_static_table_uses_rfc_9204_indices) {
    const auto authority = ruvia::http3QpackStaticEntry(0);
    RUVIA_CHECK(authority.has_value());
    if (authority) {
        RUVIA_CHECK_EQ(authority->name, std::string_view(":authority"));
        RUVIA_CHECK(authority->value.empty());
    }
    const auto get = ruvia::http3QpackStaticEntry(17);
    RUVIA_CHECK(get.has_value());
    if (get) {
        RUVIA_CHECK_EQ(get->name, std::string_view(":method"));
        RUVIA_CHECK_EQ(get->value, std::string_view("GET"));
    }
    const auto last = ruvia::http3QpackStaticEntry(98);
    RUVIA_CHECK(last.has_value());
    if (last) {
        RUVIA_CHECK_EQ(last->name, std::string_view("x-frame-options"));
        RUVIA_CHECK_EQ(last->value, std::string_view("sameorigin"));
    }
    RUVIA_CHECK(ruvia::http3QpackStaticEntry(99).error() == ruvia::Http3QpackError::kInvalidIndex);
}

RUVIA_TEST(http3_qpack_static_table_matches_rfc_9204_critical_indices) {
    constexpr std::array<ruvia::Http3QpackStaticEntry, 9> cacheAndContentEntries{{
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
    for (std::size_t offset = 0; offset < cacheAndContentEntries.size(); ++offset) {
        const auto entry = ruvia::http3QpackStaticEntry(36 + offset);
        RUVIA_CHECK(entry.has_value());
        if (entry) {
            RUVIA_CHECK_EQ(entry->name, cacheAndContentEntries[offset].name);
            RUVIA_CHECK_EQ(entry->value, cacheAndContentEntries[offset].value);
        }
    }

    constexpr std::array<ruvia::Http3QpackStaticEntry, 11> xssStatusAndAcceptEntries{{
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
    for (std::size_t offset = 0; offset < xssStatusAndAcceptEntries.size(); ++offset) {
        const auto entry = ruvia::http3QpackStaticEntry(62 + offset);
        RUVIA_CHECK(entry.has_value());
        if (entry) {
            RUVIA_CHECK_EQ(entry->name, xssStatusAndAcceptEntries[offset].name);
            RUVIA_CHECK_EQ(entry->value, xssStatusAndAcceptEntries[offset].value);
        }
    }

    const auto last = ruvia::http3QpackStaticEntry(98);
    RUVIA_CHECK(last.has_value());
    if (last) {
        RUVIA_CHECK_EQ(last->name, std::string_view("x-frame-options"));
        RUVIA_CHECK_EQ(last->value, std::string_view("sameorigin"));
    }
}

RUVIA_TEST(http3_qpack_prefixed_integer_round_trips_large_values) {
    constexpr std::uint64_t value = 0x123456789abcdef0ULL;
    std::array<char, 16> wire{};
    const auto written = ruvia::encodeHttp3QpackInteger(wire, 5, 0xe0, value);
    RUVIA_CHECK(written.has_value());
    if (!written) {
        return;
    }
    const auto decoded = ruvia::decodeHttp3QpackInteger(std::span<const char>(wire).first(*written), 5);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(decoded->value, value);
        RUVIA_CHECK_EQ(decoded->encodedBytes, *written);
    }
    const std::array<char, 1> truncated{static_cast<char>(0xff)};
    RUVIA_CHECK(ruvia::decodeHttp3QpackInteger(truncated, 5).error() ==
                ruvia::Http3QpackError::kNeedMoreData);
}

RUVIA_TEST(http3_qpack_prefixed_integer_rejects_overflow_and_bad_prefixes) {
    const std::array<char, 11> overflow{
        static_cast<char>(0xff), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0x02)};
    RUVIA_CHECK(ruvia::decodeHttp3QpackInteger(overflow, 5).error() ==
                ruvia::Http3QpackError::kIntegerOverflow);
    RUVIA_CHECK(ruvia::decodeHttp3QpackInteger(overflow, 0).error() ==
                ruvia::Http3QpackError::kIntegerOverflow);
    std::array<char, 1> output{};
    RUVIA_CHECK(ruvia::encodeHttp3QpackInteger(output, 9, 0, 1).error() ==
                ruvia::Http3QpackError::kIntegerOverflow);
}

RUVIA_TEST(http3_qpack_string_literals_support_raw_and_shared_huffman) {
    std::array<char, 32> wire{};
    const auto written = ruvia::encodeHttp3QpackString(wire, "hello");
    RUVIA_CHECK(written.has_value());
    std::pmr::string decoded;
    if (written) {
        const auto consumed = ruvia::decodeHttp3QpackString(
            std::span<const char>(wire).first(*written), decoded);
        RUVIA_CHECK(consumed.has_value());
        RUVIA_CHECK_EQ(decoded, std::string_view("hello"));
        if (consumed) {
            RUVIA_CHECK_EQ(*consumed, *written);
        }
    }

    constexpr std::array<char, 13> huffman{
        static_cast<char>(0x8c), static_cast<char>(0xf1), static_cast<char>(0xe3),
        static_cast<char>(0xc2), static_cast<char>(0xe5), static_cast<char>(0xf2),
        static_cast<char>(0x3a), static_cast<char>(0x6b), static_cast<char>(0xa0),
        static_cast<char>(0xab), static_cast<char>(0x90), static_cast<char>(0xf4),
        static_cast<char>(0xff)};
    const auto consumed = ruvia::decodeHttp3QpackString(huffman, decoded);
    RUVIA_CHECK(consumed.has_value());
    RUVIA_CHECK_EQ(decoded, std::string_view("www.example.com"));
    if (consumed) {
        RUVIA_CHECK_EQ(*consumed, huffman.size());
    }

    auto invalidPadding = huffman;
    invalidPadding.back() = static_cast<char>(0xfe);
    decoded = "stale";
    const auto badPadding = ruvia::decodeHttp3QpackString(invalidPadding, decoded);
    RUVIA_CHECK(!badPadding.has_value());
    if (!badPadding) {
        RUVIA_CHECK(badPadding.error() == ruvia::Http3QpackError::kInvalidHuffman);
    }
    RUVIA_CHECK(decoded.empty());

    constexpr std::array<char, 5> eos{
        static_cast<char>(0x84), static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xfc)};
    RUVIA_CHECK(ruvia::decodeHttp3QpackString(eos, decoded).error() ==
                ruvia::Http3QpackError::kInvalidHuffman);

    const std::array<char, 1> shortLength{static_cast<char>(0x82)};
    RUVIA_CHECK(ruvia::decodeHttp3QpackString(shortLength, decoded).error() ==
                ruvia::Http3QpackError::kNeedMoreData);
    std::array<char, 2> shortOutput{};
    RUVIA_CHECK(ruvia::encodeHttp3QpackString(shortOutput, "hello").error() ==
                ruvia::Http3QpackError::kOutputTooSmall);
    RUVIA_CHECK(ruvia::encodeHttp3QpackInteger(shortOutput, 7, 0, 1000).error() ==
                ruvia::Http3QpackError::kOutputTooSmall);
}

RUVIA_TEST(http3_qpack_huffman_strings_support_each_prefix_width_and_output_reuse) {
    // RFC 7541 C.4.1, reused by QPACK with the representation's own prefix width.
    constexpr std::array<char, 12> payload{
        '\xf1', '\xe3', '\xc2', '\xe5', '\xf2', '\x3a',
        '\x6b', '\xa0', '\xab', '\x90', '\xf4', '\xff'};
    std::pmr::string decoded;
    for (std::uint8_t prefix_bits = 1; prefix_bits <= 7; ++prefix_bits) {
        std::array<char, 16> prefix{};
        const auto written = ruvia::encodeHttp3QpackInteger(
            prefix, prefix_bits, static_cast<std::uint8_t>(1U << prefix_bits), payload.size());
        RUVIA_CHECK(written.has_value());
        if (!written) {
            continue;
        }
        std::string encoded(prefix.data(), *written);
        encoded.append(payload.data(), payload.size());
        decoded = "old output";
        const auto consumed = ruvia::decodeHttp3QpackString(encoded, prefix_bits, decoded);
        RUVIA_CHECK(consumed.has_value());
        if (consumed) {
            RUVIA_CHECK_EQ(*consumed, encoded.size());
        }
        RUVIA_CHECK_EQ(decoded, std::string_view("www.example.com"));

        encoded.back() = '\xfe';
        const auto invalid = ruvia::decodeHttp3QpackString(encoded, prefix_bits, decoded);
        RUVIA_CHECK(!invalid.has_value());
        if (!invalid) {
            RUVIA_CHECK(invalid.error() == ruvia::Http3QpackError::kInvalidHuffman);
        }
        RUVIA_CHECK(decoded.empty());

        decoded = "old output";
        const std::array<char, 1> empty{static_cast<char>(1U << prefix_bits)};
        const auto consumed_empty = ruvia::decodeHttp3QpackString(empty, prefix_bits, decoded);
        RUVIA_CHECK(consumed_empty.has_value());
        RUVIA_CHECK(decoded.empty());
    }
}
