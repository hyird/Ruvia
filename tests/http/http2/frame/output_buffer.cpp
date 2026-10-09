#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "http2/http2_frame_codec.h"
#include "http2/http2_output_buffer.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_error_code;
using ruvia::detail::http2_frame_header_bytes;
using ruvia::detail::http2_frame_type;
using ruvia::detail::http2_output_buffer;
using ruvia::detail::http2_output_consume_status;
using ruvia::detail::http2_parse_frame_header;
using ruvia::detail::http2_read32;

#if !defined(_MSC_VER)
class toggle_rejecting_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations() noexcept {
        reject_ = true;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};
#endif  // !_MSC_VER

const unsigned char* bytes(const char* value) noexcept {
    return reinterpret_cast<const unsigned char*>(value);
}

}  // namespace

RUVIA_TEST(http2_output_buffer_serializes_one_contiguous_frame) {
    http2_output_buffer output(std::pmr::get_default_resource());
    output.append_frame(http2_frame_type::headers, 0x5, 7, "abc", "de");

    const auto pending = output.pending();
    RUVIA_CHECK(output.wants_write());
    RUVIA_CHECK_EQ(pending.size(), http2_frame_header_bytes + 5);
    const auto header_value = http2_parse_frame_header(pending.substr(0, http2_frame_header_bytes));
    RUVIA_CHECK_EQ(header_value.length_, std::uint32_t{5});
    RUVIA_CHECK_EQ(header_value.flags_, std::uint8_t{0x5});
    RUVIA_CHECK_EQ(header_value.stream_id_, std::uint32_t{7});
    RUVIA_CHECK_EQ(pending.substr(http2_frame_header_bytes), std::string_view("abcde"));
}

RUVIA_TEST(http2_output_buffer_rejects_over_consumption_transactionally) {
    http2_output_buffer output(std::pmr::get_default_resource());
    output.append_bytes("abcdef");

    RUVIA_CHECK(output.consume(2) == http2_output_consume_status::pending);
    const std::string before(output.pending());
    RUVIA_CHECK(output.consume(before.size() + 1) == http2_output_consume_status::out_of_range);
    RUVIA_CHECK_EQ(output.pending(), std::string_view(before));
    RUVIA_CHECK(output.wants_write());

    RUVIA_CHECK(output.consume(before.size()) == http2_output_consume_status::drained);
    RUVIA_CHECK(output.pending().empty());
    RUVIA_CHECK(!output.wants_write());
}

RUVIA_TEST(http2_output_buffer_take_copies_only_the_pending_suffix) {
    std::pmr::monotonic_buffer_resource output_resource;
    std::pmr::monotonic_buffer_resource destination_resource;
    http2_output_buffer output(&output_resource);
    output.append_bytes("consumed-pending");
    RUVIA_CHECK(
        output.consume(std::string_view("consumed-").size()) == http2_output_consume_status::pending);

    std::pmr::string destination(&destination_resource);
    output.take(destination);
    RUVIA_CHECK_EQ(std::string_view(destination), std::string_view("pending"));
    RUVIA_CHECK(output.pending().empty());
    RUVIA_CHECK(!output.wants_write());
}

RUVIA_TEST(http2_output_buffer_owns_reset_frame_serialization) {
    http2_output_buffer output(std::pmr::get_default_resource());
    output.append_rst_stream(9, http2_error_code::cancel);

    const auto pending = output.pending();
    const auto header_value = http2_parse_frame_header(pending.substr(0, http2_frame_header_bytes));
    RUVIA_CHECK(header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(header_value.length_, std::uint32_t{4});
    RUVIA_CHECK_EQ(header_value.stream_id_, std::uint32_t{9});
    RUVIA_CHECK_EQ(http2_read32(bytes(pending.data()) + http2_frame_header_bytes),
        static_cast<std::uint32_t>(http2_error_code::cancel));
}

#if !defined(_MSC_VER)
// MSVC's debug pmr::string stalls in this synthetic throwing-growth probe.
RUVIA_TEST(http2_output_buffer_frame_append_is_atomic_on_allocation_failure) {
    toggle_rejecting_memory_resource resource;
    http2_output_buffer output(&resource);
    output.append_frame(http2_frame_type::headers, 0, 1, "seed");
    const std::string before(output.pending());

    resource.reject_allocations();
    bool allocation_failed = false;
    try {
        output.append_frame(http2_frame_type::data, 0, 1, std::string(128, 'x'));
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }

    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK_EQ(output.pending(), std::string_view(before));
}
#endif  // !_MSC_VER
