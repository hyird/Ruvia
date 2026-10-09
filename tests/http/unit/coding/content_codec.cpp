#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_encoder.h"

#include "coding/http_content_coding.h"
#include "content_decoding_fixture.h"

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
        // Debug iterator proxies may be allocated inside noexcept STL code;
        // exercise codec state and payload allocation failures instead.
        if (bytes_value >= 32 && ++allocation_attempts_ == fail_at_) {
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

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        live_bytes_ += bytes_value;
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

RUVIA_TEST(http_content_decode_handles_deterministic_arbitrary_compressed_bytes) {
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
            const auto result_value = decode_http_content(
                coding, input, {.max_decoded_bytes_ = max_decoded_bytes, .resource_ = &resource});
            RUVIA_CHECK_EQ(static_cast<unsigned int>(result_value.decoded() != nullptr) +
                               static_cast<unsigned int>(result_value.failure() != nullptr),
                1U);
            if (const auto* decoded = result_value.decoded()) {
                RUVIA_CHECK(decoded->bytes().size() <= max_decoded_bytes);
            }
        }
    }
}

// The Content-Encoding field and the codecs behind it, independent of any message.

RUVIA_TEST(http_content_coding_field_mapping_is_protocol_generic) {
    const auto check_coding = [&](std::string_view value, http_content_coding expected) {
        const auto parsed_value = parse_http_content_coding(value);
        RUVIA_CHECK(parsed_value.invalid() == nullptr);
        RUVIA_CHECK(parsed_value.unsupported() == nullptr);
        RUVIA_CHECK_EQ(parsed_value.codings().size(), 1U);
        if (!parsed_value.codings().empty()) {
            RUVIA_CHECK(parsed_value.codings().front() == expected);
        }
    };
    check_coding("gzip", http_content_coding::gzip);
    check_coding("x-gzip", http_content_coding::gzip);
    check_coding("X-GZIP", http_content_coding::gzip);
    check_coding("GZIP", http_content_coding::gzip);
    check_coding("  br ", http_content_coding::brotli);
    check_coding("zstd", http_content_coding::zstd);
    check_coding("deflate", http_content_coding::deflate);
    check_coding("identity", http_content_coding::identity);
    const auto empty = parse_http_content_coding("");
    RUVIA_CHECK(empty.invalid() == nullptr);
    RUVIA_CHECK(empty.unsupported() == nullptr);
    RUVIA_CHECK(empty.codings().empty());

    constexpr std::array mappings{
        std::pair{http_content_coding::identity, std::string_view("identity")},
        std::pair{http_content_coding::gzip, std::string_view("gzip")},
        std::pair{http_content_coding::deflate, std::string_view("deflate")},
        std::pair{http_content_coding::brotli, std::string_view("br")},
        std::pair{http_content_coding::zstd, std::string_view("zstd")},
    };
    for (const auto& [coding, token] : mappings) {
        RUVIA_CHECK_EQ(ruvia::http_content_coding_token(coding), token);
        const auto parsed_value = parse_http_content_coding(token);
        RUVIA_CHECK_EQ(parsed_value.codings().size(), 1U);
        RUVIA_CHECK(parsed_value.codings().front() == coding);
    }

    const auto unsupported = parse_http_content_coding("compress");
    const auto stacked = parse_http_content_coding("gzip, br");
    RUVIA_CHECK(unsupported.invalid() == nullptr);
    RUVIA_CHECK(unsupported.unsupported() != nullptr);
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(stacked.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stacked.codings().size(), 2U);
    RUVIA_CHECK(stacked.codings()[0] == http_content_coding::gzip);
    RUVIA_CHECK(stacked.codings()[1] == http_content_coding::brotli);

    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        const auto invalid = parse_http_content_coding(value);
        RUVIA_CHECK(invalid.unsupported() == nullptr);
        RUVIA_CHECK(invalid.invalid() != nullptr);
        if (invalid.invalid() != nullptr) {
            RUVIA_CHECK_EQ(invalid.invalid()->status(), ruvia::http_status::bad_request);
        }
    }
}

