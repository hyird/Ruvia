#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http2/Http2Hpack.h"

namespace {

using ruvia::detail::HpackDecoder;
using ruvia::detail::HpackDecodeError;
using ruvia::detail::HpackDecodeResult;
using ruvia::detail::HpackEncoder;

template <typename T>
concept HasAnyRvalueHpackDecodeAccessor =
    requires(T&& result) { std::move(result).decoded(); } ||
    requires(T&& result) { std::move(result).failure(); };

static_assert(!HasAnyRvalueHpackDecodeAccessor<HpackDecodeResult>);

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
    const auto result = decoder.decode(block, &out, &collect);
    return result.decoded() != nullptr;
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
    // An indexed field whose index integer overflows uint32 (FF FF FF FF FF 0F)
    // must be reported as an integer overflow, not silently wrapped to a small
    // (and possibly valid) index (RFC 7541 5.1).
    HpackDecoder decoder(std::pmr::get_default_resource());
    Collector out;
    const auto block = bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
    const auto result = decoder.decode(block, &out, &collect);
    const auto* failure = result.failure();
    RUVIA_CHECK(failure != nullptr);
    if (failure != nullptr) {
        RUVIA_CHECK(failure->error() == HpackDecodeError::kIntegerOverflow);
    }
}

RUVIA_TEST(hpack_integer_overflow_chunk_bound_is_rejected) {
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
    const auto* failure = result.failure();
    RUVIA_CHECK(failure != nullptr);
    if (failure != nullptr) {
        RUVIA_CHECK(failure->error() == HpackDecodeError::kIntegerOverflow);
    }
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

RUVIA_TEST(hpack_indexed_name_referencing_the_evicted_entry_is_safe) {
    // RFC 7541 4.4: a new entry may reference (by indexed name) an existing entry
    // that the same insertion evicts. Here the referenced name is heap-allocated
    // (20 bytes, past any small-string buffer) and the insertion fully evicts it,
    // so addDynamic must copy the name BEFORE eviction frees that heap buffer --
    // otherwise the copy reads freed memory (a remotely reachable heap
    // use-after-free during HPACK decode). Under AddressSanitizer this block
    // aborts without the fix; it also pins the functional invariant that the
    // newly inserted entry resolves to the correct (uncorrupted) name.
    const std::string name(20, 'a');
    std::string block;
    block += bytes({0x3f, 0x16});          // dynamic table size update -> 53 (fits exactly one entry)
    block += bytes({0x40, 0x14});          // literal, incremental indexing, new name, name length 20
    block += name;                          // 20-byte name -> heap allocation
    block += bytes({0x01, 0x76});          // value length 1, "v"
    // Literal, incremental indexing, indexed name = 62 (the entry just added).
    // Its size (20 + 1 + 32 = 53) forces evicting that very entry before insert.
    block += bytes({0x7e, 0x01, 0x77});    // indexed name 62, value length 1, "w"
    block += bytes({0xbe});                 // indexed field, index 62 (the new entry)

    Collector out;
    RUVIA_CHECK(decodeBlock(block, out));
    RUVIA_CHECK_EQ(out.headers.size(), std::size_t{3});
    RUVIA_CHECK_EQ(out.headers[0], std::make_pair(name, std::string("v")));
    RUVIA_CHECK_EQ(out.headers[1], std::make_pair(name, std::string("w")));
    // The reference reads the STORED dynamic entry -- the byte the use-after-free
    // would corrupt. It must still be the full 20-byte name.
    RUVIA_CHECK_EQ(out.headers[2], std::make_pair(name, std::string("w")));
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

RUVIA_TEST(hpack_encoder_dynamic_table_size_update_uses_five_bit_integer) {
    std::pmr::string encoded(std::pmr::get_default_resource());
    HpackEncoder::encodeDynamicTableSizeUpdate(encoded, 0);
    RUVIA_CHECK_EQ(std::string_view(encoded), std::string_view(bytes({0x20})));

    encoded.clear();
    HpackEncoder::encodeDynamicTableSizeUpdate(encoded, 4096);
    RUVIA_CHECK_EQ(
        std::string_view(encoded),
        std::string_view(bytes({0x3f, 0xe1, 0x1f})));
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
    const auto addResult = decoder.decode(add, &added, &collect);
    RUVIA_CHECK(addResult.decoded() != nullptr);

    // Index 62 (static 61 + newest dynamic) resolves to the entry just added.
    Collector referenced;
    const auto referencedResult =
        decoder.decode(bytes({0xBE}), &referenced, &collect);
    RUVIA_CHECK(referencedResult.decoded() != nullptr);
    RUVIA_CHECK_EQ(referenced.headers.size(), std::size_t{1});

    // A size update to 0 (0x20) must evict every dynamic entry (RFC 7541 4.3).
    Collector evicted;
    const auto evictionResult =
        decoder.decode(bytes({0x20}), &evicted, &collect);
    RUVIA_CHECK(evictionResult.decoded() != nullptr);

    // The evicted entry is no longer in the table: index 62 is now out of range.
    Collector dangling;
    const auto result = decoder.decode(bytes({0xBE}), &dangling, &collect);
    const auto* failure = result.failure();
    RUVIA_CHECK(failure != nullptr);
    if (failure != nullptr) {
        RUVIA_CHECK(failure->error() == HpackDecodeError::kInvalidIndex);
    }
}

// A callback that rejects at a chosen header index (mid-block), like the h2 core does
// when a decoded header violates policy (over-limit list, duplicate singleton, ...).
struct RejectAt final {
    std::size_t rejectIndex;
    std::size_t seen{0};
    std::vector<std::pair<std::string, std::string>> before;
};

bool rejectAtCallback(void* target, std::string_view name, std::string_view value) {
    auto* r = static_cast<RejectAt*>(target);
    if (r->seen == r->rejectIndex) {
        ++r->seen;
        return false;  // reject THIS header
    }
    if (r->seen < r->rejectIndex) {
        r->before.emplace_back(std::string(name), std::string(value));
    }
    ++r->seen;
    return true;
}

// A callback-rejected block must STILL decode fully so the connection-global dynamic
// table stays consistent (RFC 7541 4.1 / RFC 9113 4.3): the decoder reports the
// rejection, but a later block referencing an entry the rejected block inserted must
// still decode correctly. Regression for the P0 where decode aborted mid-block.
RUVIA_TEST(hpack_callback_rejection_keeps_dynamic_table_consistent) {
    HpackDecoder decoder(std::pmr::get_default_resource());

    // Block A: three literal-with-incremental-indexing headers (0x40 prefix, new name),
    // hand-encoded so each is inserted into the dynamic table. The callback rejects the
    // SECOND; all three must nonetheless be inserted.
    const auto litIncremental = [](std::string_view name, std::string_view value) {
        std::string out;
        out.push_back(static_cast<char>(0x40));                  // literal, incremental, new name
        out.push_back(static_cast<char>(name.size()));           // name len (no huffman)
        out.append(name);
        out.push_back(static_cast<char>(value.size()));          // value len (no huffman)
        out.append(value);
        return out;
    };
    const std::string blockA =
        litIncremental("a-one", "1") + litIncremental("b-two", "2") + litIncremental("c-three", "3");

    RejectAt rejectAt{.rejectIndex = 1};
    const auto resultA = decoder.decode(blockA, &rejectAt, &rejectAtCallback);
    const auto* failure = resultA.failure();
    RUVIA_CHECK(failure != nullptr);  // rejection surfaced to the caller...
    if (failure != nullptr) {
        RUVIA_CHECK(failure->error() == HpackDecodeError::kCallbackRejected);
    }
    // The callback is suppressed after it rejects, so it fires only for a-one (emitted)
    // and b-two (the rejecting call) -- never c-three. But c-three is STILL inserted
    // into the dynamic table (verified by block B below), which is the whole point.
    RUVIA_CHECK_EQ(rejectAt.seen, static_cast<std::size_t>(2));
    RUVIA_CHECK_EQ(rejectAt.before.size(), static_cast<std::size_t>(1));  // only 'a-one' emitted

    // Block B on the same decoder: reference the newest dynamic entry (index 62 = the
    // last inserted, 'c-three'). If block A had aborted mid-decode, the table would be
    // desynced and this would decode wrong (or fail). 0xBE = indexed, dynamic idx 62.
    Collector out;
    const auto resultB = decoder.decode(bytes({0xBE}), &out, &collect);
    RUVIA_CHECK(resultB.decoded() != nullptr);
    RUVIA_CHECK_EQ(out.headers.size(), static_cast<std::size_t>(1));
    RUVIA_CHECK(out.headers[0].first == "c-three");
    RUVIA_CHECK(out.headers[0].second == "3");
}
