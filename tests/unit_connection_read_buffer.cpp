#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/server/HttpConnectionState.h"
#include "net/server/HttpServerConnectionGuards.h"
#include "http/HttpParserInternal.h"
#include "ruvia/http/HttpLimits.h"

namespace {

using ruvia::detail::compactConnectionReadBuffer;
using ruvia::detail::ConnectionCountGuard;
using ruvia::detail::growReadBuffer;
using ruvia::detail::trimReadBufferStorage;
using ruvia::detail::HttpServerParseResult;
using ruvia::kMaxHttpHeaderBytes;

// growReadBuffer keys off parsed.consumedBytes only; the rest is inert here.
HttpServerParseResult consumed(std::size_t consumedBytes) {
    HttpServerParseResult parsed;
    parsed.consumedBytes = consumedBytes;
    return parsed;
}

std::pmr::string sizedBuffer(std::size_t size) {
    std::pmr::string out(std::pmr::new_delete_resource());
    out.resize(size);
    return out;
}

std::pmr::string buffer(std::string_view contents) {
    std::pmr::string out(std::pmr::new_delete_resource());
    out.assign(contents.data(), contents.size());
    return out;
}

// The live region is the first `usedBytes` of the buffer's storage.
std::string_view live(const std::pmr::string& buffer, std::size_t usedBytes) {
    return std::string_view(buffer.data(), usedBytes);
}

}  // namespace

RUVIA_TEST(connection_read_buffer_partial_consume_moves_remainder) {
    // Two pipelined requests; consuming the first slides the second to the front.
    auto readBuffer = buffer("REQ1REQ2");
    std::size_t usedBytes = 8;
    compactConnectionReadBuffer(readBuffer, usedBytes, 4);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{4});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("REQ2"));
}

RUVIA_TEST(connection_read_buffer_consume_nothing_is_unchanged) {
    auto readBuffer = buffer("REQ1REQ2");
    std::size_t usedBytes = 8;
    compactConnectionReadBuffer(readBuffer, usedBytes, 0);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{8});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("REQ1REQ2"));
}

RUVIA_TEST(connection_read_buffer_consume_all_empties_region) {
    auto readBuffer = buffer("REQ1REQ2");
    std::size_t usedBytes = 8;
    compactConnectionReadBuffer(readBuffer, usedBytes, 8);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{0});
}

RUVIA_TEST(connection_read_buffer_overlapping_move_is_correct) {
    // The remaining region (4 bytes) overlaps its destination; the move must
    // still reproduce it exactly.
    auto readBuffer = buffer("ABCDEF");
    std::size_t usedBytes = 6;
    compactConnectionReadBuffer(readBuffer, usedBytes, 2);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{4});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("CDEF"));
}

RUVIA_TEST(grow_read_buffer_doubles_when_full) {
    auto readBuffer = sizedBuffer(8 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/8 * 1024, consumed(0));  // full -> grow
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{16 * 1024});
}

RUVIA_TEST(grow_read_buffer_caps_at_header_limit) {
    // Doubling would overshoot the header limit; growth clamps to it so an
    // attacker cannot drive unbounded buffer growth with header bursts.
    auto readBuffer = sizedBuffer(40 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/40 * 1024, consumed(0));
    RUVIA_CHECK_EQ(readBuffer.size(), kMaxHttpHeaderBytes);  // min(80K, 64K)
}

RUVIA_TEST(grow_read_buffer_at_limit_does_not_grow) {
    auto readBuffer = sizedBuffer(kMaxHttpHeaderBytes);
    growReadBuffer(readBuffer, /*usedBytes=*/kMaxHttpHeaderBytes, consumed(0));
    RUVIA_CHECK_EQ(readBuffer.size(), kMaxHttpHeaderBytes);  // hard ceiling holds
}

RUVIA_TEST(grow_read_buffer_no_growth_when_not_full) {
    auto readBuffer = sizedBuffer(8 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/100, consumed(0));  // room remains
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{8 * 1024});
}

RUVIA_TEST(grow_read_buffer_expands_to_hold_consumed_span) {
    // A parse unit needs more than the current buffer: grow exactly to fit it.
    auto readBuffer = sizedBuffer(8 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/8 * 1024, consumed(9000));
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{9000});
}

RUVIA_TEST(trim_read_buffer_reclaims_overgrown_capacity) {
    // A buffer that spilled past the header limit is reclaimed to the initial
    // size once mostly drained, and the still-live prefix is preserved.
    auto readBuffer = sizedBuffer(70 * 1024);  // capacity > 64K shrink threshold
    readBuffer[0] = 'A';
    readBuffer[1] = 'B';
    readBuffer[2] = 'C';
    trimReadBufferStorage(readBuffer, /*usedBytes=*/3);
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{8 * 1024});      // back to initial
    RUVIA_CHECK(readBuffer.capacity() < kMaxHttpHeaderBytes);      // capacity reclaimed
    RUVIA_CHECK(readBuffer[0] == 'A' && readBuffer[1] == 'B' && readBuffer[2] == 'C');
}

RUVIA_TEST(trim_read_buffer_normalizes_moderately_grown_buffer_in_place) {
    // The common case: a buffer that doubled to 16K (capacity still under the 64K
    // shrink threshold) and is now mostly drained is resized back to the initial
    // size IN PLACE -- the normalize branch, distinct from the fresh-allocation
    // reclaim path for buffers that overgrew past the header limit. The live prefix
    // (within the initial size) must survive the in-place shrink.
    auto readBuffer = sizedBuffer(16 * 1024);
    readBuffer[0] = 'X';
    readBuffer[7000] = 'Y';
    trimReadBufferStorage(readBuffer, /*usedBytes=*/7001);
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{8 * 1024});  // normalized to initial
    RUVIA_CHECK(readBuffer[0] == 'X');
    RUVIA_CHECK(readBuffer[7000] == 'Y');                      // live bytes preserved
}

RUVIA_TEST(trim_read_buffer_keeps_buffer_when_still_heavily_used) {
    auto readBuffer = sizedBuffer(70 * 1024);
    trimReadBufferStorage(readBuffer, /*usedBytes=*/9000);  // > initial -> keep as-is
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{70 * 1024});
}

RUVIA_TEST(connection_count_guard_decrements_with_underflow_protection) {
    // The guard decrements the live-connection count on scope exit (the accept path
    // increments). It must never decrement below zero: a stray release at zero would
    // wrap the count to SIZE_MAX and effectively remove the concurrent-connection cap.
    std::size_t count = 3;
    {
        ConnectionCountGuard guard(count);
        RUVIA_CHECK_EQ(count, std::size_t{3});  // construction does not change the count
    }
    RUVIA_CHECK_EQ(count, std::size_t{2});       // decremented on scope exit

    std::size_t zero = 0;
    {
        ConnectionCountGuard guard(zero);
    }
    RUVIA_CHECK_EQ(zero, std::size_t{0});         // never underflows past zero
}
