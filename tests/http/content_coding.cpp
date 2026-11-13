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

#include <zlib.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

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

namespace {

using ruvia::http_request;
using ruvia::validate_websocket_handshake;
using ruvia::test::parse_http1_request_message;

http_request parse_request(std::string_view raw_request) {
    auto parsed_value = parse_http1_request_message(raw_request);
    return std::move(parsed_value.state_).take_request();
}

[[nodiscard]] auto validate_request(std::string_view raw_request) {
    const auto parsed_value = parse_http1_request_message(raw_request);
    return validate_websocket_handshake(parsed_value.state_.request(), parsed_value.state_.body_plan());
}

[[nodiscard]] bool accepts_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.accepted() != nullptr;
}

[[nodiscard]] bool rejects_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.failure() != nullptr;
}

std::string_view valid_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view post_handshake() {
    return "POST /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view http10_handshake() {
    return "GET /ws HTTP/1.0\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view bad_version_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 8\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view content_length_zero_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}

std::string_view content_length_one_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 1\r\n"
           "\r\n"
           "x";
}

}  // namespace

TEST(Ruvia, ws_h1_handshake_preserves_application_request_headers) {
    constexpr std::string_view raw =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Origin: https://origin.example\r\n"
        "Authorization: Bearer credential\r\n"
        "Cookie: sid=one; theme=dark\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n"
        "X-End-To-End: retained\r\n"
        "\r\n";
    const auto request = parse_request(raw);

    EXPECT_TRUE(request.header("Host") == "example.test");
    EXPECT_TRUE(request.header("Origin") == "https://origin.example");
    EXPECT_TRUE(request.header("Authorization") == "Bearer credential");
    EXPECT_TRUE(request.header("Cookie") == "sid=one; theme=dark");
    EXPECT_TRUE(request.header("Sec-WebSocket-Protocol") == "chat");
    EXPECT_TRUE(request.header("Sec-WebSocket-Extensions") == "permessage-deflate");
    EXPECT_TRUE(request.header("X-End-To-End") == "retained");
    EXPECT_TRUE(accepts_request(raw));
}

TEST(Ruvia, ws_public_subprotocol_set_validates_unique_tokens) {
    ruvia::websocket_subprotocol_set protocols;
    EXPECT_TRUE(protocols.append_list(", chat, superchat,"));
    EXPECT_TRUE(protocols.contains("chat"));
    EXPECT_TRUE(protocols.contains("superchat"));
    EXPECT_TRUE(!protocols.append("chat"));
    EXPECT_TRUE(!protocols.append_list("other, chat"));
    EXPECT_TRUE(!protocols.append("bad token"));

    // A received offer skips repeats instead of failing (RFC 6455 4.2.1).
    ruvia::websocket_subprotocol_set offered;
    EXPECT_TRUE(offered.append_offered_list("chat, chat"));
    EXPECT_TRUE(offered.append_offered_list("superchat, chat"));
    EXPECT_TRUE(offered.contains("chat"));
    EXPECT_TRUE(offered.contains("superchat"));
    EXPECT_TRUE(!offered.append_offered_list("chat, bad token"));
}

TEST(Ruvia, ws_subprotocol_offers_require_http_tokens_and_skip_repeats) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: , chat,, superchat,\r\n")));
    // Subprotocol identifiers are case-sensitive, so these are distinct.
    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, Chat\r\n")));

    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: bad token\r\n")));
    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: \"chat\"\r\n")));
    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: , ,\r\n")));
    // Uniqueness binds the sending client (RFC 6455 section 4.1 item 10); the
    // server's checks (section 4.2.1) have no rule for a repeated offer.
    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n")));
    EXPECT_TRUE(accepts_request(with_protocols(
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Protocol: superchat, chat\r\n")));
    constexpr std::array<std::string_view, 1> supported{"chat"};
    // 请求头借用原始报文，握手完成前必须保持报文存储存活。
    const auto repeated_wire = with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n");
    const auto repeated = parse_request(repeated_wire);
    const auto selected = ruvia::make_websocket_server_handshake(repeated, {.supported_subprotocols_ = supported});
    EXPECT_TRUE(selected.subprotocol() == "chat");

    std::string too_many = "Sec-WebSocket-Protocol: ";
    for (std::size_t i = 0; i <= ruvia::max_websocket_subprotocols; ++i) {
        if (i != 0) {
            too_many.append(", ");
        }
        too_many.append("protocol-");
        too_many.append(std::to_string(i));
    }
    too_many.append("\r\n");
    EXPECT_TRUE(rejects_request(with_protocols(too_many)));
}

TEST(Ruvia, ws_required_subprotocol_rejects_offers_without_a_supported_entry) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };
    constexpr std::array<std::string_view, 2> supported{"chat.v2", "chat.v1"};
    const auto validate = [&](std::string_view raw_request, bool required) {
        const auto parsed_value = parse_http1_request_message(raw_request);
        return validate_websocket_handshake(parsed_value.state_.request(), parsed_value.state_.body_plan(),
            {.supported_subprotocols_ = std::span<const std::string_view>(supported),
                .require_subprotocol_ = required});
    };
    const auto accepts = [&](std::string_view raw_request, bool required) {
        const auto result_value = validate(raw_request, required);
        return result_value.accepted() != nullptr;
    };

    // Not required: an offer sharing nothing is still a valid handshake.
    EXPECT_TRUE(accepts(valid_handshake(), false));
    EXPECT_TRUE(accepts(with_protocols("Sec-WebSocket-Protocol: other\r\n"), false));

    EXPECT_TRUE(accepts(with_protocols("Sec-WebSocket-Protocol: other, chat.v1\r\n"), true));
    for (const std::string_view raw_request :
        {valid_handshake(), std::string_view("GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
                                             "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                             "Sec-WebSocket-Protocol: other, Chat.v1\r\n\r\n")}) {
        const auto result_value = validate(raw_request, true);
        const auto* failure = result_value.failure();
        EXPECT_TRUE(failure != nullptr);
        if (failure != nullptr) {
            EXPECT_TRUE(failure->protocol_error().status() == ruvia::http_status::bad_request);
            ruvia::http_response response;
            failure->apply_required_response_headers(response);
            EXPECT_TRUE(!response.header("Sec-WebSocket-Version").has_value());
        }
    }
}

TEST(Ruvia, ws_extension_offers_must_match_the_rfc6455_abnf) {
    const auto with_extensions = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    EXPECT_TRUE(accepts_request(
        with_extensions("Sec-WebSocket-Extensions: , x-test; flag; value=token,,\r\n")));
    EXPECT_TRUE(accepts_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\"to\\ken\"\r\n")));
    EXPECT_TRUE(accepts_request(with_extensions(
        "Sec-WebSocket-Extensions: x-test\r\n"
        "Sec-WebSocket-Extensions: y-test; value=token\r\n")));

    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: , ,\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: \"x-test\"\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x test\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;; flag\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; =value\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"bad value\"\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"unterminated\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"token\"junk\r\n")));
}

TEST(Ruvia, ws_valid_request_requires_all_conditions) {
    EXPECT_TRUE(accepts_request(valid_handshake()));

    // Every individual requirement is necessary.
    EXPECT_TRUE(rejects_request(post_handshake()));
    EXPECT_TRUE(rejects_request(http10_handshake()));
    // RFC 6455 permits additional HTTP fields, and RFC 9112 framing makes a
    // zero Content-Length an empty request. Its mere presence must not block
    // an otherwise valid protocol switch.
    EXPECT_TRUE(accepts_request(content_length_zero_handshake()));
    // Actual request content still prevents switching protocols because those
    // octets belong to the HTTP message rather than to the websocket stream.
    EXPECT_TRUE(rejects_request(content_length_one_handshake()));

    constexpr std::string_view no_connection_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(no_connection_upgrade));

    constexpr std::string_view duplicate_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(duplicate_key));

    constexpr std::string_view duplicate_version =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(duplicate_version));

    // The Upgrade header must name "websocket", not another protocol token.
    constexpr std::string_view wrong_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: not-websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(wrong_upgrade));

    // Sec-websocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    constexpr std::string_view bad_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n";
    EXPECT_TRUE(rejects_request(bad_key));

    // "...ZR==" decodes to the same 16 bytes as the canonical "...ZQ==",
    // but sets unused base64 padding bits and must therefore be rejected.
    constexpr std::string_view non_canonical_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n\r\n";
    EXPECT_TRUE(rejects_request(non_canonical_key));

    const auto unsupported_version = validate_request(bad_version_handshake());
    EXPECT_TRUE(unsupported_version.failure() != nullptr);
    if (const auto* failure = unsupported_version.failure()) {
        const auto error = failure->protocol_error();
        EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("unsupported WebSocket version"));
        ruvia::http_response response;
        failure->apply_required_response_headers(response);
        EXPECT_EQ(response.header("Sec-WebSocket-Version"), std::string_view("13"));
    }
}

TEST(Ruvia, ws_upgrade_uses_the_shared_recipient_list_semantics) {
    constexpr std::string_view request =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , custom/1, websocket,\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    EXPECT_TRUE(accepts_request(request));
}

