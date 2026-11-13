#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_coding.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

using namespace ruvia::test;

using ruvia::http_content_encoder;

#include <chrono>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

namespace {

class codec_test_allocation_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "codec test allocation failure";
    }
};

}  // namespace

TEST(Ruvia, http_content_coding_field_mapping_is_protocol_generic) {
    const auto check_coding = [&](std::string_view value, http_content_coding expected) {
        const auto parsed_value = parse_http_content_coding(value);
        EXPECT_TRUE(parsed_value.invalid() == nullptr);
        EXPECT_TRUE(parsed_value.unsupported() == nullptr);
        EXPECT_EQ(parsed_value.codings().size(), 1U);
        if (!parsed_value.codings().empty()) {
            EXPECT_TRUE(parsed_value.codings().front() == expected);
        }
    };
    check_coding("gzip", http_content_coding::gzip);
    check_coding("x-gzip", http_content_coding::gzip);
    check_coding("X-GZIP", http_content_coding::gzip);
    check_coding("GZIP", http_content_coding::gzip);
    check_coding("  br ", http_content_coding::brotli);
    check_coding("zstd", http_content_coding::zstd);
    check_coding("deflate", http_content_coding::deflate);
    for (const std::string_view value : {std::string_view(""), std::string_view("identity"),
             std::string_view("IDENTITY, identity")}) {
        const auto empty = parse_http_content_coding(value);
        EXPECT_TRUE(empty.invalid() == nullptr);
        EXPECT_TRUE(empty.unsupported() == nullptr);
        EXPECT_TRUE(empty.codings().empty());
    }

    constexpr std::array mappings{
        std::pair{http_content_coding::identity, std::string_view("identity")},
        std::pair{http_content_coding::gzip, std::string_view("gzip")},
        std::pair{http_content_coding::deflate, std::string_view("deflate")},
        std::pair{http_content_coding::brotli, std::string_view("br")},
        std::pair{http_content_coding::zstd, std::string_view("zstd")},
    };
    for (const auto& [coding, token] : mappings) {
        EXPECT_EQ(ruvia::http_content_coding_token(coding), token);
        const auto parsed_value = parse_http_content_coding(token);
        if (coding == http_content_coding::identity) {
            EXPECT_TRUE(parsed_value.codings().empty());
        } else {
            EXPECT_EQ(parsed_value.codings().size(), 1U);
            EXPECT_TRUE(parsed_value.codings().front() == coding);
        }
        EXPECT_TRUE(ruvia::http_content_coding_from_token(token) == coding);
    }
    EXPECT_TRUE(ruvia::http_content_coding_from_token("X-GZIP") == http_content_coding::gzip);
    EXPECT_TRUE(ruvia::http_content_coding_from_token("BR") == http_content_coding::brotli);
    EXPECT_TRUE(!ruvia::http_content_coding_from_token("compress").has_value());
    EXPECT_TRUE(!ruvia::http_content_coding_from_token("").has_value());

    const auto unsupported = parse_http_content_coding("compress");
    const auto stacked = parse_http_content_coding("gzip, br");
    EXPECT_TRUE(unsupported.invalid() == nullptr);
    EXPECT_TRUE(unsupported.unsupported() != nullptr);
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), 2U);
    EXPECT_TRUE(stacked.codings()[0] == http_content_coding::gzip);
    EXPECT_TRUE(stacked.codings()[1] == http_content_coding::brotli);

    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        const auto invalid = parse_http_content_coding(value);
        EXPECT_TRUE(invalid.unsupported() == nullptr);
        EXPECT_TRUE(invalid.invalid() != nullptr);
        if (invalid.invalid() != nullptr) {
            EXPECT_EQ(invalid.invalid()->status(), ruvia::http_status::bad_request);
        }
    }
}

TEST(Ruvia, http_content_coding_field_bounds_the_coding_stack) {
    std::string value("identity");
    for (std::size_t index = 0; index < ruvia::max_content_codings; ++index) {
        value += ", gzip, identity";
    }
    const auto bounded = parse_http_content_coding(value);
    EXPECT_TRUE(bounded.unsupported() == nullptr);
    EXPECT_EQ(bounded.codings().size(), ruvia::max_content_codings);

    value += ", identity";
    const auto still_bounded = parse_http_content_coding(value);
    EXPECT_TRUE(still_bounded.unsupported() == nullptr);
    EXPECT_EQ(still_bounded.codings().size(), ruvia::max_content_codings);

    value += ", gzip";
    const auto oversized = parse_http_content_coding(value);
    EXPECT_TRUE(oversized.invalid() == nullptr);
    EXPECT_TRUE(oversized.unsupported() != nullptr);
    if (oversized.unsupported() != nullptr) {
        EXPECT_TRUE(oversized.unsupported()->depth_exceeded());
    }
    EXPECT_TRUE(oversized.codings().empty());

    const auto oversized_with_unknown = parse_http_content_coding(
        "gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, x-custom");
    EXPECT_TRUE(oversized_with_unknown.invalid() == nullptr);
    EXPECT_TRUE(oversized_with_unknown.unsupported() != nullptr);
    if (oversized_with_unknown.unsupported() != nullptr) {
        EXPECT_TRUE(oversized_with_unknown.unsupported()->depth_exceeded());
    }

    const auto oversized_unknown_only =
        parse_http_content_coding("x1, x2, x3, x4, x5, x6, x7, x8, x9");
    EXPECT_TRUE(oversized_unknown_only.invalid() == nullptr);
    EXPECT_TRUE(oversized_unknown_only.unsupported() != nullptr);
    if (oversized_unknown_only.unsupported() != nullptr) {
        EXPECT_TRUE(oversized_unknown_only.unsupported()->depth_exceeded());
    }

    const auto eight_known_plus_unknown = parse_http_content_coding(
        "gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, x-custom");
    EXPECT_TRUE(eight_known_plus_unknown.invalid() == nullptr);
    EXPECT_TRUE(eight_known_plus_unknown.unsupported() != nullptr);
    if (eight_known_plus_unknown.unsupported() != nullptr) {
        EXPECT_TRUE(eight_known_plus_unknown.unsupported()->depth_exceeded());
    }
    value += ", bad coding";
    const auto malformed = parse_http_content_coding(value);
    EXPECT_TRUE(malformed.invalid() != nullptr);

    // identity stages transform nothing but still observe the decoded limit.
    constexpr std::array identity_stack{http_content_coding::identity, http_content_coding::identity};
    const auto identity = decode_http_content(identity_stack, "plain", {.max_decoded_bytes_ = 5});
    EXPECT_TRUE(std::holds_alternative<std::string>(identity));
    if (const auto* decoded = std::get_if<std::string>(&identity)) {
        EXPECT_EQ(std::string_view(*decoded), std::string_view("plain"));
    }
    const auto oversized_identity = decode_http_content(identity_stack, "plain", {.max_decoded_bytes_ = 4});
    EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(oversized_identity));
    if (const auto* failure = std::get_if<http_content_decode_error>(&oversized_identity)) {
        EXPECT_TRUE(*failure == ruvia::http_content_decode_error::decoded_size_exceeded);
    }
}

TEST(Ruvia, http_deflate_content_uses_the_rfc9110_zlib_wrapper) {
    const std::string input(4096, 'd');
    auto encoded = encode_http_content(
        http_content_coding::deflate, input, {.max_encoded_bytes_ = input.size()});
    EXPECT_TRUE(encoded.has_value());
    if (encoded.has_value()) {
        auto decoded = decode_http_content(std::array{http_content_coding::deflate}, *encoded,
            {.max_decoded_bytes_ = input.size()});
        EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
        if (const auto* output = std::get_if<std::string>(&decoded)) {
            EXPECT_EQ(std::string_view(*output), input);
        }
    }
}

TEST(Ruvia, http_zstd_content_rejects_window_above_rfc9659_limit) {
    const std::string plain(9 * 1024 * 1024, 'w');
    const std::string encoded = zstd_compress_with_window(plain, 24);
    EXPECT_TRUE(!encoded.empty());
    EXPECT_TRUE(decode_error(http_content_coding::zstd, encoded, plain.size()) ==
                http_content_decode_error::invalid_content);

    auto conformant = encode_http_content(http_content_coding::zstd, plain,
        {.max_encoded_bytes_ = plain.size()});
    EXPECT_TRUE(conformant.has_value());
    if (conformant.has_value()) {
        EXPECT_EQ(decoded(http_content_coding::zstd, *conformant, plain.size()), plain);
    }
}

TEST(Ruvia, http_content_encode_enforces_exact_cap_without_partial_output) {
    const std::string input(2048, 'e');
    for (const auto coding :
        {http_content_coding::gzip, http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        const auto full = encode_http_content(coding, input,
            {.max_encoded_bytes_ = input.size()});
        EXPECT_TRUE(full.has_value());
        if (!full.has_value()) {
            continue;
        }
        const auto encoded_size = full->size();
        EXPECT_TRUE(encoded_size > 1);

        const auto exact = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size});
        EXPECT_TRUE(exact.has_value());
        if (exact.has_value()) {
            EXPECT_EQ(exact->size(), encoded_size);
        }

        const auto too_small = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size - 1});
        EXPECT_TRUE(!too_small.has_value());
        EXPECT_TRUE(!encode_http_content(coding, {}, {.max_encoded_bytes_ = 0}).has_value());
        EXPECT_TRUE(!encode_http_content(coding, input, {.max_encoded_bytes_ = 0}).has_value());
    }

    const auto identity = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 8});
    EXPECT_TRUE(identity.has_value());
    if (identity.has_value()) {
        EXPECT_EQ(std::string_view(*identity), std::string_view("identity"));
    }
    const auto identity_too_large = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 0});
    EXPECT_TRUE(!identity_too_large.has_value());
    const auto empty_identity = encode_http_content(http_content_coding::identity, {}, {.max_encoded_bytes_ = 0});
    EXPECT_TRUE(empty_identity.has_value() && empty_identity->empty());
}

