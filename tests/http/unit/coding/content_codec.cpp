#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpContentEncoder.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"

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
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Debug iterator proxies may be allocated inside noexcept STL code;
        // exercise codec state and payload allocation failures instead.
        if (bytes >= 32 && ++allocation_attempts_ == fail_at_) {
            throw codec_test_allocation_error();
        }
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live_bytes_ += bytes;
        ++allocations_;
        return allocation;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        live_bytes_ -= bytes;
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

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t liveBytes() const noexcept {
        return liveBytes_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations_;
        liveBytes_ += bytes;
        return allocation;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        liveBytes_ -= bytes;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
    std::size_t liveBytes_{0};
};

class RejectOutputCapAllocationResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // The Brotli state for a one-megabyte input is intentionally much
        // larger than the output cap. Reject only the cap-sized allocation so
        // the test remains about output reservation, not codec initialization.
        if (bytes >= (1u << 20) && bytes < (2u << 20)) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(http_content_decode_handles_deterministic_arbitrary_compressed_bytes) {
    std::uint64_t state = 0xC0DE'C0DE'5EED'F00DULL;
    const auto next = [&state]() {
        state ^= state << 7U;
        state ^= state >> 9U;
        return state;
    };

    for (std::size_t sample = 0; sample < 1024; ++sample) {
        std::string input(static_cast<std::size_t>(next() % 513U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next());
        }

        for (const auto coding : {HttpContentCoding::kGzip, HttpContentCoding::deflate,
                 HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
            const auto maxDecodedBytes = static_cast<std::size_t>(next() % 513U);
            std::pmr::monotonic_buffer_resource resource;
            const auto result = decodeHttpContent(
                coding, input, {.maxDecodedBytes = maxDecodedBytes, .resource = &resource});
            RUVIA_CHECK_EQ(static_cast<unsigned int>(result.decoded() != nullptr) +
                               static_cast<unsigned int>(result.failure() != nullptr),
                1U);
            if (const auto* decoded = result.decoded()) {
                RUVIA_CHECK(decoded->bytes().size() <= maxDecodedBytes);
            }
        }
    }
}

// The Content-Encoding field and the codecs behind it, independent of any message.

RUVIA_TEST(http_content_coding_field_mapping_is_protocol_generic) {
    const auto checkCoding = [&](std::string_view value, HttpContentCoding expected) {
        const auto parsed = parseHttpContentCoding(value);
        RUVIA_CHECK(parsed.invalid() == nullptr);
        RUVIA_CHECK(parsed.unsupported() == nullptr);
        RUVIA_CHECK_EQ(parsed.codings().size(), 1U);
        if (!parsed.codings().empty()) {
            RUVIA_CHECK(parsed.codings().front() == expected);
        }
    };
    checkCoding("gzip", HttpContentCoding::kGzip);
    checkCoding("x-gzip", HttpContentCoding::kGzip);
    checkCoding("X-GZIP", HttpContentCoding::kGzip);
    checkCoding("GZIP", HttpContentCoding::kGzip);
    checkCoding("  br ", HttpContentCoding::kBrotli);
    checkCoding("zstd", HttpContentCoding::kZstd);
    checkCoding("deflate", HttpContentCoding::deflate);
    checkCoding("identity", HttpContentCoding::kIdentity);
    const auto empty = parseHttpContentCoding("");
    RUVIA_CHECK(empty.invalid() == nullptr);
    RUVIA_CHECK(empty.unsupported() == nullptr);
    RUVIA_CHECK(empty.codings().empty());

    constexpr std::array mappings{
        std::pair{HttpContentCoding::kIdentity, std::string_view("identity")},
        std::pair{HttpContentCoding::kGzip, std::string_view("gzip")},
        std::pair{HttpContentCoding::deflate, std::string_view("deflate")},
        std::pair{HttpContentCoding::kBrotli, std::string_view("br")},
        std::pair{HttpContentCoding::kZstd, std::string_view("zstd")},
    };
    for (const auto& [coding, token] : mappings) {
        RUVIA_CHECK_EQ(ruvia::httpContentCodingToken(coding), token);
        const auto parsed = parseHttpContentCoding(token);
        RUVIA_CHECK_EQ(parsed.codings().size(), 1U);
        RUVIA_CHECK(parsed.codings().front() == coding);
    }

    const auto unsupported = parseHttpContentCoding("compress");
    const auto stacked = parseHttpContentCoding("gzip, br");
    RUVIA_CHECK(unsupported.invalid() == nullptr);
    RUVIA_CHECK(unsupported.unsupported() != nullptr);
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(stacked.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stacked.codings().size(), 2U);
    RUVIA_CHECK(stacked.codings()[0] == HttpContentCoding::kGzip);
    RUVIA_CHECK(stacked.codings()[1] == HttpContentCoding::kBrotli);

    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        const auto invalid = parseHttpContentCoding(value);
        RUVIA_CHECK(invalid.unsupported() == nullptr);
        RUVIA_CHECK(invalid.invalid() != nullptr);
        if (invalid.invalid() != nullptr) {
            RUVIA_CHECK_EQ(invalid.invalid()->status(), ruvia::http_status::kBadRequest);
        }
    }
}