TEST(Ruvia, ws_server_handshake_response_serialization_is_http_owned) {
    const auto request = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15\r\n"
        "\r\n");
    std::string supported = "chat";
    const std::array<std::string_view, 1> supported_views{supported};
    const auto handshake =
        ruvia::make_websocket_server_handshake(request, {.supported_subprotocols_ = supported_views});
    supported.front() = 'X';

    std::vector<std::string_view> response_parts;
    handshake.for_each_response_part(
        [&response_parts](std::string_view part) { response_parts.push_back(part); });
    std::string response;
    for (const auto part : response_parts) {
        response.append(part);
    }
    EXPECT_EQ(response, std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                    "Sec-WebSocket-Protocol: chat\r\n"
                                    "Sec-WebSocket-Extensions: permessage-deflate; "
                                    "server_no_context_takeover; client_no_context_takeover; "
                                    "server_max_window_bits=15\r\n"
                                    "\r\n"));
    EXPECT_TRUE(handshake.compression() ==
                (ruvia::websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    EXPECT_EQ(handshake.subprotocol(), "chat");
}

TEST(Ruvia, ws_server_handshake_negotiates_directional_context_takeover_independently) {
    const auto request = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n"
        "\r\n");
    const auto handshake = ruvia::make_websocket_server_handshake(
        request, {.deflate_ = {.enabled_ = true, .context_takeover_ = true}});
    EXPECT_TRUE(handshake.compression().enabled_);
    EXPECT_TRUE(!handshake.compression().server_no_context_takeover_);
    EXPECT_TRUE(handshake.compression().client_no_context_takeover_);
}