TEST(Ruvia, http_content_encode_round_trips_across_output_block_boundaries) {
    for (const std::size_t size : {std::size_t{0}, std::size_t{8191}, std::size_t{8192},
             std::size_t{8193}, std::size_t{65537}, std::size_t{(4u << 20) + 1}}) {
        std::string input;
        input.reserve(size);
        std::uint32_t state_value = 0x12345678;
        for (std::size_t i = 0; i < size; ++i) {
            state_value ^= state_value << 13;
            state_value ^= state_value >> 17;
            state_value ^= state_value << 5;
            input.push_back(static_cast<char>(state_value >> 24));
        }
        for (const auto coding : {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
            const auto full = encode_http_content(coding, input, {.max_encoded_bytes_ = size * 2 + 1024});
            EXPECT_TRUE(full.has_value());
            if (!full.has_value()) {
                continue;
            }
            const auto bytes_value = std::string_view(*full);
            EXPECT_TRUE(!bytes_value.empty());
            EXPECT_EQ(decoded(coding, bytes_value, input.size()), input);
            if (size >= 8192) {
                EXPECT_TRUE(bytes_value.size() > 8192);
            }
            const auto exact = encode_http_content(coding, input, {.max_encoded_bytes_ = bytes_value.size()});
            EXPECT_TRUE(exact.has_value());
            if (exact.has_value()) {
                EXPECT_EQ(std::string_view(*exact), bytes_value);
            }
            if (!bytes_value.empty()) {
                const auto too_small = encode_http_content(coding, input, {.max_encoded_bytes_ = bytes_value.size() - 1});
                EXPECT_TRUE(!too_small.has_value());
            }
        }
    }
}

TEST(Ruvia, http_content_encoder_round_trips_incremental_chunks) {
    const std::string input =
        "incremental HTTP response data with enough repetition to exercise "
        "each encoder's pending output and finalization state. ";
    const std::string repeated = input + input + input + input + input;

    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::brotli, http_content_coding::zstd}) {
        std::string encoded;
        std::string chunk;
        http_content_encoder encoder(coding);
        for (std::size_t offset = 0; offset < repeated.size();) {
            const auto size = std::min<std::size_t>(13, repeated.size() - offset);
            chunk.clear();
            encoder.write(std::string_view(repeated).substr(offset, size), chunk);
            encoded.append(chunk);
            offset += size;
        }
        chunk.clear();
        encoder.finish(chunk);
        encoded.append(chunk);
        EXPECT_EQ(decoded(coding, encoded, repeated.size()), repeated);
    }
}

TEST(Ruvia, http_content_encoder_rejects_writes_after_finish) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::brotli, http_content_coding::zstd}) {
        http_content_encoder encoder(coding);
        std::string output;
        encoder.write("body", output);
        output.clear();
        encoder.finish(output);
        output.clear();
        bool rejected = false;
        try {
            encoder.write("late body", output);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_TRUE(output.empty());
        encoder.finish(output);
        EXPECT_TRUE(output.empty());
    }
}

TEST(Ruvia, http_content_encoder_flushes_each_incremental_chunk) {
    const std::string input(4096, 's');

    for (const auto coding :
        {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
        std::string encoded;
        std::string chunk;
        http_content_encoder encoder(coding);
        for (std::size_t offset = 0; offset < input.size();) {
            const auto size = std::min<std::size_t>(257, input.size() - offset);
            chunk.clear();
            encoder.write(std::string_view(input).substr(offset, size), chunk, true);
            encoded.append(chunk);
            offset += size;
        }
        chunk.clear();
        encoder.finish(chunk);
        encoded.append(chunk);
        EXPECT_EQ(decoded(coding, encoded, input.size()), input);
    }
}

TEST(Ruvia, http_content_encoder_accepts_empty_flushes_between_chunks) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        std::string encoded;
        http_content_encoder encoder(coding);
        encoder.write({}, encoded, true);
        encoder.write({}, encoded, true);
        encoder.write("first", encoded, true);
        encoder.write({}, encoded, true);
        encoder.write({}, encoded, true);
        encoder.write("second", encoded, true);
        encoder.write({}, encoded, true);
        encoder.write({}, encoded, true);
        encoder.finish(encoded);
        EXPECT_EQ(decoded(coding, encoded, 11), std::string("firstsecond"));
    }
}

TEST(Ruvia, http_brotli_decode_checks_limits_and_complete_stream_after_output_blocks) {
    const std::string input(65537, 'b');
    const auto encoded = brotli_compress(input);
    for (const std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{16383},
             std::size_t{16384}, std::size_t{65536}}) {
        EXPECT_EQ(decode_error(http_content_coding::brotli, encoded, cap), http_content_decode_error::decoded_size_exceeded);
    }
    EXPECT_EQ(decoded(http_content_coding::brotli, encoded, input.size()), input);
    EXPECT_EQ(decode_error(http_content_coding::brotli, std::string_view(encoded).substr(0, encoded.size() - 1), input.size()), http_content_decode_error::invalid_content);
    EXPECT_EQ(decode_error(http_content_coding::brotli, encoded + "trailing", input.size()), http_content_decode_error::invalid_content);
}

TEST(Ruvia, http_content_decode_accepts_empty_encoded_input_as_empty_content) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        EXPECT_EQ(decoded(coding, {}, 0), std::string{});
    }

    constexpr std::array codings{http_content_coding::gzip, http_content_coding::brotli,
        http_content_coding::zstd};
    auto stacked = decode_http_content(
        codings, {}, {.max_decoded_bytes_ = 0});
    EXPECT_TRUE(std::holds_alternative<std::string>(stacked));
    if (const auto* content = std::get_if<std::string>(&stacked)) {
        EXPECT_TRUE(content->empty());
    }
}

TEST(Ruvia, http_content_decode_zero_cap_allows_only_empty_content) {
    const struct {
        http_content_coding coding_;
        std::string encoded_;
    } empty_cases[] = {
        {http_content_coding::gzip, gzip_compress({})},
        {http_content_coding::brotli, brotli_compress({})},
        {http_content_coding::zstd, zstd_compress({})},
    };
    for (const auto& test : empty_cases) {
        auto result_value = decode_http_content(std::array{test.coding_}, test.encoded_,
            {.max_decoded_bytes_ = 0});
        EXPECT_TRUE(std::holds_alternative<std::string>(result_value));
        if (const auto* content = std::get_if<std::string>(&result_value)) {
            EXPECT_TRUE(content->empty());
        }
    }
    EXPECT_TRUE(decode_error(http_content_coding::gzip, gzip_compress("x"), 0) ==
                http_content_decode_error::decoded_size_exceeded);
}

TEST(Ruvia, zstd_decode_full_frame_succeeds) {
    const std::string plain(4096, 'z');  // compressible payload spanning a block
    const auto decoded = zstd_round_trip(plain, 0);
    EXPECT_TRUE(decoded.has_value());
    if (decoded) {
        EXPECT_EQ(*decoded, plain);
    }
}

TEST(Ruvia, zstd_decode_truncated_frame_rejected) {
    const std::string plain(4096, 'z');
    // Dropping the final bytes yields an incomplete frame that must be rejected,
    // matching the zlib/brotli decoders (regression guard for silent-truncation).
    EXPECT_TRUE(!zstd_round_trip(plain, 4).has_value());
}

TEST(Ruvia, response_coding_sets_and_selection_snapshots_distinguish_every_supported_coding) {
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::identity,
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate,
        ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    for (const auto coding : codings) {
        auto candidates = ruvia::http_response_coding_candidates::empty();
        candidates.include(coding);
        ruvia::http_response_coding_qualities qualities;
        const std::string field = std::string(ruvia::http_content_coding_token(coding)) +
                                  ";q=1,*;q=0" + (coding == ruvia::http_content_coding::identity ? "" : ",identity;q=0");
        qualities.update(field);
        const auto selected = ruvia::http_response_coding_selection::select(qualities, candidates);
        EXPECT_TRUE(selected.acceptable());
        EXPECT_TRUE(selected.coding() == coding);
        for (const auto other : codings) {
            EXPECT_TRUE(candidates.contains(other) == (other == coding));
            EXPECT_TRUE(selected.accepts(other) == (other == coding));
            EXPECT_TRUE(ruvia::http_response_coding_candidates::all().contains(other));
        }
    }
}

namespace {

using ruvia::http_accepted_encoding_quality;
using ruvia::http_content_coding;
using ruvia::http_response_coding_candidates;
using ruvia::http_response_coding_qualities;
using ruvia::http_response_coding_selection;

// Reference form: one full Accept-Encoding scan per coding. The aggregate
// single-pass update must produce identical qualities.
struct reference_qualities final {
    http_accepted_encoding_quality gzip_;
    http_accepted_encoding_quality brotli_;
    http_accepted_encoding_quality zstd_;
    http_accepted_encoding_quality identity_;
};

reference_qualities reference_three_pass(std::string_view header_value) {
    reference_qualities ref;
    ref.gzip_.update(header_value, "gzip");
    ref.brotli_.update(header_value, "br");
    ref.zstd_.update(header_value, "zstd");
    ref.identity_.update(header_value, "identity");
    return ref;
}

bool same_quality(const http_accepted_encoding_quality& a, const http_accepted_encoding_quality& b) {
    return a.explicit_quality_ == b.explicit_quality_ && a.wildcard_quality_ == b.wildcard_quality_;
}

http_response_coding_selection select_field(std::string_view header_value) {
    http_response_coding_qualities qualities;
    qualities.update(header_value);
    return http_response_coding_selection::select(qualities);
}

}  // namespace

TEST(Ruvia, response_coding_single_pass_matches_per_coding_scans) {
    const std::string_view cases[] = {
        "gzip, br, zstd",
        "gzip;q=0.5, br;q=0.8, zstd;q=0.3",
        "*",
        "*;q=0.1, gzip;q=0.9",
        "br;q=0, *;q=0.5",
        "identity, gzip",
        "  gzip ,  br ",
        "GZIP, Br, ZSTD",  // token match is case-insensitive
        "x-gzip, br",
        "X-GZIP;q=0.8, gzip;q=0.2, *;q=0.1",
        "gzip;q=0.2, x-gzip;q=0.8",
        "",
        "deflate;q=0.2",  // unknown coding: leaves all three untouched
        "gzip;q=0, gzip;q=0.9",
        ", gzip, , br,",  // empty items are skipped
        "gzip;q=1.0, br;q=0.500",
        R"(gzip;note="a,b";q=0, br;q=0.5)",
    };
    for (const auto header : cases) {
        http_response_coding_qualities qualities;
        qualities.update(header);

        const auto ref = reference_three_pass(header);
        EXPECT_TRUE(same_quality(qualities.gzip_, ref.gzip_));
        EXPECT_TRUE(same_quality(qualities.brotli_, ref.brotli_));
        EXPECT_TRUE(same_quality(qualities.zstd_, ref.zstd_));
        EXPECT_TRUE(same_quality(qualities.identity_, ref.identity_));
    }
}