RUVIA_TEST(http_deflate_content_uses_the_rfc9110_zlib_wrapper) {
    const std::string input(4096, 'd');
    auto encoded = encodeHttpContent(
        HttpContentCoding::deflate, input, {.maxEncodedBytes = input.size()});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (const auto* content = encoded.encoded()) {
        auto decoded = decodeHttpContent(HttpContentCoding::deflate, content->bytes(),
            {.maxDecodedBytes = input.size()});
        RUVIA_CHECK(decoded.decoded() != nullptr);
        if (const auto* output = decoded.decoded()) {
            RUVIA_CHECK_EQ(output->bytes(), input);
        }
    }
}

RUVIA_TEST(http_content_encoding_stack_round_trips_in_protocol_order) {
    constexpr std::array codings{HttpContentCoding::kGzip, HttpContentCoding::deflate,
        HttpContentCoding::kBrotli};
    const std::string input(8192, 's');
    CountingMemoryResource resource;
    {
        auto encoded = encodeHttpContent(codings, input,
            {.maxEncodedBytes = input.size(), .resource = &resource});
        RUVIA_CHECK(encoded.encoded() != nullptr);
        if (const auto* content = encoded.encoded()) {
            auto decoded = decodeHttpContent(codings, content->bytes(),
                {.maxDecodedBytes = input.size(), .resource = &resource});
            RUVIA_CHECK(decoded.decoded() != nullptr);
            if (const auto* output = decoded.decoded()) {
                RUVIA_CHECK_EQ(output->bytes(), input);
            }
            auto limited = decodeHttpContent(codings, content->bytes(),
                {.maxDecodedBytes = input.size() - 1, .resource = &resource});
            RUVIA_CHECK(limited.failure() != nullptr);
            if (limited.failure() != nullptr) {
                RUVIA_CHECK(limited.failure()->error() ==
                            HttpContentDecodeError::kDecodedSizeExceeded);
            }
            std::string truncated(content->bytes());
            truncated.pop_back();
            auto incomplete = decodeHttpContent(codings, truncated,
                {.maxDecodedBytes = input.size(), .resource = &resource});
            RUVIA_CHECK(incomplete.failure() != nullptr);
            if (incomplete.failure() != nullptr) {
                RUVIA_CHECK(incomplete.failure()->error() ==
                            HttpContentDecodeError::kInvalidContent);
            }
        }
        auto capped = encodeHttpContent(codings, input,
            {.maxEncodedBytes = 8, .resource = &resource});
        RUVIA_CHECK(capped.failure() != nullptr);
        if (capped.failure() != nullptr) {
            RUVIA_CHECK(capped.failure()->error() ==
                        ruvia::HttpContentEncodeError::kEncodedSizeExceeded);
        }
    }
    RUVIA_CHECK_EQ(resource.liveBytes(), 0U);
}