TEST(Ruvia, ws_handshake_copies_application_headers_and_preserves_multiple_cookies) {
    const auto request = parse_request(valid_handshake());
    std::string cookie = "sid=0123456789abcdef; HttpOnly";
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", cookie),
        ruvia::http_header_view("Set-Cookie", "theme=dark"),
        ruvia::http_header_view("X-Request-Id", "request-1"),
        ruvia::http_header_view("Alt-Svc", "h3=\":443\"; ma=86400")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    cookie.assign(cookie.size(), 'x');
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    EXPECT_TRUE((response.find("set-cookie: sid=0123456789abcdef; HttpOnly\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("set-cookie: theme=dark\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("x-request-id: request-1\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("alt-svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

TEST(Ruvia, ws_handshake_rejects_application_framing_and_invalid_fields) {
    const auto request = parse_request(valid_handshake());
    for (const auto name : {"Connection", "Upgrade", "Content-Length", "Transfer-Encoding",
             "Sec-WebSocket-Accept", "Sec-WebSocket-Protocol", "TE", "Trailer", "bad name"}) {
        const std::array fields_value{ruvia::http_header_view(name, "value")};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", "sid=a\r\nInjected: value")};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, ws_handshake_bounds_application_header_size) {
    const auto request = parse_request(valid_handshake());
    const std::vector<ruvia::http_header_view> many(80, {"x", "v"});
    (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = many});
    const std::string oversized(ruvia::max_http_header_bytes, 'a');
    const std::array fields_value{ruvia::http_header_view("x", oversized)};
    bool size_rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::length_error&) {
        size_rejected = true;
    }
    EXPECT_TRUE(size_rejected);
}

TEST(Ruvia, ws_handshake_budget_covers_status_line_and_final_empty_line) {
    const auto request = parse_request(valid_handshake());
    const auto serialize = [](const ruvia::websocket_server_handshake& handshake) {
        std::string response;
        handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
        return response;
    };
    const auto base_size = serialize(ruvia::make_websocket_server_handshake(request, {})).size();
    // "x: " + value + CRLF fills the head to exactly max_http_header_bytes.
    const std::string largest(ruvia::max_http_header_bytes - base_size - 5, 'a');
    const std::array fitting{ruvia::http_header_view("x", largest)};
    EXPECT_EQ(serialize(ruvia::make_websocket_server_handshake(request, {.response_headers_ = fitting})).size(), ruvia::max_http_header_bytes);
    const std::string oversized = largest + 'a';
    const std::array too_large{ruvia::http_header_view("x", oversized)};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = too_large});
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, ws_handshake_application_values_are_valid_for_both_http_versions) {
    const auto request = parse_request(valid_handshake());
    for (const auto value : {" leading", "trailing ", "\tleading", "trailing\t"}) {
        const std::array headers{ruvia::http_header_view("X-Test", value)};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    const std::array headers{ruvia::http_header_view("X-Test", "")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    EXPECT_TRUE((response.find("x-test: \r\n") != std::string_view::npos));
}

namespace {

template <typename subprotocols_type>
concept ws_server_handshake_accepts_subprotocols = requires(subprotocols_type&& subprotocols) {
    ruvia::websocket_server_handshake_options{.supported_subprotocols_ = std::forward<subprotocols_type>(subprotocols)};
};

template <typename subprotocols_type>
concept ws_handshake_validation_accepts_subprotocols = requires(subprotocols_type&& subprotocols) {
    ruvia::websocket_handshake_validation_options{.supported_subprotocols_ = std::forward<subprotocols_type>(subprotocols)};
};

static_assert(ws_server_handshake_accepts_subprotocols<const std::vector<std::string_view>&>);
static_assert(!ws_server_handshake_accepts_subprotocols<std::vector<std::string_view>>);
static_assert(!ws_server_handshake_accepts_subprotocols<std::array<std::string_view, 1>>);
static_assert(ws_handshake_validation_accepts_subprotocols<const std::vector<std::string_view>&>);
static_assert(!ws_handshake_validation_accepts_subprotocols<std::vector<std::string_view>>);
static_assert(!ws_handshake_validation_accepts_subprotocols<std::array<std::string_view, 1>>);

template <typename protocol_type>
concept ws_subprotocol_set_accepts_append = requires(ruvia::websocket_subprotocol_set& set, protocol_type&& protocol) {
    set.append(std::forward<protocol_type>(protocol));
};

template <typename list_type>
concept ws_subprotocol_set_accepts_append_list = requires(ruvia::websocket_subprotocol_set& set, list_type&& list) {
    set.append_list(std::forward<list_type>(list));
};

template <typename list_type>
concept ws_subprotocol_set_accepts_append_offered_list =
    requires(ruvia::websocket_subprotocol_set& set, list_type&& list) {
        set.append_offered_list(std::forward<list_type>(list));
    };

static_assert(ws_subprotocol_set_accepts_append<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append<std::string>);
static_assert(ws_subprotocol_set_accepts_append_list<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append_list<std::string>);
static_assert(ws_subprotocol_set_accepts_append_offered_list<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append_offered_list<std::string>);

}  // namespace

namespace {

constexpr std::array<std::uint8_t, 16> nonce{
    't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};

constexpr std::string_view accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// What a client holds across one opening handshake: the encoded key, the
// negotiation and the application headers it offers.
class client_handshake final {
public:
    explicit client_handshake(std::span<const ruvia::http_header_view> headers = {},
        std::span<const std::string_view> protocols = {})
        : negotiation_({.subprotocols_ = protocols}) {
        ruvia::validate_http1_websocket_client_handshake_configuration(headers);
        ruvia::http_base64_encode(key_.data(), nonce);
        for (const auto header : headers) {
            headers_.emplace_back(header.name(), header.value());
        }
    }

    [[nodiscard]] std::variant<ruvia::prepared_http1_client_request,
        ruvia::http1_client_request_buffer_too_small, ruvia::http1_client_request_prepare_error>
    prepare(const ruvia::http_origin_view& origin, std::string_view target, std::span<char> buffer) const {
        return ruvia::prepare_http1_websocket_client_request(origin, target, key_, negotiation_, headers_, buffer);
    }

    // The response parser takes a freshly prepared request's exchange state.
    [[nodiscard]] ruvia::http1_client_response_parser parser() const {
        std::array<char, 2048> buffer{};
        auto prepared = prepare(ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
        if (prepared.index() != 0) {
            throw std::runtime_error("expected prepared handshake request");
        }
        return ruvia::http1_client_response_parser(std::get<0>(std::move(prepared)).take_exchange_state());
    }

    [[nodiscard]] ruvia::http1_parsed_client_response_head parse(std::string_view wire) const {
        auto response_parser = parser();
        auto result_value = response_parser.parse(wire);
        if (result_value.parsed() == nullptr) {
            throw std::runtime_error("expected parsed response");
        }
        return std::move(*result_value.parsed());
    }

    [[nodiscard]] std::variant<ruvia::websocket_client_negotiation_result_view,
        ruvia::websocket_client_handshake_error>
    validate(const ruvia::http1_parsed_client_response_head& response) const {
        return ruvia::validate_http1_websocket_client_response(key_, negotiation_, response);
    }

private:
    std::array<char, 24> key_{};
    ruvia::websocket_client_negotiation negotiation_;
    std::vector<ruvia::http_header> headers_;
};

std::string valid_response(std::string_view extra = {}) {
    std::string wire =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    wire += accept;
    wire += "\r\n";
    wire += extra;
    wire += "\r\n";
    return wire;
}

bool rejects_configuration(std::span<const ruvia::http_header_view> headers = {},
    std::span<const std::string_view> protocols = {}) {
    try {
        const client_handshake handshake(headers, protocols);
        (void)handshake;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, websocket_client_handshake_prepares_rfc_request) {
    const std::array protocols{std::string_view{"chat"}};
    const std::array headers{
        ruvia::http_header_view{"X-Trace", "one"}, ruvia::http_header_view{"User-Agent", "RuviaTest/1"}};
    const client_handshake handshake(headers, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/chat", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    EXPECT_TRUE(wire.find("GET /chat HTTP/1.1\r\n") == 0);
    EXPECT_TRUE(wire.find("Host: example.com\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
                std::string::npos);
    EXPECT_TRUE(wire.find("sec-websocket-protocol: chat\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("User-Agent: RuviaTest/1\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("X-Trace: one\r\n") != std::string::npos);
}

TEST(Ruvia, websocket_client_handshake_matches_subprotocol_case_exactly) {
    const std::array protocols{std::string_view{"chat"}, std::string_view{"Chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: Chat\r\n"));
    const auto result_value = handshake.validate(response);
    EXPECT_TRUE((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        EXPECT_EQ(std::get<0>(result_value).selected_subprotocol_, std::string_view("Chat"));
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_unoffered_or_duplicate_protocol) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    for (const auto response_protocol : {std::string_view{"Chat"}, std::string_view{"chat"}}) {
        const std::string extra = response_protocol == "chat"
                                      ? "Sec-WebSocket-Protocol: chat\r\n"
                                        "Sec-WebSocket-Protocol: chat\r\n"
                                      : "Sec-WebSocket-Protocol: Chat\r\n";
        const auto response = handshake.parse(valid_response(extra));
        const auto result_value = handshake.validate(response);
        EXPECT_TRUE(!(result_value.index() == 0));
        if (!(result_value.index() == 0)) {
            EXPECT_TRUE(std::get<1>(result_value) == ruvia::websocket_client_handshake_error::subprotocol);
        }
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_bad_accept_and_extensions) {
    const client_handshake handshake;
    const auto bad = handshake.parse(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: wrong\r\n\r\n");
    EXPECT_TRUE(bad.plan().protocol_upgrade() != nullptr);
    const auto bad_result = handshake.validate(bad);
    EXPECT_TRUE(!(bad_result.index() == 0));
    if (!(bad_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(bad_result) == ruvia::websocket_client_handshake_error::accept);
    }

    const auto duplicate =
        handshake.parse(valid_response("Sec-WebSocket-Accept: " + std::string(accept) + "\r\n"));
    const auto duplicate_result = handshake.validate(duplicate);
    EXPECT_TRUE(!(duplicate_result.index() == 0));
    if (!(duplicate_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(duplicate_result) == ruvia::websocket_client_handshake_error::accept);
    }

    const auto response =
        handshake.parse(valid_response("Sec-WebSocket-Extensions: permessage-deflate\r\n"));
    const auto extension_result = handshake.validate(response);
    EXPECT_TRUE(!(extension_result.index() == 0));
    if (!(extension_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(extension_result) == ruvia::websocket_client_handshake_error::extensions);
    }
}

TEST(Ruvia, websocket_client_handshake_parser_accepts_informational_before_upgrade) {
    const client_handshake handshake;
    auto parser = handshake.parser();
    auto informational = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(informational.parsed() != nullptr);
    if (informational.parsed() != nullptr) {
        EXPECT_TRUE(informational.parsed()->plan().informational() != nullptr);
    }
    auto final = parser.parse(valid_response());
    EXPECT_TRUE(final.parsed() != nullptr);
    if (final.parsed() != nullptr) {
        const auto result_value = handshake.validate(*final.parsed());
        EXPECT_TRUE((result_value.index() == 0));
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_invalid_configuration_and_reserved_headers) {
    const std::array bad_space{ruvia::http_header_view{"X-Test", "bad value\r\n"}};
    const std::array bad_comma{ruvia::http_header_view{"X-Test", "bad,\r\nvalue"}};
    const std::array reserved{ruvia::http_header_view{"Host", "evil.example"}};
    const std::array transfer_encoding{
        ruvia::http_header_view{"Transfer-Encoding", "chunked"}};
    const std::array bad_user_agent{ruvia::http_header_view{"User-Agent", "bad\r\nagent"}};
    const std::array trailer{ruvia::http_header_view{"Trailer", "X-End"}};
    const std::array expect{ruvia::http_header_view{"Expect", "100-continue"}};
    const std::array pseudo{ruvia::http_header_view{":authority", "evil.example"}};
    const std::array extensions{ruvia::http_header_view{"Sec-WebSocket-Extensions", "permessage-deflate"}};
    const std::array duplicate{std::string_view{"chat"}, std::string_view{"chat"}};
    const std::array empty{std::string_view{""}};
    const std::array bad_token{std::string_view{"bad token"}};
    const std::array comma_token{std::string_view{"chat,superchat"}};
    EXPECT_TRUE(rejects_configuration(bad_space));
    EXPECT_TRUE(rejects_configuration(bad_comma));
    EXPECT_TRUE(rejects_configuration(reserved));
    EXPECT_TRUE(rejects_configuration(transfer_encoding));
    EXPECT_TRUE(rejects_configuration(trailer));
    EXPECT_TRUE(rejects_configuration(expect));
    EXPECT_TRUE(rejects_configuration(pseudo));
    EXPECT_TRUE(rejects_configuration(extensions));
    EXPECT_TRUE(rejects_configuration({}, duplicate));
    EXPECT_TRUE(rejects_configuration({}, empty));
    EXPECT_TRUE(rejects_configuration({}, bad_token));
    EXPECT_TRUE(rejects_configuration({}, comma_token));
    EXPECT_TRUE(rejects_configuration(bad_user_agent));
}

TEST(Ruvia, websocket_client_handshake_prepares_ipv6_host) {
    const client_handshake handshake;
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "[2001:db8::1]", .port_ = 8443}), "/before", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    EXPECT_TRUE(wire.find("Host: [2001:db8::1]:8443\r\n") != std::string::npos);
}

TEST(Ruvia, websocket_client_handshake_result_view_is_used_while_response_lives) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: chat\r\n"));
    const auto result_value = handshake.validate(response);
    EXPECT_TRUE((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        const auto selected = std::get<0>(result_value).selected_subprotocol_;
        EXPECT_EQ(selected, std::string_view("chat"));
        EXPECT_EQ(response.head().headers().back().value(), selected);
    }
}

namespace {
void add(std::vector<ruvia::http_header>& head, std::string_view name, std::string_view value) {
    head.emplace_back(name, value);
}

}  // namespace

TEST(Ruvia, websocket_client_negotiation_accepts_asymmetric_context_and_window_parameters) {
    const std::array protocols{std::string_view("chat")};
    ruvia::websocket_client_negotiation client({.subprotocols_ = protocols, .deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = 10, .client_max_window_bits_ = 12}}});
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "chat");
    add(head, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover; server_max_window_bits=10; client_max_window_bits=12");
    auto accepted = client.validate_fields(head);
    EXPECT_TRUE((accepted.index() == 0));
    EXPECT_EQ(std::get<0>(accepted).selected_subprotocol_, std::string_view("chat"));
    EXPECT_TRUE(!std::get<0>(accepted).compression_.server_no_context_takeover_);
    EXPECT_TRUE(std::get<0>(accepted).compression_.client_no_context_takeover_);
    EXPECT_EQ(std::get<0>(accepted).compression_.server_max_window_bits_.value(), 10);
    EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value(), 12);
}
TEST(Ruvia, websocket_client_negotiation_rejects_duplicate_unknown_or_unoffered_selection) {
    ruvia::websocket_client_negotiation client({.deflate_ = {.compression_ = {.enabled_ = true, .server_max_window_bits_ = 10}, .offer_client_max_window_bits_ = false}});
    for (auto extension : std::array{"permessage-deflate", "permessage-deflate; server_no_context_takeover; server_max_window_bits=11", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits=9", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; unknown", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; server_max_window_bits=10"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        EXPECT_TRUE(!(client.validate_fields(head).index() == 0));
    }
    // A server may add its own window bound without an offer.
    ruvia::websocket_client_negotiation no_server_window_client({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        EXPECT_TRUE((no_server_window_client.validate_fields(head).index() == 0));
    }
    // The server may select a larger client window than the offered hint, or
    // none, but the client still never compresses with more than it offered.
    ruvia::websocket_client_negotiation limited_client_window({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .client_max_window_bits_ = 9}}});
    for (auto extension : std::array{"permessage-deflate; client_max_window_bits=10", "permessage-deflate"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        auto accepted = limited_client_window.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        if (accepted.index() == 0) {
            EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 9);
        }
    }
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits=8");
        auto accepted = limited_client_window.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        if (accepted.index() == 0) {
            EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 8);
        }
    }
    // A client takeover preference in an offer is a hint.
    ruvia::websocket_client_negotiation require_client_no_takeover({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = true}}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate");
        auto accepted = require_client_no_takeover.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        EXPECT_TRUE(std::get<0>(accepted).compression_.client_no_context_takeover_);
    }
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "unoffered");
    EXPECT_TRUE(!(client.validate_fields(head).index() == 0));
}
TEST(Ruvia, websocket_h1_client_handshake_negotiates_deflate) {
    constexpr std::array<std::uint8_t, 16> nonce{'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    std::array<char, 24> encoded_key{};
    ruvia::http_base64_encode(encoded_key.data(), nonce);
    const ruvia::websocket_client_negotiation negotiation({.deflate_ = {.compression_ = {.enabled_ = true}}});
    std::array<char, 2048> buffer{};
    auto prepared = ruvia::prepare_http1_websocket_client_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", encoded_key, negotiation, std::span<const ruvia::http_header>{}, buffer);
    auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    EXPECT_TRUE((prepared_req->head().find("sec-websocket-extensions: permessage-deflate") != std::string_view::npos));
    ruvia::http1_client_response_parser parser(std::move(*prepared_req).take_exchange_state());
    auto response = parser.parse("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n\r\n");
    EXPECT_TRUE(response.parsed() != nullptr);
    auto accepted = ruvia::validate_http1_websocket_client_response(encoded_key, negotiation, *response.parsed());
    EXPECT_TRUE((accepted.index() == 0));
    EXPECT_TRUE(std::get<0>(accepted).compression_.enabled_);
}

TEST(Ruvia, websocket_client_negotiation_supports_multiline_and_comma_extensions_while_rejecting_duplicates) {
    ruvia::websocket_client_negotiation client(
        {.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .server_max_window_bits_ = 12}}});

    // Multi-line Sec-WebSocket-Extensions with empty/comma lines per RFC 6455 §9.1 / RFC 9110 §5.3.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", ",");
        add(head, "sec-websocket-extensions", " , permessage-deflate; server_max_window_bits=10 , ");
        auto accepted = client.validate_fields(head);
        ASSERT_EQ(accepted.index(), 0U);
        EXPECT_TRUE(std::get<0>(accepted).compression_.enabled_);
        EXPECT_EQ(std::get<0>(accepted).compression_.server_max_window_bits_, std::optional<int>(10));
    }

    // Duplicate permessage-deflate across multiple header lines is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Duplicate permessage-deflate in a single comma-separated line is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions",
            "permessage-deflate; server_max_window_bits=10, permessage-deflate; server_max_window_bits=10");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Unknown extension on a second header line or comma-separated item is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        add(head, "sec-websocket-extensions", "x-unknown-ext");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10, x-unknown-ext");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Present but empty Sec-WebSocket-Extensions violates 1#extension.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", " , ");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }
}

namespace {

template <typename fields_type>
concept ws_client_negotiation_accepts_fields =
    requires(const ruvia::websocket_client_negotiation& negotiation, fields_type&& fields) {
        negotiation.validate_fields(std::forward<fields_type>(fields));
    };

static_assert(ws_client_negotiation_accepts_fields<const std::vector<ruvia::http_header>&>);
static_assert(!ws_client_negotiation_accepts_fields<std::vector<ruvia::http_header>>);
static_assert(!ws_client_negotiation_accepts_fields<std::array<ruvia::http_header, 1>>);

}  // namespace

namespace {
using namespace ruvia;

bool zero_mask(void*, websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

struct client_endpoint final {
    explicit client_endpoint(websocket_compression compression)
        : protocol_(input_, {.message_limit_ = protocol_byte_limit::unlimited(),
                                .compression_ = compression,
                                .role_ = websocket_connection_role::client,
                                .mask_key_generator_ = &zero_mask}) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

// Takes every pending output byte of a sender.
template <typename sender_type>
[[nodiscard]] std::string drain(sender_type& sender) {
    const auto plan = sender.output_plan();
    std::string bytes_value(plan.bytes());
    bytes_value.append(plan.borrowed_bytes());
    EXPECT_TRUE(sender.consume_output(bytes_value.size()) == websocket_output_consume_status::drained);
    return bytes_value;
}

struct server_endpoint final {
    explicit server_endpoint(websocket_connection_options options)
        : input_(),
          protocol_(input_, [&] {
              options.message_limit_ = protocol_byte_limit::unlimited();
              return options;
          }()) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

}  // namespace

// A direction without context takeover produces the same wire bytes whether
// it borrows the shared codecs or owns a per-connection codec, and with the
// shared codecs the connection's resource never holds codec state.

namespace {
using namespace ruvia;

}  // namespace

namespace {
using namespace ruvia;

struct mask_source {
    unsigned calls_{0};
    bool fail_{false};
    static bool generate(void* context_value, websocket_mask_key_type& key) noexcept {
        auto& self = *static_cast<mask_source*>(context_value);
        ++self.calls_;
        key = {static_cast<char>(self.calls_), '\x23', '\x45', '\x67'};
        return !self.fail_;
    }
};

websocket_connection client(http_io_buffer& input, mask_source& source_value, protocol_byte_limit limit = protocol_byte_limit::unlimited()) {
    return websocket_connection(input, {.message_limit_ = limit, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source_value});
}

std::string drain(websocket_connection& connection) {
    const std::string bytes_value(connection.output_plan().bytes());
    if (connection.consume_output(bytes_value.size()) != websocket_output_consume_status::drained) {
        throw std::runtime_error("output did not drain");
    }
    return bytes_value;
}
}  // namespace

TEST(Ruvia, ws_public_close_code_rejects_wide_and_reserved_values_before_narrowing) {
    const auto rejects = [](auto input) {
        try {
            (void)websocket_close_code::from_value(input);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 14> invalid_values{
        std::numeric_limits<std::int64_t>::min(), -64536, -1, 0, 999,
        1004, 1005, 1006, 1015, 1016, 2999, 5000, 66536,
        std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid_values) {
        EXPECT_TRUE(!websocket_close_code::try_from_value(input).has_value());
        EXPECT_TRUE(rejects(input));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(!websocket_close_code::try_from_value(maximum).has_value());
    EXPECT_TRUE(rejects(maximum));
}

TEST(Ruvia, ws_public_checked_close_codes_preserve_wire_value_and_role_rules) {
    for (const std::uint64_t input : {1000u, 1003u, 1007u, 1014u, 3000u, 3999u, 4000u, 4999u, 1010u}) {
        mask_source source;
        http_io_buffer client_input;
        auto sender = client(client_input, source);
        http_io_buffer server_input;
        websocket_connection receiver(server_input);
        const auto code = websocket_close_code::from_value(input);
        EXPECT_TRUE(sender.submit_close(code, "checked") == websocket_close_submit_status::accepted);
        server_input.append(drain(sender));
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->close() && event->close()->close_code() == input);
        EXPECT_TRUE(event && event->close() && event->close()->reason() == "checked");
    }

    http_io_buffer server_input;
    websocket_connection sender(server_input);
    EXPECT_TRUE(sender.submit_close(websocket_close_code::from_value(1010), {}) == websocket_close_submit_status::invalid_code);
    EXPECT_TRUE(sender.submit_frame(websocket_opcode::text, "still-open") == websocket_frame_submit_status::accepted);
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, std::string_view("\xff", 1)) == websocket_close_submit_status::invalid_reason);
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, std::string(124, 'x')) == websocket_close_submit_status::reason_too_large);
    const std::string reason(123, 'x');
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, reason) == websocket_close_submit_status::accepted);
    mask_source source;
    http_io_buffer client_input;
    auto receiver = client(client_input, source);
    client_input.append(drain(sender));
    const auto message = receiver.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "still-open");
    const auto closed = receiver.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    EXPECT_TRUE(closed && closed->close() && closed->close()->reason() == reason);
}