TEST(Ruvia, response_coding_selection_end_to_end) {
    const auto select = [](std::string_view header_value) {
        const auto selected = select_field(header_value);
        if (!selected.acceptable()) {
            throw std::runtime_error("test expected an acceptable response coding");
        }
        return selected.coding();
    };
    // Server tie-break prefers br > zstd > gzip > deflate at equal q.
    EXPECT_TRUE(select("gzip, br, zstd") == http_content_coding::brotli);
    // Explicit q ordering wins over the tie-break.
    EXPECT_TRUE(select("identity;q=0, gzip;q=0.9, br;q=0.1") == http_content_coding::gzip);
    // A coding at q=0 is excluded even under a permissive wildcard.
    EXPECT_TRUE(select("identity;q=0, br;q=0, *;q=0.5") == http_content_coding::zstd);
    // identity is implicitly q=1, so a lower-quality coding must not override it.
    EXPECT_TRUE(select("gzip;q=0.9") == http_content_coding::identity);
    // A positive wildcard does not lower identity's implicit quality, while an
    // explicit identity preference does.
    EXPECT_TRUE(select("*;q=0.5") == http_content_coding::identity);
    EXPECT_TRUE(select("identity;q=0.1, gzip;q=0.5") == http_content_coding::gzip);
    EXPECT_TRUE(select("identity;q=0.1, deflate;q=0.5") == http_content_coding::deflate);
    // Repeating the same coding is equivalent to multiple matching alternatives:
    // the highest qvalue wins, independently of list order.
    EXPECT_TRUE(select("identity;q=0.5, gzip;q=0.9, gzip;q=0.1") == http_content_coding::gzip);
    EXPECT_TRUE(select("identity;q=0.5, gzip;q=0.1, gzip;q=0.9") == http_content_coding::gzip);
    // The same rule applies to repeated wildcard entries for an unlisted coding.
    EXPECT_TRUE(select("identity;q=0, *;q=0.8, *;q=0.1") == http_content_coding::brotli);
    http_response_coding_qualities repeated;
    repeated.update("gzip;q=0.9, gzip;q=0.1, *;q=0.8, *;q=0.2");
    EXPECT_EQ(repeated.gzip_.explicit_quality_, 900);
    EXPECT_EQ(repeated.brotli_.wildcard_quality_, 800);
    http_response_coding_qualities split_lines;
    split_lines.update("gzip;q=0.9, *;q=0.8");
    split_lines.update("gzip;q=0.1, *;q=0.2");
    EXPECT_EQ(split_lines.gzip_.explicit_quality_, 900);
    EXPECT_EQ(split_lines.brotli_.wildcard_quality_, 800);
    // Accept-Encoding allows only an optional weight after a coding. Unknown
    // parameters and whitespace around q's '=' make the item invalid; they must
    // not inherit the default q=1 and outrank identity.
    EXPECT_TRUE(select("identity;q=0.5, gzip;level=9") == http_content_coding::identity);
    EXPECT_TRUE(select("identity;q=0.5, gzip;q =1") == http_content_coding::identity);
    EXPECT_TRUE(select("identity;q=0.5, gzip;q= 1") == http_content_coding::identity);
    // OWS around the weight delimiter itself is explicitly allowed.
    EXPECT_TRUE(select("identity;q=0.5, gzip \t; \tq=0.8") == http_content_coding::gzip);
    // No acceptable coding is an explicit protocol outcome, not identity.
    for (const auto header : {std::string_view{"identity;q=0"}, std::string_view{"*;q=0"}}) {
        const auto result_value = select_field(header);
        EXPECT_TRUE(!result_value.acceptable());
        EXPECT_TRUE(!result_value.identity_accepted());
    }
    const auto explicit_empty = select_field("");
    EXPECT_TRUE(explicit_empty.acceptable());
    EXPECT_TRUE(explicit_empty.coding() == http_content_coding::identity);
    EXPECT_TRUE(explicit_empty.identity_accepted());
    EXPECT_TRUE(explicit_empty.accepts(http_content_coding::identity));
    EXPECT_TRUE(!explicit_empty.accepts(http_content_coding::gzip));

    const auto no_header = http_response_coding_selection::select(http_response_coding_qualities{});
    EXPECT_TRUE(no_header.acceptable());
    EXPECT_TRUE(no_header.coding() == http_content_coding::identity);
    EXPECT_TRUE(no_header.identity_accepted());
    EXPECT_TRUE(no_header.accepts(http_content_coding::identity));
    EXPECT_TRUE(no_header.accepts(http_content_coding::gzip));

    auto gzip_only = http_response_coding_candidates::empty();
    gzip_only.include(http_content_coding::gzip);
    const auto absent_header_gzip =
        http_response_coding_selection::select(http_response_coding_qualities{}, gzip_only);
    EXPECT_TRUE(absent_header_gzip.acceptable());
    EXPECT_TRUE(absent_header_gzip.coding() == http_content_coding::gzip);
    EXPECT_TRUE(absent_header_gzip.accepts(http_content_coding::gzip));

    http_response_coding_qualities empty_header;
    empty_header.update("");
    // An explicitly empty Accept-Encoding allows only an uncoded representation,
    // so a response policy with only gzip has no acceptable candidate.
    const auto explicit_empty_gzip = http_response_coding_selection::select(empty_header, gzip_only);
    EXPECT_TRUE(!explicit_empty_gzip.acceptable());
}

TEST(Ruvia, accepted_encoding_quality_treats_gzip_aliases_as_the_same_coding) {
    for (const auto coding : {std::string_view{"gzip"}, std::string_view{"x-gzip"}, std::string_view{"X-GZIP"}}) {
        EXPECT_TRUE(ruvia::http_accepts_encoding("x-gzip", coding));
        EXPECT_TRUE(ruvia::http_accepts_encoding("GZIP", coding));
        EXPECT_TRUE(!ruvia::http_accepts_encoding("x-gzip;q=0, *;q=1", coding));
        EXPECT_TRUE(!ruvia::http_accepts_encoding("gzip;q=0, *;q=1", coding));
        EXPECT_TRUE(ruvia::http_accepts_encoding("x-gzip;q=0.8, *;q=0", coding));
    }
}

TEST(Ruvia, response_coding_selection_accepts_the_gzip_alias) {
    for (const auto header : {std::string_view{"x-gzip, identity;q=0"}, std::string_view{"X-GZIP;q=0.8, identity;q=0"}}) {
        const auto selected = select_field(header);
        EXPECT_TRUE(selected.acceptable());
        EXPECT_TRUE(selected.coding() == http_content_coding::gzip);
        EXPECT_TRUE(selected.accepts(http_content_coding::gzip));
        EXPECT_TRUE(!selected.identity_accepted());
        EXPECT_TRUE(!selected.accepts(http_content_coding::identity));
        EXPECT_EQ(ruvia::http_content_coding_token(selected.coding()), "gzip");
    }
}

TEST(Ruvia, response_coding_gzip_aliases_share_weights_across_field_lines) {
    for (const bool alias_first : {false, true}) {
        const auto first = alias_first ? "x-gzip;q=0.8" : "gzip;q=0.2";
        const auto second = alias_first ? "gzip;q=0.2" : "X-GZIP;q=0.8";
        http_response_coding_qualities qualities;
        qualities.update(first);
        qualities.update(second);
        EXPECT_EQ(qualities.gzip_.explicit_quality_, 800);
        for (const auto coding : {std::string_view{"gzip"}, std::string_view{"x-gzip"}}) {
            http_accepted_encoding_quality quality;
            quality.update(first, coding);
            quality.update(second, coding);
            EXPECT_EQ(quality.explicit_quality_, qualities.gzip_.explicit_quality_);
        }
    }
}

TEST(Ruvia, response_coding_selection_retains_client_preference_until_representation_policy) {
    // The selector records the client's acceptable representation set. Whether
    // a Web runtime can create gzip is enforced only after the representation
    // source (sidecar, buffered body, or stream) is known.
    const auto coding = select_field("gzip, identity;q=0");
    EXPECT_TRUE(coding.acceptable());
    EXPECT_TRUE(coding.coding() == http_content_coding::gzip);
    EXPECT_TRUE(!coding.identity_accepted());
    EXPECT_TRUE(coding.accepts(http_content_coding::gzip));
    EXPECT_TRUE(!coding.accepts(http_content_coding::identity));
}

TEST(Ruvia, response_coding_selection_uses_only_available_representations) {
    http_response_coding_qualities qualities;
    qualities.update("br, gzip, identity;q=0");

    auto gzip_only = http_response_coding_candidates::identity_only();
    gzip_only.include(http_content_coding::gzip);
    const auto gzip_selection = http_response_coding_selection::select(qualities, gzip_only);
    EXPECT_TRUE(gzip_selection.acceptable());
    EXPECT_TRUE(gzip_selection.coding() == http_content_coding::gzip);

    auto identity_only = http_response_coding_candidates::identity_only();
    const auto identity_selection = http_response_coding_selection::select(qualities, identity_only);
    EXPECT_TRUE(!identity_selection.acceptable());

    // A failed ranking keeps the client's snapshot: the same selection still
    // answers for codings it accepts and ranks another representation set.
    auto brotli_only = http_response_coding_candidates::empty();
    brotli_only.include(http_content_coding::brotli);
    EXPECT_TRUE(identity_selection.accepts(http_content_coding::brotli));
    const auto brotli_selection = identity_selection.select(brotli_only);
    EXPECT_TRUE(brotli_selection.acceptable());
    EXPECT_TRUE(brotli_selection.coding() == http_content_coding::brotli);
}

