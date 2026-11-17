#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "http2_connection.h"
#include "http_range.h"
#include "http_response.h"

// Replacement operators belong only to this isolated test executable.
// The thread-local arm confines each fault to one public operation.
namespace {
std::atomic<std::size_t> live_allocations{};
thread_local bool counting = false;
thread_local std::size_t allocation_index = 0;
thread_local std::size_t failure_index = (std::numeric_limits<std::size_t>::max)();

void before_allocation() {
    if (counting && allocation_index++ == failure_index) {
        throw std::bad_alloc();
    }
}

void release_allocation(void* pointer) noexcept {
    if (pointer != nullptr) {
        std::free(pointer);
        --live_allocations;
    }
}

void release_aligned_allocation(void* pointer) noexcept {
    if (pointer != nullptr) {
        void* storage = nullptr;
        std::memcpy(&storage, static_cast<char*>(pointer) - sizeof(storage), sizeof(storage));
        std::free(storage);
        --live_allocations;
    }
}

struct allocation_scope final {
    explicit allocation_scope(std::size_t failure) {
        allocation_index = 0;
        failure_index = failure;
        counting = true;
    }
    ~allocation_scope() {
        counting = false;
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct outbound_allocation_observation final {
    std::size_t allocations{};
    bool failed{};
    bool terminal{};
};
}  // namespace

void* operator new(std::size_t bytes) {
    before_allocation();
    void* pointer = std::malloc(bytes == 0 ? 1 : bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    ++live_allocations;
    return pointer;
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    before_allocation();
    const auto requested_alignment = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    bytes = std::max(bytes, std::size_t{1});
    const auto overhead = sizeof(void*) + requested_alignment - 1;
    if (bytes > (std::numeric_limits<std::size_t>::max)() - overhead) {
        throw std::bad_alloc();
    }
    void* storage = std::malloc(bytes + overhead);
    if (storage == nullptr) {
        throw std::bad_alloc();
    }
    void* pointer = static_cast<char*>(storage) + sizeof(storage);
    auto space = bytes + requested_alignment - 1;
    if (std::align(requested_alignment, bytes, pointer, space) == nullptr) {
        std::terminate();
    }
    std::memcpy(static_cast<char*>(pointer) - sizeof(storage), &storage, sizeof(storage));
    ++live_allocations;
    return pointer;
}
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return ::operator new(bytes, alignment);
}
void operator delete(void* pointer) noexcept {
    release_allocation(pointer);
}
void operator delete[](void* pointer) noexcept {
    release_allocation(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    release_allocation(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    release_allocation(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    release_allocation(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    release_allocation(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    release_aligned_allocation(pointer);
}

outbound_allocation_observation exercise_warmed_outbound_compression(std::size_t failure) {
    const auto initial_live = live_allocations.load();
    outbound_allocation_observation observed;
    {
        std::deque<std::string> retained;
        auto client = ruvia::http2_connection::client({});
        auto server = ruvia::http2_connection::server({});
        const auto transfer = [&](ruvia::http2_connection& from, ruvia::http2_connection& to) {
            auto& bytes = retained.emplace_back();
            (void)from.take_output_batch((std::numeric_limits<std::size_t>::max)(), bytes);
            require(to.feed(bytes) == ruvia::http2_feed_result::accepted, "peer rejected encoded output");
        };
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        for (unsigned round = 0; round != 2; ++round) {
            const auto submitted = client.submit_request_head(ruvia::http_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/fault"});
            require(std::holds_alternative<std::uint32_t>(submitted), "request rejected");
            transfer(client, server);
            auto request = server.next_event();
            auto* head = request ? request->request_head() : nullptr;
            require(head != nullptr, "missing request head");
            auto end = server.next_event();
            require(end && end->message_end() != nullptr, "missing request end");
            const std::string value(round == 0 ? 16 : 2000, round == 0 ? 'a' : 'b');
            ruvia::http_response response;
            response.header("x-fault", value);
            response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
            const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
            bool committed = false;
            if (round == 1) {
                allocation_scope scope(failure);
                try {
                    const auto result = server.submit_response_head(head->stream_id(), response, plan);
                    committed = std::holds_alternative<ruvia::http_buffered_response_write_plan>(result);
                } catch (const std::bad_alloc&) {
                    observed.failed = true;
                }
                observed.allocations = allocation_index;
            } else {
                const auto result = server.submit_response_head(head->stream_id(), response, plan);
                committed = std::holds_alternative<ruvia::http_buffered_response_write_plan>(result);
            }
            observed.terminal = server.connection_error().has_value();
            if (observed.failed && observed.terminal) {
                const auto retry = server.submit_response_head(head->stream_id(), response, plan);
                const auto* error = std::get_if<ruvia::http_response_head_error>(&retry);
                require(error != nullptr && *error == ruvia::http_response_head_error::closed, "fatal compression failure accepted another submission");
            } else {
                if (observed.failed) {
                    const auto retry = server.submit_response_head(head->stream_id(), response, plan);
                    committed = std::holds_alternative<ruvia::http_buffered_response_write_plan>(retry);
                }
                require(committed, "recoverable preparation failure could not commit");
                transfer(server, client);
                auto received = client.next_event();
                auto* received_head = received ? received->response_head() : nullptr;
                require(received_head != nullptr, "peer lost response header after compression");
                bool matched = false;
                for (const auto& field : received_head->head().headers()) {
                    if (field.name() == "x-fault") {
                        matched = field.value() == value;
                    }
                }
                require(matched, "peer decoded a stale compressed field");
                auto received_end = client.next_event();
                require(received_end && received_end->message_end() != nullptr, "peer lost response end");
                require(!client.connection_error(), "peer compression state was corrupted");
            }
            (void)server.release(std::move(*head));
        }
    }
    require(live_allocations.load() == initial_live, "connection or encoder leaked an allocation");
    return observed;
}

TEST(Ruvia, http2_outbound_hpack_allocation_failures_preserve_state_and_reclaim_memory) {
    const auto baseline = exercise_warmed_outbound_compression((std::numeric_limits<std::size_t>::max)());
    ASSERT_FALSE(baseline.failed);
    ASSERT_FALSE(baseline.terminal);
    ASSERT_NE(baseline.allocations, 0U);
    std::size_t terminal = 0;
    for (std::size_t index = 0; index != baseline.allocations; ++index) {
        const auto observed = exercise_warmed_outbound_compression(index);
        EXPECT_TRUE(observed.failed) << "allocation " << index;
        terminal += observed.terminal;
    }
    EXPECT_NE(terminal, 0U);
    ::testing::Test::RecordProperty("allocation_failure_sites", std::to_string(baseline.allocations));
    ::testing::Test::RecordProperty("terminal_failure_sites", std::to_string(terminal));
}

TEST(Ruvia, multipart_range_slicer_can_retry_after_an_allocation_failure) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,4-5", 6);
    ruvia::http_byte_range_slicer slicer(ranges, 6, "text/plain", "parts");
    std::string output;
    slicer.write("a", output);
    ASSERT_EQ(output, "--parts\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-1/6\r\n\r\na");
    output.reserve(output.size() + 1);
    // The first payload byte fits, but appending the following CRLF must grow.
    output.resize(output.capacity() - 1, '_');
    const auto before = output;
    bool failed = false;
    {
        allocation_scope scope(0);
        try {
            slicer.write("bcdef", output);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
    }
    ASSERT_TRUE(failed);
    EXPECT_EQ(output, before);
    ASSERT_EQ(slicer.current_offset(), 1U);
    EXPECT_FALSE(slicer.complete());

    slicer.write("bcdef", output);
    EXPECT_TRUE(slicer.complete());
    EXPECT_EQ(output, before + "b\r\n--parts\r\nContent-Type: text/plain\r\nContent-Range: bytes 4-5/6\r\n\r\nef\r\n--parts--\r\n");
}