RUVIA_TEST(http_deflate_content_uses_the_rfc9110_zlib_wrapper) {
    const std::string input(4096, 'd');
    auto encoded = encode_http_content(
        http_content_coding::deflate, input, {.max_encoded_bytes_ = input.size()});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (const auto* content = encoded.encoded()) {
        auto decoded = decode_http_content(http_content_coding::deflate, content->bytes(),
            {.max_decoded_bytes_ = input.size()});
        RUVIA_CHECK(decoded.decoded() != nullptr);
        if (const auto* output = decoded.decoded()) {
            RUVIA_CHECK_EQ(output->bytes(), input);
        }
    }
}

RUVIA_TEST(http_content_encoding_stack_round_trips_in_protocol_order) {
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate,
        http_content_coding::brotli};
    const std::string input(8192, 's');
    counting_memory_resource resource;
    {
        auto encoded = encode_http_content(codings, input,
            {.max_encoded_bytes_ = input.size(), .resource_ = &resource});
        RUVIA_CHECK(encoded.encoded() != nullptr);
        if (const auto* content = encoded.encoded()) {
            auto decoded = decode_http_content(codings, content->bytes(),
                {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
            RUVIA_CHECK(decoded.decoded() != nullptr);
            if (const auto* output = decoded.decoded()) {
                RUVIA_CHECK_EQ(output->bytes(), input);
            }
            auto limited = decode_http_content(codings, content->bytes(),
                {.max_decoded_bytes_ = input.size() - 1, .resource_ = &resource});
            RUVIA_CHECK(limited.failure() != nullptr);
            if (limited.failure() != nullptr) {
                RUVIA_CHECK(limited.failure()->error() ==
                            http_content_decode_error::decoded_size_exceeded);
            }
            std::string truncated(content->bytes());
            truncated.pop_back();
            auto incomplete = decode_http_content(codings, truncated,
                {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
            RUVIA_CHECK(incomplete.failure() != nullptr);
            if (incomplete.failure() != nullptr) {
                RUVIA_CHECK(incomplete.failure()->error() ==
                            http_content_decode_error::invalid_content);
            }
        }
        auto capped = encode_http_content(codings, input,
            {.max_encoded_bytes_ = 8, .resource_ = &resource});
        RUVIA_CHECK(capped.failure() != nullptr);
        if (capped.failure() != nullptr) {
            RUVIA_CHECK(capped.failure()->error() ==
                        ruvia::http_content_encode_error::encoded_size_exceeded);
        }
    }
    RUVIA_CHECK_EQ(resource.live_bytes(), 0U);
}

RUVIA_TEST(http_content_coding_parser_separates_capability_from_syntax) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::http_content_coding_field_parser unknown(
        ruvia::detail::http_field_list_role::recipient, &resource);
    unknown.update("compress");
    unknown.update("gzip");
    const auto unknown_result = std::move(unknown).finish();
    RUVIA_CHECK(unknown_result.invalid() == nullptr);
    RUVIA_CHECK(unknown_result.unsupported() != nullptr);

    ruvia::detail::http_content_coding_field_parser stacked(
        ruvia::detail::http_field_list_role::recipient, &resource);
    stacked.update("gzip");
    stacked.update("");
    stacked.update("br");
    const auto stacked_result = std::move(stacked).finish();
    RUVIA_CHECK(stacked_result.invalid() == nullptr);
    RUVIA_CHECK(stacked_result.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stacked_result.codings().size(), 2U);

    ruvia::detail::http_content_coding_field_parser malformed_after_unknown(
        ruvia::detail::http_field_list_role::recipient, &resource);
    malformed_after_unknown.update("compress");
    malformed_after_unknown.update("gzip;level=9");
    const auto malformed_result = std::move(malformed_after_unknown).finish();
    RUVIA_CHECK(malformed_result.unsupported() == nullptr);
    RUVIA_CHECK(malformed_result.invalid() != nullptr);
}

RUVIA_TEST(http_content_coding_empty_members_follow_field_list_role) {
    for (const std::string_view value : {"", ",gzip", "gzip,", "gzip,,br", "deflate,"}) {
        RUVIA_CHECK(ruvia::detail::is_valid_http_content_encoding_field_value(
            value, ruvia::detail::http_field_list_role::recipient));
        RUVIA_CHECK(!ruvia::detail::is_valid_http_content_encoding_field_value(
            value, ruvia::detail::http_field_list_role::sender));
    }

    RUVIA_CHECK(ruvia::detail::is_valid_http_content_encoding_field_value(
        "deflate", ruvia::detail::http_field_list_role::sender));
    RUVIA_CHECK(ruvia::detail::is_valid_http_content_encoding_field_value(
        "gzip, br", ruvia::detail::http_field_list_role::sender));
}

RUVIA_TEST(http_zstd_content_rejects_window_above_rfc9659_limit) {
    const std::string plain(9 * 1024 * 1024, 'w');
    const std::string encoded = zstd_compress_with_window(plain, 24);
    RUVIA_CHECK(!encoded.empty());
    RUVIA_CHECK(decode_error(http_content_coding::zstd, encoded, plain.size()) ==
                http_content_decode_error::invalid_content);

    auto conformant = encode_http_content(http_content_coding::zstd, plain,
        {.max_encoded_bytes_ = plain.size(), .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(conformant.encoded() != nullptr);
    if (const auto* content = conformant.encoded()) {
        RUVIA_CHECK_EQ(decoded(http_content_coding::zstd, content->bytes(), plain.size()), plain);
    }
}

RUVIA_TEST(http_content_encode_enforces_exact_cap_without_partial_output) {
    const std::string input(2048, 'e');
    for (const auto coding :
        {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
        const auto full = encode_http_content(coding, input,
            {.max_encoded_bytes_ = input.size(), .resource_ = std::pmr::get_default_resource()});
        RUVIA_CHECK(full.encoded() != nullptr);
        if (full.encoded() == nullptr) {
            continue;
        }
        const auto encoded_size = full.encoded()->bytes().size();
        RUVIA_CHECK(encoded_size > 1);

        const auto exact = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size, .resource_ = std::pmr::get_default_resource()});
        RUVIA_CHECK(exact.encoded() != nullptr);
        if (const auto* encoded = exact.encoded()) {
            RUVIA_CHECK_EQ(encoded->bytes().size(), encoded_size);
        }

        const auto too_small = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size - 1, .resource_ = std::pmr::get_default_resource()});
        RUVIA_CHECK(too_small.encoded() == nullptr);
        RUVIA_CHECK(too_small.failure() != nullptr);
        if (const auto* failure = too_small.failure()) {
            RUVIA_CHECK(failure->error() == http_content_encode_error::encoded_size_exceeded);
        }
    }

    const auto identity = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 8, .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(identity.encoded() != nullptr);
    RUVIA_CHECK(identity.failure() == nullptr);
    if (const auto* encoded = identity.encoded()) {
        RUVIA_CHECK_EQ(encoded->bytes(), std::string_view("identity"));
    }
    const auto identity_too_large = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 0, .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(identity_too_large.encoded() == nullptr);
    RUVIA_CHECK(identity_too_large.failure() != nullptr);
    if (const auto* failure = identity_too_large.failure()) {
        RUVIA_CHECK(failure->error() == http_content_encode_error::encoded_size_exceeded);
    }
}

RUVIA_TEST(http_content_encode_round_trips_across_output_block_boundaries) {
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
            RUVIA_CHECK(full.encoded() != nullptr);
            if (full.encoded() == nullptr) {
                continue;
            }
            const auto bytes_value = full.encoded()->bytes();
            RUVIA_CHECK(!bytes_value.empty());
            RUVIA_CHECK_EQ(decoded(coding, bytes_value, input.size()), input);
            if (size >= 8192) {
                RUVIA_CHECK(bytes_value.size() > 8192);
            }
            const auto exact = encode_http_content(coding, input, {.max_encoded_bytes_ = bytes_value.size()});
            RUVIA_CHECK(exact.encoded() != nullptr);
            if (exact.encoded() != nullptr) {
                RUVIA_CHECK_EQ(exact.encoded()->bytes(), bytes_value);
            }
            if (!bytes_value.empty()) {
                const auto too_small = encode_http_content(coding, input, {.max_encoded_bytes_ = bytes_value.size() - 1});
                RUVIA_CHECK(too_small.failure() != nullptr);
                if (too_small.failure() != nullptr) {
                    RUVIA_CHECK_EQ(too_small.failure()->error(), http_content_encode_error::encoded_size_exceeded);
                }
            }
        }
    }
}