TEST(Ruvia, ws_public_compression_accepts_final_blocks_and_preserves_required_history) {
    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        // RFC 7692 section 7.2.3.4: BFINAL=1, padding, then an empty block.
        input.append(std::string_view("\xc1\x08\xf3\x48\xcd\xc9\xc9\x07\x00\x00", 10));
        auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "Hello");
        // Two terminated streams within one message share their LZ77 history
        // even when context takeover between messages is disabled.
        input.append(std::string_view("\xc1\x0c\xf3\x48\xcd\xc9\xc9\x07\x00\xf3\x00\x11\x00\x00", 14));
        event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "HelloHello");
        input.append(std::string_view("\xc1\x01\x00", 3));
        event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload().empty());
        // This message refers back to the preceding message's dictionary.
        input.append(std::string_view("\xc1\x05\xf2\x00\x11\x00\x00", 7));
        event = receiver.next_event();
        if (no_takeover) {
            EXPECT_TRUE(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        } else {
            EXPECT_TRUE(event && event->message() && event->message()->payload() == "Hello");
        }
    }
}

TEST(Ruvia, ws_public_compression_rejects_incomplete_empty_block_headers) {
    for (const auto wire : {std::string_view("\xc1\x00", 2), std::string_view("\xc1\x02\x03\x00", 4)}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        input.append(wire);
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        EXPECT_TRUE(receiver.submit_frame(websocket_opcode::text, "after-error") == websocket_frame_submit_status::not_open);
    }
}

