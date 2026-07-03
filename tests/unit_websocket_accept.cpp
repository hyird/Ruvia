#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/ws/HttpWebSocketUtils.h"

namespace {

std::string accept(std::string_view key) {
    const auto out = ruvia::detail::webSocketAccept(key, std::pmr::get_default_resource());
    return std::string(out.data(), out.size());
}

}  // namespace

// Sec-WebSocket-Accept = base64(SHA-1(key + GUID)), RFC 6455 §1.3. This exercises
// the hand-rolled SHA-1: the canonical example is a known-answer vector.
RUVIA_TEST(websocket_accept_rfc6455_vector) {
    RUVIA_CHECK_EQ(accept("dGhlIHNhbXBsZSBub25jZQ=="), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

RUVIA_TEST(websocket_accept_trims_ows_and_is_key_sensitive) {
    // Leading/trailing OWS on the header value is trimmed before hashing.
    RUVIA_CHECK_EQ(accept("  dGhlIHNhbXBsZSBub25jZQ==  "), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    // The accept is always 28 base64 characters (a 20-byte SHA-1 digest).
    RUVIA_CHECK_EQ(accept("dGhlIHNhbXBsZSBub25jZQ==").size(), std::size_t{28});
    // A different key yields a different accept.
    RUVIA_CHECK(accept("dGhlIHNhbXBsZSBub25jZQ==") != accept("AAAAAAAAAAAAAAAAAAAAAA=="));
}

RUVIA_TEST(websocket_accept_is_deterministic_across_block_sizes) {
    // An empty key (key+GUID = 36 bytes -> single SHA-1 block) still produces a
    // stable 28-char accept, exercising the one-block padding path.
    const auto empty = accept("");
    RUVIA_CHECK_EQ(empty.size(), std::size_t{28});
    RUVIA_CHECK_EQ(empty, accept(""));
}