RUVIA_TEST(http_content_whole_buffer_results_keep_their_memory_resource_and_release_failures) {
    const std::string plain(1024, 'r');
    constexpr std::array codings{http_content_coding::identity, http_content_coding::gzip,
        http_content_coding::brotli, http_content_coding::zstd};

    for (const auto coding : codings) {
        counting_memory_resource resource;
        {
            auto first_encoded = encode_http_content(coding, plain,
                {.max_encoded_bytes_ = plain.size() * 2, .resource_ = &resource});
            RUVIA_CHECK(first_encoded.encoded() != nullptr);
            if (first_encoded.encoded() == nullptr) {
                continue;
            }
            const auto encoded_bytes = first_encoded.encoded()->bytes();
            auto first_decoded = decode_http_content(coding, encoded_bytes,
                {.max_decoded_bytes_ = plain.size(), .resource_ = &resource});
            RUVIA_CHECK(first_decoded.decoded() != nullptr);
            if (first_decoded.decoded() == nullptr) {
                continue;
            }
            RUVIA_CHECK_EQ(first_decoded.decoded()->bytes(), std::string_view(plain));
            const auto retained_baseline = resource.live_bytes();

            for (int iteration = 0; iteration < 8; ++iteration) {
                {
                    auto repeated_encoded = encode_http_content(coding, plain,
                        {.max_encoded_bytes_ = plain.size() * 2, .resource_ = &resource});
                    RUVIA_CHECK(repeated_encoded.encoded() != nullptr);
                    if (repeated_encoded.encoded() != nullptr) {
                        RUVIA_CHECK_EQ(repeated_encoded.encoded()->bytes(), encoded_bytes);
                    }
                }
                RUVIA_CHECK_EQ(resource.live_bytes(), retained_baseline);
                {
                    auto repeated_decoded = decode_http_content(coding, encoded_bytes,
                        {.max_decoded_bytes_ = plain.size(), .resource_ = &resource});
                    RUVIA_CHECK(repeated_decoded.decoded() != nullptr);
                    if (repeated_decoded.decoded() != nullptr) {
                        RUVIA_CHECK_EQ(repeated_decoded.decoded()->bytes(), std::string_view(plain));
                    }
                }
                RUVIA_CHECK_EQ(resource.live_bytes(), retained_baseline);
                RUVIA_CHECK_EQ(first_encoded.encoded()->bytes(), encoded_bytes);
                RUVIA_CHECK_EQ(first_decoded.decoded()->bytes(), std::string_view(plain));
            }

            for (const std::size_t cap : {std::size_t{0}, std::size_t{1}}) {
                const auto encode_failure = encode_http_content(coding, plain,
                    {.max_encoded_bytes_ = cap, .resource_ = &resource});
                RUVIA_CHECK(encode_failure.encoded() == nullptr);
                RUVIA_CHECK(encode_failure.failure() != nullptr);
                if (encode_failure.failure() != nullptr) {
                    RUVIA_CHECK(encode_failure.failure()->error() ==
                                http_content_encode_error::encoded_size_exceeded);
                }
                RUVIA_CHECK_EQ(resource.live_bytes(), retained_baseline);

                const auto decode_failure = decode_http_content(coding, encoded_bytes,
                    {.max_decoded_bytes_ = cap, .resource_ = &resource});
                RUVIA_CHECK(decode_failure.decoded() == nullptr);
                RUVIA_CHECK(decode_failure.failure() != nullptr);
                if (decode_failure.failure() != nullptr) {
                    RUVIA_CHECK(decode_failure.failure()->error() ==
                                http_content_decode_error::decoded_size_exceeded);
                }
                RUVIA_CHECK_EQ(resource.live_bytes(), retained_baseline);
            }

            if (coding != http_content_coding::identity) {
                const auto invalid = decode_http_content(coding, "not valid coded data",
                    {.max_decoded_bytes_ = plain.size(), .resource_ = &resource});
                RUVIA_CHECK(invalid.decoded() == nullptr);
                RUVIA_CHECK(invalid.failure() != nullptr);
                if (invalid.failure() != nullptr) {
                    RUVIA_CHECK(invalid.failure()->error() ==
                                http_content_decode_error::invalid_content);
                }
                RUVIA_CHECK_EQ(resource.live_bytes(), retained_baseline);
            }

            const auto empty_string_bytes = [&] {
                const std::pmr::string empty(&resource);
                return resource.live_bytes() - retained_baseline;
            }();
            const auto* decoded_data = first_decoded.decoded()->bytes().data();
            auto decoded_bytes = std::move(*first_decoded.decoded()).take_bytes();
            RUVIA_CHECK(decoded_bytes.get_allocator().resource() == &resource);
            RUVIA_CHECK(decoded_bytes.data() == decoded_data);
            RUVIA_CHECK_EQ(std::string_view(decoded_bytes), std::string_view(plain));
            RUVIA_CHECK_EQ(first_encoded.encoded()->bytes(), encoded_bytes);
            // The moved-from result can retain an empty string's debug proxy.
            RUVIA_CHECK_EQ(resource.live_bytes(), retained_baseline + empty_string_bytes);
        }
        RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
    }

    bool encode_allocation_threw = false;
    try {
        (void)encode_http_content(http_content_coding::identity, plain,
            {.max_encoded_bytes_ = plain.size(), .resource_ = std::pmr::null_memory_resource()});
    } catch (const std::bad_alloc&) {
        encode_allocation_threw = true;
    }
    RUVIA_CHECK(encode_allocation_threw);

    bool decode_allocation_threw = false;
    try {
        (void)decode_http_content(http_content_coding::identity, plain,
            {.max_decoded_bytes_ = plain.size(), .resource_ = std::pmr::null_memory_resource()});
    } catch (const std::bad_alloc&) {
        decode_allocation_threw = true;
    }
    RUVIA_CHECK(decode_allocation_threw);
}