TEST(Ruvia, ws_public_server_events_use_public_payload_types) {
    mask_source mask;
    http_io_buffer sender_input;
    auto sender = client(sender_input, mask);
    http_io_buffer input;
    websocket_connection protocol(input);

    EXPECT_TRUE(sender.submit_frame(websocket_opcode::text, "hey") ==
                websocket_frame_submit_status::accepted);
    const std::string message_wire(sender.output_plan().bytes());
    EXPECT_TRUE(sender.consume_output(message_wire.size()) == websocket_output_consume_status::drained);
    input.append(message_wire);
    auto message = protocol.next_event();
    EXPECT_TRUE(message && message->kind() == websocket_event_kind::message);
    EXPECT_TRUE(message && message->message());
    if (message && message->message()) {
        EXPECT_TRUE(message->message()->opcode() == websocket_opcode::text);
        EXPECT_EQ(message->message()->payload(), "hey");
    }

    EXPECT_TRUE(sender.submit_frame(websocket_opcode::ping, "p") ==
                websocket_frame_submit_status::accepted);
    const std::string ping_wire(sender.output_plan().bytes());
    EXPECT_TRUE(sender.consume_output(ping_wire.size()) == websocket_output_consume_status::drained);
    input.append(ping_wire);
    auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->kind() == websocket_event_kind::ping);
    EXPECT_TRUE(ping && ping->ping());
    if (ping && ping->ping()) {
        EXPECT_EQ(ping->ping()->payload(), "p");
    }
}

TEST(Ruvia, ws_public_server_protocol_reports_protocol_error_as_public_value) {
    http_io_buffer input;
    websocket_connection protocol(input);
    // A server-side connection must reject an unmasked client frame.
    input.append(std::string_view("\x81\x01x", 3));
    auto error = protocol.next_event();
    EXPECT_TRUE(error && error->kind() == websocket_event_kind::protocol_error);
    EXPECT_TRUE(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
}

TEST(Ruvia, ws_public_server_protocol_controls_permessage_deflate_per_frame) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited(),
                                             .compression_ = websocket_compression{.enabled_ = true},
                                             .deflater_ = {.compression_level_ = 6}});
    const std::string payload_value(200, 'x');

    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, payload_value, {.compress_ = false}) ==
                websocket_frame_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    if (!output.empty()) {
        EXPECT_TRUE((static_cast<unsigned char>(output[0]) & 0x40U) == 0);
    }
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, payload_value) ==
                websocket_frame_submit_status::accepted);
    output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    if (!output.empty()) {
        EXPECT_TRUE((static_cast<unsigned char>(output[0]) & 0x40U) != 0);
    }
}

TEST(Ruvia, ws_public_server_protocol_preserves_transport_end_semantics) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    const auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);
    // After our Close frame is sent, the protocol still awaits the peer's
    // Close; transport EOF terminates that wait without fabricating a reply.
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    protocol.notify_transport_eof();
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::end_transport);
}

TEST(Ruvia, ws_public_server_eof_discards_pending_output_before_transport_end) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, "unsent") ==
                websocket_frame_submit_status::accepted);
    const auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(output.size() > 1);
    EXPECT_TRUE(protocol.consume_output(1) == websocket_output_consume_status::pending);

    protocol.notify_transport_eof();
    const auto end = protocol.output_plan();
    EXPECT_TRUE(end.bytes().empty());
    EXPECT_TRUE(end.disposition() == websocket_transport_disposition::end_transport);
    // The driver can finish its direction without consuming bytes discarded by EOF.
    EXPECT_TRUE(protocol.consume_output(end.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    EXPECT_TRUE(protocol.abort() == websocket_abort_disposition::no_transport_action);
}

TEST(Ruvia, ws_public_server_keeps_pending_pong_before_peer_close) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_close(websocket_close_status::normal_closure, "done") ==
                websocket_close_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    // Masked client Ping("p") followed by an empty masked Close.
    input.append(std::string_view(
        "\x89\x81\x01\x02\x03\x04q"
        "\x88\x80\x05\x06\x07\x08",
        13));
    const auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->kind() == websocket_event_kind::ping);
    EXPECT_TRUE(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto close = protocol.next_event();
    EXPECT_TRUE(close && close->kind() == websocket_event_kind::close);

    const auto plan = protocol.output_plan();
    EXPECT_EQ(plan.bytes(), std::string_view("\x8a\x01p", 3));
    EXPECT_TRUE(plan.disposition() ==
                websocket_transport_disposition::end_transport);
    EXPECT_TRUE(protocol.consume_output(plan.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    EXPECT_TRUE(protocol.liveness_mode() == websocket_liveness_mode::inactive);
}

TEST(Ruvia, ws_public_client_server_exchange_and_partial_output) {
    mask_source source;
    http_io_buffer sender_input;
    auto sender = client(sender_input, source);
    http_io_buffer receiver_input;
    websocket_connection receiver(receiver_input);
    const std::string payload_value(65536, '\xff');
    for (unsigned round = 0; round < 3; ++round) {
        EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
        const std::string wire(sender.output_plan().bytes());
        EXPECT_TRUE((static_cast<unsigned char>(wire[1]) & 0x80U) != 0);
        EXPECT_TRUE(sender.consume_output(wire.size() + 1) == websocket_output_consume_status::out_of_range);
        EXPECT_TRUE(sender.consume_output(1) == websocket_output_consume_status::pending);
        EXPECT_EQ(sender.output_plan().bytes(), std::string_view(wire).substr(1));
        EXPECT_TRUE(sender.consume_output(wire.size() - 1) == websocket_output_consume_status::drained);
        receiver_input.append(std::string_view(wire).substr(0, 5));
        EXPECT_TRUE(!receiver.next_event());
        receiver_input.append(std::string_view(wire).substr(5));
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        EXPECT_EQ(event->message()->payload(), payload_value);
        EXPECT_TRUE(receiver.submit_frame(websocket_opcode::binary, event->message()->payload()) == websocket_frame_submit_status::accepted);
        const auto reply = drain(receiver);
        EXPECT_TRUE((static_cast<unsigned char>(reply[1]) & 0x80U) == 0);
        sender_input.append(reply);
        const auto echo = sender.next_event();
        EXPECT_TRUE(echo && echo->message());
        if (!echo || !echo->message()) {
            return;
        }
        EXPECT_EQ(echo->message()->payload(), payload_value);
    }
    EXPECT_EQ(source.calls_, 3U);
}

TEST(Ruvia, ws_public_client_fragmented_message_with_ping_and_close) {
    mask_source source;
    http_io_buffer input;
    auto connection = client(input, source);
    http_io_buffer server_input;
    websocket_connection server(server_input);
    // A fragmented binary message with a control frame between fragments.
    const std::string wire(
        "\x02\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x80\x02"
        "cd",
        11);
    input.append(wire);
    const auto ping = connection.next_event();
    EXPECT_TRUE(ping && ping->ping());
    const auto pong = drain(connection);
    server_input.append(pong);
    const auto pong_event = server.next_event();
    EXPECT_TRUE(pong_event && pong_event->pong() && pong_event->pong()->payload() == "p");
    const auto message = connection.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "abcd");
    EXPECT_TRUE(server.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    const auto close = drain(server);
    input.append(close);
    const auto closed = connection.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    // RFC 6455 section 7.1.1: the server closes the transport first, so the
    // client flushes its echo and keeps the transport open until server EOF.
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::keep_open);
    const auto response = drain(connection);
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::keep_open);
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof);
    server_input.append(response);
    const auto acknowledged = server.next_event();
    EXPECT_TRUE(acknowledged && acknowledged->close());
    // Bytes after the close handshake are discarded while waiting.
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::accepted);
    input.append("x");
    EXPECT_TRUE(!connection.next_event());
    EXPECT_TRUE(connection.submit_frame(websocket_opcode::text, "late") == websocket_frame_submit_status::not_open);
    connection.notify_transport_eof();
    const auto end = connection.next_event();
    EXPECT_TRUE(end && end->transport_end());
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    connection.commit_transport_end();
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::inactive);
    EXPECT_EQ(source.calls_, 2U);
    EXPECT_TRUE(pong.substr(2, 4) != response.substr(2, 4));
}

TEST(Ruvia, ws_public_client_waits_for_server_eof_after_initiating_close) {
    mask_source source;
    http_io_buffer input;
    websocket_connection connection(input, {.role_ = websocket_connection_role::client,
                                               .mask_key_generator_ = &mask_source::generate,
                                               .mask_key_context_ = &source});
    http_io_buffer server_input;
    websocket_connection server(server_input);
    EXPECT_TRUE(connection.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    const auto request_bytes = drain(connection);
    server_input.append(request_bytes);
    const auto request = server.next_event();
    EXPECT_TRUE(request && request->close());
    const auto reply = drain(server);
    EXPECT_TRUE(server.output_plan().disposition() == websocket_transport_disposition::end_transport);

    // The server's Close reply, then bytes the client must ignore.
    input.append(reply);
    input.append(std::string_view("\x81\x01x", 3));
    const auto closed = connection.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    const auto waiting = connection.output_plan();
    EXPECT_TRUE(waiting.bytes().empty());
    EXPECT_TRUE(waiting.disposition() == websocket_transport_disposition::keep_open);
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof);
    EXPECT_TRUE(!connection.next_event());
    EXPECT_EQ(input.size(), std::size_t{0});
    connection.notify_transport_eof();
    const auto end = connection.next_event();
    EXPECT_TRUE(end && end->transport_end());
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    connection.commit_transport_end();
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::inactive);
}

// A borrowed payload is a distinct parameter form: it never binds an owning
// temporary, and a plain string never borrows implicitly.
static_assert(!std::is_constructible_v<websocket_borrowed_payload, std::string>);
static_assert(!std::is_constructible_v<websocket_borrowed_payload, std::string>);
static_assert(std::is_constructible_v<websocket_borrowed_payload, const std::string&>);
static_assert(!std::is_convertible_v<std::string_view, websocket_borrowed_payload>);

