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