RUVIA_TEST(http_content_codecs_rethrow_allocator_exceptions_and_release_partial_state) {
    const struct {
        http_content_coding coding_;
        std::string encoded_;
    } cases[] = {
        {http_content_coding::gzip, gzip_compress({})},
        {http_content_coding::brotli, brotli_compress({})},
        {http_content_coding::zstd, zstd_compress({})},
    };

    for (const auto& test : cases) {
        for (const bool decode : {false, true}) {
            bool observed_allocation_exception = false;
            for (std::size_t fail_at = 1; fail_at <= 16; ++fail_at) {
                throwing_memory_resource resource(fail_at);
                try {
                    if (decode) {
                        auto result_value = decode_http_content(test.coding_, test.encoded_,
                            {.max_decoded_bytes_ = 0, .resource_ = &resource});
                        RUVIA_CHECK(result_value.decoded() != nullptr);
                    } else {
                        auto result_value = encode_http_content(test.coding_, {},
                            {.max_encoded_bytes_ = 1024, .resource_ = &resource});
                        RUVIA_CHECK(result_value.encoded() != nullptr);
                    }
                } catch (const codec_test_allocation_error&) {
                    observed_allocation_exception = true;
                }
                RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
                RUVIA_CHECK_EQ(resource.releases(), resource.allocations());
            }
            RUVIA_CHECK(observed_allocation_exception);
        }
    }
}

