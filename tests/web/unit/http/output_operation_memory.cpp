#include <array>
#include <chrono>
#include <coroutine>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "http/streaming_access.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "websocket/websocket_access.h"

namespace {

struct output_sink final {
    std::vector<std::string> writes_;
    std::vector<std::string> trailers_;
    bool suspend_next_{false};
    bool fail_next_{false};
    bool fail_after_suspend_{false};
    const char* expected_storage_{nullptr};
    bool retained_storage_{false};
    std::coroutine_handle<> continuation_{};
};

struct suspend final {
    output_sink& sink_;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        sink_.continuation_ = continuation;
    }

    void await_resume() const noexcept {}
};

ruvia::task<void> write_output(void* target, std::string_view value) {
    auto& sink_value = *static_cast<output_sink*>(target);
    sink_value.retained_storage_ = value.data() == sink_value.expected_storage_;
    sink_value.writes_.emplace_back(value);
    if (std::exchange(sink_value.fail_next_, false)) {
        throw std::runtime_error("output failed");
    }
    if (std::exchange(sink_value.suspend_next_, false)) {
        co_await suspend{sink_value};
        if (std::exchange(sink_value.fail_after_suspend_, false)) {
            throw std::runtime_error("output failed after suspension");
        }
    }
}

ruvia::task<void> end_output(void* target, std::span<const ruvia::http_header_view> trailers) {
    auto& sink_value = *static_cast<output_sink*>(target);
    for (const auto trailer : trailers) {
        sink_value.trailers_.emplace_back(std::string(trailer.name()) + "=" + std::string(trailer.value()));
    }
    co_return;
}

ruvia::task<ruvia::timer_sleep_result> sleep_output(
    void*, std::chrono::milliseconds, const ruvia::stop_token&) {
    co_return ruvia::timer_sleep_result::elapsed;
}

void bind_output(void*, ruvia::context*, ruvia::task<ruvia::http_response> (*)(ruvia::context&)) noexcept {}
void release_output(void*) noexcept {}
bool output_false(void*) noexcept {
    return false;
}

ruvia::response_stream_writer make_writer(
    output_sink& sink_value, std::pmr::memory_resource& resource) noexcept {
    return ruvia::detail::streaming_access::make_response_stream_writer(resource, &sink_value, &write_output,
        &end_output, &sleep_output, &bind_output, &release_output, &output_false, &output_false);
}

ruvia::task<void> write_many(ruvia::response_stream_writer& writer) {
    for (int i = 0; i < 16; ++i) {
        co_await writer.write(std::string(256, static_cast<char>('a' + i)));
    }
}

struct websocket_sink final {
    std::vector<std::string> writes_;
    std::string close_reason_;
    bool fail_next_{false};
    bool last_compress_{true};
};

ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void*) {
    co_return std::nullopt;
}

ruvia::task<void> write_websocket(void* target, ruvia::websocket_opcode, std::string_view payload_value, bool compress) {
    auto& sink_value = *static_cast<websocket_sink*>(target);
    sink_value.last_compress_ = compress;
    sink_value.writes_.emplace_back(payload_value);
    if (std::exchange(sink_value.fail_next_, false)) {
        throw std::runtime_error("websocket output failed");
    }
    co_return;
}

ruvia::task<void> close_websocket(void* target, ruvia::websocket_close_options options) {
    static_cast<websocket_sink*>(target)->close_reason_ = std::string(options.reason_.view());
    co_return;
}

ruvia::websocket make_websocket(
    websocket_sink& sink_value, std::pmr::memory_resource& resource) noexcept {
    return ruvia::detail::websocket_access::make(resource, &sink_value, &read_websocket,
        &write_websocket, &close_websocket);
}

ruvia::task<void> await_operation(ruvia::scoped_operation<void>& operation) {
    co_await std::move(operation);
}

ruvia::task<void> write_many_websocket(ruvia::websocket& socket) {
    for (int i = 0; i < 16; ++i) {
        co_await socket.text(std::string(256, static_cast<char>('a' + i)));
    }
}