RUVIA_TEST(http_content_coding_parser_separates_capability_from_syntax) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::HttpContentCodingFieldParser unknown(
        ruvia::detail::HttpFieldListRole::kRecipient, &resource);
    unknown.update("compress");
    unknown.update("gzip");
    const auto unknownResult = std::move(unknown).finish();
    RUVIA_CHECK(unknownResult.invalid() == nullptr);
    RUVIA_CHECK(unknownResult.unsupported() != nullptr);

    ruvia::detail::HttpContentCodingFieldParser stacked(
        ruvia::detail::HttpFieldListRole::kRecipient, &resource);
    stacked.update("gzip");
    stacked.update("");
    stacked.update("br");
    const auto stackedResult = std::move(stacked).finish();
    RUVIA_CHECK(stackedResult.invalid() == nullptr);
    RUVIA_CHECK(stackedResult.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stackedResult.codings().size(), 2U);

    ruvia::detail::HttpContentCodingFieldParser malformedAfterUnknown(
        ruvia::detail::HttpFieldListRole::kRecipient, &resource);
    malformedAfterUnknown.update("compress");
    malformedAfterUnknown.update("gzip;level=9");
    const auto malformedResult = std::move(malformedAfterUnknown).finish();
    RUVIA_CHECK(malformedResult.unsupported() == nullptr);
    RUVIA_CHECK(malformedResult.invalid() != nullptr);
}

RUVIA_TEST(http_content_coding_empty_members_follow_field_list_role) {
    for (const std::string_view value : {"", ",gzip", "gzip,", "gzip,,br", "deflate,"}) {
        RUVIA_CHECK(ruvia::detail::isValidHttpContentEncodingFieldValue(
            value, ruvia::detail::HttpFieldListRole::kRecipient));
        RUVIA_CHECK(!ruvia::detail::isValidHttpContentEncodingFieldValue(
            value, ruvia::detail::HttpFieldListRole::kSender));
    }

    RUVIA_CHECK(ruvia::detail::isValidHttpContentEncodingFieldValue(
        "deflate", ruvia::detail::HttpFieldListRole::kSender));
    RUVIA_CHECK(ruvia::detail::isValidHttpContentEncodingFieldValue(
        "gzip, br", ruvia::detail::HttpFieldListRole::kSender));
}