RUVIA_TEST(http_content_decoder_state_uses_the_callers_memory_resource) {
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
            test.coding_, test.encoded_, {.max_decoded_bytes_ = 0, .resource_ = &resource});
        RUVIA_CHECK(result_value.decoded() != nullptr);
        // The decoded representation is empty and stays in the string's SSO
        // buffer. Any observed allocation therefore belongs to codec state.
        RUVIA_CHECK(resource.allocations() != 0);
    }
}

RUVIA_TEST(http_content_encoder_round_trips_incremental_chunks) {
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
        RUVIA_CHECK_EQ(decoded(coding, encoded, repeated.size()), repeated);
    }
}

RUVIA_TEST(http_content_encoder_rejects_writes_after_finish) {
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
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(output.empty());
        encoder.finish(output);
        RUVIA_CHECK(output.empty());
    }
}

#if !defined(_MSC_VER)
// MSVC's debug pmr::string does not complete a growth operation when
// null_memory_resource throws. The same standard-library limitation is
// already accounted for by the response-head spill probe; keep this exact
// output-allocation failure contract on the other standard libraries.
RUVIA_TEST(http_content_encoder_failure_is_terminal) {
    http_content_encoder encoder(http_content_coding::identity, std::pmr::get_default_resource());
    std::pmr::string output(std::pmr::null_memory_resource());
    const std::string input(128, 'f');
    bool allocation_failed = false;
    try {
        encoder.write(input, output);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { encoder.finish(output); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { encoder.write("retry", output); }));
}
#endif  // !_MSC_VER

