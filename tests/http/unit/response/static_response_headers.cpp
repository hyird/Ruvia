#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"

#include "response/http_response_static_headers.h"
#include "test_harness.h"

namespace {

using ruvia::detail::builtin_static_response_header;

// Assert that an interned (name, value) pair round-trips: both the name and the
// value substrings must match, which is what guards the hardcoded name_size used
// to split the concatenated static byte blob.
void check_interned(ruvia::testing::test_context& ruvia_ctx, std::uint32_t known_bit,
    std::string_view value, std::string_view expected_name) {
    const auto header_value = builtin_static_response_header(known_bit, value);
    RUVIA_CHECK(header_value.has_value());
    RUVIA_CHECK_EQ(header_value->name(), expected_name);
    RUVIA_CHECK_EQ(header_value->value(), value);
}

}  // namespace

RUVIA_TEST(static_header_content_type_variants_split_correctly) {
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_content_type, "application/json", "Content-Type");
    check_interned(ruvia_ctx, ruvia::detail::response_header_content_type, "text/plain; charset=UTF-8",
        "Content-Type");
    check_interned(ruvia_ctx, ruvia::detail::response_header_content_type, "text/html; charset=UTF-8",
        "Content-Type");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_content_type, "text/event-stream", "Content-Type");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_content_type, "image/svg+xml", "Content-Type");
    // Last entry in the chain.
    check_interned(ruvia_ctx, ruvia::detail::response_header_content_type, "application/octet-stream",
        "Content-Type");
}

RUVIA_TEST(static_header_single_value_names) {
    check_interned(ruvia_ctx, ruvia::detail::response_header_connection, "close", "Connection");
    check_interned(ruvia_ctx, ruvia::detail::response_header_accept_ranges, "bytes", "Accept-Ranges");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_content_encoding, "gzip", "Content-Encoding");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_transfer_encoding, "chunked", "Transfer-Encoding");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_cache_control, "no-store", "Cache-Control");
}

RUVIA_TEST(static_header_vary_and_credentials) {
    check_interned(ruvia_ctx, ruvia::detail::response_header_vary, "Accept-Encoding", "Vary");
    check_interned(ruvia_ctx, ruvia::detail::response_header_vary, "Origin", "Vary");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_vary, "Access-Control-Request-Headers", "Vary");
    check_interned(
        ruvia_ctx, ruvia::detail::response_header_vary, "Access-Control-Request-Method", "Vary");
    // Longest interned name (32 bytes) split from a short value.
    check_interned(ruvia_ctx, ruvia::detail::response_header_access_control_allow_credentials, "true",
        "Access-Control-Allow-Credentials");
}

RUVIA_TEST(static_header_unknown_value_or_bit_is_nullopt) {
    // A known header name but a value not in the intern table.
    RUVIA_CHECK(
        !builtin_static_response_header(ruvia::detail::response_header_content_type, "text/markdown")
            .has_value());
    RUVIA_CHECK(!builtin_static_response_header(ruvia::detail::response_header_connection, "keep-alive")
            .has_value());
    RUVIA_CHECK(
        !builtin_static_response_header(ruvia::detail::response_header_vary, "User-Agent").has_value());
    // An unhandled known-bit falls through to nullopt.
    RUVIA_CHECK(!builtin_static_response_header(0, "anything").has_value());
}

RUVIA_TEST(response_known_header_slot_maps_single_bits) {
    using ruvia::detail::response_known_header_count;
    using ruvia::detail::response_known_header_slot;
    // A single known-header bit maps to its bit position.
    RUVIA_CHECK_EQ(
        response_known_header_slot(ruvia::detail::response_header_content_length), std::size_t{0});
    RUVIA_CHECK_EQ(
        response_known_header_slot(ruvia::detail::response_header_content_encoding), std::size_t{1});
    RUVIA_CHECK_EQ(
        response_known_header_slot(ruvia::detail::response_header_content_type), std::size_t{2});
    RUVIA_CHECK_EQ(response_known_header_slot(ruvia::detail::response_header_vary), std::size_t{4});

    // Zero, multi-bit, and out-of-range values return the sentinel (count).
    RUVIA_CHECK_EQ(response_known_header_slot(0), response_known_header_count);
    RUVIA_CHECK_EQ(response_known_header_slot(ruvia::detail::response_header_content_type |
                                              ruvia::detail::response_header_connection),
        response_known_header_count);
    RUVIA_CHECK_EQ(response_known_header_slot(1U << 25), response_known_header_count);
}