// RFC 6455 section 5.4: one entry sends whole messages and fragments; a frame
// is the final fragment of its message.
TEST(Ruvia, ws_public_submit_frame_sends_fragments_and_whole_messages) {
    http_io_buffer input;
    websocket_connection sender(input);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, "ab", {.final_ = false}), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, "x"), websocket_frame_submit_status::message_in_progress);
    // Control frames are never fragmented (section 5.5) and may interleave.
    EXPECT_EQ(sender.submit_frame(websocket_opcode::ping, "p", {.final_ = false}), websocket_frame_submit_status::invalid_opcode);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::ping, "p"), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, "\xC3", {.final_ = false}), websocket_frame_submit_status::accepted);
    const std::string tail(
        "\xA9"
        "cd");
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, websocket_borrowed_payload(tail)), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.output_plan().borrowed_bytes(), tail);
    // The next frame copies the borrowed payload ahead of itself.
    EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, "z"), websocket_frame_submit_status::accepted);
    EXPECT_TRUE(sender.output_plan().borrowed_bytes().empty());
    const std::string_view expected(
        "\x01\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x00\x01"
        "\xC3"
        "\x80\x03"
        "\xA9"
        "cd"
        "\x82\x01"
        "z",
        18);
    const auto wire = drain(sender);
    EXPECT_EQ(std::string_view(wire), expected);

    mask_source source;
    http_io_buffer client_input;
    auto receiver = client(client_input, source);
    client_input.append(wire);
    const auto ping = receiver.next_event();
    EXPECT_TRUE(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto text = receiver.next_event();
    EXPECT_TRUE(text && text->message() && text->message()->payload() ==
                                               "ab\xC3\xA9"
                                               "cd");
    const auto binary = receiver.next_event();
    EXPECT_TRUE(binary && binary->message() && binary->message()->payload() == "z");
}

TEST(Ruvia, ws_public_client_rejects_masked_server_and_oversized_message) {
    mask_source source;
    http_io_buffer invalid_server_input;
    auto invalid_server = client(invalid_server_input, source);
    EXPECT_TRUE(invalid_server.submit_frame(websocket_opcode::text, "bad") == websocket_frame_submit_status::accepted);
    const auto masked = drain(invalid_server);
    http_io_buffer receiver_input;
    auto receiver = client(receiver_input, source);
    receiver_input.append(masked);
    const auto error = receiver.next_event();
    EXPECT_TRUE(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
    http_io_buffer limited_input;
    auto limited = client(limited_input, source, protocol_byte_limit::limited(2));
    limited_input.append(std::string_view(
        "\x82\x03"
        "abc",
        5));
    const auto large = limited.next_event();
    EXPECT_TRUE(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    EXPECT_TRUE(limited.submit_frame(websocket_opcode::binary, "x") == websocket_frame_submit_status::not_open);
}

TEST(Ruvia, ws_public_client_requires_entropy_and_handles_transport_end) {
    EXPECT_TRUE(ruvia::test::throws_on([] {
        http_io_buffer input;
        websocket_connection connection(input, {.role_ = websocket_connection_role::client});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([] {
        http_io_buffer input;
        websocket_connection connection(input, {.role_ = static_cast<websocket_connection_role>(255)});
    }));
    mask_source source;
    http_io_buffer input;
    auto connection = client(input, source);
    source.fail_ = true;
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)connection.submit_frame(websocket_opcode::binary, "x");
    }));
    EXPECT_TRUE(connection.output_plan().bytes().empty());
    EXPECT_TRUE(connection.abort() == websocket_abort_disposition::abort_transport);
    EXPECT_TRUE(connection.abort() == websocket_abort_disposition::no_transport_action);
    http_io_buffer eof_input;
    auto eof = client(eof_input, source);
    eof.notify_transport_eof();
    const auto event = eof.next_event();
    EXPECT_TRUE(event && event->transport_end());
    eof.commit_transport_end();
    EXPECT_TRUE(eof.liveness_mode() == websocket_liveness_mode::inactive);
}

TEST(Ruvia, ws_public_wire_bytes_count_whole_frames_and_close_roles_fix_termination) {
    mask_source source;
    http_io_buffer client_input;
    auto client_side = client(client_input, source);
    http_io_buffer server_input;
    websocket_connection server(server_input);
    EXPECT_TRUE(client_side.termination() == websocket_termination::none);
    EXPECT_TRUE(server.termination() == websocket_termination::none);

    // Masked text "hello": 2-byte header, 4-byte mask key and 5 payload bytes.
    EXPECT_TRUE(client_side.submit_frame(websocket_opcode::text, "hello") == websocket_frame_submit_status::accepted);
    EXPECT_EQ(client_side.bytes_sent(), 0U);
    const auto text = drain(client_side);
    EXPECT_EQ(text.size(), 11U);
    EXPECT_EQ(client_side.bytes_sent(), 11U);
    // A frame counts once it is parsed whole.
    server_input.append(std::string_view(text).substr(0, 4));
    EXPECT_TRUE(!server.next_event());
    EXPECT_EQ(server.bytes_received(), 0U);
    server_input.append(std::string_view(text).substr(4));
    const auto message = server.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "hello");
    EXPECT_EQ(server.bytes_received(), 11U);

    // Ping "abc" (5 bytes) and the client's automatic masked Pong (9 bytes).
    EXPECT_TRUE(server.submit_frame(websocket_opcode::ping, "abc") == websocket_frame_submit_status::accepted);
    client_input.append(drain(server));
    const auto ping = client_side.next_event();
    EXPECT_TRUE(ping && ping->ping());
    server_input.append(drain(client_side));
    const auto pong = server.next_event();
    EXPECT_TRUE(pong && pong->pong() && pong->pong()->payload() == "abc");

    // Client-initiated Close 1000 (8 bytes), then the server echo (4 bytes).
    EXPECT_TRUE(client_side.submit_close(websocket_close_status::normal_closure, {}) == websocket_close_submit_status::accepted);
    server_input.append(drain(client_side));
    const auto close = server.next_event();
    EXPECT_TRUE(close && close->close() && close->close()->close_code() == 1000);
    EXPECT_TRUE(server.termination() == websocket_termination::peer_close);
    EXPECT_TRUE(client_side.termination() == websocket_termination::none);
    client_input.append(drain(server));
    const auto echoed = client_side.next_event();
    EXPECT_TRUE(echoed && echoed->close());
    EXPECT_TRUE(client_side.termination() == websocket_termination::local_close);

    EXPECT_EQ(client_side.bytes_sent(), 11U + 9U + 8U);
    EXPECT_EQ(server.bytes_received(), client_side.bytes_sent());
    EXPECT_EQ(server.bytes_sent(), 5U + 4U);
    EXPECT_EQ(client_side.bytes_received(), server.bytes_sent());

    // The first cause survives the transport end and any later abort.
    server.commit_transport_end();
    EXPECT_TRUE(server.abort(websocket_termination::io_error) == websocket_abort_disposition::no_transport_action);
    EXPECT_TRUE(server.termination() == websocket_termination::peer_close);
    client_side.notify_transport_eof();
    EXPECT_TRUE(client_side.abort(websocket_termination::timeout) == websocket_abort_disposition::abort_transport);
    EXPECT_TRUE(client_side.termination() == websocket_termination::local_close);
}

TEST(Ruvia, ws_public_termination_reports_the_first_failure_cause) {
    // An unmasked client frame fails as 1002 and is not counted as received.
    http_io_buffer unmasked_input;
    websocket_connection unmasked(unmasked_input);
    unmasked_input.append(std::string_view("\x81\x01x", 3));
    const auto unmasked_error = unmasked.next_event();
    EXPECT_TRUE(unmasked_error && unmasked_error->protocol_error());
    EXPECT_TRUE(unmasked.termination() == websocket_termination::protocol_error);
    EXPECT_EQ(unmasked.bytes_received(), 0U);
    unmasked.notify_transport_eof();
    EXPECT_TRUE(unmasked.termination() == websocket_termination::protocol_error);

    mask_source source;
    http_io_buffer utf8_input;
    auto utf8 = client(utf8_input, source);
    utf8_input.append(std::string_view("\x81\x01\xff", 3));
    const auto utf8_error = utf8.next_event();
    EXPECT_TRUE(utf8_error && utf8_error->protocol_error() && utf8_error->protocol_error()->close_code() == 1007);
    EXPECT_TRUE(utf8.termination() == websocket_termination::invalid_payload_data);

    http_io_buffer limited_input;
    auto limited = client(limited_input, source, protocol_byte_limit::limited(2));
    limited_input.append(std::string_view(
        "\x82\x03"
        "abc",
        5));
    const auto large = limited.next_event();
    EXPECT_TRUE(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    EXPECT_TRUE(limited.termination() == websocket_termination::message_too_large);

    http_io_buffer eof_input;
    auto eof = client(eof_input, source);
    eof.notify_transport_eof();
    EXPECT_TRUE(eof.termination() == websocket_termination::eof);
    (void)eof.abort(websocket_termination::timeout);
    EXPECT_TRUE(eof.termination() == websocket_termination::eof);

    for (const auto cause : {websocket_termination::none, websocket_termination::aborted,
             websocket_termination::timeout, websocket_termination::io_error}) {
        http_io_buffer input;
        websocket_connection connection(input);
        EXPECT_TRUE(connection.abort(cause) == websocket_abort_disposition::abort_transport);
        connection.notify_transport_eof();
        EXPECT_TRUE(connection.termination() == (cause == websocket_termination::none ? websocket_termination::aborted : cause));
    }
}

TEST(Ruvia, ws_public_wire_bytes_count_deflated_payloads) {
    mask_source source;
    const websocket_compression compression{.enabled_ = true};
    http_io_buffer server_input;
    websocket_connection server(server_input, {.compression_ = compression});
    http_io_buffer client_input;
    websocket_connection client_side(client_input, {.compression_ = compression, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source});
    const std::string payload_value(4096, 'x');
    EXPECT_TRUE(server.submit_frame(websocket_opcode::text, payload_value) == websocket_frame_submit_status::accepted);
    const auto wire = drain(server);
    EXPECT_TRUE(wire.size() < payload_value.size());
    EXPECT_EQ(server.bytes_sent(), wire.size());
    client_input.append(wire);
    const auto message = client_side.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == payload_value);
    EXPECT_EQ(client_side.bytes_received(), wire.size());
}