RUVIA_TEST(http_content_encoder_flushes_each_incremental_chunk) {
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
        RUVIA_CHECK_EQ(decoded(coding, encoded, input.size()), input);
    }
}

RUVIA_TEST(http_content_encoder_results_survive_subsequent_writes_and_encoder_destruction) {
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
                    RUVIA_CHECK_EQ(std::string_view(first), std::string_view(saved));
                }
                encoder.finish(rest);
            }
            RUVIA_CHECK_EQ(std::string_view(first), std::string_view(saved));
            saved.append(rest);
            RUVIA_CHECK_EQ(decoded(coding, saved, input.size() * 17), std::string(input.size() * 17, 'x'));
            RUVIA_CHECK(resource.live_bytes() != 0);
        }
        RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
    }
}

RUVIA_TEST(http_content_encoder_rethrows_codec_allocations_and_reclaims_partial_state) {
    const std::string input(4096, 'x');
    // Brotli's default encoder build exits on internal OOM instead of returning
    // through its C API. Exercise its recoverable constructor path separately.
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
                    RUVIA_CHECK_EQ(decoded(coding, output, input.size()), input);
                } catch (const codec_test_allocation_error&) {
                    ++failures;
                    RUVIA_CHECK(ruvia::testing::throws_on([&] { encoder.write("retry", output); }));
                    RUVIA_CHECK(ruvia::testing::throws_on([&] { encoder.finish(output); }));
                }
            } catch (const codec_test_allocation_error&) {
                ++failures;
            }
            RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
            RUVIA_CHECK_EQ(resource.allocations(), resource.releases());
        }
        RUVIA_CHECK(completed);
        RUVIA_CHECK(failures != 0);
    }
}

RUVIA_TEST(http_content_encoder_brotli_constructor_rethrows_allocator_exception) {
    for (const auto fail_at : {std::size_t{1}, std::size_t{2}}) {
        throwing_memory_resource resource(fail_at);
        bool failed = false;
        try {
            http_content_encoder encoder(http_content_coding::brotli, &resource);
        } catch (const codec_test_allocation_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocations(), resource.releases());
    }
}

RUVIA_TEST(http_content_encoder_rejects_unsupported_coding_and_reclaims_owner) {
    counting_memory_resource resource;
    bool rejected = false;
    try {
        http_content_encoder encoder(static_cast<http_content_coding>(255), &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
}

RUVIA_TEST(http_brotli_decode_checks_limits_and_complete_stream_after_output_blocks) {
    const std::string input(65537, 'b');
    const auto encoded = brotli_compress(input);
    for (const std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{16383},
             std::size_t{16384}, std::size_t{65536}}) {
        RUVIA_CHECK_EQ(decode_error(http_content_coding::brotli, encoded, cap), http_content_decode_error::decoded_size_exceeded);
    }
    RUVIA_CHECK_EQ(decoded(http_content_coding::brotli, encoded, input.size()), input);
    RUVIA_CHECK_EQ(decode_error(http_content_coding::brotli, std::string_view(encoded).substr(0, encoded.size() - 1), input.size()),
        http_content_decode_error::invalid_content);
    RUVIA_CHECK_EQ(decode_error(http_content_coding::brotli, encoded + "trailing", input.size()),
        http_content_decode_error::invalid_content);
}

RUVIA_TEST(http_brotli_encode_does_not_reserve_the_output_cap) {
    const std::string input(1u << 20, 'b');
    reject_output_cap_allocation_resource resource;
    bool completed = false;
    bool round_tripped = false;
    try {
        auto result_value = encode_http_content(http_content_coding::brotli, input,
            {.max_encoded_bytes_ = input.size() - 1, .resource_ = &resource});
        completed = result_value.encoded() != nullptr;
        if (const auto* encoded = result_value.encoded()) {
            round_tripped =
                decoded(http_content_coding::brotli, encoded->bytes(), input.size()) == input;
        }
    } catch (const std::bad_alloc&) {
    }
    RUVIA_CHECK(completed);
    RUVIA_CHECK(round_tripped);
}

RUVIA_TEST(http_identity_content_uses_the_default_resource_when_none_is_supplied) {
    const std::string input(1024, 'i');

    auto decoded = decode_http_content(http_content_coding::identity, input,
        {.max_decoded_bytes_ = input.size(), .resource_ = nullptr});
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (decoded.decoded() != nullptr) {
        auto bytes_value = std::move(*decoded.decoded()).take_bytes();
        RUVIA_CHECK_EQ(std::string_view(bytes_value), std::string_view(input));
        RUVIA_CHECK(bytes_value.get_allocator().resource() == std::pmr::get_default_resource());
    }

    auto encoded = encode_http_content(http_content_coding::identity, input,
        {.max_encoded_bytes_ = input.size(), .resource_ = nullptr});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() != nullptr) {
        auto bytes_value = std::move(*encoded.encoded()).take_bytes();
        RUVIA_CHECK_EQ(std::string_view(bytes_value), std::string_view(input));
        RUVIA_CHECK(bytes_value.get_allocator().resource() == std::pmr::get_default_resource());
    }
}

