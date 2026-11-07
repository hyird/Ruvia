#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_encoder.h"

#include "content_decoding_fixture.h"
#include "failing_memory_resource.h"
#include "stl_allocation_fixture.h"
#include "test_exceptions.h"

using ruvia::http_content_encoder;

namespace {

class codec_test_allocation_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "codec test allocation failure";
    }
};

class throwing_memory_resource final : public std::pmr::memory_resource {
public:
    explicit throwing_memory_resource(std::size_t fail_at)
        : fail_at_(fail_at) {}

    [[nodiscard]] std::size_t live_bytes() const noexcept {
        return live_bytes_;
    }
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t releases() const noexcept {
        return releases_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (++allocation_attempts_ == fail_at_) {
            throw codec_test_allocation_error();
        }
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        ++allocations_;
        return allocation;
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        live_bytes_ -= bytes_value;
        ++releases_;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    const std::size_t fail_at_;
    std::size_t allocation_attempts_{};
    std::size_t allocations_{};
    std::size_t live_bytes_{};
    std::size_t releases_{};
};

class counting_memory_resource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t live_bytes() const noexcept {
        return live_bytes_;
    }
    [[nodiscard]] std::size_t largest_allocation() const noexcept {
        return largest_allocation_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        live_bytes_ += bytes_value;
        largest_allocation_ = (std::max)(largest_allocation_, bytes_value);
        return allocation;
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        live_bytes_ -= bytes_value;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
    std::size_t live_bytes_{0};
    std::size_t largest_allocation_{0};
};

class reject_output_cap_allocation_resource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // The Brotli state for a one-megabyte input is intentionally much
        // larger than the output cap. Reject only the cap-sized allocation so
        // the test remains about output reservation, not codec initialization.
        if (bytes_value >= (1u << 20) && bytes_value < (2u << 20)) {
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
};

}  // namespace

TEST(Ruvia, http_content_decode_handles_deterministic_arbitrary_compressed_bytes) {
    std::uint64_t state_value = 0xC0DE'C0DE'5EED'F00DULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 1024; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 513U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        for (const auto coding : {http_content_coding::gzip, http_content_coding::deflate,
                 http_content_coding::brotli, http_content_coding::zstd}) {
            const auto max_decoded_bytes = static_cast<std::size_t>(next_value() % 513U);
            std::pmr::monotonic_buffer_resource resource;
            const auto result_value = decode_http_content(std::array{coding}, input,
                {.max_decoded_bytes_ = max_decoded_bytes, .resource_ = &resource});
            if (const auto* decoded = std::get_if<std::pmr::string>(&result_value)) {
                EXPECT_TRUE(decoded->size() <= max_decoded_bytes);
            } else {
                EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(result_value));
            }
        }
    }
}

// The Content-Encoding field and the codecs behind it, independent of any message.

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
    EXPECT_TRUE(std::holds_alternative<std::pmr::string>(identity));
    if (const auto* decoded = std::get_if<std::pmr::string>(&identity)) {
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
        EXPECT_TRUE(std::holds_alternative<std::pmr::string>(decoded));
        if (const auto* output = std::get_if<std::pmr::string>(&decoded)) {
            EXPECT_EQ(std::string_view(*output), input);
        }
    }
}

