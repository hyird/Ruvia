#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "server/http_response_head_buffer.h"
#include "test_harness.h"

namespace {

using ruvia::detail::response_head_buffer_type;
using ruvia::detail::response_head_stack_bytes;

#if !defined(_MSC_VER)
// The MSVC debug pmr::string does not complete this synthetic
// resource-thrown-bad_alloc growth probe, so the spill guarantee below is
// unverified on that standard library. Nothing else covers it: the overflow
// test rejects on the max_size precondition and returns before spill_to_heap
// runs, so only this probe pins spill_to_heap's strong guarantee (a throwing
// reserve must leave the discriminant on stack_state_type).
class rejecting_memory_resource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t, std::size_t) override {
        throw std::bad_alloc();
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
#endif  // !_MSC_VER

}  // namespace

RUVIA_TEST(head_buffer_stack_appends) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());
    buffer.append("HTTP/1.1 ");
    buffer.append_unsigned(200);
    buffer.append(' ');
    buffer.append("OK");
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("HTTP/1.1 200 OK"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));
}

RUVIA_TEST(head_buffer_empty_view_append_preserves_storage_and_content) {
    const std::string_view empty;
    response_head_buffer_type buffer(std::pmr::get_default_resource());

    buffer.append(empty);
    RUVIA_CHECK(buffer.view().empty());
    RUVIA_CHECK(buffer.can_append_on_stack(0));
    RUVIA_CHECK(buffer.can_append_on_stack(1));

    buffer.append("prefix");
    buffer.append(empty);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("prefix"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));

    const std::string full(response_head_stack_bytes, 's');
    buffer.reset();
    buffer.append(full);
    buffer.append(empty);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(full));
    RUVIA_CHECK(buffer.can_append_on_stack(0));
    RUVIA_CHECK(!buffer.can_append_on_stack(1));

    const std::string large(response_head_stack_bytes + 1, 'h');
    buffer.reset();
    buffer.append(large);
    buffer.append(empty);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(large));
    RUVIA_CHECK(!buffer.can_append_on_stack(0));

    buffer.reset();
    buffer.append(empty);
    RUVIA_CHECK(buffer.view().empty());
    RUVIA_CHECK(buffer.can_append_on_stack(0));
    RUVIA_CHECK(buffer.can_append_on_stack(1));
}

RUVIA_TEST(head_buffer_stack_cursor_bulk_write_commits_and_guards_bounds) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());

    // The bulk fast path hands out a raw cursor when the bound fits; writing
    // through it and committing the end advances the buffer, visible via view().
    char* cursor_value = buffer.stack_cursor(3);
    RUVIA_CHECK(cursor_value != nullptr);
    cursor_value[0] = 'a';
    cursor_value[1] = 'b';
    cursor_value[2] = 'c';
    buffer.commit_stack(cursor_value + 3);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("abc"));

    // A subsequent cursor starts after the committed bytes (used_ advanced).
    char* next_value = buffer.stack_cursor(2);
    RUVIA_CHECK(next_value != nullptr);
    RUVIA_CHECK(next_value == cursor_value + 3);
    next_value[0] = 'd';
    next_value[1] = 'e';
    buffer.commit_stack(next_value + 2);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("abcde"));

    // The overflow guard: a bound exceeding the remaining stack space yields
    // nullptr (the caller must fall back to append) -- without it a bulk writer
    // would run past the fixed stack buffer. Exactly-fits is still granted.
    const auto remaining = response_head_stack_bytes - buffer.view().size();
    RUVIA_CHECK(buffer.stack_cursor(remaining + 1) == nullptr);
    RUVIA_CHECK(buffer.stack_cursor(remaining) != nullptr);

    // Once spilled to the heap, no stack cursor is offered at all.
    buffer.append(std::string(response_head_stack_bytes, 'x'));  // forces the spill
    RUVIA_CHECK(!buffer.can_append_on_stack(1));
    RUVIA_CHECK(buffer.stack_cursor(1) == nullptr);
}

