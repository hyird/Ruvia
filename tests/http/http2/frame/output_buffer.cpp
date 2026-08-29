#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2OutputBuffer.h"

namespace {

using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2FrameType;
using ruvia::detail::Http2OutputBuffer;
using ruvia::detail::Http2OutputConsumeStatus;
using ruvia::detail::http2ParseFrameHeader;
using ruvia::detail::http2Read32;
using ruvia::detail::kHttp2FrameHeaderBytes;

#if !defined(_MSC_VER)
class ToggleRejectingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations() noexcept {
        reject_ = true;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};
#endif  // !_MSC_VER

template <typename T>
concept ExposesRvalueHttp2OutputBuffer = requires(T&& output) { std::move(output).pending(); };

static_assert(!ExposesRvalueHttp2OutputBuffer<Http2OutputBuffer>);

const unsigned char* bytes(const char* value) noexcept {
    return reinterpret_cast<const unsigned char*>(value);
}

}  // namespace

RUVIA_TEST(http2_output_buffer_serializes_one_contiguous_frame) {
    Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendFrame(Http2FrameType::kHeaders, 0x5, 7, "abc", "de");

    const auto pending = output.pending();
    RUVIA_CHECK(output.wantsWrite());
    RUVIA_CHECK_EQ(pending.size(), kHttp2FrameHeaderBytes + 5);
    const auto header = http2ParseFrameHeader(pending.substr(0, kHttp2FrameHeaderBytes));
    RUVIA_CHECK_EQ(header.length, std::uint32_t{5});
    RUVIA_CHECK_EQ(header.flags, std::uint8_t{0x5});
    RUVIA_CHECK_EQ(header.streamId, std::uint32_t{7});
    RUVIA_CHECK_EQ(pending.substr(kHttp2FrameHeaderBytes), std::string_view("abcde"));
}

RUVIA_TEST(http2_output_buffer_rejects_over_consumption_transactionally) {
    Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendBytes("abcdef");

    RUVIA_CHECK(output.consume(2) == Http2OutputConsumeStatus::kPending);
    const std::string before(output.pending());
    RUVIA_CHECK(output.consume(before.size() + 1) == Http2OutputConsumeStatus::kOutOfRange);
    RUVIA_CHECK_EQ(output.pending(), std::string_view(before));
    RUVIA_CHECK(output.wantsWrite());

    RUVIA_CHECK(output.consume(before.size()) == Http2OutputConsumeStatus::kDrained);
    RUVIA_CHECK(output.pending().empty());
    RUVIA_CHECK(!output.wantsWrite());
}

RUVIA_TEST(http2_output_buffer_take_copies_only_the_pending_suffix) {
    std::pmr::monotonic_buffer_resource outputResource;
    std::pmr::monotonic_buffer_resource destinationResource;
    Http2OutputBuffer output(&outputResource);
    output.appendBytes("consumed-pending");
    RUVIA_CHECK(
        output.consume(std::string_view("consumed-").size()) == Http2OutputConsumeStatus::kPending);

    std::pmr::string destination(&destinationResource);
    output.take(destination);
    RUVIA_CHECK_EQ(std::string_view(destination), std::string_view("pending"));
    RUVIA_CHECK(output.pending().empty());
    RUVIA_CHECK(!output.wantsWrite());
}

RUVIA_TEST(http2_output_buffer_owns_reset_frame_serialization) {
    Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendRstStream(9, Http2ErrorCode::kCancel);

    const auto pending = output.pending();
    const auto header = http2ParseFrameHeader(pending.substr(0, kHttp2FrameHeaderBytes));
    RUVIA_CHECK(header.type == static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(header.length, std::uint32_t{4});
    RUVIA_CHECK_EQ(header.streamId, std::uint32_t{9});
    RUVIA_CHECK_EQ(http2Read32(bytes(pending.data()) + kHttp2FrameHeaderBytes),
        static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
}

#if !defined(_MSC_VER)
// MSVC's debug pmr::string stalls in this synthetic throwing-growth probe.
RUVIA_TEST(http2_output_buffer_frame_append_is_atomic_on_allocation_failure) {
    ToggleRejectingMemoryResource resource;
    Http2OutputBuffer output(&resource);
    output.appendFrame(Http2FrameType::kHeaders, 0, 1, "seed");
    const std::string before(output.pending());

    resource.rejectAllocations();
    bool allocationFailed = false;
    try {
        output.appendFrame(Http2FrameType::kData, 0, 1, std::string(128, 'x'));
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }

    RUVIA_CHECK(allocationFailed);
    RUVIA_CHECK_EQ(output.pending(), std::string_view(before));
}
#endif  // !_MSC_VER
