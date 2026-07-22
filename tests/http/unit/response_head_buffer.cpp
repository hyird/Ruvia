#include "test_harness.h"

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"

namespace {

using ruvia::detail::kResponseHeadStackBytes;
using ruvia::detail::ResponseHeadBuffer;

template <typename T>
concept ExposesRvalueResponseHeadBufferStorage =
    requires(T&& buffer) { std::move(buffer).view(); } ||
    requires(T&& buffer) { std::move(buffer).stackCursor(std::size_t{}); };

static_assert(!ExposesRvalueResponseHeadBufferStorage<ResponseHeadBuffer>);

class RejectingMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t, std::size_t) override {
        throw std::bad_alloc();
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

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

RUVIA_TEST(head_buffer_stack_cursor_bulk_write_commits_and_guards_bounds) {
    ResponseHeadBuffer buffer(std::pmr::get_default_resource());

    // The bulk fast path hands out a raw cursor when the bound fits; writing
    // through it and committing the end advances the buffer, visible via view().
    char* cursor = buffer.stackCursor(3);
    RUVIA_CHECK(cursor != nullptr);
    cursor[0] = 'a';
    cursor[1] = 'b';
    cursor[2] = 'c';
    buffer.commitStack(cursor + 3);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("abc"));

    // A subsequent cursor starts after the committed bytes (used_ advanced).
    char* next = buffer.stackCursor(2);
    RUVIA_CHECK(next != nullptr);
    RUVIA_CHECK(next == cursor + 3);
    next[0] = 'd';
    next[1] = 'e';
    buffer.commitStack(next + 2);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("abcde"));

    // The overflow guard: a bound exceeding the remaining stack space yields
    // nullptr (the caller must fall back to append) -- without it a bulk writer
    // would run past the fixed stack buffer. Exactly-fits is still granted.
    const auto remaining = kResponseHeadStackBytes - buffer.view().size();
    RUVIA_CHECK(buffer.stackCursor(remaining + 1) == nullptr);
    RUVIA_CHECK(buffer.stackCursor(remaining) != nullptr);

    // Once spilled to the heap, no stack cursor is offered at all.
    buffer.append(std::string(kResponseHeadStackBytes, 'x'));  // forces the spill
    RUVIA_CHECK(!buffer.canAppendOnStack(1));
    RUVIA_CHECK(buffer.stackCursor(1) == nullptr);
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

RUVIA_TEST(head_buffer_failed_spill_preserves_stack_state) {
    RejectingMemoryResource resource;
    ResponseHeadBuffer buffer(&resource);
    const std::string stackContents(kResponseHeadStackBytes, 's');
    buffer.append(stackContents);

    bool allocationFailed = false;
    try {
        buffer.append('x');
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    RUVIA_CHECK(allocationFailed);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(stackContents));
    RUVIA_CHECK(!buffer.canAppendOnStack(1));

    buffer.reset();
    buffer.append("retry");
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("retry"));
    RUVIA_CHECK(buffer.canAppendOnStack(1));
}

RUVIA_TEST(head_buffer_rejects_overflow_without_changing_storage_state) {
    ResponseHeadBuffer buffer(std::pmr::get_default_resource());
    buffer.append("prefix");

    bool lengthRejected = false;
    try {
        buffer.reserveAdditional(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error&) {
        lengthRejected = true;
    }
    RUVIA_CHECK(lengthRejected);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("prefix"));
    RUVIA_CHECK(buffer.canAppendOnStack(1));
}
