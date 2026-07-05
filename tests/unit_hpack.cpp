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

RUVIA_TEST(hpack_encoder_uses_without_indexing_representation) {
    // Security-relevant invariant: the response encoder MUST emit "Literal Header
    // Field without Indexing" (RFC 7541 6.2.2, high nibble 0000) and never "with
    // Incremental Indexing" (0x40-0x7f). Incremental indexing would add per-response
    // entries to the dynamic table -- growing memory unboundedly across a connection
    // and, worse, creating a cross-response HPACK compression side channel
    // (CRIME-class) whose observable encoded sizes can leak secret header values.
    // The round-trip test alone cannot catch a regression here because the decoder
    // accepts both representations; only the wire format distinguishes them.

    // New header name -> "without Indexing, New Name" starts with the octet 0x00.
    {
        std::pmr::string out(std::pmr::get_default_resource());
        HpackEncoder::encodeHeader(out, "x-secret-token", "s3cr3t");
        RUVIA_CHECK(!out.empty());
        RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), 0x00u);
    }
    // A header whose NAME is a static-table entry ("content-type", index 31) with a
    // non-indexed value still uses the without-indexing form (high nibble 0000),
    // i.e. a name index plus a literal value -- never the 0x40-0x7f incremental range.
    {
        std::pmr::string out(std::pmr::get_default_resource());
        HpackEncoder::encodeHeader(out, "content-type", "application/x-ruvia-test");
        RUVIA_CHECK(!out.empty());
        const auto first = static_cast<unsigned char>(out[0]);
        RUVIA_CHECK_EQ(first & 0xf0u, 0x00u);   // literal WITHOUT indexing
        RUVIA_CHECK((first & 0xc0u) != 0x40u);  // specifically not incremental indexing
    }
}

RUVIA_TEST(hpack_encoder_marks_credentials_never_indexed) {
    // RFC 7541 7.1.3: credential-bearing fields SHOULD use the never-indexed literal
    // (high nibble 0001) so that an intermediary along the path never commits them to
    // a shared dynamic table (compression side-channel hardening). This covers both
    // encode branches: static name-index ("authorization"/"cookie") and a literal new
    // name that happens to be sensitive. The decoder accepts both 0x00 and 0x10, so
    // only the wire nibble distinguishes the hardened form -- a round-trip cannot.
    for (const auto* name : {"authorization", "cookie", "set-cookie", "proxy-authorization"}) {
        std::pmr::string out(std::pmr::get_default_resource());
        HpackEncoder::encodeHeader(out, name, "token-value");
        RUVIA_CHECK(!out.empty());
        RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]) & 0xf0u, 0x10u);  // never indexed
    }
    // A non-credential field with an identical value must stay without-indexing, so
    // the choice discriminates by field name rather than blanket-marking everything.
    {
        std::pmr::string out(std::pmr::get_default_resource());
        HpackEncoder::encodeHeader(out, "x-trace-id", "token-value");
        RUVIA_CHECK(!out.empty());
        RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]) & 0xf0u, 0x00u);  // without indexing
    }
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

