#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "field/hpack_huffman_tables.h"
#include "http2/http2_hpack.h"
#include "http2/http2_request_headers.h"
#include "test_harness.h"

namespace {

using ruvia::hpack_decode_error;
using ruvia::hpack_decode_result;
using ruvia::detail::hpack_decoder;
using ruvia::detail::hpack_encoder;
using ruvia::detail::http2_header_decode_context;
using ruvia::detail::http2_on_decoded_initial_header;
using ruvia::detail::http2_stream_header_decode_transaction;
using ruvia::detail::http2_stream_state;

struct collector final {
    std::vector<std::pair<std::string, std::string>> headers_;
};

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<collector*>(target)->headers_.emplace_back(std::string(name), std::string(value));
    return true;
}

struct header_counter final {
    std::size_t count_{0};
};

bool count_header(void* target, std::string_view, std::string_view) {
    ++static_cast<header_counter*>(target)->count_;
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

#if !defined(_MSC_VER)
class toggle_rejecting_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};

class reject_large_allocations_resource final : public std::pmr::memory_resource {
public:
    void reject_large_allocations(bool value = true) noexcept {
        reject_large_ = value;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_large_ && bytes_value >= 1024) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_large_{false};
};
#endif  // !_MSC_VER

// Decode an HPACK block into (name, value) pairs; returns whether it succeeded.
bool decode_block(std::string_view block, collector& out) {
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    const auto result_value = decoder.decode(block, &out, &collect);
    return result_value.decoded();
}

}  // namespace

RUVIA_TEST(hpack_indexed_static_header) {
    // RFC 7541 C.2.4: 0x82 -> static index 2 -> :method: GET.
    collector out;
    RUVIA_CHECK(decode_block(bytes({0x82}), out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(out.headers_[0].first, std::string(":method"));
    RUVIA_CHECK_EQ(out.headers_[0].second, std::string("GET"));
}

RUVIA_TEST(hpack_decoder_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0xC0DE'CAFE'1234'5678ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string block(static_cast<std::size_t>(next_value() % 257U), '\0');
        for (auto& byte : block) {
            byte = static_cast<char>(next_value());
        }

        std::pmr::monotonic_buffer_resource resource;
        hpack_decoder decoder({.resource_ = &resource});
        header_counter headers;
        const auto result_value = decoder.decode(block, &headers, &count_header);
        const auto active_alternatives = static_cast<unsigned int>(result_value.decoded()) +
                                         static_cast<unsigned int>(result_value.error().has_value());
        RUVIA_CHECK_EQ(active_alternatives, 1U);
        RUVIA_CHECK(headers.count_ <= block.size());
    }
}

RUVIA_TEST(hpack_request_literal_no_huffman) {
    // RFC 7541 C.3.1: indexed :method/:scheme/:path plus a literal :authority.
    collector out;
    RUVIA_CHECK(decode_block(bytes({0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65, 0x78,
                                 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d}),
        out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{4});
    RUVIA_CHECK_EQ(out.headers_[0], (std::pair{std::string(":method"), std::string("GET")}));
    RUVIA_CHECK_EQ(out.headers_[1], (std::pair{std::string(":scheme"), std::string("http")}));
    RUVIA_CHECK_EQ(out.headers_[2], (std::pair{std::string(":path"), std::string("/")}));
    RUVIA_CHECK_EQ(
        out.headers_[3], (std::pair{std::string(":authority"), std::string("www.example.com")}));
}

RUVIA_TEST(hpack_request_literal_huffman) {
    // RFC 7541 C.4.1: same request but :authority is Huffman-encoded. This
    // exercises the Huffman decoder against a known-answer vector.
    collector out;
    RUVIA_CHECK(decode_block(bytes({0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a,
                                 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff}),
        out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{4});
    RUVIA_CHECK_EQ(
        out.headers_[3], (std::pair{std::string(":authority"), std::string("www.example.com")}));
}

RUVIA_TEST(hpack_encode_decode_round_trip) {
    std::pmr::string encoded(std::pmr::get_default_resource());
    hpack_encoder::encode_header(encoded, "x-custom-header", "custom value");
    collector out;
    RUVIA_CHECK(decode_block(std::string_view(encoded.data(), encoded.size()), out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(out.headers_[0],
        (std::pair{std::string("x-custom-header"), std::string("custom value")}));
}

RUVIA_TEST(hpack_encoder_rejects_unrepresentable_string_length_without_partial_output) {
    // The view only carries metadata; the encoder must reject it before it ever
    // reads the pointed-to bytes, so this does not allocate or dereference 4 GiB.
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        const auto oversized_length =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
        const std::string_view oversized("x", oversized_length);

        std::pmr::string indexed_name_output(std::pmr::get_default_resource());
        bool rejected_value = false;
        try {
            hpack_encoder::encode_header_with_name_index(
                indexed_name_output, ruvia::detail::hpack_static_index::content_type, oversized);
        } catch (const std::length_error&) {
            rejected_value = true;
        }
        RUVIA_CHECK(rejected_value);
        RUVIA_CHECK(indexed_name_output.empty());

        std::pmr::string literal_name_output(std::pmr::get_default_resource());
        bool rejected_name = false;
        try {
            hpack_encoder::encode_header(literal_name_output, oversized, "value");
        } catch (const std::length_error&) {
            rejected_name = true;
        }
        RUVIA_CHECK(rejected_name);
        RUVIA_CHECK(literal_name_output.empty());
    }
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
        hpack_encoder::encode_header(out, "x-secret-token", "s3cr3t");
        RUVIA_CHECK(!out.empty());
        RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), 0x00u);
    }
    // A header whose NAME is a static-table entry ("content-type", index 31) with a
    // non-indexed value still uses the without-indexing form (high nibble 0000),
    // i.e. a name index plus a literal value -- never the 0x40-0x7f incremental range.
    {
        std::pmr::string out(std::pmr::get_default_resource());
        hpack_encoder::encode_header(out, "content-type", "application/x-ruvia-test");
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
        hpack_encoder::encode_header(out, name, "token-value");
        RUVIA_CHECK(!out.empty());
        RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]) & 0xf0u, 0x10u);  // never indexed
    }
    // A non-credential field with an identical value must stay without-indexing, so
    // the choice discriminates by field name rather than blanket-marking everything.
    {
        std::pmr::string out(std::pmr::get_default_resource());
        hpack_encoder::encode_header(out, "x-trace-id", "token-value");
        RUVIA_CHECK(!out.empty());
        RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]) & 0xf0u, 0x00u);  // without indexing
    }
}