TEST(Ruvia, http_accepts_encoding_rfc9110_identity_rules) {
    using ruvia::http_accepts_encoding;

    // RFC 9110 §12.5.3: Empty field-value accepts only identity.
    EXPECT_TRUE(http_accepts_encoding("", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("", "gzip"));

    // RFC 9110 §12.5.3: "identity" is always acceptable unless specifically refused.
    EXPECT_TRUE(http_accepts_encoding("gzip", "identity"));
    EXPECT_TRUE(http_accepts_encoding("gzip, br", "identity"));
    EXPECT_TRUE(http_accepts_encoding("*;q=0.5", "identity"));

    // Explicitly refused with identity;q=0.
    EXPECT_TRUE(!http_accepts_encoding("identity;q=0", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("gzip, identity;q=0", "identity"));

    // Refused by wildcard *;q=0 when identity is not specifically included.
    EXPECT_TRUE(!http_accepts_encoding("*;q=0", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("gzip, *;q=0", "identity"));

    // Explicit identity;q>0 overrides *;q=0.
    EXPECT_TRUE(http_accepts_encoding("gzip, *;q=0, identity;q=0.5", "identity"));

    // Explicit identity;q>0.
    EXPECT_TRUE(http_accepts_encoding("identity;q=0.1", "identity"));

    // http_accepted_encoding_quality accepts(bool) overload check.
    http_accepted_encoding_quality quality;
    quality.update("gzip", "identity");
    EXPECT_TRUE(quality.accepts(true));
    EXPECT_TRUE(!quality.accepts(false));
}

TEST(Ruvia, accepted_encoding_weights_do_not_allow_media_parameter_slots) {
    for (const std::string_view item : {"gzip;", "gzip;;q=1", "gzip;q=1;", "gzip; ;q=1;"}) {
        EXPECT_TRUE(!ruvia::http_accepts_encoding(item, "gzip"));
        http_response_coding_qualities qualities;
        qualities.update("identity;q=0.5");
        qualities.update(item);
        const auto selected = http_response_coding_selection::select(qualities);
        EXPECT_TRUE(selected.acceptable());
        EXPECT_TRUE(selected.coding() == http_content_coding::identity);
        EXPECT_TRUE(!selected.accepts(http_content_coding::gzip));
    }
    EXPECT_TRUE(!ruvia::http_accepts_encoding("*;;q=1", "extension"));
    EXPECT_TRUE(!ruvia::http_accepts_encoding("extension;q=0.8;", "extension"));
    EXPECT_TRUE(ruvia::http_accepts_encoding("extension;q=0.8", "extension"));
}

TEST(Ruvia, response_custom_coding_distinguishes_missing_and_empty_preferences) {
    const auto missing = ruvia::test::parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto empty = ruvia::test::parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding:\r\n\r\n");
    ASSERT_TRUE(missing.complete());
    ASSERT_TRUE(empty.complete());
    EXPECT_TRUE(ruvia::http_accepts_encoding(missing.state_.request(), "custom"));
    EXPECT_FALSE(ruvia::http_accepts_encoding(empty.state_.request(), "custom"));
    EXPECT_TRUE(ruvia::http_accepts_encoding(empty.state_.request(), "identity"));
    ruvia::http_response response;
    response.header("Content-Encoding", "custom");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(
                  ruvia::http_response_coding_selection_for(missing.state_.request()), response, &missing.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(
                  ruvia::http_response_coding_selection_for(empty.state_.request()), response, &empty.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
}

TEST(Ruvia, response_custom_coding_explicit_quality_overrides_wildcard_across_lines) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: *;q=0.8, custom;q=0, gzip\r\n"
        "Accept-Encoding: CUSTOM;q=0.4, x-compress;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    EXPECT_TRUE(selection.accepts("CuStOm", parsed.state_.request()));
    EXPECT_TRUE(selection.accepts("another", parsed.state_.request()));
    EXPECT_FALSE(selection.accepts("compress", parsed.state_.request()));
    ruvia::http_response response;
    response.header("Content-Encoding", "gzip, CUSTOM");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    response.header("Content-Encoding", "compress", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
}

TEST(Ruvia, response_coding_stack_checks_every_known_and_custom_member_without_common_case_allocation) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip, custom, other, br;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    ruvia::http_response response;
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::absent);
    response.header("Content-Encoding", "gzip, CUSTOM, other, custom");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    response.header("Content-Encoding", "br", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
    EXPECT_THROW(response.header("Content-Encoding", "gzip;param=value"), std::invalid_argument);
}

TEST(Ruvia, response_coding_extension_stack_is_not_limited_by_decoder_depth) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: *, ninth;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    ruvia::http_response response;
    response.header("Content-Encoding", "first, second, third, fourth, fifth, sixth, seventh, eighth, ninth");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
    response.header("Content-Encoding", "first, second, third, fourth, fifth, sixth, seventh, eighth, tenth");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
}

using ruvia::decode_http_content;
using ruvia::encode_http_content;
using ruvia::http_content_coding;
using ruvia::http_content_encode_context;

namespace {

constexpr std::array compressing_codings{http_content_coding::gzip, http_content_coding::deflate,
    http_content_coding::brotli, http_content_coding::zstd};

// Compressible but not trivially repetitive, so encoders emit several output
// blocks for the larger sizes.
[[nodiscard]] std::string sample_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 1U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 7U) + ((i / 64U) % 3U));
    }
    return body;
}

[[nodiscard]] std::string one_shot(http_content_coding coding, std::string_view input, std::size_t cap) {
    auto result_value = encode_http_content(coding, input, {.max_encoded_bytes_ = cap});
    return result_value.has_value() ? std::move(*result_value) : std::string();
}

[[nodiscard]] std::optional<std::string> encode_with(http_content_encode_context& context,
    std::string_view input, std::size_t cap) {
    auto result_value = context.encode(input, {.max_encoded_bytes_ = cap});
    if (!result_value.has_value()) {
        return std::nullopt;
    }
    return std::string(*result_value);
}

}  // namespace

TEST(Ruvia, http_content_encode_context_reuse_matches_one_shot_bytes) {
    constexpr std::array sizes{std::size_t{0}, std::size_t{1}, std::size_t{1000}, std::size_t{8192},
        std::size_t{70'000}, std::size_t{300'000}};
    for (const auto coding : compressing_codings) {
        http_content_encode_context context(coding);
        EXPECT_TRUE(context.coding() == coding);
        unsigned seed = 0;
        // Twice through, so every size also follows a larger representation.
        for (int round = 0; round < 2; ++round) {
            for (const auto size : sizes) {
                const auto input = sample_body(size, ++seed);
                const auto expected = one_shot(coding, input, input.size() + 1024);
                const auto actual = encode_with(context, input, input.size() + 1024);
                EXPECT_TRUE(actual.has_value());
                EXPECT_TRUE(actual.value_or(std::string()) == expected);
                const auto decoded = decode_http_content(
                    std::array{coding}, actual.value_or(std::string()), {.max_decoded_bytes_ = size});
                EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
                if (const auto* content = std::get_if<std::string>(&decoded)) {
                    EXPECT_TRUE(std::string_view(*content) == input);
                }
            }
        }
    }
}

namespace {

void check_encoder_alias(ruvia::http_content_coding coding) {
    std::string input(8 * 1024 * 1024 + 37, '\0');
    std::uint64_t random = 0x2b19ab247313fc6dULL;
    for (auto& byte : input) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }

    for (const std::size_t input_offset : {std::size_t{0}, std::size_t{23}}) {
        {
            std::string output(input_offset, 'p');
            output.append(input);
            const auto padding_offset = output.size();
            output.resize(output.capacity(), '#');
            const auto prefix_size = output.size();
            {
                ruvia::http_content_encoder encoder(coding);
                encoder.write(std::string_view(output).substr(input_offset, input.size()), output, true);
                encoder.finish(output);
            }
            EXPECT_EQ(std::string_view(output).substr(0, input_offset), std::string(input_offset, 'p'));
            EXPECT_EQ(std::string_view(output).substr(input_offset, input.size()), std::string_view(input));
            EXPECT_EQ(std::string_view(output).substr(padding_offset, prefix_size - padding_offset), std::string(prefix_size - padding_offset, '#'));

            const auto decoded = ruvia::decode_http_content(std::array{coding},
                std::string_view(output).substr(prefix_size),
                {.max_decoded_bytes_ = input.size()});
            EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
            if (const auto* content = std::get_if<std::string>(&decoded)) {
                EXPECT_EQ(std::string_view(*content), std::string_view(input));
            }
        }
    }
}

}  // namespace

TEST(Ruvia, http_content_encoder_identity_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::identity);
}

TEST(Ruvia, http_content_encoder_gzip_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::gzip);
}

TEST(Ruvia, http_content_encoder_deflate_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::deflate);
}

TEST(Ruvia, http_content_encoder_brotli_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::brotli);
}

TEST(Ruvia, http_content_encoder_zstd_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::zstd);
}

TEST(Ruvia, response_content_encoding_folds_header_lines_in_application_order) {
    ruvia::http_response response;
    const auto missing = ruvia::parse_http_content_coding_headers(response.headers());
    EXPECT_TRUE(missing.codings().empty());
    EXPECT_TRUE(missing.invalid() == nullptr);
    EXPECT_TRUE(missing.unsupported() == nullptr);

    response.header("Content-Encoding", "gzip");
    response.header("Content-Encoding", "deflate",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Content-Encoding", "br",
        {.mode_ = ruvia::http_response_header_mode::append});
    const auto stacked = ruvia::parse_http_content_coding_headers(response.headers());
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(stacked.codings()[i] == expected[i]);
    }
}

TEST(Ruvia, content_encoding_field_lines_fold_in_order_ignoring_other_fields) {
    const std::array unrelated{ruvia::http_header("Content-Type", "text/plain")};
    const auto missing = ruvia::parse_http_content_coding_headers(unrelated);
    EXPECT_TRUE(missing.codings().empty());
    EXPECT_TRUE(missing.invalid() == nullptr);
    EXPECT_TRUE(missing.unsupported() == nullptr);

    const std::array lines{ruvia::http_header("Content-Encoding", "gzip"),
        ruvia::http_header("Content-Type", "text/plain"), ruvia::http_header("content-encoding", "deflate"),
        ruvia::http_header("Content-Encoding", "br")};
    const auto stacked = ruvia::parse_http_content_coding_headers(lines);
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(stacked.codings()[i] == expected[i]);
    }
}