TEST(Ruvia, http_content_encoding_stack_round_trips_in_protocol_order) {
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate,
        http_content_coding::brotli};
    const std::string input(8192, 's');
    counting_memory_resource resource;
    {
        auto encoded = encode_http_content(codings, input,
            {.max_encoded_bytes_ = input.size(), .resource_ = &resource});
        EXPECT_TRUE(encoded.has_value());
        if (encoded.has_value()) {
            auto decoded = decode_http_content(codings, *encoded,
                {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
            EXPECT_TRUE(std::holds_alternative<std::pmr::string>(decoded));
            if (const auto* output = std::get_if<std::pmr::string>(&decoded)) {
                EXPECT_EQ(std::string_view(*output), input);
            }
            auto limited = decode_http_content(codings, *encoded,
                {.max_decoded_bytes_ = input.size() - 1, .resource_ = &resource});
            EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(limited));
            if (const auto* failure = std::get_if<http_content_decode_error>(&limited)) {
                EXPECT_TRUE(*failure ==
                            http_content_decode_error::decoded_size_exceeded);
            }
            std::string truncated(*encoded);
            truncated.pop_back();
            auto incomplete = decode_http_content(codings, truncated,
                {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
            EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(incomplete));
            if (const auto* failure = std::get_if<http_content_decode_error>(&incomplete)) {
                EXPECT_TRUE(*failure ==
                            http_content_decode_error::invalid_content);
            }
        }
        auto capped = encode_http_content(codings, input,
            {.max_encoded_bytes_ = 8, .resource_ = &resource});
        EXPECT_TRUE(!capped.has_value());
    }
    EXPECT_EQ(resource.live_bytes(), 0U);
}

TEST(Ruvia, http_content_encoding_stack_skips_identity_stages_without_copying) {
    const std::string input(8192, 'i');
    constexpr std::array stacked{http_content_coding::gzip, http_content_coding::identity};
    counting_memory_resource single_resource;
    counting_memory_resource stacked_resource;
    auto single = encode_http_content(http_content_coding::gzip, input,
        {.max_encoded_bytes_ = input.size(), .resource_ = &single_resource});
    auto encoded = encode_http_content(stacked, input,
        {.max_encoded_bytes_ = input.size(), .resource_ = &stacked_resource});
    EXPECT_TRUE(single.has_value());
    EXPECT_TRUE(encoded.has_value());
    if (single.has_value() && encoded.has_value()) {
        EXPECT_EQ(*encoded, *single);
    }
    if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
        EXPECT_EQ(stacked_resource.allocations(), single_resource.allocations());
    }

    constexpr std::array identity_only{http_content_coding::identity, http_content_coding::identity};
    auto copied = encode_http_content(identity_only, input, {.max_encoded_bytes_ = input.size()});
    EXPECT_TRUE(copied.has_value() && std::string_view(*copied) == input);
    auto capped = encode_http_content(identity_only, input, {.max_encoded_bytes_ = input.size() - 1});
    EXPECT_TRUE(!capped.has_value());
}

TEST(Ruvia, http_content_encoder_reset_starts_an_equivalent_fresh_stream) {
    const std::string input(16384, 'e');
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip, http_content_coding::deflate,
             http_content_coding::brotli, http_content_coding::zstd}) {
        counting_memory_resource resource;
        {
            http_content_encoder encoder(coding, &resource);
            std::pmr::string first(&resource);
            encoder.write(input.substr(0, 100), first, true);
            encoder.write(input.substr(100), first);
            encoder.finish(first);

            encoder.reset();
            std::pmr::string second(&resource);
            encoder.write(input.substr(0, 100), second, true);
            encoder.write(input.substr(100), second);
            encoder.finish(second);
            EXPECT_EQ(second, first);

            // A reset mid-stream discards the unfinished stream.
            encoder.reset();
            std::pmr::string discarded(&resource);
            encoder.write(input, discarded);
            encoder.reset();
            std::pmr::string third(&resource);
            encoder.write(input.substr(0, 100), third, true);
            encoder.write(input.substr(100), third);
            encoder.finish(third);
            EXPECT_EQ(third, first);

            auto decoded = decode_http_content(
                std::array{coding}, third, {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
            EXPECT_TRUE(std::holds_alternative<std::pmr::string>(decoded) &&
                        std::string_view(std::get<std::pmr::string>(decoded)) == input);
        }
        EXPECT_EQ(resource.live_bytes(), 0U);
    }
}

TEST(Ruvia, http_zstd_content_rejects_window_above_rfc9659_limit) {
    const std::string plain(9 * 1024 * 1024, 'w');
    const std::string encoded = zstd_compress_with_window(plain, 24);
    EXPECT_TRUE(!encoded.empty());
    EXPECT_TRUE(decode_error(http_content_coding::zstd, encoded, plain.size()) ==
                http_content_decode_error::invalid_content);

    auto conformant = encode_http_content(http_content_coding::zstd, plain,
        {.max_encoded_bytes_ = plain.size(), .resource_ = std::pmr::get_default_resource()});
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
            {.max_encoded_bytes_ = input.size(), .resource_ = std::pmr::get_default_resource()});
        EXPECT_TRUE(full.has_value());
        if (!full.has_value()) {
            continue;
        }
        const auto encoded_size = full->size();
        EXPECT_TRUE(encoded_size > 1);

        const auto exact = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size, .resource_ = std::pmr::get_default_resource()});
        EXPECT_TRUE(exact.has_value());
        if (exact.has_value()) {
            EXPECT_EQ(exact->size(), encoded_size);
        }

        const auto too_small = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size - 1, .resource_ = std::pmr::get_default_resource()});
        EXPECT_TRUE(!too_small.has_value());
        EXPECT_TRUE(!encode_http_content(coding, {}, {.max_encoded_bytes_ = 0}).has_value());
        EXPECT_TRUE(!encode_http_content(coding, input, {.max_encoded_bytes_ = 0}).has_value());
    }

    const auto identity = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 8, .resource_ = std::pmr::get_default_resource()});
    EXPECT_TRUE(identity.has_value());
    if (identity.has_value()) {
        EXPECT_EQ(std::string_view(*identity), std::string_view("identity"));
    }
    const auto identity_too_large = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 0, .resource_ = std::pmr::get_default_resource()});
    EXPECT_TRUE(!identity_too_large.has_value());
    const auto empty_identity = encode_http_content(http_content_coding::identity, {}, {.max_encoded_bytes_ = 0});
    EXPECT_TRUE(empty_identity.has_value() && empty_identity->empty());
}