RUVIA_TEST(hpack_integer_overflow_chunk_bound_is_rejected) {
    using ruvia::detail::HpackError;
    // The continuation decode has a second, distinct overflow guard from the
    // accumulated-sum one above: once the shift reaches 28, a chunk whose 7-bit
    // payload exceeds 0x0f is rejected up front, because (payload << 28) would
    // lose its high bits to uint32 truncation and could then slip past the
    // sum guard. FF 80 80 80 80 1F holds the running value at 0x7f through four
    // zero-payload continuations, so ONLY this chunk-bound guard can catch the
    // final 0x1f<<28 overflow -- removing it would silently wrap (RFC 7541 5.1).
    HpackDecoder decoder(std::pmr::get_default_resource());
    Collector out;
    const auto block = bytes({0xFF, 0x80, 0x80, 0x80, 0x80, 0x1F});
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

RUVIA_TEST(hpack_huffman_rejects_bad_padding_and_eos) {
    // Literal (name :authority via 0x41) with a Huffman value.
    // 0x00: after the 5-bit code for '0' the trailing 000 padding is not all-ones.
    Collector out;
    RUVIA_CHECK(!decodeBlock(bytes({0x41, 0x81, 0x00}), out));
    // Four 0xFF bytes walk 30 one-bits into the EOS symbol, which must be rejected.
    Collector out2;
    RUVIA_CHECK(!decodeBlock(bytes({0x41, 0x84, 0xFF, 0xFF, 0xFF, 0xFF}), out2));
    // A single 0xFF: eight all-ones bits walk the shared all-ones prefix without
    // completing any symbol, leaving 8 padding bits. RFC 7541 5.2 forbids padding
    // longer than 7 bits even when it is all ones -- this exercises the depth>7
    // guard, distinct from the non-all-ones case (0x00) and the complete-EOS case
    // (four 0xFF) above.
    Collector out3;
    RUVIA_CHECK(!decodeBlock(bytes({0x41, 0x81, 0xFF}), out3));
}

RUVIA_TEST(hpack_dynamic_table_add_then_reference) {
    // Literal with incremental indexing + literal name adds "custom-key:
    // custom-value" to the dynamic table; a following index 62 (static size 61 + 1)
    // references that newest dynamic entry.
    std::string block;
    block += static_cast<char>(0x40);                 // literal, incremental indexing, literal name
    block += static_cast<char>(0x0A);                 // name length 10
    block += "custom-key";
    block += static_cast<char>(0x0C);                 // value length 12
    block += "custom-value";
    block += static_cast<char>(0xBE);                 // indexed field, index 62 (0x80 | 62)

    Collector out;
    RUVIA_CHECK(decodeBlock(block, out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{2});
    const auto expected = std::make_pair(std::string("custom-key"), std::string("custom-value"));
    RUVIA_CHECK_EQ(out.headers[0], expected);
    RUVIA_CHECK_EQ(out.headers[1], expected);  // resolved via the dynamic table
}

RUVIA_TEST(hpack_size_update_after_header_is_rejected) {
    // A dynamic-table size update must precede any header field (RFC 7541 4.2):
    // 0x82 (:method GET) then 0x20 (size update) is a decoding error.
    Collector out;
    RUVIA_CHECK(!decodeBlock(bytes({0x82, 0x20}), out));
    // A size update before the header is fine.
    Collector out2;
    RUVIA_CHECK(decodeBlock(bytes({0x20, 0x82}), out2));
    RUVIA_CHECK_EQ(out2.headers.size(), std::size_t{1});
}

RUVIA_TEST(hpack_size_update_exceeding_settings_max_is_rejected) {
    // RFC 7541 §6.3: a dynamic-table size update must not exceed the maximum the
    // decoder advertised via SETTINGS_HEADER_TABLE_SIZE (default 4096). Accepting a
    // larger value would let a peer coerce an oversized dynamic table (a memory-DoS
    // vector). A size update to exactly the ceiling is allowed; one byte over is not.
    // Encoding: 0x20 | 5-bit-prefix(31), then the HPACK varint remainder.
    Collector atCeiling;
    RUVIA_CHECK(decodeBlock(bytes({0x3f, 0xe1, 0x1f}), atCeiling));     // 31 + 97 + 31*128 = 4096
    Collector overCeiling;
    RUVIA_CHECK(!decodeBlock(bytes({0x3f, 0xe2, 0x1f}), overCeiling));  // 4097 > 4096 -> rejected
}

RUVIA_TEST(hpack_size_update_to_zero_evicts_dynamic_table) {
    using ruvia::detail::HpackError;
    // The dynamic table persists across decode() calls, so use one decoder.
    HpackDecoder decoder(std::pmr::get_default_resource());

    // Literal with incremental indexing adds "custom-key: custom-value".
    std::string add;
    add += static_cast<char>(0x40);  // literal, incremental indexing, new name
    add += static_cast<char>(0x0A);  // name length 10
    add += "custom-key";
    add += static_cast<char>(0x0C);  // value length 12
    add += "custom-value";
    Collector added;
    RUVIA_CHECK(decoder.decode(add, &added, &collect).ok());

    // Index 62 (static 61 + newest dynamic) resolves to the entry just added.
    Collector referenced;
    RUVIA_CHECK(decoder.decode(bytes({0xBE}), &referenced, &collect).ok());
    RUVIA_CHECK_EQ(referenced.headers.size(), std::size_t{1});

    // A size update to 0 (0x20) must evict every dynamic entry (RFC 7541 4.3).
    Collector evicted;
    RUVIA_CHECK(decoder.decode(bytes({0x20}), &evicted, &collect).ok());

    // The evicted entry is no longer in the table: index 62 is now out of range.
    Collector dangling;
    const auto result = decoder.decode(bytes({0xBE}), &dangling, &collect);
    RUVIA_CHECK(!result.ok());
    RUVIA_CHECK(result.error == HpackError::kInvalidIndex);
}