namespace {

// True if a public cookie plan rejects the options.
bool rejects(const ruvia::cookie_options& options) {
    try {
        (void)ruvia::set_cookie_plan("sid", "value", options);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, cookie_public_validation_and_prefix_parsing) {
    EXPECT_TRUE(ruvia::is_valid_cookie_value("session-token"));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("invalid;value"));
    EXPECT_TRUE(ruvia::is_valid_cookie_value("\"abc\""));
    EXPECT_TRUE(ruvia::is_valid_cookie_value("\"\""));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\"abc"));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\"a\"b\""));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\""));
    const ruvia::cookie_options no_attributes{.path_ = ""};
    const ruvia::set_cookie_plan quoted("sid", "\"abc\"", no_attributes);
    std::string quoted_wire(quoted.size(), '\0');
    quoted.write(quoted_wire.data());
    EXPECT_EQ(quoted_wire, "sid=\"abc\"");
    EXPECT_TRUE(ruvia::cookie_name_prefix("__secure-id") == ruvia::cookie_prefix::secure);
    EXPECT_TRUE(ruvia::cookie_name_prefix("__HOST-id") == ruvia::cookie_prefix::host);
    EXPECT_TRUE(!ruvia::cookie_name_prefix("id").has_value());
    EXPECT_TRUE(ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::secure, true, "/a", "example.com"));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::secure, false, "/", ""));
    EXPECT_TRUE(ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "/", ""));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "", ""));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "/", "example.com"));
    EXPECT_EQ(ruvia::http_cookie_prefix_text(ruvia::cookie_prefix::host), std::string_view("__Host-"));
}

TEST(Ruvia, cookie_borrowed_text_accepts_stable_string_owners) {
    const std::string path = "/account";
    const std::string domain = "example.com";
    std::string name = "sid";
    const std::string value = "value";

    ruvia::cookie_options options;
    options.path_ = path;
    options.domain_ = domain;
    EXPECT_TRUE(!rejects(options));

    const ruvia::set_cookie_plan plan(name, value, options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    name.assign("changed");
    EXPECT_EQ(wire, std::string("sid=value; Path=/account; Domain=example.com"));
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    EXPECT_TRUE(parsed_value.has_value());
    if (parsed_value) {
        EXPECT_EQ(parsed_value->name(), "sid");
        EXPECT_EQ(parsed_value->path(), "/account");
        EXPECT_EQ(parsed_value->domain(), "example.com");
        EXPECT_TRUE(parsed_value->name().data() == wire.data());
        EXPECT_TRUE(parsed_value->path().data() == wire.data() + wire.find("/account"));
        EXPECT_TRUE(parsed_value->domain().data() == wire.data() + wire.find("example.com"));
    }
}

TEST(Ruvia, cookie_expires_formats_historical_dates_at_second_resolution) {
    using namespace std::chrono;
    ruvia::cookie_options options;
    options.expires_ = system_clock::time_point{seconds{-315619200}};
    EXPECT_TRUE(!rejects(options));
    const ruvia::set_cookie_plan historical("sid", "value", options);
    std::string wire(historical.size(), '\0');
    historical.write(wire.data());
    EXPECT_EQ(wire, std::string("sid=value; Path=/; Expires=Fri, 01 Jan 1960 00:00:00 GMT"));

    options.expires_ = system_clock::time_point{} - system_clock::duration{1};
    const ruvia::set_cookie_plan fractional("sid", "value", options);
    wire.resize(fractional.size());
    fractional.write(wire.data());
    EXPECT_EQ(wire, std::string("sid=value; Path=/; Expires=Wed, 31 Dec 1969 23:59:59 GMT"));
}

TEST(Ruvia, cookie_expires_rejects_dates_before_the_cookie_calendar_range) {
    using namespace std::chrono;
    constexpr auto cutoff = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
    // Clocks with nanosecond int64 storage cannot represent dates this early;
    // wider calendar ranges (including MSVC's 100ns clock) exercise the edge.
    if constexpr (ceil<seconds>(system_clock::duration::min()) <= cutoff) {
        ruvia::cookie_options options;
        options.expires_ = system_clock::time_point{duration_cast<system_clock::duration>(cutoff)};
        EXPECT_TRUE(!rejects(options));
        const ruvia::set_cookie_plan first("sid", "value", options);
        std::string wire(first.size(), '\0');
        first.write(wire.data());
        EXPECT_TRUE(wire.find("01 Jan 1601") != std::string::npos);
        options.expires_ = *options.expires_ - system_clock::duration{1};
        EXPECT_TRUE(rejects(options));
        bool rejected = false;
        try {
            (void)ruvia::set_cookie_plan("sid", "value", options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, cookie_samesite_enum_maps_to_wire_tokens) {
    ruvia::cookie_options strict;
    strict.same_site_ = ruvia::cookie_same_site::strict;
    EXPECT_TRUE(!rejects(strict));

    ruvia::cookie_options lax;
    lax.same_site_ = ruvia::cookie_same_site::lax;
    EXPECT_TRUE(!rejects(lax));

    ruvia::cookie_options none;
    none.same_site_ = ruvia::cookie_same_site::none;
    none.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(none));

    EXPECT_TRUE(!ruvia::cookie_options{}.same_site_.has_value());
}

TEST(Ruvia, cookie_samesite_none_requires_secure) {
    ruvia::cookie_options insecure_none;
    insecure_none.same_site_ = ruvia::cookie_same_site::none;
    insecure_none.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(insecure_none));  // RFC 6265bis §5.5

    ruvia::cookie_options secure_none;
    secure_none.same_site_ = ruvia::cookie_same_site::none;
    secure_none.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(secure_none));
}

TEST(Ruvia, cookie_domain_requires_dns_subdomain_syntax) {
    const auto accepts_domain = [](std::string_view domain) {
        ruvia::cookie_options options;
        options.domain_ = domain;
        return !rejects(options);
    };

    EXPECT_TRUE(accepts_domain("example.com"));
    EXPECT_TRUE(accepts_domain("EXAMPLE.com"));
    EXPECT_TRUE(accepts_domain("localhost"));
    EXPECT_TRUE(accepts_domain("3.example"));
    EXPECT_TRUE(accepts_domain("xn--bcher-kva.example"));

    // Domain= is a DNS subdomain, not the generic Set-Cookie av-octet syntax
    // used by Path. Emitting any of these values produces a non-conforming
    // Set-Cookie field that user agents ignore or interpret inconsistently.
    EXPECT_TRUE(!accepts_domain(".example.com"));
    EXPECT_TRUE(!accepts_domain("example.com."));
    EXPECT_TRUE(!accepts_domain("bad domain.example"));
    EXPECT_TRUE(!accepts_domain("bad_domain.example"));
    EXPECT_TRUE(!accepts_domain("-bad.example"));
    EXPECT_TRUE(!accepts_domain("bad-.example"));
    EXPECT_TRUE(!accepts_domain("bad..example"));
    EXPECT_TRUE(!accepts_domain("caf\xc3\xa9.example"));
    EXPECT_TRUE(!accepts_domain(std::string(64, 'a') + ".example"));
}

TEST(Ruvia, cookie_validation_rejects_injection_and_bad_options) {
    const auto rejects_cookie = [](std::string_view name, std::string_view value,
                                    const ruvia::cookie_options& options) {
        try {
            (void)ruvia::set_cookie_plan(name, value, options);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    const ruvia::cookie_options clean;
    EXPECT_TRUE(!rejects_cookie("sid", "value", clean));
    EXPECT_TRUE(rejects_cookie("bad name", "value", clean));  // space -> not a token name
    EXPECT_TRUE(rejects_cookie("sid", "a;b", clean));         // ';' in value
    ruvia::cookie_options bad_path;
    bad_path.path_ = "a;b";
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_path));  // ';' in path attribute
    // Inputs the Set-Cookie parser would trim, drop, or ignore cannot be found
    // again for same-name replacement.
    for (const std::string_view path : {" /", "/ ", " "}) {
        ruvia::cookie_options spaced_path;
        spaced_path.path_ = path;
        EXPECT_TRUE(rejects_cookie("sid", "value", spaced_path));
    }
    const std::string long_path(1025, 'p');
    ruvia::cookie_options oversized_path;
    oversized_path.path_ = long_path;
    EXPECT_TRUE(rejects_cookie("sid", "value", oversized_path));
    oversized_path.path_ = std::string_view(long_path).substr(1);
    EXPECT_TRUE(!rejects_cookie("sid", "value", oversized_path));
    const std::string long_value(4093, 'v');
    EXPECT_TRUE(!rejects_cookie("sid", long_value, clean));
    EXPECT_TRUE(rejects_cookie("sidx", long_value, clean));
    ruvia::cookie_options typed_prefix;
    typed_prefix.secure_ = ruvia::cookie_attribute_policy::emit;
    typed_prefix.prefix_ = ruvia::cookie_prefix::secure;
    EXPECT_TRUE(rejects_cookie("sid", long_value, typed_prefix));
    ruvia::cookie_options bad_priority;
    bad_priority.priority_ = static_cast<ruvia::cookie_priority>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_priority));
    ruvia::cookie_options bad_same_site;
    bad_same_site.same_site_ = static_cast<ruvia::cookie_same_site>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_same_site));
    ruvia::cookie_options bad_prefix;
    bad_prefix.prefix_ = static_cast<ruvia::cookie_prefix>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_prefix));
    ruvia::cookie_options partitioned;
    partitioned.partitioned_ = ruvia::cookie_attribute_policy::emit;  // partitioned requires Secure
    EXPECT_TRUE(rejects_cookie("sid", "value", partitioned));
    ruvia::cookie_options bad_http_only;
    bad_http_only.http_only_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_http_only));
    ruvia::cookie_options bad_secure;
    bad_secure.secure_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_secure));
    ruvia::cookie_options bad_partitioned;
    bad_partitioned.partitioned_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_partitioned));
    ruvia::cookie_options negative_max_age;
    negative_max_age.max_age_ = std::chrono::seconds{-1};
    EXPECT_TRUE(rejects_cookie("sid", "value", negative_max_age));
}