TEST(Ruvia, ws_default_message_limit_rejects_oversized_declared_frame_before_payload) {
    // A masked binary frame declaring 16 MiB + 1 bytes requires no payload to
    // establish that it cannot fit the finite default message policy.
    const std::string header_value("\x82\xff\x00\x00\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00", 14);
    websocket_connection connection;
    EXPECT_TRUE(connection.feed(header_value) == websocket_feed_status::accepted);
    const auto event = connection.next_event();
    EXPECT_TRUE(event && event->protocol_error());
    EXPECT_EQ(event->protocol_error()->close_code(), std::uint16_t{1009});
    http_io_buffer input;
    input.append(header_value);
    websocket_connection protocol(input);
    const auto borrowed_event = protocol.next_event();
    EXPECT_TRUE(borrowed_event && borrowed_event->protocol_error());
    EXPECT_EQ(borrowed_event->protocol_error()->close_code(), std::uint16_t{1009});
}

TEST(Ruvia, ws_owned_input_bound_excludes_parsed_prefix) {
    websocket_connection connection({.max_buffered_input_bytes_ = 32});
    const std::string ping("\x89\x81\x00\x00\x00\x00p", 7);
    std::string binary("\x82\x94\x00\x00\x00\x00", 6);
    binary.append(20, 'b');
    const auto first_chunk = ping + binary.substr(0, 10);
    EXPECT_TRUE(connection.feed(first_chunk) == websocket_feed_status::accepted);
    const auto ping_event = connection.next_event();
    EXPECT_TRUE(ping_event && ping_event->ping());
    (void)connection.consume_output(connection.output_plan().bytes().size());
    EXPECT_TRUE(!connection.next_event());
    // 16 pending + 10 buffered frame bytes fit the 32-byte bound once the
    // already-delivered Ping no longer occupies the buffer.
    EXPECT_TRUE(connection.feed(std::string_view(binary).substr(10)) == websocket_feed_status::accepted);
    const auto message_event = connection.next_event();
    EXPECT_TRUE(message_event && message_event->message());
    if (message_event && message_event->message()) {
        EXPECT_EQ(message_event->message()->payload(), std::string(20, 'b'));
    }
}

TEST(Ruvia, ws_owned_input_accepts_message_payload_during_compaction) {
    for (const bool following_frame : {false, true}) {
        const std::string nested_frame("\x82\x01x", 3);
        std::string wire("\x82\x03", 2);
        wire += nested_frame;
        if (following_frame) {
            wire.append(
                "\x82\x08"
                "abcdefgh",
                10);
        }
        mask_source mask;
        websocket_connection connection({.role_ = websocket_connection_role::client,
            .mask_key_generator_ = &mask_source::generate,
            .mask_key_context_ = &mask,
            .max_buffered_input_bytes_ = wire.size()});
        EXPECT_TRUE(connection.feed(wire) == websocket_feed_status::accepted);
        const auto wrapper = connection.next_event();
        EXPECT_TRUE(wrapper && wrapper->message());
        if (!wrapper || !wrapper->message()) {
            return;
        }
        EXPECT_EQ(wrapper->message()->payload(), nested_frame);

        // Feed bytes borrowed from the completed message while any following
        // frame is still buffered. The input bound requires reclaiming the
        // consumed wrapper before accepting the nested frame.
        EXPECT_TRUE(connection.feed(wrapper->message()->payload()) == websocket_feed_status::accepted);
        if (following_frame) {
            const auto following = connection.next_event();
            EXPECT_TRUE(following && following->message());
            if (!following || !following->message()) {
                return;
            }
            EXPECT_EQ(following->message()->payload(), "abcdefgh");
        }
        const auto nested = connection.next_event();
        EXPECT_TRUE(nested && nested->message());
        if (nested && nested->message()) {
            EXPECT_EQ(nested->message()->payload(), "x");
        }
        EXPECT_TRUE(!connection.next_event());
        EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::open);
    }
}

TEST(Ruvia, ws_input_accepts_borrowed_message_payload_during_compaction) {
    for (const bool following_frame : {false, true}) {
        const std::string nested_frame("\x82\x01x", 3);
        std::string wire("\x82\x03", 2);
        wire += nested_frame;
        if (following_frame) {
            wire.append(
                "\x82\x08"
                "abcdefgh",
                10);
        }
        mask_source mask;
        http_io_buffer input;
        websocket_connection connection(input,
            {.role_ = websocket_connection_role::client,
                .mask_key_generator_ = &mask_source::generate,
                .mask_key_context_ = &mask});
        input.append(wire);
        const auto wrapper = connection.next_event();
        EXPECT_TRUE(wrapper && wrapper->message());
        if (!wrapper || !wrapper->message()) {
            return;
        }
        EXPECT_EQ(wrapper->message()->payload(), nested_frame);

        // Append bytes borrowed from the completed message while any following
        // frame is still buffered. Making room must copy them before the
        // consumed wrapper is reclaimed.
        input.append(wrapper->message()->payload());
        if (following_frame) {
            const auto following = connection.next_event();
            EXPECT_TRUE(following && following->message());
            if (!following || !following->message()) {
                return;
            }
            EXPECT_EQ(following->message()->payload(), "abcdefgh");
        }
        const auto nested = connection.next_event();
        EXPECT_TRUE(nested && nested->message());
        if (nested && nested->message()) {
            EXPECT_EQ(nested->message()->payload(), "x");
        }
        EXPECT_TRUE(!connection.next_event());
        EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::open);
    }
}

TEST(Ruvia, ws_public_masking_round_trips_over_odd_lengths_and_offsets) {
    mask_source mask;
    http_io_buffer sender_input;
    auto sender = client(sender_input, mask, protocol_byte_limit::unlimited());
    http_io_buffer receiver_input;
    websocket_connection receiver(receiver_input, {.message_limit_ = protocol_byte_limit::unlimited()});
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
             std::size_t{5}, std::size_t{7}, std::size_t{8}, std::size_t{9}, std::size_t{15}, std::size_t{16},
             std::size_t{17}, std::size_t{31}, std::size_t{33}, std::size_t{125}, std::size_t{126},
             std::size_t{127}, std::size_t{1021}, std::size_t{65535}, std::size_t{65537}}) {
        std::string payload_value(length, '\0');
        for (std::size_t i = 0; i < length; ++i) {
            payload_value[i] = static_cast<char>((i * 31U + length) & 0xFFU);
        }
        // A leading frame of `offset` bytes moves where the payload lands in
        // the receiver's input, so every alignment is unmasked in place.
        for (std::size_t offset = 0; offset < 8; ++offset) {
            const std::string leading(offset, 'L');
            EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, leading) == websocket_frame_submit_status::accepted);
            const auto leading_wire = drain(sender);
            EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
            const auto wire = drain(sender);

            // Check the wire against RFC 6455 section 5.3 directly: a mask that
            // is only self-inverse would still round-trip.
            const std::size_t header_size = 2 + (length <= 125 ? 0 : (length <= 0xFFFF ? 2 : 8));
            EXPECT_EQ(wire.size(), header_size + 4 + length);
            if (wire.size() != header_size + 4 + length) {
                return;
            }
            const auto key = std::string_view(wire).substr(header_size, 4);
            bool masked_on_wire = true;
            for (std::size_t i = 0; i < length; ++i) {
                masked_on_wire = masked_on_wire && wire[header_size + 4 + i] == static_cast<char>(payload_value[i] ^ key[i % 4]);
            }
            EXPECT_TRUE(masked_on_wire);

            const auto frames = leading_wire + wire;
            receiver_input.append(frames);
            const auto leading_event = receiver.next_event();
            EXPECT_TRUE(leading_event && leading_event->message());
            if (leading_event && leading_event->message()) {
                EXPECT_EQ(leading_event->message()->payload(), leading);
            }
            const auto event = receiver.next_event();
            EXPECT_TRUE(event && event->message());
            if (event && event->message()) {
                EXPECT_TRUE(event->message()->payload() == payload_value);
            }
        }
    }
}

