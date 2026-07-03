#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>

#include "net/ws/HttpWebSocketPermessageDeflate.h"

namespace {

using ruvia::detail::WebSocketDeflate;
using ruvia::detail::WebSocketInflateResult;

// Compress then decompress on the same codec (separate deflate/inflate streams,
// each reset per message for no-context-takeover) must reproduce the input.
bool roundTrips(WebSocketDeflate& codec, std::string_view message) {
    std::pmr::string compressed(std::pmr::get_default_resource());
    if (!codec.compress(message, compressed)) {
        return false;
    }
    std::pmr::string restored(std::pmr::get_default_resource());
    if (codec.decompress(compressed, restored, 0) != WebSocketInflateResult::kOk) {
        return false;
    }
    return std::string_view(restored.data(), restored.size()) == message;
}

std::string patterned(std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(static_cast<char>('A' + (i * 7 + (i >> 3)) % 26));
    }
    return s;
}

}  // namespace

RUVIA_TEST(websocket_deflate_constructs_ok) {
    WebSocketDeflate codec;
    RUVIA_CHECK(codec.ok());
}

RUVIA_TEST(websocket_deflate_round_trips_various_sizes) {
    WebSocketDeflate codec;
    RUVIA_CHECK(roundTrips(codec, ""));
    RUVIA_CHECK(roundTrips(codec, "a"));
    RUVIA_CHECK(roundTrips(codec, "hello world"));
    RUVIA_CHECK(roundTrips(codec, std::string(4096, 'z')));   // highly compressible
    RUVIA_CHECK(roundTrips(codec, patterned(10000)));         // varied content
    // Reusing the same codec across messages must keep working (per-message reset).
    RUVIA_CHECK(roundTrips(codec, "second message on the same codec"));
}

RUVIA_TEST(websocket_deflate_inflate_respects_max_bytes) {
    WebSocketDeflate codec;
    std::pmr::string compressed(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.compress(std::string(10000, 'a'), compressed));  // tiny compressed form
    // Decompressing a bomb under a small cap must be refused, not expanded.
    std::pmr::string restored(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(compressed, restored, 100) == WebSocketInflateResult::kTooLarge);
    // With a sufficient cap the same payload inflates fully.
    std::pmr::string ok(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(compressed, ok, 10000) == WebSocketInflateResult::kOk);
    RUVIA_CHECK_EQ(ok.size(), std::size_t{10000});
}

RUVIA_TEST(websocket_deflate_rejects_corrupt_input) {
    WebSocketDeflate codec;
    std::pmr::string restored(std::pmr::get_default_resource());
    // Random bytes are not a valid raw-DEFLATE block; inflate must report an error.
    const auto result = codec.decompress("\xff\xff\xff\xff\xff\xff", restored, 0);
    RUVIA_CHECK(result == WebSocketInflateResult::kError);
}