TEST(Ruvia, cookie_secure_prefix_requires_secure) {
    ruvia::cookie_options secured;
    secured.prefix_ = ruvia::cookie_prefix::secure;
    secured.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(secured));

    ruvia::cookie_options insecure;
    insecure.prefix_ = ruvia::cookie_prefix::secure;
    insecure.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(insecure));  // __Secure- requires Secure
}

TEST(Ruvia, cookie_host_prefix_requires_secure_root_path_no_domain) {
    // __Host- is the strictest prefix (RFC 6265bis 4.1.3.2).
    ruvia::cookie_options valid;
    valid.prefix_ = ruvia::cookie_prefix::host;
    valid.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain is empty
    EXPECT_TRUE(!rejects(valid));

    ruvia::cookie_options not_secure = valid;
    not_secure.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(not_secure));

    ruvia::cookie_options sub_path = valid;
    sub_path.path_ = "/sub";
    EXPECT_TRUE(rejects(sub_path));  // Path must be exactly "/"

    ruvia::cookie_options with_domain = valid;
    with_domain.domain_ = "example.com";
    EXPECT_TRUE(rejects(with_domain));  // Domain must be absent
}

TEST(Ruvia, cookie_literal_prefix_name_enforces_requirements) {
    const auto rejects_with_name = [](std::string_view name, const ruvia::cookie_options& options) {
        try {
            (void)ruvia::set_cookie_plan(name, "value", options);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // RFC 6265bis user agents match these prefixes case-insensitively. Reject
    // every spelling the UA would reject instead of emitting a silently dropped
    // cookie.
    ruvia::cookie_options insecure;                            // Secure defaults to omitted
    EXPECT_TRUE(rejects_with_name("__Secure-tok", insecure));  // __Secure- requires Secure
    EXPECT_TRUE(rejects_with_name("__secure-tok", insecure));
    EXPECT_TRUE(rejects_with_name("__SeCuRe-tok", insecure));

    ruvia::cookie_options host_bad_domain;
    host_bad_domain.secure_ = ruvia::cookie_attribute_policy::emit;
    host_bad_domain.domain_ = "example.com";  // __Host- forbids Domain
    EXPECT_TRUE(rejects_with_name("__Host-sid", host_bad_domain));
    EXPECT_TRUE(rejects_with_name("__HOST-sid", host_bad_domain));
    EXPECT_TRUE(rejects_with_name("__hOsT-sid", host_bad_domain));

    // A literal-prefixed name that meets the constraints is accepted.
    ruvia::cookie_options ok_secure;
    ok_secure.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects_with_name("__Secure-tok", ok_secure));
    EXPECT_TRUE(!rejects_with_name("__sEcUrE-tok", ok_secure));
    ruvia::cookie_options ok_host;
    ok_host.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain empty
    EXPECT_TRUE(!rejects_with_name("__Host-sid", ok_host));
    EXPECT_TRUE(!rejects_with_name("__hOsT-sid", ok_host));

    // A name that only resembles a prefix (missing the trailing '-') is unaffected.
    ruvia::cookie_options plain;
    EXPECT_TRUE(!rejects_with_name("__Secured", plain));
    EXPECT_TRUE(!rejects_with_name("__Hostname", plain));
}

TEST(Ruvia, cookie_writer_leaves_lifetime_caps_to_user_agents) {
    // RFC 6265bis' 400-day cookie-age-limit binds user agents, not servers.
    ruvia::cookie_options over_cap;
    over_cap.max_age_ = std::chrono::seconds(34560001);
    EXPECT_TRUE(!rejects(over_cap));

    ruvia::cookie_options far_expires;
    far_expires.expires_ = std::chrono::system_clock::now() + std::chrono::hours(24 * 500);
    EXPECT_TRUE(!rejects(far_expires));

    ruvia::cookie_options deletion;
    deletion.max_age_ = std::chrono::seconds(0);
    EXPECT_TRUE(!rejects(deletion));

    ruvia::cookie_options negative;
    negative.max_age_ = std::chrono::seconds(-1);
    EXPECT_TRUE(rejects(negative));
}

TEST(Ruvia, cookie_request_pair_keeps_equals_for_empty_names) {
    const auto pair = [&](std::string_view name, std::string_view value) {
        std::string wire(ruvia::cookie_request_pair_size(name, value), '\0');
        const auto* const end = ruvia::write_cookie_request_pair(wire.data(), name, value);
        EXPECT_TRUE(end == wire.data() + wire.size());
        return wire;
    };
    EXPECT_EQ(pair("sid", "abc"), std::string("sid=abc"));
    // A nameless value that looks like a cookie-pair must stay behind '='.
    EXPECT_EQ(pair("", "session=forged"), std::string("=session=forged"));
    EXPECT_EQ(pair("", "sid"), std::string("=sid"));
    EXPECT_EQ(pair("sid", ""), std::string("sid="));
    EXPECT_EQ(pair(std::string_view("\0", 1), std::string_view("\0", 1)), std::string("\0=\0", 3));
}

TEST(Ruvia, cookie_request_pair_accepts_inputs_inside_the_written_bytes) {
    // Each input is consumed before an output byte overwrites it.
    std::string buffer("valuename");
    buffer.resize(16, '#');
    const std::string_view value(buffer.data(), 5);
    const std::string_view name(buffer.data() + 5, 4);
    const auto* const end = ruvia::write_cookie_request_pair(buffer.data(), name, value);
    EXPECT_TRUE(end == buffer.data() + 10);
    EXPECT_EQ(std::string_view(buffer.data(), 10), std::string_view("name=value"));
}

TEST(Ruvia, cookie_request_pair_preserves_aliased_inputs_and_terminators) {
    std::string header = "a";
    header.reserve(16);
    const std::string_view terminator(header.data() + header.size(), 1);
    ruvia::append_cookie_request_pair(header, "b", terminator);
    EXPECT_EQ(header, std::string("a; b=\0", 6));

    std::string empty;
    const std::string_view empty_terminator(empty.data(), 1);
    ruvia::append_cookie_request_pair(empty, empty_terminator, empty_terminator);
    EXPECT_EQ(std::string_view(empty), std::string_view("\0=\0", 3));

    std::string growing = "sid=abc";
    growing.resize(growing.capacity(), 'x');
    const auto original = growing;
    const std::string_view name(growing.data(), 3);
    const std::string_view value(growing.data() + 4, growing.size() - 3);
    ruvia::append_cookie_request_pair(growing, name, value);
    auto expected = original + "; sid=" + original.substr(4);
    expected.push_back('\0');
    EXPECT_EQ(growing, expected);
}

TEST(Ruvia, cookie_request_pair_preserves_contents_when_allocations_are_rejected) {
    {
        std::string header;
        header.reserve(1024);
        const std::string value(300, 'v');
        const auto expected = std::string("\0=", 2) + value;
        try {
            ruvia::append_cookie_request_pair(header, std::string_view(header.data(), 1), value);
        } catch (const std::bad_alloc&) {
            ASSERT_TRUE(header.empty());
            ruvia::append_cookie_request_pair(header, std::string_view(header.data(), 1), value);
        }
        EXPECT_EQ(std::string_view(header), std::string_view(expected));
    }
}

TEST(Ruvia, cookie_request_pair_snapshots_only_overlapping_inputs) {
    for (const bool alias_name : {false, true}) {
        for (const auto aliased_size : {3U, 300U}) {
            SCOPED_TRACE(alias_name);
            SCOPED_TRACE(aliased_size);
            const std::string aliased(aliased_size, alias_name ? 'n' : 'v');
            const std::string independent(300, alias_name ? 'v' : 'n');
            const auto expected = alias_name ? aliased + "=" + independent : independent + "=" + aliased;
            std::string output = aliased;
            output.resize(expected.size());
            const std::string_view borrowed(output.data(), aliased.size());

            (void)ruvia::write_cookie_request_pair(output.data(),
                alias_name ? borrowed : independent, alias_name ? independent : borrowed);

            EXPECT_EQ(output, expected);
        }
    }
}

TEST(Ruvia, cookie_request_pair_preserves_both_long_aliased_inputs_on_failure_and_retry) {
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    std::string output = value + name;
    output.resize(name.size() + 1 + value.size());
    const auto original = output;
    const std::string_view borrowed_name(output.data() + value.size(), name.size());
    const std::string_view borrowed_value(output.data(), value.size());

    try {
        (void)ruvia::write_cookie_request_pair(output.data(), borrowed_name, borrowed_value);
    } catch (const std::bad_alloc&) {
        ASSERT_EQ(output, original);
        (void)ruvia::write_cookie_request_pair(output.data(), borrowed_name, borrowed_value);
    }

    EXPECT_EQ(output, name + "=" + value);
}

TEST(Ruvia, cookie_plan_serialization_emits_fields_without_attributes) {
    const ruvia::cookie_options options{.path_ = ""};
    const ruvia::set_cookie_plan plan("sid", "value", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    EXPECT_EQ(wire, "sid=value");
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    EXPECT_TRUE(parsed_value.has_value());
    if (parsed_value) {
        EXPECT_EQ(parsed_value->name(), "sid");
        EXPECT_TRUE(parsed_value->name().data() == wire.data());
        EXPECT_TRUE(parsed_value->path().empty());
        EXPECT_TRUE(parsed_value->domain().empty());
    }
}

TEST(Ruvia, cookie_plan_serialization_formats_exact_max_age_widths_and_prefixed_fields) {
    constexpr std::int64_t ages[]{1, 9, 10, 99, 100, 999, 1000, ruvia::max_cookie_age_seconds};
    for (const auto age : ages) {
        const ruvia::cookie_options options{
            .path_ = "/", .max_age_ = std::chrono::seconds{age}, .prefix_ = ruvia::cookie_prefix::host, .secure_ = ruvia::cookie_attribute_policy::emit};
        const ruvia::set_cookie_plan plan("sid", "value", options);
        std::string wire(plan.size(), '\0');
        plan.write(wire.data());
        EXPECT_EQ(wire, std::string("__Host-sid=value; Path=/; Max-Age=") + std::to_string(age).c_str() + "; Secure");
        const auto parsed_value = ruvia::parse_set_cookie(wire);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_EQ(parsed_value->name(), "__Host-sid");
            EXPECT_EQ(parsed_value->path(), "/");
            EXPECT_TRUE(parsed_value->domain().empty());
            EXPECT_TRUE(parsed_value->name().data() == wire.data());
            EXPECT_TRUE(parsed_value->path().data() == wire.data() + wire.find('/'));
        }
    }
}

TEST(Ruvia, cookie_plan_serialization_writes_zero_max_age_as_deletion_expires) {
    // max-age-av = "Max-Age=" non-zero-digit *DIGIT (RFC 6265 section 4.1.1).
    const ruvia::cookie_options options{.path_ = "/",
        .expires_ = std::chrono::system_clock::time_point{std::chrono::hours{24 * 365 * 40}},
        .max_age_ = std::chrono::seconds{0}};
    const ruvia::set_cookie_plan plan("sid", "", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    EXPECT_EQ(wire, "sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
}

namespace {
using ruvia::decode_url_component;
using ruvia::has_url_encoding;
using mode_type = ruvia::url_decode_mode;

std::optional<std::string> url_decode(std::string_view in, ruvia::url_decode_mode mode) {
    std::string storage;
    const auto decoded = ruvia::decode_url_component(in, storage, mode);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return std::string(*decoded);
}

}  // namespace

// Percent-encoding: decoding a component or a form field, validating one, and walking encoded
// pairs.

TEST(Ruvia, url_decode_percent) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("hello", m_type::percent).value(), std::string("hello"));
    EXPECT_EQ(url_decode("%41%42%43", m_type::percent).value(), std::string("ABC"));
    EXPECT_EQ(url_decode("a%2Fb", m_type::percent).value(), std::string("a/b"));
    EXPECT_EQ(url_decode("%00", m_type::percent).value(), std::string(1, '\0'));
    // '+' is literal in percent mode
    EXPECT_EQ(url_decode("a+b", m_type::percent).value(), std::string("a+b"));
}

TEST(Ruvia, url_decode_form) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("a+b", m_type::form).value(), std::string("a b"));
    EXPECT_EQ(url_decode("a+b%20c", m_type::form).value(), std::string("a b c"));
}

