#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http2/Http2HeaderBlock.h"

namespace {

using ruvia::detail::Http2StreamState;
using ruvia::detail::http2AppendHeaderBlock;
using ruvia::detail::http2ResetHeaderBlock;
using ruvia::detail::http2StartHeaderBlock;

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
