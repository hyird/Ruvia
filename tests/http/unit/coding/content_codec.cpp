#include "content_decoding_fixture.h"

#include <cstdint>

#include "ruvia/http/detail/coding/HttpContentEncoder.h"

using ruvia::detail::HttpContentEncoder;
using ruvia::detail::HttpContentEncodeStep;

namespace {

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
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

        for (const auto coding :
            {HttpContentCoding::kGzip, HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
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
        RUVIA_CHECK(parsed.coding() != nullptr);
        if (parsed.coding() != nullptr) {
            RUVIA_CHECK(*parsed.coding() == expected);
        }
    };
    checkCoding("gzip", HttpContentCoding::kGzip);
    checkCoding("x-gzip", HttpContentCoding::kGzip);
    checkCoding("GZIP", HttpContentCoding::kGzip);
    checkCoding("  br ", HttpContentCoding::kBrotli);
    checkCoding("zstd", HttpContentCoding::kZstd);
    checkCoding("identity", HttpContentCoding::kIdentity);
    checkCoding("", HttpContentCoding::kIdentity);

    constexpr std::array mappings{
        std::pair{HttpContentCoding::kIdentity, std::string_view("identity")},
        std::pair{HttpContentCoding::kGzip, std::string_view("gzip")},
        std::pair{HttpContentCoding::kBrotli, std::string_view("br")},
        std::pair{HttpContentCoding::kZstd, std::string_view("zstd")},
    };
    for (const auto& [coding, token] : mappings) {
        RUVIA_CHECK_EQ(ruvia::httpContentCodingToken(coding), token);
        const auto parsed = parseHttpContentCoding(token);
        RUVIA_CHECK(parsed.coding() != nullptr);
        if (parsed.coding() != nullptr) {
            RUVIA_CHECK(*parsed.coding() == coding);
        }
    }

    const auto unsupported = parseHttpContentCoding("deflate");
    const auto stacked = parseHttpContentCoding("gzip, br");
    RUVIA_CHECK(unsupported.invalid() == nullptr);
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(unsupported.unsupported() != nullptr);
    RUVIA_CHECK(stacked.unsupported() != nullptr);

    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        const auto invalid = parseHttpContentCoding(value);
        RUVIA_CHECK(invalid.coding() == nullptr);
        RUVIA_CHECK(invalid.unsupported() == nullptr);
        RUVIA_CHECK(invalid.invalid() != nullptr);
        if (invalid.invalid() != nullptr) {
            RUVIA_CHECK_EQ(invalid.invalid()->status(), ruvia::http_status::kBadRequest);
        }
    }
}

RUVIA_TEST(http_content_coding_parser_separates_capability_from_syntax) {
    ruvia::detail::HttpContentCodingFieldParser unknown;
    unknown.update("deflate");
    unknown.update("gzip");
    const auto unknownResult = unknown.finish();
    RUVIA_CHECK(unknownResult.invalid() == nullptr);
    RUVIA_CHECK(unknownResult.unsupported() != nullptr);

    ruvia::detail::HttpContentCodingFieldParser stacked;
    stacked.update("gzip");
    stacked.update("");
    stacked.update("br");
    const auto stackedResult = stacked.finish();
    RUVIA_CHECK(stackedResult.invalid() == nullptr);
    RUVIA_CHECK(stackedResult.unsupported() != nullptr);

    ruvia::detail::HttpContentCodingFieldParser malformedAfterUnknown;
    malformedAfterUnknown.update("deflate");
    malformedAfterUnknown.update("gzip;level=9");
    const auto malformedResult = malformedAfterUnknown.finish();
    RUVIA_CHECK(malformedResult.coding() == nullptr);
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
        HttpContentEncoder encoder(coding, std::pmr::get_default_resource());
        for (std::size_t offset = 0; offset < repeated.size();) {
            const auto size = std::min<std::size_t>(13, repeated.size() - offset);
            chunk.clear();
            const auto step = encoder.write(std::string_view(repeated).substr(offset, size), chunk);
            RUVIA_CHECK(step != HttpContentEncodeStep::kFailure);
            encoded.append(chunk);
            offset += size;
        }
        chunk.clear();
        RUVIA_CHECK(encoder.finish(chunk) == HttpContentEncodeStep::kFinished);
        encoded.append(chunk);
        RUVIA_CHECK_EQ(decoded(coding, encoded, repeated.size()), repeated);
    }
}

RUVIA_TEST(http_content_encoder_rejects_writes_after_finish) {
    for (const auto coding : {HttpContentCoding::kIdentity, HttpContentCoding::kGzip,
             HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
        HttpContentEncoder encoder(coding, std::pmr::get_default_resource());
        std::pmr::string output(std::pmr::get_default_resource());
        RUVIA_CHECK(encoder.write("body", output) != HttpContentEncodeStep::kFailure);
        output.clear();
        RUVIA_CHECK(encoder.finish(output) == HttpContentEncodeStep::kFinished);
        output.clear();
        RUVIA_CHECK(encoder.write("late body", output) == HttpContentEncodeStep::kFailure);
        RUVIA_CHECK(output.empty());
        RUVIA_CHECK(encoder.finish(output) == HttpContentEncodeStep::kFinished);
    }
}

#if !defined(_MSC_VER)
// MSVC's debug pmr::string does not complete a growth operation when
// null_memory_resource throws. The same standard-library limitation is
// already accounted for by the response-head spill probe; keep this exact
// output-allocation failure contract on the other standard libraries.
RUVIA_TEST(http_content_encoder_failure_is_terminal) {
    HttpContentEncoder encoder(HttpContentCoding::kIdentity, std::pmr::get_default_resource());
    std::pmr::string output(std::pmr::null_memory_resource());
    const std::string input(128, 'f');
    RUVIA_CHECK(encoder.write(input, output) == HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(encoder.finish(output) == HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(encoder.write("retry", output) == HttpContentEncodeStep::kFailure);
}
#endif  // !_MSC_VER

RUVIA_TEST(http_content_encoder_flushes_each_incremental_chunk) {
    const std::string input(4096, 's');

    for (const auto coding :
        {HttpContentCoding::kGzip, HttpContentCoding::kBrotli, HttpContentCoding::kZstd}) {
        std::pmr::string encoded(std::pmr::get_default_resource());
        std::pmr::string chunk(std::pmr::get_default_resource());
        HttpContentEncoder encoder(coding, std::pmr::get_default_resource());
        for (std::size_t offset = 0; offset < input.size();) {
            const auto size = std::min<std::size_t>(257, input.size() - offset);
            chunk.clear();
            const auto step =
                encoder.write(std::string_view(input).substr(offset, size), chunk, true);
            RUVIA_CHECK(step != HttpContentEncodeStep::kFailure);
            encoded.append(chunk);
            offset += size;
        }
        chunk.clear();
        RUVIA_CHECK(encoder.finish(chunk) == HttpContentEncodeStep::kFinished);
        encoded.append(chunk);
        RUVIA_CHECK_EQ(decoded(coding, encoded, input.size()), input);
    }
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