RUVIA_TEST(http_zstd_content_rejects_window_above_rfc9659_limit) {
    const std::string plain(9 * 1024 * 1024, 'w');
    const std::string encoded = zstdCompressWithWindow(plain, 24);
    RUVIA_CHECK(!encoded.empty());
    RUVIA_CHECK(decodeError(HttpContentCoding::kZstd, encoded, plain.size()) ==
                HttpContentDecodeError::kInvalidContent);

    auto conformant = encodeHttpContent(HttpContentCoding::kZstd, plain,
        {.maxEncodedBytes = plain.size(), .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(conformant.encoded() != nullptr);
    if (const auto* content = conformant.encoded()) {
        RUVIA_CHECK_EQ(decoded(HttpContentCoding::kZstd, content->bytes(), plain.size()), plain);
    }
}

RUVIA_TEST(http_content_encode_enforces_exact_cap_without_partial_output) {
    const std::string input(2048, 'e');
    for (const auto coding :
        {HttpContentCoding::kGzip, HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
        const auto full = encodeHttpContent(coding, input,
            {.maxEncodedBytes = input.size(), .resource = std::pmr::get_default_resource()});
        RUVIA_CHECK(full.encoded() != nullptr);
        if (full.encoded() == nullptr) {
            continue;
        }
        const auto encodedSize = full.encoded()->bytes().size();
        RUVIA_CHECK(encodedSize > 1);

        const auto exact = encodeHttpContent(coding, input,
            {.maxEncodedBytes = encodedSize, .resource = std::pmr::get_default_resource()});
        RUVIA_CHECK(exact.encoded() != nullptr);
        if (const auto* encoded = exact.encoded()) {
            RUVIA_CHECK_EQ(encoded->bytes().size(), encodedSize);
        }

        const auto tooSmall = encodeHttpContent(coding, input,
            {.maxEncodedBytes = encodedSize - 1, .resource = std::pmr::get_default_resource()});
        RUVIA_CHECK(tooSmall.encoded() == nullptr);
        RUVIA_CHECK(tooSmall.failure() != nullptr);
        if (const auto* failure = tooSmall.failure()) {
            RUVIA_CHECK(failure->error() == HttpContentEncodeError::kEncodedSizeExceeded);
        }
    }

    const auto identity = encodeHttpContent(HttpContentCoding::kIdentity, "identity",
        {.maxEncodedBytes = 8, .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(identity.encoded() != nullptr);
    RUVIA_CHECK(identity.failure() == nullptr);
    if (const auto* encoded = identity.encoded()) {
        RUVIA_CHECK_EQ(encoded->bytes(), std::string_view("identity"));
    }
    const auto identityTooLarge = encodeHttpContent(HttpContentCoding::kIdentity, "identity",
        {.maxEncodedBytes = 0, .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(identityTooLarge.encoded() == nullptr);
    RUVIA_CHECK(identityTooLarge.failure() != nullptr);
    if (const auto* failure = identityTooLarge.failure()) {
        RUVIA_CHECK(failure->error() == HttpContentEncodeError::kEncodedSizeExceeded);
    }
}

RUVIA_TEST(http_content_encode_round_trips_across_output_block_boundaries) {
    for (const std::size_t size : {std::size_t{0}, std::size_t{8191}, std::size_t{8192},
             std::size_t{8193}, std::size_t{65537}, std::size_t{(4u << 20) + 1}}) {
        std::string input;
        input.reserve(size);
        std::uint32_t state = 0x12345678;
        for (std::size_t i = 0; i < size; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            input.push_back(static_cast<char>(state >> 24));
        }
        for (const auto coding : {HttpContentCoding::kGzip, HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
            const auto full = encodeHttpContent(coding, input, {.maxEncodedBytes = size * 2 + 1024});
            RUVIA_CHECK(full.encoded() != nullptr);
            if (full.encoded() == nullptr) {
                continue;
            }
            const auto bytes = full.encoded()->bytes();
            RUVIA_CHECK(!bytes.empty());
            RUVIA_CHECK_EQ(decoded(coding, bytes, input.size()), input);
            if (size >= 8192) {
                RUVIA_CHECK(bytes.size() > 8192);
            }
            const auto exact = encodeHttpContent(coding, input, {.maxEncodedBytes = bytes.size()});
            RUVIA_CHECK(exact.encoded() != nullptr);
            if (exact.encoded() != nullptr) {
                RUVIA_CHECK_EQ(exact.encoded()->bytes(), bytes);
            }
            if (!bytes.empty()) {
                const auto tooSmall = encodeHttpContent(coding, input, {.maxEncodedBytes = bytes.size() - 1});
                RUVIA_CHECK(tooSmall.failure() != nullptr);
                if (tooSmall.failure() != nullptr) {
                    RUVIA_CHECK_EQ(tooSmall.failure()->error(), HttpContentEncodeError::kEncodedSizeExceeded);
                }
            }
        }
    }
}

RUVIA_TEST(http_content_whole_buffer_results_keep_their_memory_resource_and_release_failures) {
    const std::string plain(1024, 'r');
    constexpr std::array codings{HttpContentCoding::kIdentity, HttpContentCoding::kGzip,
        HttpContentCoding::kBrotli, HttpContentCoding::kZstd};

    for (const auto coding : codings) {
        CountingMemoryResource resource;
        {
            auto firstEncoded = encodeHttpContent(coding, plain,
                {.maxEncodedBytes = plain.size() * 2, .resource = &resource});
            RUVIA_CHECK(firstEncoded.encoded() != nullptr);
            if (firstEncoded.encoded() == nullptr) {
                continue;
            }
            const auto encodedBytes = firstEncoded.encoded()->bytes();
            auto firstDecoded = decodeHttpContent(coding, encodedBytes,
                {.maxDecodedBytes = plain.size(), .resource = &resource});
            RUVIA_CHECK(firstDecoded.decoded() != nullptr);
            if (firstDecoded.decoded() == nullptr) {
                continue;
            }
            RUVIA_CHECK_EQ(firstDecoded.decoded()->bytes(), std::string_view(plain));
            const auto retainedBaseline = resource.liveBytes();

            for (int iteration = 0; iteration < 8; ++iteration) {
                {
                    auto repeatedEncoded = encodeHttpContent(coding, plain,
                        {.maxEncodedBytes = plain.size() * 2, .resource = &resource});
                    RUVIA_CHECK(repeatedEncoded.encoded() != nullptr);
                    if (repeatedEncoded.encoded() != nullptr) {
                        RUVIA_CHECK_EQ(repeatedEncoded.encoded()->bytes(), encodedBytes);
                    }
                }
                RUVIA_CHECK_EQ(resource.liveBytes(), retainedBaseline);
                {
                    auto repeatedDecoded = decodeHttpContent(coding, encodedBytes,
                        {.maxDecodedBytes = plain.size(), .resource = &resource});
                    RUVIA_CHECK(repeatedDecoded.decoded() != nullptr);
                    if (repeatedDecoded.decoded() != nullptr) {
                        RUVIA_CHECK_EQ(repeatedDecoded.decoded()->bytes(), std::string_view(plain));
                    }
                }
                RUVIA_CHECK_EQ(resource.liveBytes(), retainedBaseline);
                RUVIA_CHECK_EQ(firstEncoded.encoded()->bytes(), encodedBytes);
                RUVIA_CHECK_EQ(firstDecoded.decoded()->bytes(), std::string_view(plain));
            }

            for (const std::size_t cap : {std::size_t{0}, std::size_t{1}}) {
                const auto encodeFailure = encodeHttpContent(coding, plain,
                    {.maxEncodedBytes = cap, .resource = &resource});
                RUVIA_CHECK(encodeFailure.encoded() == nullptr);
                RUVIA_CHECK(encodeFailure.failure() != nullptr);
                if (encodeFailure.failure() != nullptr) {
                    RUVIA_CHECK(encodeFailure.failure()->error() ==
                                HttpContentEncodeError::kEncodedSizeExceeded);
                }
                RUVIA_CHECK_EQ(resource.liveBytes(), retainedBaseline);

                const auto decodeFailure = decodeHttpContent(coding, encodedBytes,
                    {.maxDecodedBytes = cap, .resource = &resource});
                RUVIA_CHECK(decodeFailure.decoded() == nullptr);
                RUVIA_CHECK(decodeFailure.failure() != nullptr);
                if (decodeFailure.failure() != nullptr) {
                    RUVIA_CHECK(decodeFailure.failure()->error() ==
                                HttpContentDecodeError::kDecodedSizeExceeded);
                }
                RUVIA_CHECK_EQ(resource.liveBytes(), retainedBaseline);
            }

            if (coding != HttpContentCoding::kIdentity) {
                const auto invalid = decodeHttpContent(coding, "not valid coded data",
                    {.maxDecodedBytes = plain.size(), .resource = &resource});
                RUVIA_CHECK(invalid.decoded() == nullptr);
                RUVIA_CHECK(invalid.failure() != nullptr);
                if (invalid.failure() != nullptr) {
                    RUVIA_CHECK(invalid.failure()->error() ==
                                HttpContentDecodeError::kInvalidContent);
                }
                RUVIA_CHECK_EQ(resource.liveBytes(), retainedBaseline);
            }

            const auto empty_string_bytes = [&] {
                const std::pmr::string empty(&resource);
                return resource.liveBytes() - retainedBaseline;
            }();
            const auto* decoded_data = firstDecoded.decoded()->bytes().data();
            auto decodedBytes = std::move(*firstDecoded.decoded()).takeBytes();
            RUVIA_CHECK(decodedBytes.get_allocator().resource() == &resource);
            RUVIA_CHECK(decodedBytes.data() == decoded_data);
            RUVIA_CHECK_EQ(std::string_view(decodedBytes), std::string_view(plain));
            RUVIA_CHECK_EQ(firstEncoded.encoded()->bytes(), encodedBytes);
            // The moved-from result can retain an empty string's debug proxy.
            RUVIA_CHECK_EQ(resource.liveBytes(), retainedBaseline + empty_string_bytes);
        }
        RUVIA_CHECK_EQ(resource.liveBytes(), std::size_t{0});
    }

    bool encodeAllocationThrew = false;
    try {
        (void)encodeHttpContent(HttpContentCoding::kIdentity, plain,
            {.maxEncodedBytes = plain.size(), .resource = std::pmr::null_memory_resource()});
    } catch (const std::bad_alloc&) {
        encodeAllocationThrew = true;
    }
    RUVIA_CHECK(encodeAllocationThrew);

    bool decodeAllocationThrew = false;
    try {
        (void)decodeHttpContent(HttpContentCoding::kIdentity, plain,
            {.maxDecodedBytes = plain.size(), .resource = std::pmr::null_memory_resource()});
    } catch (const std::bad_alloc&) {
        decodeAllocationThrew = true;
    }
    RUVIA_CHECK(decodeAllocationThrew);
}

RUVIA_TEST(http_content_codecs_rethrow_allocator_exceptions_and_release_partial_state) {
    const struct {
        HttpContentCoding coding;
        std::string encoded;
    } cases[] = {
        {HttpContentCoding::kGzip, gzipCompress({})},
        {HttpContentCoding::kBrotli, brotliCompress({})},
        {HttpContentCoding::kZstd, zstdCompress({})},
    };

    for (const auto& test : cases) {
        for (const bool decode : {false, true}) {
            bool observed_allocation_exception = false;
            for (std::size_t fail_at = 1; fail_at <= 16; ++fail_at) {
                throwing_memory_resource resource(fail_at);
                try {
                    if (decode) {
                        auto result = decodeHttpContent(test.coding, test.encoded,
                            {.maxDecodedBytes = 0, .resource = &resource});
                        RUVIA_CHECK(result.decoded() != nullptr);
                    } else {
                        auto result = encodeHttpContent(test.coding, {},
                            {.maxEncodedBytes = 1024, .resource = &resource});
                        RUVIA_CHECK(result.encoded() != nullptr);
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
        HttpContentCoding coding;
        std::string encoded;
    } cases[] = {
        {HttpContentCoding::kGzip, gzipCompress({})},
        {HttpContentCoding::kBrotli, brotliCompress({})},
        {HttpContentCoding::kZstd, zstdCompress({})},
    };

    for (const auto& test : cases) {
        CountingMemoryResource resource;
        const auto result = decodeHttpContent(
            test.coding, test.encoded, {.maxDecodedBytes = 0, .resource = &resource});
        RUVIA_CHECK(result.decoded() != nullptr);
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

    for (const auto coding : {HttpContentCoding::kIdentity, HttpContentCoding::kGzip,
             HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
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
    for (const auto coding : {HttpContentCoding::kIdentity, HttpContentCoding::kGzip,
             HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
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
    http_content_encoder encoder(HttpContentCoding::kIdentity, std::pmr::get_default_resource());
    std::pmr::string output(std::pmr::null_memory_resource());
    const std::string input(128, 'f');
    bool allocation_failed = false;
    try {
        encoder.write(input, output);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { encoder.finish(output); }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { encoder.write("retry", output); }));
}
#endif  // !_MSC_VER

RUVIA_TEST(http_content_encoder_flushes_each_incremental_chunk) {
    const std::string input(4096, 's');

    for (const auto coding :
        {HttpContentCoding::kGzip, HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
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
    for (const auto coding : {HttpContentCoding::kGzip, HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
        CountingMemoryResource resource;
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
            RUVIA_CHECK(resource.liveBytes() != 0);
        }
        RUVIA_CHECK_EQ(resource.liveBytes(), std::size_t{0});
    }
}

RUVIA_TEST(http_content_encoder_rethrows_codec_allocations_and_reclaims_partial_state) {
    const std::string input(4096, 'x');
    // Brotli's default encoder build exits on internal OOM instead of returning
    // through its C API. Exercise its recoverable constructor path separately.
    for (const auto coding : {HttpContentCoding::kGzip, HttpContentCoding::deflate,
             HttpContentCoding::kZstd}) {
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
                    RUVIA_CHECK(ruvia::testing::throwsOn([&] { encoder.write("retry", output); }));
                    RUVIA_CHECK(ruvia::testing::throwsOn([&] { encoder.finish(output); }));
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
            http_content_encoder encoder(HttpContentCoding::kBrotli, &resource);
        } catch (const codec_test_allocation_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocations(), resource.releases());
    }
}

RUVIA_TEST(http_content_encoder_rejects_unsupported_coding_and_reclaims_owner) {
    CountingMemoryResource resource;
    bool rejected = false;
    try {
        http_content_encoder encoder(static_cast<HttpContentCoding>(255), &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.liveBytes(), std::size_t{0});
}

RUVIA_TEST(http_brotli_decode_checks_limits_and_complete_stream_after_output_blocks) {
    const std::string input(65537, 'b');
    const auto encoded = brotliCompress(input);
    for (const std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{16383},
             std::size_t{16384}, std::size_t{65536}}) {
        RUVIA_CHECK_EQ(decodeError(HttpContentCoding::kBrotli, encoded, cap), HttpContentDecodeError::kDecodedSizeExceeded);
    }
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kBrotli, encoded, input.size()), input);
    RUVIA_CHECK_EQ(decodeError(HttpContentCoding::kBrotli, std::string_view(encoded).substr(0, encoded.size() - 1), input.size()),
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK_EQ(decodeError(HttpContentCoding::kBrotli, encoded + "trailing", input.size()),
        HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(http_brotli_encode_does_not_reserve_the_output_cap) {
    const std::string input(1u << 20, 'b');
    RejectOutputCapAllocationResource resource;
    bool completed = false;
    bool roundTripped = false;
    try {
        auto result = encodeHttpContent(HttpContentCoding::kBrotli, input,
            {.maxEncodedBytes = input.size() - 1, .resource = &resource});
        completed = result.encoded() != nullptr;
        if (const auto* encoded = result.encoded()) {
            roundTripped =
                decoded(HttpContentCoding::kBrotli, encoded->bytes(), input.size()) == input;
        }
    } catch (const std::bad_alloc&) {
    }
    RUVIA_CHECK(completed);
    RUVIA_CHECK(roundTripped);
}

RUVIA_TEST(http_identity_content_uses_the_default_resource_when_none_is_supplied) {
    const std::string input(1024, 'i');

    auto decoded = decodeHttpContent(HttpContentCoding::kIdentity, input,
        {.maxDecodedBytes = input.size(), .resource = nullptr});
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (decoded.decoded() != nullptr) {
        auto bytes = std::move(*decoded.decoded()).takeBytes();
        RUVIA_CHECK_EQ(std::string_view(bytes), std::string_view(input));
        RUVIA_CHECK(bytes.get_allocator().resource() == std::pmr::get_default_resource());
    }

    auto encoded = encodeHttpContent(HttpContentCoding::kIdentity, input,
        {.maxEncodedBytes = input.size(), .resource = nullptr});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() != nullptr) {
        auto bytes = std::move(*encoded.encoded()).takeBytes();
        RUVIA_CHECK_EQ(std::string_view(bytes), std::string_view(input));
        RUVIA_CHECK(bytes.get_allocator().resource() == std::pmr::get_default_resource());
    }
}

RUVIA_TEST(http_identity_content_rejects_oversize_before_allocating) {
    const std::string input(1024, 'i');

    const auto decoded = decodeHttpContent(HttpContentCoding::kIdentity, input,
        {.maxDecodedBytes = input.size() - 1, .resource = std::pmr::null_memory_resource()});
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(decoded.failure()->error() == HttpContentDecodeError::kDecodedSizeExceeded);
    }

    const auto encoded = encodeHttpContent(HttpContentCoding::kIdentity, input,
        {.maxEncodedBytes = input.size() - 1, .resource = std::pmr::null_memory_resource()});
    RUVIA_CHECK(encoded.encoded() == nullptr);
    RUVIA_CHECK(encoded.failure() != nullptr);
    if (encoded.failure() != nullptr) {
        RUVIA_CHECK(encoded.failure()->error() == HttpContentEncodeError::kEncodedSizeExceeded);
    }
}

RUVIA_TEST(http_content_decode_rejects_empty_encoded_input) {
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kGzip, {}) == HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kBrotli, {}) == HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kZstd, {}) == HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kIdentity, {}, 0), std::string{});
}

RUVIA_TEST(http_content_decode_zero_cap_allows_only_empty_content) {
    const struct {
        HttpContentCoding coding;
        std::string encoded;
    } emptyCases[] = {
        {HttpContentCoding::kGzip, gzipCompress({})},
        {HttpContentCoding::kBrotli, brotliCompress({})},
        {HttpContentCoding::kZstd, zstdCompress({})},
    };
    for (const auto& test : emptyCases) {
        auto result = decodeHttpContent(test.coding, test.encoded,
            {.maxDecodedBytes = 0, .resource = std::pmr::get_default_resource()});
        RUVIA_CHECK(result.decoded() != nullptr);
        if (const auto* content = result.decoded()) {
            RUVIA_CHECK(content->bytes().empty());
        }
    }
    RUVIA_CHECK(decodeError(HttpContentCoding::kGzip, gzipCompress("x"), 0) ==
                HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(zstd_decode_full_frame_succeeds) {
    const std::string plain(4096, 'z');  // compressible payload spanning a block
    const auto decoded = zstdRoundTrip(plain, 0);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(*decoded, plain);
    }
}

RUVIA_TEST(zstd_decode_truncated_frame_rejected) {
    const std::string plain(4096, 'z');
    // Dropping the final bytes yields an incomplete frame that must be rejected,
    // matching the zlib/brotli decoders (regression guard for silent-truncation).
    RUVIA_CHECK(!zstdRoundTrip(plain, 4).has_value());
}