TEST(Ruvia, http_content_encoding_stack_caps_each_stage_and_throws_for_unknown_coding) {
    const std::string input(2048, 's');
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::gzip};
    const auto first = encode_http_content(codings.front(), input, {.max_encoded_bytes_ = input.size()});
    const auto full = encode_http_content(codings, input, {.max_encoded_bytes_ = input.size()});
    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(full.has_value());
    if (first.has_value() && full.has_value()) {
        EXPECT_TRUE(full->size() > first->size());
        const auto exact = encode_http_content(codings, input, {.max_encoded_bytes_ = full->size()});
        EXPECT_TRUE(exact.has_value() && *exact == *full);
        EXPECT_TRUE(!encode_http_content(codings, input, {.max_encoded_bytes_ = first->size()}).has_value());
        EXPECT_TRUE(!encode_http_content(codings, input, {.max_encoded_bytes_ = first->size() - 1}).has_value());
    }

    counting_memory_resource resource;
    constexpr std::array invalid_stack{http_content_coding::gzip, static_cast<http_content_coding>(255)};
    bool stack_threw = false;
    try {
        static_cast<void>(encode_http_content(invalid_stack, input,
            {.max_encoded_bytes_ = input.size(), .resource_ = &resource}));
    } catch (const ruvia::http_content_codec_error&) {
        stack_threw = true;
    }
    EXPECT_TRUE(stack_threw);
    EXPECT_EQ(resource.live_bytes(), 0U);
}