RUVIA_TEST(http_identity_content_rejects_oversize_before_allocating) {
    const std::string input(1024, 'i');

    const auto decoded = decode_http_content(http_content_coding::identity, input,
        {.max_decoded_bytes_ = input.size() - 1, .resource_ = std::pmr::null_memory_resource()});
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(decoded.failure()->error() == http_content_decode_error::decoded_size_exceeded);
    }

    const auto encoded = encode_http_content(http_content_coding::identity, input,
        {.max_encoded_bytes_ = input.size() - 1, .resource_ = std::pmr::null_memory_resource()});
    RUVIA_CHECK(encoded.encoded() == nullptr);
    RUVIA_CHECK(encoded.failure() != nullptr);
    if (encoded.failure() != nullptr) {
        RUVIA_CHECK(encoded.failure()->error() == http_content_encode_error::encoded_size_exceeded);
    }
}

RUVIA_TEST(http_content_decode_rejects_empty_encoded_input) {
    RUVIA_CHECK(
        decode_error(http_content_coding::gzip, {}) == http_content_decode_error::invalid_content);
    RUVIA_CHECK(
        decode_error(http_content_coding::brotli, {}) == http_content_decode_error::invalid_content);
    RUVIA_CHECK(
        decode_error(http_content_coding::zstd, {}) == http_content_decode_error::invalid_content);
    RUVIA_CHECK_EQ(decoded(http_content_coding::identity, {}, 0), std::string{});
}

RUVIA_TEST(http_content_decode_zero_cap_allows_only_empty_content) {
    const struct {
        http_content_coding coding_;
        std::string encoded_;
    } empty_cases[] = {
        {http_content_coding::gzip, gzip_compress({})},
        {http_content_coding::brotli, brotli_compress({})},
        {http_content_coding::zstd, zstd_compress({})},
    };
    for (const auto& test : empty_cases) {
        auto result_value = decode_http_content(test.coding_, test.encoded_,
            {.max_decoded_bytes_ = 0, .resource_ = std::pmr::get_default_resource()});
        RUVIA_CHECK(result_value.decoded() != nullptr);
        if (const auto* content = result_value.decoded()) {
            RUVIA_CHECK(content->bytes().empty());
        }
    }
    RUVIA_CHECK(decode_error(http_content_coding::gzip, gzip_compress("x"), 0) ==
                http_content_decode_error::decoded_size_exceeded);
}

RUVIA_TEST(zstd_decode_full_frame_succeeds) {
    const std::string plain(4096, 'z');  // compressible payload spanning a block
    const auto decoded = zstd_round_trip(plain, 0);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(*decoded, plain);
    }
}

RUVIA_TEST(zstd_decode_truncated_frame_rejected) {
    const std::string plain(4096, 'z');
    // Dropping the final bytes yields an incomplete frame that must be rejected,
    // matching the zlib/brotli decoders (regression guard for silent-truncation).
    RUVIA_CHECK(!zstd_round_trip(plain, 4).has_value());
}