RUVIA_TEST(hpack_rejects_truncated_and_bad_index) {
    collector out;
    // A literal header claiming a 15-byte value but supplying only one byte.
    RUVIA_CHECK(!decode_block(bytes({0x41, 0x0f, 0x77}), out));
    // An indexed field referencing index 0 is invalid (RFC 7541 6.1).
    collector out2;
    RUVIA_CHECK(!decode_block(bytes({0x80}), out2));
}

RUVIA_TEST(hpack_integer_overflow_is_rejected) {
    // An indexed field whose index integer overflows uint32 (FF FF FF FF FF 0F)
    // must be reported as an integer overflow, not silently wrapped to a small
    // (and possibly valid) index (RFC 7541 5.1).
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    collector out;
    const auto block = bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
    const auto result_value = decoder.decode(block, &out, &collect);
    const auto failure = result_value.error();
    RUVIA_CHECK(failure.has_value());
    if (failure.has_value()) {
        RUVIA_CHECK(*failure == hpack_decode_error::integer_overflow);
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
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    collector out;
    const auto block = bytes({0xFF, 0x80, 0x80, 0x80, 0x80, 0x1F});
    const auto result_value = decoder.decode(block, &out, &collect);
    const auto failure = result_value.error();
    RUVIA_CHECK(failure.has_value());
    if (failure.has_value()) {
        RUVIA_CHECK(*failure == hpack_decode_error::integer_overflow);
    }
}

RUVIA_TEST(hpack_long_value_round_trips) {
    // A value longer than 127 bytes forces a multi-byte length prefix, exercising
    // the continuation-integer decode on the valid (non-overflowing) path.
    const std::string long_value(300, 'x');
    std::pmr::string encoded(std::pmr::get_default_resource());
    hpack_encoder::encode_header(encoded, "x-long", long_value);
    collector out;
    RUVIA_CHECK(decode_block(std::string_view(encoded.data(), encoded.size()), out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(out.headers_[0].first, std::string("x-long"));
    RUVIA_CHECK_EQ(out.headers_[0].second, long_value);
}

RUVIA_TEST(hpack_huffman_decodes_every_byte_symbol) {
    for (std::size_t symbol = 0; symbol < 256; ++symbol) {
        const auto bit_count = ruvia::detail::hpack_huffman_lengths[symbol];
        const auto byte_count = (bit_count + 7) / 8;
        const auto padding = byte_count * 8 - bit_count;
        const auto encoded = (ruvia::detail::hpack_huffman_codes[symbol] << padding) |
                             ((std::uint32_t{1} << padding) - 1);
        std::string block;
        block.push_back('\x41');
        block.push_back(static_cast<char>(0x80 | byte_count));
        for (int byte = byte_count - 1; byte >= 0; --byte) {
            block.push_back(static_cast<char>((encoded >> (byte * 8)) & 0xff));
        }
        collector out;
        RUVIA_CHECK(decode_block(block, out));
        RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{1});
        if (out.headers_.size() == 1) {
            RUVIA_CHECK_EQ(out.headers_[0].second, std::string(1, static_cast<char>(symbol)));
        }
    }
}

RUVIA_TEST(hpack_huffman_empty_value_follows_nonempty_value) {
    collector out;
    RUVIA_CHECK(decode_block(bytes({0x01, 0x81, 0x1f, 0x01, 0x80}), out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{2});
    if (out.headers_.size() == 2) {
        RUVIA_CHECK_EQ(out.headers_[0], (std::pair{std::string(":authority"), std::string("a")}));
        RUVIA_CHECK_EQ(out.headers_[1], (std::pair{std::string(":authority"), std::string()}));
    }
}

RUVIA_TEST(hpack_huffman_rejects_bad_padding_and_eos) {
    // Literal (name :authority via 0x41) with a Huffman value.
    // 0x00: after the 5-bit code for '0' the trailing 000 padding is not all-ones.
    collector out;
    RUVIA_CHECK(!decode_block(bytes({0x41, 0x81, 0x00}), out));
    // Four 0xFF bytes walk 30 one-bits into the EOS symbol, which must be rejected.
    collector out2;
    RUVIA_CHECK(!decode_block(bytes({0x41, 0x84, 0xFF, 0xFF, 0xFF, 0xFF}), out2));
    // A single 0xFF: eight all-ones bits walk the shared all-ones prefix without
    // completing any symbol, leaving 8 padding bits. RFC 7541 5.2 forbids padding
    // longer than 7 bits even when it is all ones -- this exercises the depth>7
    // guard, distinct from the non-all-ones case (0x00) and the complete-EOS case
    // (four 0xFF) above.
    collector out3;
    RUVIA_CHECK(!decode_block(bytes({0x41, 0x81, 0xFF}), out3));
}

RUVIA_TEST(hpack_dynamic_table_add_then_reference) {
    // Literal with incremental indexing + literal name adds "custom-key:
    // custom-value" to the dynamic table; a following index 62 (static size 61 + 1)
    // references that newest dynamic entry.
    std::string block;
    block += static_cast<char>(0x40);  // literal, incremental indexing, literal name
    block += static_cast<char>(0x0A);  // name length 10
    block += "custom-key";
    block += static_cast<char>(0x0C);  // value length 12
    block += "custom-value";
    block += static_cast<char>(0xBE);  // indexed field, index 62 (0x80 | 62)

    collector out;
    RUVIA_CHECK(decode_block(block, out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{2});
    const auto expected = std::pair{std::string("custom-key"), std::string("custom-value")};
    RUVIA_CHECK_EQ(out.headers_[0], expected);
    RUVIA_CHECK_EQ(out.headers_[1], expected);  // resolved via the dynamic table
}

RUVIA_TEST(hpack_indexed_inline_name_referencing_the_evicted_entry_is_safe) {
    const auto block = bytes({0x3f, 0x03,  // dynamic table maximum 34
        0x40, 0x01, 'a', 0x01, 'b',        // inline entry, size 34
        0x7e, 0x01, 'c',                   // indexed name 62; insertion evicts its own source
        0xbe});
    collector out;
    RUVIA_CHECK(decode_block(block, out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{3});
    if (out.headers_.size() == 3) {
        RUVIA_CHECK_EQ(out.headers_[1], (std::pair{std::string("a"), std::string("c")}));
        RUVIA_CHECK_EQ(out.headers_[2], out.headers_[1]);
    }
}

RUVIA_TEST(hpack_indexed_name_referencing_the_evicted_entry_is_safe) {
    // RFC 7541 4.4: the 49-byte indexed name plus the new value forms a heap-backed
    // entry. Its insertion evicts the very entry supplying the name, so insertion
    // must first own the field bytes in an independent packed block.
    const std::string name(49, 'a');
    std::string block;
    block += bytes({0x3f, 0x33});  // dynamic table size update -> 82 (fits one entry)
    block += bytes({0x40, 0x31});  // literal, incremental indexing, new name, length 49
    block += name;
    block += bytes({0x01, 0x76});  // value length 1, "v"
    // Indexed name 62 refers to the just-added entry; size 49 + 1 + 32 = 82
    // forces that source entry's eviction before the new entry is committed.
    block += bytes({0x7e, 0x01, 0x77});
    block += bytes({0xbe});  // indexed field, index 62 (the new entry)

    collector out;
    RUVIA_CHECK(decode_block(block, out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{3});
    RUVIA_CHECK_EQ(out.headers_[0], (std::pair{name, std::string("v")}));
    RUVIA_CHECK_EQ(out.headers_[1], (std::pair{name, std::string("w")}));
    RUVIA_CHECK_EQ(out.headers_[2], (std::pair{name, std::string("w")}));
}

RUVIA_TEST(hpack_explicit_transaction_rollback_restores_dynamic_table) {
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    decoder.set_max_dynamic_table_size(34);  // Exactly one entry; insertion evicts its predecessor.
    const auto original = bytes({0x40, 0x01, 'a', 0x01, 'b'});
    header_counter counter;
    const auto original_result = decoder.decode(original, &counter, &count_header);
    RUVIA_CHECK(original_result.decoded());

    const auto inserted = bytes({0x40, 0x01, 'c', 0x01, 'd'});
    {
        auto transaction = decoder.begin_transaction();
        const auto result_value = decoder.decode(inserted, &counter, &count_header, transaction);
        RUVIA_CHECK(result_value.decoded());
        transaction.rollback();
    }

    collector retained_headers;
    const auto retained = decoder.decode(bytes({0xbe}), &retained_headers, &collect);
    RUVIA_CHECK(retained.decoded());
    RUVIA_CHECK_EQ(retained_headers.headers_.size(), std::size_t{1});
    if (!retained_headers.headers_.empty()) {
        RUVIA_CHECK_EQ(retained_headers.headers_[0],
            (std::pair{std::string("a"), std::string("b")}));
    }
    const auto absent = decoder.decode(bytes({0xbf}), &counter, &count_header);
    RUVIA_CHECK(absent.error().has_value());
    if (const auto failure = absent.error()) {
        RUVIA_CHECK(*failure == hpack_decode_error::invalid_index);
    }
}

#if !defined(_MSC_VER)
// These HPACK transactions include throwing PMR string growth, which does not
// complete in the MSVC debug standard library.
RUVIA_TEST(hpack_dynamic_insert_allocation_failure_preserves_table) {
    toggle_rejecting_memory_resource resource;
    hpack_decoder decoder({.resource_ = &resource});

    // Set the table to exactly one tiny entry, then insert "a: b". The vector
    // has one live element and normally one capacity slot at this point.
    std::string first = bytes({0x3f, 0x03, 0x40, 0x01, 'a', 0x01, 'b'});  // max = 34
    collector initial;
    const auto initial_result = decoder.decode(first, &initial, &collect);
    RUVIA_CHECK(initial_result.decoded());

    // The next entry uses the existing dynamic name (index 62) and must evict the
    // first one. Reject the vector growth after the entry is decoded. The failed
    // insertion must not make the old indexed entry disappear.
    const std::string second = bytes({0x7e, 0x01, 'c'});  // incremental, name index 62
    collector failed_insert;
    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        (void)decoder.decode(second, &failed_insert, &collect);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);

    resource.reject_allocations(false);
    collector retained;
    const auto retained_result =
        decoder.decode(bytes({0xbe}), &retained, &collect);  // indexed dynamic entry 62
    RUVIA_CHECK(retained_result.decoded());
    RUVIA_CHECK_EQ(retained.headers_.size(), std::size_t{1});
    if (!retained.headers_.empty()) {
        RUVIA_CHECK_EQ(retained.headers_[0], (std::pair{std::string("a"), std::string("b")}));
    }
}

RUVIA_TEST(hpack_header_callback_allocation_failure_rolls_back_stream_state) {
    toggle_rejecting_memory_resource resource;
    hpack_decoder decoder({.resource_ = &resource});
    http2_stream_state stream(1, &resource);

    std::string large_path(4096, 'p');
    large_path.front() = '/';
    std::pmr::string block(std::pmr::get_default_resource());
    hpack_encoder::encode_header(block, ":method", "GET");
    hpack_encoder::encode_header(block, ":path", large_path);

    const auto decode = [](void* target, std::string_view name, std::string_view value) {
        return http2_on_decoded_initial_header(
            *static_cast<http2_header_decode_context*>(target), name, value);
    };

    bool allocation_failed = false;
    {
        http2_stream_header_decode_transaction transaction(stream, true);
        http2_header_decode_context context_value(stream, &transaction);
        resource.reject_allocations();
        try {
            (void)decoder.decode(block, &context_value, decode);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(!stream.has_method());
    RUVIA_CHECK(!stream.has_path());
    RUVIA_CHECK_EQ(stream.remote_header_count(), std::size_t{0});

    resource.reject_allocations(false);
    {
        http2_stream_header_decode_transaction transaction(stream, true);
        http2_header_decode_context context_value(stream, &transaction);
        const auto result_value = decoder.decode(block, &context_value, decode);
        RUVIA_CHECK(result_value.decoded());
        if (result_value.decoded()) {
            transaction.commit();
        }
    }
    RUVIA_CHECK_EQ(stream.request_method(), std::string_view("GET"));
    RUVIA_CHECK_EQ(stream.request_path(), std::string_view(large_path));
}

RUVIA_TEST(hpack_field_block_allocation_failure_rolls_back_prior_dynamic_inserts) {
    reject_large_allocations_resource resource;
    hpack_decoder decoder({.resource_ = &resource});

    // Both fields use incremental indexing. The second value is large enough
    // to fail while materialising its dynamic-table entry, after the first
    // entry has already been inserted.
    const std::string large_value(3000, 'v');
    std::string block;
    {
        std::pmr::string first(std::pmr::get_default_resource());
        hpack_encoder::encode_header(first, "x-one", "one");
        first[0] = static_cast<char>(0x40);  // literal with incremental indexing
        block.append(first.data(), first.size());
    }
    {
        std::pmr::string second(std::pmr::get_default_resource());
        hpack_encoder::encode_header(second, "x-two", large_value);
        second[0] = static_cast<char>(0x40);  // literal with incremental indexing
        block.append(second.data(), second.size());
    }

    resource.reject_large_allocations();
    collector failed;
    bool allocation_failed = false;
    try {
        (void)decoder.decode(block, &failed, &collect);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);

    // Index 62 is the first dynamic entry. It must not be visible after a
    // failed field block, even though the first callback/insertion completed.
    collector before_retry;
    const auto before_retry_result =
        decoder.decode(std::string_view("\xbe", 1), &before_retry, &collect);
    RUVIA_CHECK(before_retry_result.error().has_value());
    if (const auto failure = before_retry_result.error()) {
        RUVIA_CHECK(*failure == hpack_decode_error::invalid_index);
    }

    resource.reject_large_allocations(false);
    collector retried;
    const auto retry_result = decoder.decode(block, &retried, &collect);
    RUVIA_CHECK(retry_result.decoded());

    // A successful retry contains exactly two dynamic entries; index 64
    // (dynamic index 3) must remain invalid rather than exposing a duplicated
    // copy of the first field.
    collector after_retry;
    const auto after_retry_result =
        decoder.decode(std::string_view("\xc0", 1), &after_retry, &collect);
    RUVIA_CHECK(after_retry_result.error().has_value());
    if (const auto failure = after_retry_result.error()) {
        RUVIA_CHECK(*failure == hpack_decode_error::invalid_index);
    }
}
#endif  // !_MSC_VER

RUVIA_TEST(hpack_size_update_after_header_is_rejected) {
    // A dynamic-table size update must precede any header field (RFC 7541 4.2):
    // 0x82 (:method GET) then 0x20 (size update) is a decoding error.
    collector out;
    RUVIA_CHECK(!decode_block(bytes({0x82, 0x20}), out));
    // A size update before the header is fine.
    collector out2;
    RUVIA_CHECK(decode_block(bytes({0x20, 0x82}), out2));
    RUVIA_CHECK_EQ(out2.headers_.size(), std::size_t{1});
}

RUVIA_TEST(hpack_rejects_more_than_two_size_updates_at_block_start) {
    // RFC 7541 section 4.2 permits at most two dynamic-table size updates at
    // the beginning of one field block: the smallest intervening maximum and
    // the final maximum. Accepting a third update admits an HPACK representation
    // that the peer is forbidden to generate.
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    collector out;
    const auto result_value = decoder.decode(bytes({0x20, 0x21, 0x22, 0x82}), &out, &collect);
    const auto failure = result_value.error();
    RUVIA_CHECK(failure.has_value());
    if (failure.has_value()) {
        RUVIA_CHECK(*failure == hpack_decode_error::dynamic_table_size);
    }
}

RUVIA_TEST(hpack_rejects_decreasing_second_size_update) {
    // With two updates, RFC 7541 section 4.2 requires the smallest value first
    // and the final value second. A 10 -> 5 sequence reverses that order.
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    collector out;
    const auto result_value = decoder.decode(bytes({0x2a, 0x25, 0x82}), &out, &collect);
    const auto failure = result_value.error();
    RUVIA_CHECK(failure.has_value());
    if (failure.has_value()) {
        RUVIA_CHECK(*failure == hpack_decode_error::dynamic_table_size);
    }
}

RUVIA_TEST(hpack_accepts_smallest_then_final_size_updates) {
    // The valid two-update form carries the smallest intervening maximum first
    // and the final maximum second, followed by the first header representation.
    collector out;
    RUVIA_CHECK(decode_block(bytes({0x20, 0x3f, 0x01, 0x82}), out));
    RUVIA_CHECK_EQ(out.headers_.size(), std::size_t{1});
    if (!out.headers_.empty()) {
        RUVIA_CHECK_EQ(out.headers_[0].first, std::string(":method"));
        RUVIA_CHECK_EQ(out.headers_[0].second, std::string("GET"));
    }
}

RUVIA_TEST(hpack_encoder_dynamic_table_size_update_uses_five_bit_integer) {
    std::pmr::string encoded(std::pmr::get_default_resource());
    hpack_encoder::encode_dynamic_table_size_update(encoded, 0);
    RUVIA_CHECK_EQ(std::string_view(encoded), std::string_view(bytes({0x20})));

    encoded.clear();
    hpack_encoder::encode_dynamic_table_size_update(encoded, 4096);
    RUVIA_CHECK_EQ(std::string_view(encoded), std::string_view(bytes({0x3f, 0xe1, 0x1f})));
}

RUVIA_TEST(hpack_size_update_exceeding_settings_max_is_rejected) {
    // RFC 7541 §6.3: a dynamic-table size update must not exceed the maximum the
    // decoder advertised via SETTINGS_HEADER_TABLE_SIZE (default 4096). Accepting a
    // larger value would let a peer coerce an oversized dynamic table (a memory-DoS
    // vector). A size update to exactly the ceiling is allowed; one byte over is not.
    // Encoding: 0x20 | 5-bit-prefix(31), then the HPACK varint remainder.
    collector at_ceiling;
    RUVIA_CHECK(decode_block(bytes({0x3f, 0xe1, 0x1f}), at_ceiling));  // 31 + 97 + 31*128 = 4096
    collector over_ceiling;
    RUVIA_CHECK(!decode_block(bytes({0x3f, 0xe2, 0x1f}), over_ceiling));  // 4097 > 4096 -> rejected
}

RUVIA_TEST(hpack_size_update_to_zero_evicts_dynamic_table) {
    // The dynamic table persists across decode() calls, so use one decoder.
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});

    // Literal with incremental indexing adds "custom-key: custom-value".
    std::string add;
    add += static_cast<char>(0x40);  // literal, incremental indexing, new name
    add += static_cast<char>(0x0A);  // name length 10
    add += "custom-key";
    add += static_cast<char>(0x0C);  // value length 12
    add += "custom-value";
    collector added;
    const auto add_result = decoder.decode(add, &added, &collect);
    RUVIA_CHECK(add_result.decoded());

    // Index 62 (static 61 + newest dynamic) resolves to the entry just added.
    collector referenced;
    const auto referenced_result = decoder.decode(bytes({0xBE}), &referenced, &collect);
    RUVIA_CHECK(referenced_result.decoded());
    RUVIA_CHECK_EQ(referenced.headers_.size(), std::size_t{1});

    // A size update to 0 (0x20) must evict every dynamic entry (RFC 7541 4.3).
    collector evicted;
    const auto eviction_result = decoder.decode(bytes({0x20}), &evicted, &collect);
    RUVIA_CHECK(eviction_result.decoded());

    // The evicted entry is no longer in the table: index 62 is now out of range.
    collector dangling;
    const auto result_value = decoder.decode(bytes({0xBE}), &dangling, &collect);
    const auto failure = result_value.error();
    RUVIA_CHECK(failure.has_value());
    if (failure.has_value()) {
        RUVIA_CHECK(*failure == hpack_decode_error::invalid_index);
    }
}

// A callback that rejects at a chosen header index (mid-block), like the h2 core does
// when a decoded header violates policy (over-limit list, duplicate singleton, ...).
struct reject_at final {
    std::size_t reject_index_;
    std::size_t seen_{0};
    std::vector<std::pair<std::string, std::string>> before_;
};

bool reject_at_callback(void* target, std::string_view name, std::string_view value) {
    auto* r = static_cast<reject_at*>(target);
    if (r->seen_ == r->reject_index_) {
        ++r->seen_;
        return false;  // reject THIS header
    }
    if (r->seen_ < r->reject_index_) {
        r->before_.emplace_back(std::string(name), std::string(value));
    }
    ++r->seen_;
    return true;
}

// A callback-rejected block must STILL decode fully so the connection-global dynamic
// table stays consistent (RFC 7541 4.1 / RFC 9113 4.3): the decoder reports the
// rejection, but a later block referencing an entry the rejected block inserted must
// still decode correctly. Regression for the P0 where decode aborted mid-block.
RUVIA_TEST(hpack_callback_rejection_keeps_dynamic_table_consistent) {
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});

    // Block A: three literal-with-incremental-indexing headers (0x40 prefix, new name),
    // hand-encoded so each is inserted into the dynamic table. The callback rejects the
    // SECOND; all three must nonetheless be inserted.
    const auto lit_incremental = [](std::string_view name, std::string_view value) {
        std::string out;
        out.push_back(static_cast<char>(0x40));         // literal, incremental, new name
        out.push_back(static_cast<char>(name.size()));  // name len (no huffman)
        out.append(name);
        out.push_back(static_cast<char>(value.size()));  // value len (no huffman)
        out.append(value);
        return out;
    };
    const std::string block_a = lit_incremental("a-one", "1") + lit_incremental("b-two", "2") +
                                lit_incremental("c-three", "3");

    reject_at reject_at_value{.reject_index_ = 1};
    const auto result_a = decoder.decode(block_a, &reject_at_value, &reject_at_callback);
    const auto failure = result_a.error();
    RUVIA_CHECK(failure.has_value());  // rejection surfaced to the caller...
    if (failure.has_value()) {
        RUVIA_CHECK(*failure == hpack_decode_error::callback_rejected);
    }
    // The callback is suppressed after it rejects, so it fires only for a-one (emitted)
    // and b-two (the rejecting call) -- never c-three. But c-three is STILL inserted
    // into the dynamic table (verified by block B below), which is the whole point.
    RUVIA_CHECK_EQ(reject_at_value.seen_, static_cast<std::size_t>(2));
    RUVIA_CHECK_EQ(reject_at_value.before_.size(), static_cast<std::size_t>(1));  // only 'a-one' emitted

    // Block B on the same decoder: reference the newest dynamic entry (index 62 = the
    // last inserted, 'c-three'). If block A had aborted mid-decode, the table would be
    // desynced and this would decode wrong (or fail). 0xBE = indexed, dynamic idx 62.
    collector out;
    const auto result_b = decoder.decode(bytes({0xBE}), &out, &collect);
    RUVIA_CHECK(result_b.decoded());
    RUVIA_CHECK_EQ(out.headers_.size(), static_cast<std::size_t>(1));
    RUVIA_CHECK(out.headers_[0].first == "c-three");
    RUVIA_CHECK(out.headers_[0].second == "3");
}