TEST(Ruvia, http_content_codec_error_is_thrown_only_when_a_codec_cannot_run) {
    const std::string input(2048, 's');
    const auto unknown = static_cast<http_content_coding>(255);
    counting_memory_resource resource;
    bool threw = false;
    try {
        static_cast<void>(encode_http_content(unknown, input, {.max_encoded_bytes_ = 0, .resource_ = &resource}));
    } catch (const ruvia::http_content_codec_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(resource.live_bytes(), 0U);

    // A coding the decoder does not know is a property of the representation.
    const auto unsupported = decode_http_content(std::array{http_content_coding::gzip, unknown}, input,
        {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
    const auto* failure = std::get_if<http_content_decode_error>(&unsupported);
    EXPECT_TRUE(failure != nullptr && *failure == http_content_decode_error::unsupported_coding);
    EXPECT_EQ(resource.live_bytes(), 0U);
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

TEST(Ruvia, http_content_whole_buffer_results_keep_their_memory_resource_and_release_failures) {
    const std::string plain(1024, 'r');
    constexpr std::array codings{http_content_coding::identity, http_content_coding::gzip,
        http_content_coding::brotli, http_content_coding::zstd};

    for (const auto coding : codings) {
        counting_memory_resource resource;
        {
            auto first_encoded = encode_http_content(coding, plain,
                {.max_encoded_bytes_ = plain.size() * 2, .resource_ = &resource});
            EXPECT_TRUE(first_encoded.has_value());
            if (!first_encoded.has_value()) {
                continue;
            }
            const auto encoded_bytes = std::string_view(*first_encoded);
            auto first_decoded = decode_http_content(std::array{coding}, encoded_bytes,
                {.max_decoded_bytes_ = plain.size(), .resource_ = &resource});
            EXPECT_TRUE(std::holds_alternative<std::pmr::string>(first_decoded));
            if (!std::holds_alternative<std::pmr::string>(first_decoded)) {
                continue;
            }
            EXPECT_EQ(std::string_view(std::get<std::pmr::string>(first_decoded)), std::string_view(plain));
            const auto retained_baseline = resource.live_bytes();

            for (int iteration = 0; iteration < 8; ++iteration) {
                {
                    auto repeated_encoded = encode_http_content(coding, plain,
                        {.max_encoded_bytes_ = plain.size() * 2, .resource_ = &resource});
                    EXPECT_TRUE(repeated_encoded.has_value());
                    if (repeated_encoded.has_value()) {
                        EXPECT_EQ(std::string_view(*repeated_encoded), encoded_bytes);
                    }
                }
                EXPECT_EQ(resource.live_bytes(), retained_baseline);
                {
                    auto repeated_decoded = decode_http_content(std::array{coding}, encoded_bytes,
                        {.max_decoded_bytes_ = plain.size(), .resource_ = &resource});
                    EXPECT_TRUE(std::holds_alternative<std::pmr::string>(repeated_decoded));
                    if (const auto* str = std::get_if<std::pmr::string>(&repeated_decoded)) {
                        EXPECT_EQ(std::string_view(*str), std::string_view(plain));
                    }
                }
                EXPECT_EQ(resource.live_bytes(), retained_baseline);
                EXPECT_EQ(std::string_view(*first_encoded), encoded_bytes);
                EXPECT_EQ(std::string_view(std::get<std::pmr::string>(first_decoded)), std::string_view(plain));
            }

            for (const std::size_t cap : {std::size_t{0}, std::size_t{1}}) {
                const auto encode_failure = encode_http_content(coding, plain,
                    {.max_encoded_bytes_ = cap, .resource_ = &resource});
                EXPECT_TRUE(!encode_failure.has_value());
                EXPECT_EQ(resource.live_bytes(), retained_baseline);

                const auto decode_failure = decode_http_content(std::array{coding}, encoded_bytes,
                    {.max_decoded_bytes_ = cap, .resource_ = &resource});
                EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(decode_failure));
                if (const auto* failure = std::get_if<http_content_decode_error>(&decode_failure)) {
                    EXPECT_TRUE(*failure ==
                                http_content_decode_error::decoded_size_exceeded);
                }
                EXPECT_EQ(resource.live_bytes(), retained_baseline);
            }

            if (coding != http_content_coding::identity) {
                const auto invalid = decode_http_content(std::array{coding}, "not valid coded data",
                    {.max_decoded_bytes_ = plain.size(), .resource_ = &resource});
                EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(invalid));
                if (const auto* failure = std::get_if<http_content_decode_error>(&invalid)) {
                    EXPECT_TRUE(*failure ==
                                http_content_decode_error::invalid_content);
                }
                EXPECT_EQ(resource.live_bytes(), retained_baseline);
            }

            const auto empty_string_bytes = [&] {
                const std::pmr::string empty(&resource);
                return resource.live_bytes() - retained_baseline;
            }();
            const auto* decoded_data = std::get<std::pmr::string>(first_decoded).data();
            auto decoded_bytes = std::move(std::get<std::pmr::string>(first_decoded));
            EXPECT_TRUE(decoded_bytes.get_allocator().resource() == &resource);
            EXPECT_TRUE(decoded_bytes.data() == decoded_data);
            EXPECT_EQ(std::string_view(decoded_bytes), std::string_view(plain));
            EXPECT_EQ(std::string_view(*first_encoded), encoded_bytes);
            EXPECT_EQ(resource.live_bytes(), retained_baseline + empty_string_bytes);
        }
        EXPECT_EQ(resource.live_bytes(), std::size_t{0});
    }

    if (ruvia::test::stl_allocation_failure_injection_enabled()) {
        bool encode_allocation_threw = false;
        try {
            (void)encode_http_content(http_content_coding::identity, plain,
                {.max_encoded_bytes_ = plain.size(), .resource_ = std::pmr::null_memory_resource()});
        } catch (const std::bad_alloc&) {
            encode_allocation_threw = true;
        }
        EXPECT_TRUE(encode_allocation_threw);

        bool decode_allocation_threw = false;
        try {
            (void)decode_http_content(std::array{http_content_coding::identity}, plain,
                {.max_decoded_bytes_ = plain.size(), .resource_ = std::pmr::null_memory_resource()});
        } catch (const std::bad_alloc&) {
            decode_allocation_threw = true;
        }
        EXPECT_TRUE(decode_allocation_threw);
    }
}

TEST(Ruvia, http_content_codecs_rethrow_allocator_exceptions_and_release_partial_state) {
    const struct {
        http_content_coding coding_;
        std::string encoded_;
    } cases[] = {
        {http_content_coding::gzip, gzip_compress({})},
        {http_content_coding::brotli, brotli_compress({})},
        {http_content_coding::zstd, zstd_compress({})},
    };
    const bool inject_failures = ruvia::test::stl_allocation_failure_injection_enabled();

    for (const auto& test : cases) {
        for (const bool decode : {false, true}) {
            const auto run = [&](throwing_memory_resource& resource) {
                if (decode) {
                    auto result_value = decode_http_content(std::array{test.coding_}, test.encoded_,
                        {.max_decoded_bytes_ = 0, .resource_ = &resource});
                    EXPECT_TRUE(std::holds_alternative<std::pmr::string>(result_value));
                } else {
                    auto result_value = encode_http_content(test.coding_, {},
                        {.max_encoded_bytes_ = 1024, .resource_ = &resource});
                    EXPECT_TRUE(result_value.has_value());
                }
            };
            throwing_memory_resource baseline(0);
            run(baseline);
            EXPECT_EQ(baseline.live_bytes(), std::size_t{0});
            EXPECT_EQ(baseline.releases(), baseline.allocations());
            if (inject_failures) {
                bool observed_allocation_exception = false;
                for (std::size_t fail_at = 1; fail_at <= baseline.allocations(); ++fail_at) {
                    throwing_memory_resource resource(fail_at);
                    bool failed = false;
                    try {
                        run(resource);
                    } catch (const codec_test_allocation_error&) {
                        failed = true;
                        observed_allocation_exception = true;
                    }
                    EXPECT_TRUE(failed);
                    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
                    EXPECT_EQ(resource.releases(), resource.allocations());
                }
                EXPECT_TRUE(observed_allocation_exception);
            }
        }
    }
}

TEST(Ruvia, http_content_decoder_state_uses_the_callers_memory_resource) {
    const struct {
        http_content_coding coding_;
        std::string encoded_;
    } cases[] = {
        {http_content_coding::gzip, gzip_compress({})},
        {http_content_coding::brotli, brotli_compress({})},
        {http_content_coding::zstd, zstd_compress({})},
    };

    for (const auto& test : cases) {
        counting_memory_resource resource;
        const auto result_value = decode_http_content(
            std::array{test.coding_}, test.encoded_, {.max_decoded_bytes_ = 0, .resource_ = &resource});
        EXPECT_TRUE(std::holds_alternative<std::pmr::string>(result_value));
        // The decoded representation is empty and stays in the string's SSO
        // buffer. Any observed allocation therefore belongs to codec state.
        EXPECT_TRUE(resource.allocations() != 0);
    }
}

TEST(Ruvia, http_content_encoder_round_trips_incremental_chunks) {
    const std::string input =
        "incremental HTTP response data with enough repetition to exercise "
        "each encoder's pending output and finalization state. ";
    const std::string repeated = input + input + input + input + input;

    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::brotli, http_content_coding::zstd}) {
        std::pmr::string encoded(std::pmr::get_default_resource());
        std::pmr::string chunk(std::pmr::get_default_resource());
        http_content_encoder encoder(coding, std::pmr::get_default_resource());
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
        http_content_encoder encoder(coding, std::pmr::get_default_resource());
        std::pmr::string output(std::pmr::get_default_resource());
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

TEST(Ruvia, http_content_encoder_failure_is_terminal) {
    http_content_encoder encoder(http_content_coding::identity, std::pmr::get_default_resource());
    failing_memory_resource resource;
    std::pmr::string output(&resource);
    const std::string input(128, 'f');
    bool allocation_failed = false;
    resource.fail_after(0);
    try {
        encoder.write(input, output);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    EXPECT_TRUE(allocation_failed);
    resource.allow_allocations();
    EXPECT_TRUE(ruvia::testing::throws_on([&] { encoder.finish(output); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { encoder.write("retry", output); }));
}

TEST(Ruvia, http_content_encoder_brotli_reset_recovers_after_output_allocation_failure) {
    const std::string input(4096, 'b');
    for (const bool fail_on_finish : {false, true}) {
        http_content_encoder encoder(http_content_coding::brotli, std::pmr::get_default_resource());
        failing_memory_resource resource;
        std::pmr::string output(&resource);
        // Fill inline storage so even a one-byte final block must allocate.
        output.resize(output.capacity(), 'p');
        bool allocation_failed = false;
        resource.fail_after(0);
        try {
            if (fail_on_finish) {
                std::pmr::string pending;
                encoder.write(input, pending);
                encoder.finish(output);
            } else {
                encoder.write(input, output, true);
            }
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        EXPECT_TRUE(allocation_failed);
        resource.allow_allocations();
        std::pmr::string recovered;
        EXPECT_TRUE(ruvia::testing::throws_on([&] { encoder.write("retry", recovered); }));
        EXPECT_TRUE(ruvia::testing::throws_on([&] { encoder.finish(recovered); }));
        EXPECT_TRUE(recovered.empty());
        encoder.reset();
        encoder.write(input, recovered, true);
        encoder.finish(recovered);
        EXPECT_EQ(decoded(http_content_coding::brotli, recovered, input.size()), input);
    }
}

TEST(Ruvia, http_content_encoder_flushes_each_incremental_chunk) {
    const std::string input(4096, 's');

    for (const auto coding :
        {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
        std::pmr::string encoded(std::pmr::get_default_resource());
        std::pmr::string chunk(std::pmr::get_default_resource());
        http_content_encoder encoder(coding, std::pmr::get_default_resource());
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
        std::pmr::string encoded(std::pmr::get_default_resource());
        http_content_encoder encoder(coding, std::pmr::get_default_resource());
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

TEST(Ruvia, http_content_encoder_results_survive_subsequent_writes_and_encoder_destruction) {
    const std::string input(16385, 'x');
    for (const auto coding : {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
        counting_memory_resource resource;
        {
            std::pmr::string first(&resource);
            std::pmr::string rest(&resource);
            std::string saved;
            {
                http_content_encoder encoder(coding, &resource);
                encoder.write(input, first, true);
                saved.assign(first);
                for (int iteration = 0; iteration < 16; ++iteration) {
                    encoder.write(input, rest, true);
                    EXPECT_EQ(std::string_view(first), std::string_view(saved));
                }
                encoder.finish(rest);
            }
            EXPECT_EQ(std::string_view(first), std::string_view(saved));
            saved.append(rest);
            EXPECT_EQ(decoded(coding, saved, input.size() * 17), std::string(input.size() * 17, 'x'));
            EXPECT_TRUE(resource.live_bytes() != 0);
        }
        EXPECT_EQ(resource.live_bytes(), std::size_t{0});
    }
}

TEST(Ruvia, http_content_encoder_rethrows_codec_allocations_and_reclaims_partial_state) {
    const std::string input(4096, 'x');
    // Brotli's default encoder build exits on internal OOM instead of returning
    // through its C API. Exercise its recoverable constructor path separately.
    // This resource owns only raw codec state. Output and its STL metadata use
    // the default resource, and the independent input needs no aliasing copy.
    for (const auto coding : {http_content_coding::gzip, http_content_coding::deflate,
             http_content_coding::zstd}) {
        bool completed = false;
        std::size_t failures = 0;
        for (std::size_t fail_at = 1; fail_at <= 128 && !completed; ++fail_at) {
            throwing_memory_resource resource(fail_at);
            try {
                http_content_encoder encoder(coding, &resource);
                std::pmr::string output;
                try {
                    encoder.write(input, output, true);
                    encoder.finish(output);
                    completed = true;
                    EXPECT_EQ(decoded(coding, output, input.size()), input);
                } catch (const codec_test_allocation_error&) {
                    ++failures;
                    EXPECT_TRUE(ruvia::testing::throws_on([&] { encoder.write("retry", output); }));
                    EXPECT_TRUE(ruvia::testing::throws_on([&] { encoder.finish(output); }));
                }
            } catch (const codec_test_allocation_error&) {
                ++failures;
            }
            EXPECT_EQ(resource.live_bytes(), std::size_t{0});
            EXPECT_EQ(resource.allocations(), resource.releases());
        }
        EXPECT_TRUE(completed);
        EXPECT_TRUE(failures != 0);
    }
}

TEST(Ruvia, http_content_encoder_brotli_constructor_rethrows_allocator_exception) {
    // Both failure points are raw owner/native-state allocations, not STL
    // container construction, so they remain injectable with debug iterators.
    for (const auto fail_at : {std::size_t{1}, std::size_t{2}}) {
        throwing_memory_resource resource(fail_at);
        bool failed = false;
        try {
            http_content_encoder encoder(http_content_coding::brotli, &resource);
        } catch (const codec_test_allocation_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(resource.live_bytes(), std::size_t{0});
        EXPECT_EQ(resource.allocations(), resource.releases());
    }
}

TEST(Ruvia, http_content_encoder_rejects_unsupported_coding_and_reclaims_owner) {
    counting_memory_resource resource;
    bool rejected = false;
    try {
        http_content_encoder encoder(static_cast<http_content_coding>(255), &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
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

TEST(Ruvia, http_brotli_encode_does_not_reserve_the_output_cap) {
    const std::string input(1u << 20, 'b');
    reject_output_cap_allocation_resource resource;
    bool completed = false;
    bool round_tripped = false;
    try {
        auto result_value = encode_http_content(http_content_coding::brotli, input,
            {.max_encoded_bytes_ = input.size() - 1, .resource_ = &resource});
        completed = result_value.has_value();
        if (result_value.has_value()) {
            round_tripped =
                decoded(http_content_coding::brotli, *result_value, input.size()) == input;
        }
    } catch (const std::bad_alloc&) {
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(round_tripped);
}

TEST(Ruvia, http_identity_content_uses_the_default_resource_when_none_is_supplied) {
    const std::string input(1024, 'i');

    auto decoded = decode_http_content(std::array{http_content_coding::identity}, input,
        {.max_decoded_bytes_ = input.size(), .resource_ = nullptr});
    EXPECT_TRUE(std::holds_alternative<std::pmr::string>(decoded));
    if (auto* decoded_str = std::get_if<std::pmr::string>(&decoded)) {
        auto bytes_value = std::move(*decoded_str);
        EXPECT_EQ(std::string_view(bytes_value), std::string_view(input));
        EXPECT_TRUE(bytes_value.get_allocator().resource() == std::pmr::get_default_resource());
    }

    auto encoded = encode_http_content(http_content_coding::identity, input,
        {.max_encoded_bytes_ = input.size(), .resource_ = nullptr});
    EXPECT_TRUE(encoded.has_value());
    if (encoded.has_value()) {
        auto bytes_value = std::move(*encoded);
        EXPECT_EQ(std::string_view(bytes_value), std::string_view(input));
        EXPECT_TRUE(bytes_value.get_allocator().resource() == std::pmr::get_default_resource());
    }
}

TEST(Ruvia, http_identity_content_rejects_oversize_before_allocating) {
    const std::string input(1024, 'i');

    const auto decoded = decode_http_content(std::array{http_content_coding::identity}, input,
        {.max_decoded_bytes_ = input.size() - 1, .resource_ = std::pmr::null_memory_resource()});
    EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(decoded));
    if (const auto* failure = std::get_if<http_content_decode_error>(&decoded)) {
        EXPECT_TRUE(*failure == http_content_decode_error::decoded_size_exceeded);
    }

    const auto encoded = encode_http_content(http_content_coding::identity, input,
        {.max_encoded_bytes_ = input.size() - 1, .resource_ = std::pmr::null_memory_resource()});
    EXPECT_TRUE(!encoded.has_value());
}

TEST(Ruvia, http_content_decode_accepts_empty_encoded_input_as_empty_content) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        EXPECT_EQ(decoded(coding, {}, 0), std::string{});
    }

    constexpr std::array codings{http_content_coding::gzip, http_content_coding::brotli,
        http_content_coding::zstd};
    auto stacked = decode_http_content(
        codings, {}, {.max_decoded_bytes_ = 0, .resource_ = std::pmr::get_default_resource()});
    EXPECT_TRUE(std::holds_alternative<std::pmr::string>(stacked));
    if (const auto* content = std::get_if<std::pmr::string>(&stacked)) {
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
            {.max_decoded_bytes_ = 0, .resource_ = std::pmr::get_default_resource()});
        EXPECT_TRUE(std::holds_alternative<std::pmr::string>(result_value));
        if (const auto* content = std::get_if<std::pmr::string>(&result_value)) {
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

TEST(Ruvia, http_content_encoder_stream_state_stays_below_the_maximum_windows) {
    // A long-lived response stream must not hold the largest window the codec
    // supports: brotli streams use a 512 KiB window and zstd its level default
    // (2 MiB), not the 4 MiB brotli default or a pinned 8 MiB zstd window.
    std::string input(16 * 1024, '\0');
    unsigned state_value = 17;
    for (auto& byte_value : input) {
        state_value = state_value * 1103515245U + 12345U;
        byte_value = static_cast<char>('a' + ((state_value >> 16U) % 23U));
    }
    for (const auto coding : {http_content_coding::brotli, http_content_coding::zstd}) {
        counting_memory_resource resource;
        {
            http_content_encoder encoder(coding, &resource);
            std::pmr::string encoded(std::pmr::new_delete_resource());
            encoder.write(input, encoded, true);
            encoder.write(input, encoded, true);
            EXPECT_TRUE(resource.live_bytes() < std::size_t{6} * 1024 * 1024);
            encoder.finish(encoded);
            EXPECT_EQ(decoded(coding, encoded, input.size() * 2), input + input);
        }
        EXPECT_EQ(resource.live_bytes(), std::size_t{0});
    }
}

TEST(Ruvia, http_brotli_decoder_memory_is_bounded_by_the_decoded_size_cap) {
    // WBITS 24, one non-last uncompressed metablock declaring MLEN 2^24, then
    // a few content bytes: the header alone asks for a 16 MiB ring buffer.
    std::string encoded("\xCF\xFF\xFF\xFF", 4);
    encoded.append(8, 'x');
    counting_memory_resource resource;
    const auto result_value = decode_http_content(std::array{http_content_coding::brotli}, encoded,
        {.max_decoded_bytes_ = 4096, .resource_ = &resource});
    EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(result_value));
    if (const auto* failure = std::get_if<http_content_decode_error>(&result_value)) {
        EXPECT_TRUE(*failure == http_content_decode_error::decoded_size_exceeded);
    }
    EXPECT_TRUE(resource.largest_allocation() < std::size_t{1} << 20);
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});

    // Content that fits the cap is never refused by the bound.
    std::string plain(300 * 1024, '\0');
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<char>('a' + (i * 7 + i / 97) % 26);
    }
    EXPECT_EQ(decoded(http_content_coding::brotli, brotli_compress(plain), plain.size()), plain);
}
