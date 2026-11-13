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

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

#include "content_decoding_fixture.h"
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
