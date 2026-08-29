#include "test_harness.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/response/HttpResponseStaticHeaders.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"

namespace {

using ruvia::detail::builtinStaticResponseHeader;

// Assert that an interned (name, value) pair round-trips: both the name and the
// value substrings must match, which is what guards the hardcoded nameSize used
// to split the concatenated static byte blob.
void checkInterned(ruvia::testing::TestContext& ruvia_ctx, std::uint32_t knownBit,
    std::string_view value, std::string_view expectedName) {
    const auto header = builtinStaticResponseHeader(knownBit, value);
    RUVIA_CHECK(header.has_value());
    RUVIA_CHECK_EQ(header->name(), expectedName);
    RUVIA_CHECK_EQ(header->value(), value);
}

}  // namespace

RUVIA_TEST(static_header_content_type_variants_split_correctly) {
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderContentType, "application/json", "Content-Type");
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderContentType, "text/plain; charset=UTF-8",
        "Content-Type");
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderContentType, "text/html; charset=UTF-8",
        "Content-Type");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderContentType, "text/event-stream", "Content-Type");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderContentType, "image/svg+xml", "Content-Type");
    // Last entry in the chain.
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderContentType, "application/octet-stream",
        "Content-Type");
}

RUVIA_TEST(static_header_single_value_names) {
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderConnection, "close", "Connection");
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderAcceptRanges, "bytes", "Accept-Ranges");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderContentEncoding, "gzip", "Content-Encoding");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderTransferEncoding, "chunked", "Transfer-Encoding");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderCacheControl, "no-store", "Cache-Control");
}

RUVIA_TEST(static_header_vary_and_credentials) {
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderVary, "Accept-Encoding", "Vary");
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderVary, "Origin", "Vary");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderVary, "Access-Control-Request-Headers", "Vary");
    checkInterned(
        ruvia_ctx, ruvia::detail::kResponseHeaderVary, "Access-Control-Request-Method", "Vary");
    // Longest interned name (32 bytes) split from a short value.
    checkInterned(ruvia_ctx, ruvia::detail::kResponseHeaderAccessControlAllowCredentials, "true",
        "Access-Control-Allow-Credentials");
}

RUVIA_TEST(static_header_unknown_value_or_bit_is_nullopt) {
    // A known header name but a value not in the intern table.
    RUVIA_CHECK(
        !builtinStaticResponseHeader(ruvia::detail::kResponseHeaderContentType, "text/markdown")
            .has_value());
    RUVIA_CHECK(!builtinStaticResponseHeader(ruvia::detail::kResponseHeaderConnection, "keep-alive")
            .has_value());
    RUVIA_CHECK(
        !builtinStaticResponseHeader(ruvia::detail::kResponseHeaderVary, "User-Agent").has_value());
    // An unhandled known-bit falls through to nullopt.
    RUVIA_CHECK(!builtinStaticResponseHeader(0, "anything").has_value());
}

RUVIA_TEST(response_known_header_slot_maps_single_bits) {
    using ruvia::detail::kResponseKnownHeaderCount;
    using ruvia::detail::responseKnownHeaderSlot;
    // A single known-header bit maps to its bit position.
    RUVIA_CHECK_EQ(
        responseKnownHeaderSlot(ruvia::detail::kResponseHeaderContentLength), std::size_t{0});
    RUVIA_CHECK_EQ(
        responseKnownHeaderSlot(ruvia::detail::kResponseHeaderContentEncoding), std::size_t{1});
    RUVIA_CHECK_EQ(
        responseKnownHeaderSlot(ruvia::detail::kResponseHeaderContentType), std::size_t{2});
    RUVIA_CHECK_EQ(responseKnownHeaderSlot(ruvia::detail::kResponseHeaderVary), std::size_t{4});

    // Zero, multi-bit, and out-of-range values return the sentinel (count).
    RUVIA_CHECK_EQ(responseKnownHeaderSlot(0), kResponseKnownHeaderCount);
    RUVIA_CHECK_EQ(responseKnownHeaderSlot(ruvia::detail::kResponseHeaderContentType |
                                           ruvia::detail::kResponseHeaderConnection),
        kResponseKnownHeaderCount);
    RUVIA_CHECK_EQ(responseKnownHeaderSlot(1U << 25), kResponseKnownHeaderCount);
}
