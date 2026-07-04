#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http2/Http2HeaderBlock.h"
#include "net/http2/Http2HeaderContinuation.h"

namespace {

using ruvia::detail::Http2FrameType;
using ruvia::detail::Http2HeaderContinuation;
using ruvia::detail::Http2StreamState;
using ruvia::detail::http2AppendHeaderBlock;
using ruvia::detail::http2ResetHeaderBlock;
using ruvia::detail::http2StartHeaderBlock;

constexpr std::uint8_t frameType(Http2FrameType type) noexcept {
    return static_cast<std::uint8_t>(type);
}

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(header_block_start_append_reset) {
    auto stream = makeStream();
    RUVIA_CHECK(http2StartHeaderBlock(stream, "abc"));
    RUVIA_CHECK_EQ(std::string_view(stream.requestHeaderBlock()), std::string_view("abc"));
    // A CONTINUATION fragment accumulates onto the block.
    RUVIA_CHECK(http2AppendHeaderBlock(stream, "def"));
    RUVIA_CHECK_EQ(std::string_view(stream.requestHeaderBlock()), std::string_view("abcdef"));
    // Starting a new block resets first.
    RUVIA_CHECK(http2StartHeaderBlock(stream, "xyz"));
    RUVIA_CHECK_EQ(std::string_view(stream.requestHeaderBlock()), std::string_view("xyz"));
    // Reset clears it.
    http2ResetHeaderBlock(stream);
    RUVIA_CHECK(stream.requestHeaderBlock().empty());
}

RUVIA_TEST(header_block_size_limit_guards_continuation_flood) {
    auto stream = makeStream();
    const std::string atLimit(64 * 1024, 'a');  // exactly kMaxHttpHeaderBytes
    RUVIA_CHECK(http2StartHeaderBlock(stream, atLimit));
    RUVIA_CHECK_EQ(stream.requestHeaderBlock().size(), std::size_t{64 * 1024});
    // One more byte would exceed the cap: rejected, block left untouched.
    RUVIA_CHECK(!http2AppendHeaderBlock(stream, "x"));
    RUVIA_CHECK_EQ(stream.requestHeaderBlock().size(), std::size_t{64 * 1024});

    // A single oversized fragment is rejected outright.
    auto fresh = makeStream();
    const std::string tooBig(64 * 1024 + 1, 'b');
    RUVIA_CHECK(!http2StartHeaderBlock(fresh, tooBig));
    RUVIA_CHECK(fresh.requestHeaderBlock().empty());
}

RUVIA_TEST(header_continuation_state_machine_enforces_same_stream_only) {
    Http2HeaderContinuation cont;
    // Idle: any frame type is acceptable and no stream is being continued.
    RUVIA_CHECK(!cont.active());
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kData)));
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kContinuation)));
    RUVIA_CHECK(!cont.matches(1));

    // Mid header block (RFC 7540 6.10): only a CONTINUATION on the SAME stream may
    // follow -- no other frame type, no other stream, never stream 0.
    cont.start(5, /*trailers=*/false);
    RUVIA_CHECK(cont.active());
    RUVIA_CHECK(cont.matches(5));
    RUVIA_CHECK(!cont.matches(3));
    RUVIA_CHECK(!cont.matches(0));
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kContinuation)));
    RUVIA_CHECK(!cont.expectsFrameType(frameType(Http2FrameType::kData)));
    RUVIA_CHECK(!cont.expectsFrameType(frameType(Http2FrameType::kHeaders)));

    // finishWasTrailers reports the block kind and clears the state (initial headers).
    RUVIA_CHECK(!cont.finishWasTrailers());
    RUVIA_CHECK(!cont.active());
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kData)));  // idle again

    // A trailer header block is flagged as such by finishWasTrailers.
    cont.start(7, /*trailers=*/true);
    RUVIA_CHECK(cont.finishWasTrailers());
    RUVIA_CHECK(!cont.active());

    // reset() also clears an in-progress block.
    cont.start(9, false);
    RUVIA_CHECK(cont.active());
    cont.reset();
    RUVIA_CHECK(!cont.active());
}