TEST(Ruvia, percent_byte_decoding_rejects_out_of_range_and_truncated_triplets) {
    EXPECT_TRUE(ruvia::http_try_decode_percent_byte("a%2Fb", 1) == std::optional<unsigned char>('/'));
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", std::string_view::npos).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", 3).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%2", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%G0", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("x20", 0).has_value());
}

TEST(Ruvia, uri_reference_encoding_borrows_or_writes_storage) {
    std::string storage("keep");
    constexpr std::string_view canonical = "/a%20b?x=1#frag";
    const auto unchanged = ruvia::percent_encode_uri_reference(canonical, storage);
    EXPECT_TRUE(unchanged.data() == canonical.data() && unchanged.size() == canonical.size());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    const auto encoded = ruvia::percent_encode_uri_reference("/a b?x=%zz", storage);
    EXPECT_TRUE(encoded.data() == storage.data());
    EXPECT_EQ(encoded, std::string_view("/a%20b?x=%25zz"));

    // The reference may be the storage itself.
    storage.assign("/c d");
    EXPECT_EQ(ruvia::percent_encode_uri_reference(storage, storage), std::string_view("/c%20d"));
    EXPECT_EQ(std::string_view(storage), std::string_view("/c%20d"));
}

TEST(Ruvia, url_decode_invalid) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_TRUE(!url_decode("%", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%4", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%zz", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("ab%2", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%g0", m_type::percent).has_value());
}

TEST(Ruvia, url_decode_form_keeps_lone_percent_literally) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("100%", m_type::form).value_or("<none>"), std::string("100%"));
    EXPECT_EQ(url_decode("%zz+%41", m_type::form).value_or("<none>"), std::string("%zz A"));
    EXPECT_EQ(url_decode("%4%41", m_type::form).value_or("<none>"), std::string("%4A"));
}

TEST(Ruvia, url_decode_all_bytes_and_malformed_suffix) {
    using m_type = ruvia::url_decode_mode;
    constexpr std::string_view hex = "0123456789aBcDeF";
    std::string encoded;
    std::string expected;
    for (unsigned int byte = 0; byte < 256; ++byte) {
        encoded.push_back('%');
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
        expected.push_back(static_cast<char>(byte));
    }
    for (const auto mode : {m_type::percent, m_type::form}) {
        const auto result_value = url_decode(encoded, mode);
        EXPECT_TRUE(result_value.has_value());
        if (result_value) {
            EXPECT_EQ(*result_value, expected);
        }
        for (const std::string_view suffix : {"%", "%0", "%xz"}) {
            const auto suffixed = url_decode(encoded + std::string(suffix), mode);
            if (mode == m_type::percent) {
                EXPECT_TRUE(!suffixed.has_value());
            } else {
                // WHATWG percent-decode keeps a '%' that starts no valid escape.
                EXPECT_TRUE(suffixed.has_value() && *suffixed == expected + std::string(suffix));
            }
        }
    }
}

TEST(Ruvia, url_validate_encoding) {
    using ruvia::validate_url_encoding;
    EXPECT_TRUE(validate_url_encoding("plain"));
    EXPECT_TRUE(validate_url_encoding("%41%42"));
    EXPECT_TRUE(!validate_url_encoding("%4"));
    EXPECT_TRUE(!validate_url_encoding("%zz"));
    EXPECT_TRUE(validate_url_encoding(""));
}

TEST(Ruvia, url_uri_paths_equivalent_and_ordered_by_canonical_form) {
    using ruvia::compare_uri_paths;
    using ruvia::uri_paths_equivalent;
    EXPECT_TRUE(uri_paths_equivalent("/users/admin", "/users/%61dmin"));
    EXPECT_TRUE(uri_paths_equivalent("/caf%C3%A9", "/caf%c3%a9"));
    EXPECT_TRUE(uri_paths_equivalent("/a:b", "/a%3Ab"));  // decoded, like route parameters
    EXPECT_TRUE(uri_paths_equivalent("", ""));
    EXPECT_TRUE(!uri_paths_equivalent("/a/b", "/a%2Fb"));  // an encoded slash is no separator
    EXPECT_TRUE(uri_paths_equivalent("/a%2fb", "/a%2Fb"));
    EXPECT_TRUE(!uri_paths_equivalent("/a+b", "/a%20b"));  // '+' is literal in a path
    EXPECT_TRUE(!uri_paths_equivalent("/api", "/apix"));
    EXPECT_TRUE(!uri_paths_equivalent("/%zz", "/%zz"));  // malformed escape
    EXPECT_TRUE(!uri_paths_equivalent("/%2", "/%2"));    // truncated escape

    EXPECT_EQ(compare_uri_paths("%61", "a"), 0);
    EXPECT_TRUE(compare_uri_paths("a", "%62") < 0);
    EXPECT_TRUE(compare_uri_paths("%62", "a") > 0);
    EXPECT_TRUE(compare_uri_paths("ab", "%61") > 0);
    EXPECT_TRUE(compare_uri_paths("a", "a%2F") < 0);
    // A separator orders after every octet, an encoded slash among them.
    EXPECT_TRUE(compare_uri_paths("a/", "a%FF") > 0);
    EXPECT_TRUE(compare_uri_paths("a%2F", "a/") < 0);
}

TEST(Ruvia, url_visit_pairs_count) {
    std::vector<std::pair<std::string, std::string>> pairs;
    (void)ruvia::visit_url_encoded_pairs(
        "x=1&y=2&z=3", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(3));
    if (pairs.size() == 3) {
        EXPECT_EQ(pairs[0].first, std::string("x"));
        EXPECT_EQ(pairs[2].second, std::string("3"));
    }
}

TEST(Ruvia, url_visit_pairs_stops_early) {
    std::size_t visited = 0;
    const bool completed = ruvia::visit_url_encoded_pairs("a=1&b=2&c=3",
        [&visited](std::string_view, std::string_view) {
            return ++visited < 2;
        });
    EXPECT_TRUE(!completed);
    EXPECT_EQ(visited, std::size_t(2));
}

TEST(Ruvia, url_visit_pairs_skips_empty_segments) {
    std::vector<std::pair<std::string, std::string>> pairs;
    // Leading, doubled, and trailing '&' produce empty segments that must NOT yield ("","") pairs.
    (void)ruvia::visit_url_encoded_pairs(
        "&a=1&&b=2&", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(2));
    if (pairs.size() == 2) {
        EXPECT_EQ(pairs[0].first, std::string("a"));
        EXPECT_EQ(pairs[1].first, std::string("b"));
    }
    // A key with an empty value ("k=") is still a real field and must be kept.
    std::vector<std::pair<std::string, std::string>> kept;
    (void)ruvia::visit_url_encoded_pairs("k=&=v", [&](std::string_view n, std::string_view v) {
        kept.emplace_back(std::string(n), std::string(v));
    });
    EXPECT_EQ(kept.size(), std::size_t(2));  // "k=" (name k, empty value) and "=v" (empty name, value v)
}

TEST(Ruvia, has_url_encoding_detection) {
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::percent));
    EXPECT_TRUE(!has_url_encoding("abc", mode_type::percent));
    // '+' is a literal in percent mode but triggers decoding in form mode.
    EXPECT_TRUE(!has_url_encoding("a+b", mode_type::percent));
    EXPECT_TRUE(has_url_encoding("a+b", mode_type::form));
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("plain", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::percent));
}

TEST(Ruvia, has_url_encoding_handles_binary_components) {
    std::string value(129, 'a');
    for (std::size_t position = 0; position < value.size(); ++position) {
        for (unsigned int byte = 0; byte < 256; ++byte) {
            value[position] = static_cast<char>(byte);
            EXPECT_EQ(has_url_encoding(value, mode_type::percent), byte == '%');
            EXPECT_EQ(has_url_encoding(value, mode_type::form), byte == '%' || byte == '+');
        }
        value[position] = 'a';
    }
}

TEST(Ruvia, has_url_encoding_respects_component_bounds) {
    const std::string storage = "%+" + std::string(129, 'a') + "+%";
    const auto value = std::string_view(storage).substr(2, 129);
    for (const auto mode : {mode_type::percent, mode_type::form}) {
        EXPECT_TRUE(!has_url_encoding(value, mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(0, 0), mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(value.size()), mode));
    }
    const std::string_view binary("a\0b%00c", 7);
    EXPECT_TRUE(has_url_encoding(binary, mode_type::percent));
    EXPECT_TRUE(has_url_encoding(binary, mode_type::form));
}

