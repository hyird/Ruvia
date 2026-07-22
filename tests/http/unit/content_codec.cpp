#include "content_decoding_fixture.h"

// The Content-Encoding field and the codecs behind it, independent of any message.

RUVIA_TEST(http_content_coding_field_mapping_is_protocol_generic) {
    const auto checkCoding = [&](std::string_view value, HttpContentCoding expected) {
        const auto parsed = httpContentCodingFromFieldValue(value);
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

    const auto unsupported = httpContentCodingFromFieldValue("deflate");
    const auto stacked = httpContentCodingFromFieldValue("gzip, br");
    RUVIA_CHECK(unsupported.invalid() == nullptr);
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(unsupported.unsupported() != nullptr);
    RUVIA_CHECK(stacked.unsupported() != nullptr);

    for (const std::string_view value : {
             "gzip;level=9", "bad coding", "gzip/deflate"}) {
        const auto invalid = httpContentCodingFromFieldValue(value);
        RUVIA_CHECK(invalid.coding() == nullptr);
        RUVIA_CHECK(invalid.unsupported() == nullptr);
        RUVIA_CHECK(invalid.invalid() != nullptr);
        if (invalid.invalid() != nullptr) {
            RUVIA_CHECK_EQ(
                invalid.invalid()->status(),
                ruvia::http_status::kBadRequest);
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
    for (const std::string_view value : {
             "", ",gzip", "gzip,", "gzip,,br", "deflate,"}) {
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
    RUVIA_CHECK(
        decodeError(
            HttpContentCoding::kZstd,
            encoded,
            plain.size()) ==
        HttpContentDecodeError::kInvalidContent);

    auto conformant = encodeHttpContent(
        HttpContentCoding::kZstd,
        plain,
        plain.size(),
        std::pmr::get_default_resource());
    RUVIA_CHECK(conformant.encoded() != nullptr);
    if (const auto* content = conformant.encoded()) {
        RUVIA_CHECK_EQ(
            decoded(
                HttpContentCoding::kZstd,
                content->bytes(),
                plain.size()),
            plain);
    }
}

RUVIA_TEST(http_content_encode_enforces_exact_cap_without_partial_output) {
    const std::string input(2048, 'e');
    for (const auto coding : {
             HttpContentCoding::kGzip,
             HttpContentCoding::kBrotli,
             HttpContentCoding::kZstd}) {
        const auto full = encodeHttpContent(
            coding,
            input,
            input.size(),
            std::pmr::get_default_resource());
        RUVIA_CHECK(full.encoded() != nullptr);
        if (full.encoded() == nullptr) {
            continue;
        }
        const auto encodedSize = full.encoded()->bytes().size();
        RUVIA_CHECK(encodedSize > 1);

        const auto exact = encodeHttpContent(
            coding,
            input,
            encodedSize,
            std::pmr::get_default_resource());
        RUVIA_CHECK(exact.encoded() != nullptr);
        if (const auto* encoded = exact.encoded()) {
            RUVIA_CHECK_EQ(encoded->bytes().size(), encodedSize);
        }

        const auto tooSmall = encodeHttpContent(
            coding,
            input,
            encodedSize - 1,
            std::pmr::get_default_resource());
        RUVIA_CHECK(tooSmall.encoded() == nullptr);
        RUVIA_CHECK(tooSmall.failure() != nullptr);
        if (const auto* failure = tooSmall.failure()) {
            RUVIA_CHECK(
                failure->error() ==
                HttpContentEncodeError::kEncodedSizeExceeded);
        }
    }

    const auto identity = encodeHttpContent(
        HttpContentCoding::kIdentity,
        "identity",
        8,
        std::pmr::get_default_resource());
    RUVIA_CHECK(identity.encoded() != nullptr);
    RUVIA_CHECK(identity.failure() == nullptr);
    if (const auto* encoded = identity.encoded()) {
        RUVIA_CHECK_EQ(encoded->bytes(), std::string_view("identity"));
    }
    const auto identityTooLarge = encodeHttpContent(
        HttpContentCoding::kIdentity,
        "identity",
        0,
        std::pmr::get_default_resource());
    RUVIA_CHECK(identityTooLarge.encoded() == nullptr);
    RUVIA_CHECK(identityTooLarge.failure() != nullptr);
    if (const auto* failure = identityTooLarge.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            HttpContentEncodeError::kEncodedSizeExceeded);
    }
}

RUVIA_TEST(http_brotli_encode_does_not_reserve_the_output_cap) {
    const std::string input(1u << 20, 'b');
    RejectLargeAllocationResource resource(4096);
    bool completed = false;
    bool roundTripped = false;
    try {
        auto result = encodeHttpContent(
            HttpContentCoding::kBrotli,
            input,
            input.size() - 1,
            &resource);
        completed = result.encoded() != nullptr;
        if (const auto* encoded = result.encoded()) {
            roundTripped = decoded(
                HttpContentCoding::kBrotli,
                encoded->bytes(),
                input.size()) == input;
        }
    } catch (const std::bad_alloc&) {
    }
    RUVIA_CHECK(completed);
    RUVIA_CHECK(roundTripped);
}

RUVIA_TEST(http_identity_content_uses_the_default_resource_when_none_is_supplied) {
    const std::string input(1024, 'i');

    auto decoded = decodeHttpContent(
        HttpContentCoding::kIdentity,
        input,
        input.size(),
        nullptr);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (decoded.decoded() != nullptr) {
        auto bytes = std::move(*decoded.decoded()).takeBytes();
        RUVIA_CHECK_EQ(std::string_view(bytes), std::string_view(input));
        RUVIA_CHECK(
            bytes.get_allocator().resource() ==
            std::pmr::get_default_resource());
    }

    auto encoded = encodeHttpContent(
        HttpContentCoding::kIdentity,
        input,
        input.size(),
        nullptr);
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() != nullptr) {
        auto bytes = std::move(*encoded.encoded()).takeBytes();
        RUVIA_CHECK_EQ(std::string_view(bytes), std::string_view(input));
        RUVIA_CHECK(
            bytes.get_allocator().resource() ==
            std::pmr::get_default_resource());
    }
}

RUVIA_TEST(http_identity_content_rejects_oversize_before_allocating) {
    const std::string input(1024, 'i');

    const auto decoded = decodeHttpContent(
        HttpContentCoding::kIdentity,
        input,
        input.size() - 1,
        std::pmr::null_memory_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(
            decoded.failure()->error() ==
            HttpContentDecodeError::kDecodedSizeExceeded);
    }

    const auto encoded = encodeHttpContent(
        HttpContentCoding::kIdentity,
        input,
        input.size() - 1,
        std::pmr::null_memory_resource());
    RUVIA_CHECK(encoded.encoded() == nullptr);
    RUVIA_CHECK(encoded.failure() != nullptr);
    if (encoded.failure() != nullptr) {
        RUVIA_CHECK(
            encoded.failure()->error() ==
            HttpContentEncodeError::kEncodedSizeExceeded);
    }
}

RUVIA_TEST(http_content_decode_rejects_empty_encoded_input) {
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kGzip, {}) ==
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kBrotli, {}) ==
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kZstd, {}) ==
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK_EQ(
        decoded(HttpContentCoding::kIdentity, {}, 0),
        std::string{});
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
        auto result = decodeHttpContent(
            test.coding,
            test.encoded,
            0,
            std::pmr::get_default_resource());
        RUVIA_CHECK(result.decoded() != nullptr);
        if (const auto* content = result.decoded()) {
            RUVIA_CHECK(content->bytes().empty());
        }
    }
    RUVIA_CHECK(
        decodeError(
            HttpContentCoding::kGzip,
            gzipCompress("x"),
            0) ==
        HttpContentDecodeError::kDecodedSizeExceeded);
}
