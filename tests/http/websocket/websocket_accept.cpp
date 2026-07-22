#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketAcceptKey.h"

namespace {

std::string accept(std::string_view key) {
    ruvia::detail::WebSocketAcceptKey out;
    ruvia::detail::encodeWebSocketAccept(out, key);
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

RUVIA_TEST(websocket_accept_sha1_block_boundary_vectors) {
    // The 36-byte GUID is appended to the key, so these key lengths place the
    // total message at the SHA-1 block boundaries the hand-rolled digest is most
    // likely to get wrong: whether padding fits in the final block (total 55) vs
    // spills to an extra block (total 56), an exactly-full block (total 64), and
    // multi-block inputs (total 66, 128). Expected values are base64(SHA1(key +
    // GUID)) computed independently with a reference implementation.
    RUVIA_CHECK_EQ(accept("aaaaaaaaaaaaaaaaaaa"),                      // total 55 (rem 55)
                   std::string("yzaGyu0mcUukN7CdsSwa30tnCpc="));
    RUVIA_CHECK_EQ(accept("aaaaaaaaaaaaaaaaaaaa"),                     // total 56 (extra pad block)
                   std::string("dUYRM7bOMwDmbriNIPr11x+r3E0="));
    RUVIA_CHECK_EQ(accept("aaaaaaaaaaaaaaaaaaaaaaaaaaaa"),             // total 64 (exact block)
                   std::string("xMWmCBqYJY4uXzUs8PNU1t+Pzro="));
    RUVIA_CHECK_EQ(accept("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),           // total 66 (multi-block)
                   std::string("NP2xQF2DW4Tsrh7mEQTDSUbA8rI="));
    RUVIA_CHECK_EQ(
        accept("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        std::string("QozTWdJkJ6Mr6+QwqAHt8yNpBdc="));               // total 128 (two full blocks)
}
