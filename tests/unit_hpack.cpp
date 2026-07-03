#include "test_harness.h"

#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/http2/Http2Hpack.h"

namespace {

using ruvia::detail::HpackDecoder;
using ruvia::detail::HpackEncoder;

struct Collector final {
    std::vector<std::pair<std::string, std::string>> headers;
};

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<Collector*>(target)->headers.emplace_back(std::string(name), std::string(value));
    return true;
}

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

// Decode an HPACK block into (name, value) pairs; returns whether it succeeded.
bool decodeBlock(std::string_view block, Collector& out) {
    HpackDecoder decoder(std::pmr::get_default_resource());
    return decoder.decode(block, &out, &collect).ok();
}

}  // namespace

RUVIA_TEST(hpack_indexed_static_header) {
    // RFC 7541 C.2.4: 0x82 -> static index 2 -> :method: GET.
    Collector out;
    RUVIA_CHECK(decodeBlock(bytes({0x82}), out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{1});
    RUVIA_CHECK_EQ(out.headers[0].first, std::string(":method"));
    RUVIA_CHECK_EQ(out.headers[0].second, std::string("GET"));
}

RUVIA_TEST(hpack_request_literal_no_huffman) {
    // RFC 7541 C.3.1: indexed :method/:scheme/:path plus a literal :authority.
    Collector out;
    RUVIA_CHECK(decodeBlock(
        bytes({0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65,
               0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d}),
        out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{4});
    RUVIA_CHECK_EQ(out.headers[0], std::make_pair(std::string(":method"), std::string("GET")));
    RUVIA_CHECK_EQ(out.headers[1], std::make_pair(std::string(":scheme"), std::string("http")));
    RUVIA_CHECK_EQ(out.headers[2], std::make_pair(std::string(":path"), std::string("/")));
    RUVIA_CHECK_EQ(out.headers[3],
                   std::make_pair(std::string(":authority"), std::string("www.example.com")));
}

RUVIA_TEST(hpack_request_literal_huffman) {
    // RFC 7541 C.4.1: same request but :authority is Huffman-encoded. This
    // exercises the Huffman decoder against a known-answer vector.
    Collector out;
    RUVIA_CHECK(decodeBlock(
        bytes({0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2,
               0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff}),
        out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{4});
    RUVIA_CHECK_EQ(out.headers[3],
                   std::make_pair(std::string(":authority"), std::string("www.example.com")));
}

RUVIA_TEST(hpack_encode_decode_round_trip) {
    std::pmr::string encoded(std::pmr::get_default_resource());
    HpackEncoder::encodeHeader(encoded, "x-custom-header", "custom value");
    Collector out;
    RUVIA_CHECK(decodeBlock(std::string_view(encoded.data(), encoded.size()), out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{1});
    RUVIA_CHECK_EQ(out.headers[0],
                   std::make_pair(std::string("x-custom-header"), std::string("custom value")));
}

RUVIA_TEST(hpack_rejects_truncated_and_bad_index) {
    Collector out;
    // A literal header claiming a 15-byte value but supplying only one byte.
    RUVIA_CHECK(!decodeBlock(bytes({0x41, 0x0f, 0x77}), out));
    // An indexed field referencing index 0 is invalid (RFC 7541 6.1).
    Collector out2;
    RUVIA_CHECK(!decodeBlock(bytes({0x80}), out2));
}

RUVIA_TEST(hpack_integer_overflow_is_rejected) {
    using ruvia::detail::HpackError;
    // An indexed field whose index integer overflows uint32 (FF FF FF FF FF 0F)
    // must be reported as an integer overflow, not silently wrapped to a small
    // (and possibly valid) index (RFC 7541 5.1).
    HpackDecoder decoder(std::pmr::get_default_resource());
    Collector out;
    const auto block = bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
    const auto result = decoder.decode(block, &out, &collect);
    RUVIA_CHECK(!result.ok());
    RUVIA_CHECK(result.error == HpackError::kIntegerOverflow);
}

RUVIA_TEST(hpack_long_value_round_trips) {
    // A value longer than 127 bytes forces a multi-byte length prefix, exercising
    // the continuation-integer decode on the valid (non-overflowing) path.
    const std::string longValue(300, 'x');
    std::pmr::string encoded(std::pmr::get_default_resource());
    HpackEncoder::encodeHeader(encoded, "x-long", longValue);
    Collector out;
    RUVIA_CHECK(decodeBlock(std::string_view(encoded.data(), encoded.size()), out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{1});
    RUVIA_CHECK_EQ(out.headers[0].first, std::string("x-long"));
    RUVIA_CHECK_EQ(out.headers[0].second, longValue);
}