TEST(Ruvia, ws_server_borrowed_frame_follows_pending_output_until_drained) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited()});
    const std::string payload_value(300, 'b');
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value)) == websocket_frame_submit_status::accepted);
    auto plan = protocol.output_plan();
    EXPECT_EQ(plan.bytes().size(), std::size_t{4});
    EXPECT_TRUE(plan.borrowed_bytes().data() == payload_value.data());
    EXPECT_EQ(plan.size(), std::size_t{304});

    // A partial write may end inside the borrowed payload.
    EXPECT_TRUE(protocol.consume_output(104) == websocket_output_consume_status::pending);
    plan = protocol.output_plan();
    EXPECT_TRUE(plan.bytes().empty());
    EXPECT_TRUE(plan.borrowed_bytes().data() == payload_value.data() + 100);
    EXPECT_EQ(plan.borrowed_bytes().size(), std::size_t{200});
    EXPECT_TRUE(protocol.consume_output(201) == websocket_output_consume_status::out_of_range);
    EXPECT_TRUE(protocol.consume_output(200) == websocket_output_consume_status::drained);
    EXPECT_EQ(protocol.output_plan().size(), std::size_t{0});

    // Output queued behind a borrowed payload copies it first, so the wire
    // keeps submission order and no longer depends on the caller's bytes.
    std::string message_value("hello");
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, websocket_borrowed_payload(message_value)) == websocket_frame_submit_status::accepted);
    mask_source mask;
    http_io_buffer peer_input;
    auto peer = client(peer_input, mask);
    EXPECT_TRUE(peer.submit_frame(websocket_opcode::ping, "p") == websocket_frame_submit_status::accepted);
    input.append(drain(peer));
    const auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->ping());
    message_value[0] = 'X';
    plan = protocol.output_plan();
    EXPECT_TRUE(plan.borrowed_bytes().empty());
    const std::string wire(plan.bytes());
    EXPECT_TRUE(protocol.consume_output(wire.size()) == websocket_output_consume_status::drained);
    peer_input.append(wire);
    const auto message = peer.next_event();
    EXPECT_TRUE(message && message->message());
    if (message && message->message()) {
        EXPECT_EQ(message->message()->payload(), "hello");
    }
    const auto pong = peer.next_event();
    EXPECT_TRUE(pong && pong->pong());
    if (pong && pong->pong()) {
        EXPECT_EQ(pong->pong()->payload(), "p");
    }
}

TEST(Ruvia, ws_server_borrowed_frame_owns_compressed_output) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited(),
                                             .compression_ = websocket_compression{.enabled_ = true}});
    const std::string payload_value(4096, 'x');
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value)) == websocket_frame_submit_status::accepted);
    const auto plan = protocol.output_plan();
    EXPECT_TRUE(plan.borrowed_bytes().empty());
    EXPECT_TRUE(!plan.bytes().empty() && (static_cast<unsigned char>(plan.bytes()[0]) & 0x40U) != 0);
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value), {.compress_ = false}) == websocket_frame_submit_status::accepted);
    EXPECT_TRUE(protocol.output_plan().borrowed_bytes().data() == payload_value.data());
}

TEST(Ruvia, ws_public_eight_bit_receive_window_accepts_zlib_nine_bit_deflater_streams) {
    // zlib has no 8-bit raw deflate window, so a zlib peer that negotiated
    // window 8 compresses with window 9. Its deflater still never refers back
    // more than 250 bytes, which the 256-byte inflate window covers. The
    // inflater consults that window only for history produced by earlier
    // output passes, so the incompressible prefix ends just past the 8 KiB
    // inflate pass and the 300-byte repeat refers back across that boundary.
    std::string message(8192 + 10, '\0');
    std::uint64_t random = 0x9e3779b97f4a7c15ULL;
    for (auto& byte : message) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }
    message += message.substr(message.size() - 300, 300);
    z_stream stream{};
    EXPECT_TRUE(deflateInit2(&stream, 6, Z_DEFLATED, -9, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string compressed(message.size() + 256, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(message.data());
    stream.avail_in = static_cast<uInt>(message.size());
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(compressed.size());
    EXPECT_TRUE(deflate(&stream, Z_SYNC_FLUSH) == Z_OK);
    compressed.resize(compressed.size() - stream.avail_out);
    deflateEnd(&stream);
    EXPECT_TRUE(compressed.size() >= 4 && compressed.size() < 65536);
    compressed.resize(compressed.size() - 4);

    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover, .server_max_window_bits_ = 8}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        std::string wire("\xc2\x7e", 2);
        wire.push_back(static_cast<char>(compressed.size() >> 8));
        wire.push_back(static_cast<char>(compressed.size() & 0xff));
        wire += compressed;
        input.append(wire);
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == message);
    }
}

TEST(Ruvia, ws_public_receiver_accepts_non_minimal_payload_length_forms) {
    // RFC 6455 section 5.2 makes the minimal length form a sender obligation;
    // the longer forms still delimit the frame exactly, so a receiver reads
    // them instead of failing the connection.
    const std::string sixteen_bit("\x81\xfe\x00\x05\x00\x00\x00\x00hello", 13);
    const std::string sixty_four_bit("\x81\xff\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x00hello", 19);
    const std::string control_sixteen_bit("\x89\xfe\x00\x02\x00\x00\x00\x00hi", 10);
    for (const auto& wire : {sixteen_bit, sixty_four_bit}) {
        http_io_buffer input;
        websocket_connection server(input);
        input.append(wire);
        const auto event = server.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "hello");
    }
    http_io_buffer input;
    websocket_connection server(input);
    input.append(control_sixteen_bit);
    const auto event = server.next_event();
    EXPECT_TRUE(event && event->ping() && event->ping()->payload() == "hi");
}

TEST(Ruvia, ws_public_feed_reports_backpressure_when_initial_buffer_exceeds_limit) {
    http_io_buffer input;
    input.append("123456789");
    websocket_connection connection(input, {.max_buffered_input_bytes_ = 8});
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::backpressured);
    EXPECT_EQ(input.size(), std::size_t{9});
}

TEST(Ruvia, ws_receive_ceiling_is_early_directional_and_only_tightens) {
    for (const std::size_t receive_limit : {std::size_t{256}, std::size_t{2048}}) {
        const auto effective = (std::min)(receive_limit, std::size_t{1024});
        websocket_connection receiver({.message_limit_ = protocol_byte_limit::limited(1024),
            .receive_message_limit_ = protocol_byte_limit::limited(receive_limit)});
        EXPECT_EQ(receiver.submit_frame(websocket_opcode::binary, std::string(1024, 'r'), {.compress_ = false}),
            websocket_frame_submit_status::accepted);
        (void)drain(receiver);
        EXPECT_EQ(receiver.submit_frame(websocket_opcode::binary, std::string(1025, 'r'), {.compress_ = false}),
            websocket_frame_submit_status::message_too_large);
        // 仅发送帧头，无正文：在分配或等待完整消息前拒绝声明的超限长度。
        std::string header("\x82\xfe", 2);
        header.push_back(static_cast<char>((effective + 1) >> 8));
        header.push_back(static_cast<char>((effective + 1) & 0xff));
        header.append(4, '\0');
        EXPECT_EQ(receiver.feed(header), websocket_feed_status::accepted);
        const auto failure = receiver.next_event();
        ASSERT_TRUE(failure && failure->protocol_error());
        EXPECT_EQ(failure->protocol_error()->close_code(), 1009);
    }
    // 各分片单独合法，累计消息长度仍受同一个接收上限约束。
    websocket_connection receiver({.receive_message_limit_ = protocol_byte_limit::limited(4)});
    const std::string first("\x02\x83\0\0\0\0abc", 9);
    const std::string last("\x80\x82\0\0\0\0de", 8);
    EXPECT_EQ(receiver.feed(first), websocket_feed_status::accepted);
    EXPECT_FALSE(receiver.next_event());
    EXPECT_EQ(receiver.feed(last), websocket_feed_status::accepted);
    const auto failure = receiver.next_event();
    ASSERT_TRUE(failure && failure->protocol_error());
    EXPECT_EQ(failure->protocol_error()->close_code(), 1009);
}

TEST(Ruvia, ws_fragmented_deflate_enforces_uncompressed_message_limit) {
    {
        websocket_connection sender({.message_limit_ = protocol_byte_limit::limited(1000),
            .compression_ = websocket_compression{.enabled_ = true}});
        const std::string first_chunk(600, 'a');
        const std::string second_chunk(500, 'a');
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, first_chunk, {.final_ = false, .compress_ = true}),
            websocket_frame_submit_status::accepted);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, second_chunk, {.final_ = true, .compress_ = true}),
            websocket_frame_submit_status::message_too_large);
    }
    {
        websocket_connection sender({.message_limit_ = protocol_byte_limit::limited(64),
            .compression_ = websocket_compression{.enabled_ = true}});
        std::string incompressible(64, '\0');
        std::uint64_t rng = 0x517cc1b727220a95ULL;
        for (char& ch : incompressible) {
            rng ^= rng << 13U;
            rng ^= rng >> 7U;
            rng ^= rng << 17U;
            ch = static_cast<char>(rng & 0xffU);
        }
        const std::string_view view(incompressible);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, view.substr(0, 32), {.final_ = false, .compress_ = true}),
            websocket_frame_submit_status::accepted);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, view.substr(32, 32), {.final_ = true, .compress_ = true}),
            websocket_frame_submit_status::accepted);
    }
}
