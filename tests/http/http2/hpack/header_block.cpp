#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/hpack/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderContinuation.h"

namespace {

using ruvia::detail::http2AppendHeaderBlock;
using ruvia::detail::Http2FrameType;
using ruvia::detail::Http2HeaderBlockKind;
using ruvia::detail::Http2HeaderContinuation;
using ruvia::detail::http2ResetHeaderBlock;
using ruvia::detail::http2StartHeaderBlock;
using ruvia::detail::Http2StreamState;
using ruvia::detail::kMaxHttp2EncodedHeaderBlockBytes;

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
    const std::string atLimit(kMaxHttp2EncodedHeaderBlockBytes, 'a');
    RUVIA_CHECK(http2StartHeaderBlock(stream, atLimit));
    RUVIA_CHECK_EQ(stream.requestHeaderBlock().size(), kMaxHttp2EncodedHeaderBlockBytes);
    // One more byte would exceed the cap: rejected, block left untouched.
    RUVIA_CHECK(!http2AppendHeaderBlock(stream, "x"));
    RUVIA_CHECK_EQ(stream.requestHeaderBlock().size(), kMaxHttp2EncodedHeaderBlockBytes);

    // A single oversized fragment is rejected outright.
    auto fresh = makeStream();
    const std::string tooBig(kMaxHttp2EncodedHeaderBlockBytes + 1, 'b');
    RUVIA_CHECK(!http2StartHeaderBlock(fresh, tooBig));
    RUVIA_CHECK(fresh.requestHeaderBlock().empty());
}

RUVIA_TEST(header_continuation_frame_budget_bounds_empty_flood) {
    using ruvia::detail::kHttp2MaxContinuationFrames;
    Http2HeaderContinuation cont;
    cont.start(1, Http2HeaderBlockKind::kInitial);
    // Empty CONTINUATION frames add no bytes and slip past the size cap, so the
    // frame count is the only bound (CVE-2024-27316). Every frame up to the budget
    // is accepted; the one past it is rejected.
    for (std::uint32_t i = 0; i < kHttp2MaxContinuationFrames; ++i) {
        RUVIA_CHECK(cont.recordContinuationFrame());
    }
    RUVIA_CHECK(!cont.recordContinuationFrame());

    // Starting a fresh block clears the counter so a legitimate next block is not
    // penalized for the previous one.
    cont.start(3, Http2HeaderBlockKind::kInitial);
    RUVIA_CHECK(cont.recordContinuationFrame());
}

RUVIA_TEST(header_continuation_state_machine_enforces_same_stream_only) {
    Http2HeaderContinuation cont;
    // Idle: any frame type is acceptable and no stream is being continued.
    RUVIA_CHECK(!cont.active());
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kData)));
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kContinuation)));
    RUVIA_CHECK(!cont.matches(1));

    // Mid field block (RFC 9113 §6.10): only a CONTINUATION on the SAME stream may
    // follow -- no other frame type, no other stream, never stream 0.
    cont.start(5, Http2HeaderBlockKind::kInitial);
    RUVIA_CHECK(cont.active());
    RUVIA_CHECK(cont.matches(5));
    RUVIA_CHECK(!cont.matches(3));
    RUVIA_CHECK(!cont.matches(0));
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kContinuation)));
    RUVIA_CHECK(!cont.expectsFrameType(frameType(Http2FrameType::kData)));
    RUVIA_CHECK(!cont.expectsFrameType(frameType(Http2FrameType::kHeaders)));

    // finishKind reports the block kind and clears the state.
    RUVIA_CHECK(cont.kind() == Http2HeaderBlockKind::kInitial);
    RUVIA_CHECK(cont.finishKind() == Http2HeaderBlockKind::kInitial);
    RUVIA_CHECK(!cont.active());
    RUVIA_CHECK(cont.expectsFrameType(frameType(Http2FrameType::kData)));  // idle again

    cont.start(7, Http2HeaderBlockKind::kTrailers);
    RUVIA_CHECK(cont.finishKind() == Http2HeaderBlockKind::kTrailers);
    RUVIA_CHECK(!cont.active());

    // Discarded blocks remain distinguishable while enforcing the same atomic
    // CONTINUATION sequence.
    cont.start(11, Http2HeaderBlockKind::kDiscarded);
    RUVIA_CHECK(cont.kind() == Http2HeaderBlockKind::kDiscarded);
    RUVIA_CHECK(cont.finishKind() == Http2HeaderBlockKind::kDiscarded);

    // reset() also clears an in-progress block.
    cont.start(9, Http2HeaderBlockKind::kInitial);
    RUVIA_CHECK(cont.active());
    cont.reset();
    RUVIA_CHECK(!cont.active());
}
