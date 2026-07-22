#include "field_parsing_fixture.h"

// Decoding zstd frames: a complete frame and a truncated one.

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