void run(asio::io_context& context_value, ruvia::task<void> operation) {
    context_value.restart();
    auto future = asio::co_spawn(context_value,
        ruvia::as_awaitable(std::move(operation)), asio::use_future);
    context_value.run();
    future.get();
}

}  // namespace

RUVIA_TEST(output_operations_return_owner_allocations_after_repeated_success) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    auto writer = make_writer(sink, owner);
    asio::io_context context_value(1);

    run(context_value, write_many(writer));

    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{16});
}

RUVIA_TEST(output_failed_operation_returns_owner_allocations) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    sink.fail_next_ = true;
    auto writer = make_writer(sink, owner);
    asio::io_context context_value(1);
    bool failed = false;
    try {
        run(context_value, write_many(writer));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(output_cold_operation_discard_returns_owner_allocations) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    auto writer = make_writer(sink, owner);
    {
        auto operation = writer.write(std::string(256, 'c'));
        RUVIA_CHECK(owner.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(output_rvalue_move_reuses_compatible_owner_allocation) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    auto writer = make_writer(sink, owner);
    asio::io_context context_value(1);

    {
        std::pmr::string payload_value(256, 'p', &owner);
        sink.expected_storage_ = payload_value.data();
        auto operation = writer.write(std::move(payload_value));
        run(context_value, await_operation(operation));
        RUVIA_CHECK(sink.retained_storage_);
    }
    // A moved-from string can retain Debug STL metadata until its destruction.
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sink.writes_[0], std::string(256, 'p'));
}

RUVIA_TEST(output_rvalue_move_copies_incompatible_input_before_source_destruction) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    auto writer = make_writer(sink, owner);
    asio::io_context context_value(1);

    auto operation = [&] {
        ruvia::test::counting_memory_resource source;
        std::pmr::string payload_value(256, 'q', &source);
        return writer.write(std::move(payload_value));
    }();

    run(context_value, await_operation(operation));
    RUVIA_CHECK_EQ(sink.writes_[0], std::string(256, 'q'));
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(output_sse_trailers_and_websocket_close_snapshot_inputs) {
    ruvia::test::counting_memory_resource owner;
    output_sink stream_sink;
    auto writer = make_writer(stream_sink, owner);
    auto sse = ruvia::detail::streaming_access::make_sse_writer(writer);
    asio::io_context context_value(1);
    std::string data = "event-data";
    std::string event = "event-name";
    auto sse_operation = sse.write(ruvia::sse_message{.data_ = data, .event_ = event});
    data.assign("changed-data");
    event.assign("changed-event");
    run(context_value, await_operation(sse_operation));

    std::string trailer_name = "X-Output-Trailer";
    std::string trailer_value = "snapshot";
    const std::array<ruvia::http_header_view, 1> trailers{
        ruvia::http_header_view{trailer_name, trailer_value}};
    auto trailer_operation = writer.end(trailers);
    trailer_name.assign("changed-name");
    trailer_value.assign("changed-value");
    run(context_value, await_operation(trailer_operation));

    RUVIA_CHECK_EQ(stream_sink.writes_[0], std::string("event: event-name\ndata: event-data\n\n"));
    RUVIA_CHECK_EQ(stream_sink.trailers_[0], std::string("X-Output-Trailer=snapshot"));

    websocket_sink socket_sink;
    auto socket = make_websocket(socket_sink, owner);
    std::string reason = "close-snapshot";
    auto close_operation = socket.close({.code_ = 1000, .reason_ = reason});
    reason.assign("changed-reason");
    run(context_value, await_operation(close_operation));
    RUVIA_CHECK_EQ(socket_sink.close_reason_, std::string("close-snapshot"));
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(output_pending_operation_returns_owner_allocation_after_resume) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    sink.suspend_next_ = true;
    auto writer = make_writer(sink, owner);
    asio::io_context context_value(1);
    auto operation = writer.write(std::string(256, 's'));
    auto future = asio::co_spawn(context_value, ruvia::as_awaitable(await_operation(operation)), asio::use_future);
    context_value.poll();
    RUVIA_CHECK(owner.live_allocations() > 0);
    sink.continuation_.resume();
    context_value.run();
    future.get();
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(output_pending_failure_returns_owner_allocation_and_releases_lane) {
    ruvia::test::counting_memory_resource owner;
    output_sink sink;
    sink.suspend_next_ = true;
    sink.fail_after_suspend_ = true;
    auto writer = make_writer(sink, owner);
    asio::io_context context_value(1);
    auto operation = writer.write(std::string(256, 'f'));
    auto future = asio::co_spawn(context_value, ruvia::as_awaitable(await_operation(operation)), asio::use_future);
    context_value.poll();
    RUVIA_CHECK(owner.live_allocations() > 0);
    sink.continuation_.resume();
    context_value.run();
    bool failed = false;
    try {
        future.get();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});

    auto next_operation = writer.write(std::string(256, 'r'));
    run(context_value, await_operation(next_operation));
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(output_facade_destruction_reclaims_pending_writer_and_websocket_operations) {
    std::optional<ruvia::test::counting_memory_resource> stream_owner(std::in_place);
    output_sink stream_sink;
    auto writer_operation = [&] {
        auto writer = make_writer(stream_sink, *stream_owner);
        return writer.write(std::string(256, 'w'));
    }();
    RUVIA_CHECK(stream_owner->allocation_count() > 0);
    RUVIA_CHECK_EQ(stream_owner->live_allocations(), std::size_t{0});
    stream_owner.reset();

    std::optional<ruvia::test::counting_memory_resource> socket_owner(std::in_place);
    websocket_sink socket_sink;
    auto websocket_operation = [&] {
        auto socket = make_websocket(socket_sink, *socket_owner);
        return socket.text(std::string(256, 's'));
    }();
    RUVIA_CHECK(socket_owner->allocation_count() > 0);
    RUVIA_CHECK_EQ(socket_owner->live_allocations(), std::size_t{0});
    socket_owner.reset();
}

RUVIA_TEST(websocket_output_operations_return_owner_allocations_after_repeated_success) {
    ruvia::test::counting_memory_resource owner;
    websocket_sink sink;
    auto socket = make_websocket(sink, owner);
    asio::io_context context_value(1);

    run(context_value, write_many_websocket(socket));

    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{16});
}

RUVIA_TEST(websocket_send_options_survive_lazy_execution_and_owned_payload) {
    ruvia::test::counting_memory_resource owner;
    websocket_sink sink;
    auto socket = make_websocket(sink, owner);
    asio::io_context context_value(1);
    {
        auto discarded = socket.binary(std::string(256, 'd'), {.compress_ = false});
    }
    RUVIA_CHECK(sink.writes_.empty());
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
    auto operation = socket.binary(std::pmr::string(256, 's', &owner), {.compress_ = false});
    run(context_value, await_operation(operation));
    RUVIA_CHECK(!sink.last_compress_);
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
    auto normal = socket.text(std::string(256, 'n'));
    run(context_value, await_operation(normal));
    RUVIA_CHECK(sink.last_compress_);
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(websocket_rvalue_move_copies_incompatible_input_before_source_destruction) {
    ruvia::test::counting_memory_resource owner;
    websocket_sink sink;
    auto socket = make_websocket(sink, owner);
    asio::io_context context_value(1);

    auto operation = [&] {
        ruvia::test::counting_memory_resource source;
        std::pmr::string payload_value(256, 'q', &source);
        return socket.text(std::move(payload_value));
    }();

    run(context_value, await_operation(operation));
    RUVIA_CHECK_EQ(sink.writes_[0], std::string(256, 'q'));
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}

RUVIA_TEST(websocket_failed_output_returns_owner_allocations) {
    ruvia::test::counting_memory_resource owner;
    websocket_sink sink;
    sink.fail_next_ = true;
    auto socket = make_websocket(sink, owner);
    asio::io_context context_value(1);
    bool failed = false;
    try {
        run(context_value, write_many_websocket(socket));
    } catch (const std::runtime_error&) {
        failed = true;
    }

    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});

    auto next_operation = socket.text(std::string(256, 'r'));
    run(context_value, await_operation(next_operation));
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}
