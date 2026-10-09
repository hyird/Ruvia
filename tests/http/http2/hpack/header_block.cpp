#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "http2/http2_header_block.h"
#include "http2/http2_header_continuation.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_append_header_block;
using ruvia::detail::http2_frame_type;
using ruvia::detail::http2_header_block_kind;
using ruvia::detail::http2_header_continuation;
using ruvia::detail::http2_reset_header_block;
using ruvia::detail::http2_start_header_block;
using ruvia::detail::http2_stream_state;
using ruvia::detail::max_http2_encoded_header_block_bytes;

constexpr std::uint8_t frame_type(http2_frame_type type) noexcept {
    return static_cast<std::uint8_t>(type);
}

http2_stream_state make_stream() {
    return http2_stream_state(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(header_block_start_append_reset) {
    auto stream = make_stream();
    RUVIA_CHECK(http2_start_header_block(stream, "abc"));
    RUVIA_CHECK_EQ(std::string_view(stream.remote_header_block()), std::string_view("abc"));
    // A CONTINUATION fragment accumulates onto the block.
    RUVIA_CHECK(http2_append_header_block(stream, "def"));
    RUVIA_CHECK_EQ(std::string_view(stream.remote_header_block()), std::string_view("abcdef"));
    // Starting a new block resets first.
    RUVIA_CHECK(http2_start_header_block(stream, "xyz"));
    RUVIA_CHECK_EQ(std::string_view(stream.remote_header_block()), std::string_view("xyz"));
    // Reset clears it.
    http2_reset_header_block(stream);
    RUVIA_CHECK(stream.remote_header_block().empty());
}

RUVIA_TEST(header_block_size_limit_guards_continuation_flood) {
    auto stream = make_stream();
    const std::string at_limit(max_http2_encoded_header_block_bytes, 'a');
    RUVIA_CHECK(http2_start_header_block(stream, at_limit));
    RUVIA_CHECK_EQ(stream.remote_header_block().size(), max_http2_encoded_header_block_bytes);
    // One more byte would exceed the cap: rejected, block left untouched.
    RUVIA_CHECK(!http2_append_header_block(stream, "x"));
    RUVIA_CHECK_EQ(stream.remote_header_block().size(), max_http2_encoded_header_block_bytes);

    // A single oversized fragment is rejected outright.
    auto fresh = make_stream();
    const std::string too_big(max_http2_encoded_header_block_bytes + 1, 'b');
    RUVIA_CHECK(!http2_start_header_block(fresh, too_big));
    RUVIA_CHECK(fresh.remote_header_block().empty());
}

RUVIA_TEST(header_continuation_frame_budget_bounds_empty_flood) {
    using ruvia::detail::http2_max_continuation_frames;
    http2_header_continuation cont;
    cont.start(1, http2_header_block_kind::initial);
    // Empty CONTINUATION frames add no bytes and slip past the size cap, so the
    // frame count is the only bound (CVE-2024-27316). Every frame up to the budget
    // is accepted; the one past it is rejected.
    for (std::uint32_t i = 0; i < http2_max_continuation_frames; ++i) {
        RUVIA_CHECK(cont.record_continuation_frame());
    }
    RUVIA_CHECK(!cont.record_continuation_frame());

    // Starting a fresh block clears the counter so a legitimate next block is not
    // penalized for the previous one.
    cont.start(3, http2_header_block_kind::initial);
    RUVIA_CHECK(cont.record_continuation_frame());
}

RUVIA_TEST(header_continuation_state_machine_enforces_same_stream_only) {
    http2_header_continuation cont;
    // Idle: any frame type is acceptable and no stream is being continued.
    RUVIA_CHECK(!cont.active());
    RUVIA_CHECK(cont.expects_frame_type(frame_type(http2_frame_type::data)));
    RUVIA_CHECK(cont.expects_frame_type(frame_type(http2_frame_type::continuation)));
    RUVIA_CHECK(!cont.matches(1));

    // Mid field block (RFC 9113 §6.10): only a CONTINUATION on the SAME stream may
    // follow -- no other frame type, no other stream, never stream 0.
    cont.start(5, http2_header_block_kind::initial);
    RUVIA_CHECK(cont.active());
    RUVIA_CHECK(cont.matches(5));
    RUVIA_CHECK(!cont.matches(3));
    RUVIA_CHECK(!cont.matches(0));
    RUVIA_CHECK(cont.expects_frame_type(frame_type(http2_frame_type::continuation)));
    RUVIA_CHECK(!cont.expects_frame_type(frame_type(http2_frame_type::data)));
    RUVIA_CHECK(!cont.expects_frame_type(frame_type(http2_frame_type::headers)));

    // finish_kind reports the block kind and clears the state.
    RUVIA_CHECK(cont.kind() == http2_header_block_kind::initial);
    RUVIA_CHECK(cont.finish_kind() == http2_header_block_kind::initial);
    RUVIA_CHECK(!cont.active());
    RUVIA_CHECK(cont.expects_frame_type(frame_type(http2_frame_type::data)));  // idle again

    cont.start(7, http2_header_block_kind::trailers);
    RUVIA_CHECK(cont.finish_kind() == http2_header_block_kind::trailers);
    RUVIA_CHECK(!cont.active());

    // Discarded blocks remain distinguishable while enforcing the same atomic
    // CONTINUATION sequence.
    cont.start(11, http2_header_block_kind::discarded);
    RUVIA_CHECK(cont.kind() == http2_header_block_kind::discarded);
    RUVIA_CHECK(cont.finish_kind() == http2_header_block_kind::discarded);

    // reset() also clears an in-progress block.
    cont.start(9, http2_header_block_kind::initial);
    RUVIA_CHECK(cont.active());
    cont.reset();
    RUVIA_CHECK(!cont.active());
}