TEST(Ruvia, no_encoding_means_decode_borrows_the_input) {
    // Callers size their decoded-storage reservations with has_url_encoding:
    // a component it reports unencoded is returned as the input itself.
    std::string storage;
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
             std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        for (const auto mode : {mode_type::percent, mode_type::form}) {
            EXPECT_TRUE(!has_url_encoding(value, mode));
            const auto decoded = decode_url_component(value, storage, mode);
            EXPECT_TRUE(decoded.has_value() && decoded->data() == value.data() && decoded->size() == value.size());
        }
    }
    EXPECT_TRUE(storage.empty());
}

TEST(Ruvia, hex_nibbles_decode_and_encode_ascii_digits) {
    EXPECT_EQ(ruvia::http_decode_hex_nibble('0'), 0);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('a'), 10);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('F'), 15);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('g'), -1);
    EXPECT_EQ(ruvia::http_lower_hex_digit(15), 'f');
    EXPECT_EQ(ruvia::http_upper_hex_digit(15), 'F');
}

TEST(Ruvia, hex_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text =
        "ab\x01\xff"
        "cdef";
    constexpr std::size_t input_start = 16;
    for (std::size_t size = 0; size <= text.size(); ++size) {
        const auto input = text.substr(0, size);
        std::string expected(2 * size, '\0');
        ruvia::http_encode_lower_hex(expected.data(), input);
        // Every output start from wholly before to wholly after the input,
        // including in place and starts inside it.
        for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
            std::array<char, 48> buffer{};
            input.copy(buffer.data() + input_start, size);
            const auto* const end =
                ruvia::http_encode_lower_hex(buffer.data() + output_start, std::string_view(buffer.data() + input_start, size));
            EXPECT_EQ(end, buffer.data() + output_start + expected.size());
            EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
        }
    }
}

TEST(Ruvia, hex_append_accepts_input_aliasing_the_output) {
    std::string whole("\x01\xab");
    whole.shrink_to_fit();
    ruvia::http_append_lower_hex(whole, std::string_view(whole));
    EXPECT_EQ(std::string_view(whole), std::string_view("\x01\xab"
                                                        "01ab"));

    // A long prefix forces reallocation while the source is a strict subrange.
    std::string part(std::string(64, 'z') + "\xff");
    part.shrink_to_fit();
    const std::span<const unsigned char> tail(reinterpret_cast<const unsigned char*>(part.data()) + 63, 2);
    ruvia::http_append_lower_hex(part, tail);
    EXPECT_EQ(std::string_view(part).substr(65), std::string_view("7aff"));

    // The terminating NUL is readable input: it ends where the digits begin.
    std::string terminated("A");
    terminated.shrink_to_fit();
    ruvia::http_append_lower_hex(terminated, std::string_view(terminated.data(), terminated.size() + 1));
    EXPECT_EQ(std::string_view(terminated), std::string_view("A4100"));

    // A full string reallocates while the NUL-only input borrows the old buffer.
    std::string nul_only("A");
    nul_only.resize(nul_only.capacity(), 'B');
    const auto filled = nul_only.size();
    ruvia::http_append_lower_hex(nul_only, std::string_view(nul_only.data() + filled, 1));
    EXPECT_EQ(nul_only.size(), filled + 2);
    EXPECT_EQ(std::string_view(nul_only).substr(filled - 1), std::string_view("B00"));
}

namespace {

std::string encode(std::string_view input, ruvia::http_base64_padding padding = ruvia::http_base64_padding::required) {
    std::string output(ruvia::http_base64_encoded_size(input.size(), padding), '\0');
    ruvia::http_base64_encode(output.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        ruvia::http_base64_charset::standard, padding);
    return output;
}

}  // namespace

TEST(Ruvia, http_base64_encodes_with_standard_alphabet_and_padding) {
    EXPECT_EQ(encode(""), std::string(""));
    EXPECT_EQ(encode("f"), std::string("Zg=="));
    EXPECT_EQ(encode("fo"), std::string("Zm8="));
    EXPECT_EQ(encode("foo"), std::string("Zm9v"));
    EXPECT_EQ(encode("foobar"), std::string("Zm9vYmFy"));
    EXPECT_EQ(encode("\xfb\xff"), std::string("+/8="));
}

TEST(Ruvia, http_base64_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text = "abcdefghij";
    constexpr std::size_t input_start = 16;
    for (const auto padding : {ruvia::http_base64_padding::required, ruvia::http_base64_padding::forbidden}) {
        for (std::size_t size = 0; size <= text.size(); ++size) {
            const auto input = text.substr(0, size);
            const auto expected = encode(input, padding);
            // Every output start from wholly before to wholly after the input,
            // including in place and starts inside it.
            for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
                std::array<char, 48> buffer{};
                input.copy(buffer.data() + input_start, size);
                ruvia::http_base64_encode(buffer.data() + output_start,
                    std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()) + input_start, size),
                    ruvia::http_base64_charset::standard, padding);
                EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
            }
        }
    }
}

TEST(Ruvia, http_base64_encoded_size_preserves_padding_at_numeric_boundaries) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto maximum_groups = maximum / 4;
    constexpr auto maximum_input = maximum_groups * 3;
    constexpr auto maximum_output = maximum_groups * 4;
    constexpr auto small_output = ruvia::http_base64_encoded_size(4);
    EXPECT_EQ(small_output, std::size_t{8});
    for (const auto input_size : {maximum_input - 2, maximum_input - 1, maximum_input}) {
        EXPECT_EQ(ruvia::http_base64_encoded_size(input_size), maximum_output);
    }
    for (const auto input_size : {maximum_input + 1, maximum - 1, maximum}) {
        bool rejected = false;
        try {
            (void)ruvia::http_base64_encoded_size(input_size);
        } catch (const std::length_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, http_base64_unpadded_size_uses_actual_tail_at_numeric_boundary) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto groups = maximum / 4;
    constexpr auto forbidden = ruvia::http_base64_padding::forbidden;
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 1, forbidden), maximum - 1);
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 2, forbidden), maximum);
    bool rejected = false;
    try {
        (void)ruvia::http_base64_encoded_size(3 * groups + 3, forbidden);
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, http_base64_decodes_single_characters_per_alphabet) {
    for (std::size_t i = 0; i < ruvia::http_base64_alphabet.size(); ++i) {
        EXPECT_EQ(ruvia::http_base64_decode_char(ruvia::http_base64_alphabet[i]), static_cast<int>(i));
        EXPECT_EQ(ruvia::http_base64_url_decode_char(ruvia::http_base64_url_alphabet[i]), static_cast<int>(i));
    }
    EXPECT_EQ(ruvia::http_base64_decode_char('='), -1);
    EXPECT_EQ(ruvia::http_base64_decode_char('-'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('+'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('/'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('='), -1);
}

TEST(Ruvia, http_base64_decode_rejects_output_overwriting_unread_input) {
    constexpr auto standard = ruvia::http_base64_charset::standard;
    constexpr auto required = ruvia::http_base64_padding::required;
    const auto decoded_text = [](const std::uint8_t* bytes, std::size_t size) {
        return std::string(reinterpret_cast<const char*>(bytes), size);
    };

    std::uint8_t shifted[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view shifted_input(reinterpret_cast<const char*>(shifted), 8);
    EXPECT_TRUE(!ruvia::http_base64_decode(shifted_input, std::span(shifted + 4, 6), standard, required));
    EXPECT_EQ(decoded_text(shifted, 8), std::string("QUJDREVG"));

    std::uint8_t in_place[8] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view in_place_input(reinterpret_cast<const char*>(in_place), 8);
    const auto in_place_size = ruvia::http_base64_decode(in_place_input, std::span(in_place), standard, required);
    EXPECT_TRUE(in_place_size && *in_place_size == 6);
    EXPECT_EQ(decoded_text(in_place, 6), std::string("ABCDEF"));

    std::uint8_t after[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view after_input(reinterpret_cast<const char*>(after), 8);
    const auto after_size = ruvia::http_base64_decode(after_input, std::span(after + 8, 8), standard, required);
    EXPECT_TRUE(after_size && *after_size == 6);
    EXPECT_EQ(decoded_text(after + 8, 6), std::string("ABCDEF"));
}

namespace {

using ruvia::http_ascii_equals_ignore_case;

}  // namespace

TEST(Ruvia, http_ascii_equals_ignore_case_folds_only_letters) {
    EXPECT_TRUE(http_ascii_equals_ignore_case("Text/HTML", "text/html"));
    EXPECT_TRUE(http_ascii_equals_ignore_case("", ""));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abcd"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abd"));

    EXPECT_TRUE(!http_ascii_equals_ignore_case("[", "{"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("@", "`"));

    EXPECT_TRUE(http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    EXPECT_TRUE(!http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}

TEST(Ruvia, number_format_decimal_width_and_string_output) {
    EXPECT_EQ(ruvia::http_unsigned_decimal_size(0), std::size_t{1});
    EXPECT_EQ(ruvia::http_unsigned_decimal_size(999), std::size_t{3});
    EXPECT_EQ(ruvia::http_unsigned_decimal_size((std::numeric_limits<std::uint64_t>::max)()), std::size_t{20});

    std::string output;
    ruvia::http_append_formatted_number(output, std::int64_t{-42}, "format failure");
    ruvia::http_append_formatted_number(output, std::uint64_t{123}, "format failure");
    EXPECT_EQ(output, "-42123");
    ruvia::http_append_formatted_finite_number(output, 1.5, "nonfinite", "format failure");
    EXPECT_EQ(output, "-421231.5");
}

TEST(Ruvia, number_format_rejects_nonfinite_without_modifying_output) {
    std::string output("prefix");
    bool rejected = false;
    try {
        ruvia::http_append_formatted_finite_number(output, std::numeric_limits<double>::infinity(),
            "nonfinite", "format failure");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(output, "prefix");
}

TEST(Ruvia, number_format_signed_extremes) {
    std::string output;
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(output, "-9223372036854775808");
    output.clear();
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::max)());
    EXPECT_EQ(output, "9223372036854775807");
}
