#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/server/HttpResponseHeadBuffer.h"

namespace {

using ruvia::detail::kResponseHeadStackBytes;
using ruvia::detail::ResponseHeadBuffer;

}  // namespace

RUVIA_TEST(head_buffer_stack_appends) {
    ResponseHeadBuffer buffer(std::pmr::get_default_resource());
    buffer.append("HTTP/1.1 ");
    buffer.appendUnsigned(200);
    buffer.append(' ');
    buffer.append("OK");
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("HTTP/1.1 200 OK"));
    RUVIA_CHECK(buffer.canAppendOnStack(1));
}

RUVIA_TEST(head_buffer_spills_to_heap_preserving_content) {
    ResponseHeadBuffer buffer(std::pmr::get_default_resource());
    const std::string large(kResponseHeadStackBytes + 100, 'x');  // exceeds the stack buffer
    buffer.append("prefix:");
    buffer.append(large);  // forces the spill to heap
    const std::string expected = "prefix:" + large;
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
    RUVIA_CHECK(!buffer.canAppendOnStack(1));  // now on the heap
}

RUVIA_TEST(head_buffer_char_append_spill_boundary) {
    ResponseHeadBuffer buffer(std::pmr::get_default_resource());
    for (std::size_t i = 0; i < kResponseHeadStackBytes; ++i) {
        buffer.append('a');
    }
    RUVIA_CHECK(buffer.canAppendOnStack(0));   // exactly full still fits a zero-byte append
    RUVIA_CHECK(!buffer.canAppendOnStack(1));
    buffer.append('b');  // one more spills to heap
    RUVIA_CHECK_EQ(buffer.view().size(), kResponseHeadStackBytes + 1);
    RUVIA_CHECK_EQ(buffer.view().back(), 'b');
    RUVIA_CHECK_EQ(buffer.view().front(), 'a');
}

RUVIA_TEST(head_buffer_reset_and_reuse) {
    ResponseHeadBuffer buffer(std::pmr::get_default_resource());
    buffer.append(std::string(kResponseHeadStackBytes + 10, 'z'));  // spill
    RUVIA_CHECK(!buffer.view().empty());
    buffer.reset();
    RUVIA_CHECK(buffer.view().empty());
    // After reset a small append is served from the stack again.
    buffer.append("small");
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("small"));
    RUVIA_CHECK(buffer.canAppendOnStack(1));
}