RUVIA_TEST(head_buffer_spills_to_heap_preserving_content) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());
    const std::string large(response_head_stack_bytes + 100, 'x');  // exceeds the stack buffer
    buffer.append("prefix:");
    buffer.append(large);  // forces the spill to heap
    const std::string expected = "prefix:" + large;
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
    RUVIA_CHECK(!buffer.can_append_on_stack(1));  // now on the heap
}

RUVIA_TEST(head_buffer_char_append_spill_boundary) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());
    for (std::size_t i = 0; i < response_head_stack_bytes; ++i) {
        buffer.append('a');
    }
    RUVIA_CHECK(buffer.can_append_on_stack(0));  // exactly full still fits a zero-byte append
    RUVIA_CHECK(!buffer.can_append_on_stack(1));
    buffer.append('b');  // one more spills to heap
    RUVIA_CHECK_EQ(buffer.view().size(), response_head_stack_bytes + 1);
    RUVIA_CHECK_EQ(buffer.view().back(), 'b');
    RUVIA_CHECK_EQ(buffer.view().front(), 'a');
}

RUVIA_TEST(head_buffer_reset_and_reuse) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());
    buffer.append(std::string(response_head_stack_bytes + 10, 'z'));  // spill
    RUVIA_CHECK(!buffer.view().empty());
    buffer.reset();
    RUVIA_CHECK(buffer.view().empty());
    // After reset a small append is served from the stack again.
    buffer.append("small");
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("small"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));
}

RUVIA_TEST(head_buffer_generated_appends_preserve_prefix_across_storage_boundaries) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());
    buffer.append("prefix:");
    buffer.append_generated(3, [](char* bytes_value) noexcept { std::fill_n(bytes_value, 3, 'a'); });
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("prefix:aaa"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));

    buffer.append_generated(response_head_stack_bytes, [](char* bytes_value) noexcept {
        std::fill_n(bytes_value, response_head_stack_bytes, 'b');
    });
    std::string expected = "prefix:aaa" + std::string(response_head_stack_bytes, 'b');
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
    RUVIA_CHECK(!buffer.can_append_on_stack(0));

    buffer.append_generated(2, [](char* bytes_value) noexcept { std::fill_n(bytes_value, 2, 'c'); });
    expected += "cc";
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
    buffer.append_generated(0, [](char*) noexcept {});
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));

    bool invoked = false;
    bool rejected = false;
    try {
        buffer.append_generated(std::numeric_limits<std::size_t>::max(),
            [&](char*) noexcept { invoked = true; });
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(!invoked);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
    buffer.reset();
    buffer.append_generated(1, [](char* bytes_value) noexcept { *bytes_value = 'z'; });
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("z"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));
}

#if !defined(_MSC_VER)
RUVIA_TEST(head_buffer_failed_spill_preserves_stack_state) {
    rejecting_memory_resource resource;
    response_head_buffer_type buffer(&resource);
    const std::string stack_contents(response_head_stack_bytes, 's');
    buffer.append(stack_contents);

    bool allocation_failed = false;
    try {
        buffer.append('x');
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view(stack_contents));
    RUVIA_CHECK(!buffer.can_append_on_stack(1));

    buffer.reset();
    buffer.append("retry");
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("retry"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));
}
#endif  // !_MSC_VER

RUVIA_TEST(head_buffer_rejects_overflow_without_changing_storage_state) {
    response_head_buffer_type buffer(std::pmr::get_default_resource());
    buffer.append("prefix");

    bool length_rejected = false;
    try {
        buffer.reserve_additional(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error&) {
        length_rejected = true;
    }
    RUVIA_CHECK(length_rejected);
    RUVIA_CHECK_EQ(buffer.view(), std::string_view("prefix"));
    RUVIA_CHECK(buffer.can_append_on_stack(1));
}
