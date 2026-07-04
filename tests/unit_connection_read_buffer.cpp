#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/server/HttpConnectionState.h"

namespace {

using ruvia::detail::compactConnectionReadBuffer;

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
