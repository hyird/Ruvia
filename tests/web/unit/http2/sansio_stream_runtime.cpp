#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_byte_range.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_modes.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "failing_memory_resource.h"
#include "http2/http2_buffered_response_write.h"
#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_request_body.h"
#include "http2/http2_sans_io_response_stream_sink.h"
#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "http2/http2_sans_io_ws_transport.h"
#include "http2_wire_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class counting_memory_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{0};
    std::size_t deallocations_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using ruvia::http_response_coding_selection;
using ruvia::protocol_byte_limit;
using ruvia::detail::http2_buffered_request_body;
using ruvia::detail::http2_buffered_response_writer;
using ruvia::detail::http2_data_output_budget;
using ruvia::detail::http2_request_body_runtime;
using ruvia::detail::http2_sans_io_body_queue;
using ruvia::detail::http2_sans_io_response_stream_sink;
using ruvia::detail::http2_sans_io_stream_runtime;
using ruvia::detail::http2_sans_io_stream_runtime_table;
using ruvia::detail::http2_sans_io_termination;
using ruvia::detail::http2_send_window_wait_result;
using ruvia::detail::http2_streaming_request_body;
using ruvia::detail::request_body_mode;
using ruvia::detail::route_resolution;

ruvia::task<ruvia::http_response> invalid_streaming_head(ruvia::context&) {
    ruvia::http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.header("Content-Length", "not-a-number");
    co_return response;
}

ruvia::task<ruvia::http_response> ok_streaming_head(ruvia::context&) {
    co_return ruvia::http_response({.resource_ = std::pmr::new_delete_resource()});
}

[[nodiscard]] http_response_coding_selection identity_response_coding() {
    ruvia::http_response_coding_qualities qualities;
    const auto selected = http_response_coding_selection::select(qualities);
    if (selected.selected() == nullptr) {
        throw std::logic_error("identity response coding selection was empty");
    }
    return *selected.selected();
}

http2_sans_io_stream_runtime& ensure_accepted_runtime(http2_sans_io_stream_runtime_table& table_value,
    std::uint32_t stream_id, std::pmr::memory_resource* resource) {
    (void)resource;
    return table_value.ensure_accepted(stream_id);
}

void handshake(ruvia::http2_connection& connection) {
    if (connection.feed(ruvia::http2_client_preface) != ruvia::http2_feed_result::accepted) {
        throw std::runtime_error("HTTP/2 server rejected preface");
    }
    char settings[ruvia::http2_frame_header_bytes];
    (void)ruvia::encode_http2_frame_header(
        settings, 0, ruvia::http2_frame_type::settings, 0, 0);
    if (connection.feed(std::string_view(settings, sizeof(settings))) !=
        ruvia::http2_feed_result::accepted) {
        throw std::runtime_error("HTTP/2 server rejected SETTINGS");
    }
    (void)connection.consume_output(connection.pending_output().size());
}

[[nodiscard]] ruvia::http2_request_head_event drive_get_request(
    ruvia::http2_connection& connection, std::pmr::memory_resource* resource) {
    std::pmr::string block(resource);
    http2_connection_test::encode_get_request(block);
    const auto frame = http2_connection_test::headers_frame(resource, 1,
        std::uint8_t{0x4} | std::uint8_t{0x1},
        std::string_view(block.data(), block.size()));
    if (connection.feed(std::string_view(frame.data(), frame.size())) !=
        ruvia::http2_feed_result::accepted) {
        throw std::runtime_error("HTTP/2 server rejected GET request");
    }
    std::optional<ruvia::http2_request_head_event> request_lease;
    while (auto event = connection.next_event()) {
        if (auto* request_head = event->request_head()) {
            request_lease.emplace(std::move(*request_head));
        }
    }
    (void)connection.consume_output(connection.pending_output().size());
    if (!request_lease.has_value()) {
        throw std::runtime_error("HTTP/2 server emitted no request head");
    }
    return std::move(*request_lease);
}

auto stop_io_on_completion(asio::io_context& io) {
    return [&io](std::exception_ptr) { asio::post(io, [&io] { io.stop(); }); };
}

asio::awaitable<void> collect_send_window_result(
    ruvia::http2_connection& connection, std::optional<http2_send_window_wait_result>& result_value) {
    result_value = co_await ruvia::as_awaitable(
        ruvia::detail::await_http2_send_window(connection, 1, nullptr));
}

asio::awaitable<void> acquire_data_budget_slots(http2_data_output_budget& budget,
    const std::array<std::uint32_t, 4>& stream_ids,
    ruvia::detail::http2_sans_io_stream_signal& signal, std::array<bool, 4>& acquired) {
    for (std::size_t i = 0; i < stream_ids.size(); ++i) {
        acquired[i] = co_await ruvia::as_awaitable(
            budget.acquire(stream_ids[i], signal));
    }
}

asio::awaitable<void> acquire_data_budget_slot(http2_data_output_budget& budget,
    std::uint32_t stream_id, ruvia::detail::http2_sans_io_stream_signal& signal, bool& acquired) {
    acquired = co_await ruvia::as_awaitable(budget.acquire(stream_id, signal));
}

}  // namespace

RUVIA_TEST(http2_send_window_wait_rejects_missing_stream_or_signal) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto connection = ruvia::http2_connection::server();
    std::optional<http2_send_window_wait_result> result;
    asio::co_spawn(io, collect_send_window_result(connection, result), stop_io_on_completion(io));
    io.run();
    io.restart();
    RUVIA_CHECK(result.has_value());
    RUVIA_CHECK(result->ready() == nullptr);
    RUVIA_CHECK(result->aborted() != nullptr);
}

RUVIA_TEST(http2_stream_sleep_reports_elapsed_result) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    http2_sans_io_termination termination;
    std::optional<ruvia::timer_sleep_result> observed;
    const auto wait_for_elapsed = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        co_return co_await ruvia::detail::http2_sans_io_sleep_awaiter(
            worker_value, termination, std::chrono::milliseconds(0));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::as_awaitable(wait_for_elapsed());
        },
        stop_io_on_completion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::timer_sleep_result::elapsed);
}

RUVIA_TEST(http2_worker_shutdown_reports_typed_sleep_result) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    http2_sans_io_termination termination;
    std::optional<ruvia::timer_sleep_result> observed;
    const auto wait_for_shutdown = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        co_return co_await ruvia::detail::http2_sans_io_sleep_awaiter(
            worker_value, termination, std::chrono::hours(1));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::as_awaitable(wait_for_shutdown());
        },
        stop_io_on_completion(io));
    asio::post(io, [&attachment] { attachment.stop(); });
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::timer_sleep_result::stop_requested);
}

RUVIA_TEST(http2_stream_sleep_observes_request_stop_token) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    http2_sans_io_termination termination;
    ruvia::stop_source source;
    auto token = source.token();
    std::optional<ruvia::timer_sleep_result> observed;
    const auto wait_for_stop = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        co_return co_await ruvia::detail::http2_sans_io_sleep_awaiter(
            worker_value, termination, std::chrono::hours(1), token);
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::as_awaitable(wait_for_stop());
        },
        stop_io_on_completion(io));
    asio::post(io, [&source] { source.request_stop(); });
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::timer_sleep_result::stop_requested);
}

RUVIA_TEST(http2_stream_sleep_transfers_off_worker_stop_to_timer) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    http2_sans_io_termination termination;
    ruvia::stop_source source;
    std::optional<ruvia::timer_sleep_result> observed;
    const auto wait_for_stop = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        co_return co_await ruvia::detail::http2_sans_io_sleep_awaiter(
            worker_value, termination, std::chrono::hours(1), source.token());
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            observed = co_await ruvia::as_awaitable(wait_for_stop());
        },
        stop_io_on_completion(io));
    (void)io.poll();
    std::thread requester([&source] { source.request_stop(); });
    requester.join();
    io.restart();
    io.run();
    io.restart();

    RUVIA_CHECK(observed.has_value());
    RUVIA_CHECK_EQ(*observed, ruvia::timer_sleep_result::stop_requested);
}

RUVIA_TEST(http2_session_termination_cancels_stream_sleep_with_exact_error) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    http2_sans_io_termination termination;
    std::error_code observed;
    const auto wait_for_termination = [&]() -> ruvia::task<ruvia::timer_sleep_result> {
        co_return co_await ruvia::detail::http2_sans_io_sleep_awaiter(
            worker_value, termination, std::chrono::hours(1));
    };

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::as_awaitable(wait_for_termination());
            } catch (const std::system_error& error) {
                observed = error.code();
            }
        },
        stop_io_on_completion(io));
    asio::post(io, [&termination] {
        (void)termination.terminate(std::make_error_code(std::errc::connection_reset));
    });
    io.run();
    io.restart();

    RUVIA_CHECK_EQ(observed, std::make_error_code(std::errc::connection_reset));
}

RUVIA_TEST(http2_stream_head_failure_aborts_precommit_state) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::http2_connection::server({.resource_ = &resource});
    handshake(connection);
    [[maybe_unused]] auto request_lease = drive_get_request(connection, &resource);

    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal write_signal(worker_value);
    ruvia::detail::http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);

    ruvia::worker_memory worker_memory;
    ruvia::request_memory request_memory(worker_memory);
    auto [request, parse_error] =
        ruvia::make_parsed_http_request("GET", "/", {}, {}, request_memory.resource());
    RUVIA_CHECK(!parse_error.has_value());
    auto context_value = ruvia::detail::context_access::make(
        request_memory, request, ruvia::test::test_context_services());

    http2_sans_io_response_stream_sink sink_value(connection, 1, ruvia::http_response_stream_kind::generic,
        write_signal, stream_signal, &resource, ruvia::http_known_method::get,
        identity_response_coding(), ruvia::detail::http_response_coding_availability::identity_only);
    sink_value.bind_context(&context_value, &invalid_streaming_head);

    bool first_failed = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::as_awaitable(sink_value.write("body"));
            } catch (const std::exception&) {
                first_failed = true;
            }
        },
        stop_io_on_completion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(first_failed);
    RUVIA_CHECK(!sink_value.committed());
    RUVIA_CHECK(sink_value.aborted());
    RUVIA_CHECK_EQ(connection.stream_receive_status(1),
        ruvia::http2_stream_receive_status::ended);

    // A failed head is terminal even though no HEADERS were emitted. The
    // second attempt must not reach the compression object's "already
    // prepared" state or manufacture a different representation.
    bool retry_rejected = false;
    io.restart();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::as_awaitable(sink_value.write("retry"));
            } catch (const std::logic_error&) {
                retry_rejected = true;
            }
        },
        stop_io_on_completion(io));
    io.run();
    io.restart();
    RUVIA_CHECK(retry_rejected);
}

RUVIA_TEST(http2_response_stream_empty_end_is_idempotent_after_late_termination) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::http2_connection::server({.resource_ = &resource});
    handshake(connection);
    [[maybe_unused]] auto request_lease = drive_get_request(connection, &resource);

    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal write_signal(worker_value);
    ruvia::detail::http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);

    ruvia::worker_memory worker_memory;
    ruvia::request_memory request_memory(worker_memory);
    auto [request, parse_error] =
        ruvia::make_parsed_http_request("GET", "/", {}, {}, request_memory.resource());
    RUVIA_CHECK(!parse_error.has_value());
    auto context_value = ruvia::detail::context_access::make(
        request_memory, request, ruvia::test::test_context_services());

    http2_sans_io_response_stream_sink sink_value(connection, 1, ruvia::http_response_stream_kind::generic,
        write_signal, stream_signal, &resource, ruvia::http_known_method::get,
        identity_response_coding(), ruvia::detail::http_response_coding_availability::identity_only);
    sink_value.bind_context(&context_value, &ok_streaming_head);

    bool first_end_completed = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await ruvia::as_awaitable(sink_value.end({}));
            first_end_completed = true;
        },
        stop_io_on_completion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(first_end_completed);
    RUVIA_CHECK(sink_value.committed());

    (void)termination.terminate(std::make_error_code(std::errc::connection_reset));

    bool second_end_completed = false;
    bool second_end_rejected = false;
    io.restart();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::as_awaitable(sink_value.end({}));
                second_end_completed = true;
            } catch (const std::system_error&) {
                second_end_rejected = true;
            }
        },
        stop_io_on_completion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(!second_end_rejected);
    RUVIA_CHECK(second_end_completed);
}

RUVIA_TEST(http2_web_body_queue_preserves_fifo_and_tracks_backlog) {
    http2_sans_io_body_queue queue(std::pmr::get_default_resource());
    RUVIA_CHECK(queue.empty());
    queue.enqueue("first");
    queue.enqueue("second");
    queue.enqueue("third");
    RUVIA_CHECK_EQ(queue.queued_bytes(), std::size_t{16});
    const auto active = queue.pop();
    RUVIA_CHECK_EQ(active, std::string_view("firstsecondthird"));
    RUVIA_CHECK_EQ(queue.queued_bytes(), std::size_t{0});
    queue.enqueue("fourth");
    RUVIA_CHECK_EQ(active, std::string_view("firstsecondthird"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("fourth"));
    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK_EQ(queue.queued_bytes(), std::size_t{0});
}

RUVIA_TEST(http2_web_body_queue_reuses_storage_and_ignores_empty_chunks) {
    http2_sans_io_body_queue queue(std::pmr::get_default_resource());
    queue.enqueue({});
    RUVIA_CHECK(queue.empty());
    std::string expected;
    for (int i = 0; i < 50; ++i) {
        const auto piece = std::to_string(i);
        expected += piece;
        queue.enqueue(piece);
    }
    RUVIA_CHECK_EQ(queue.pop(), std::string_view(expected));
    RUVIA_CHECK(queue.empty());
    queue.enqueue("reused");
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("reused"));
}

RUVIA_TEST(http2_web_body_queue_holds_data_credit_until_consumption_and_releases_storage) {
    auto connection = ruvia::http2_connection::server();
    handshake(connection);
    std::pmr::string request_head(std::pmr::get_default_resource());
    ruvia::hpack_encoder::encode_header(request_head, ":method", "POST");
    ruvia::hpack_encoder::encode_header(request_head, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(request_head, ":path", "/stream");
    ruvia::hpack_encoder::encode_header(request_head, ":authority", "example.com");
    ruvia::hpack_encoder::encode_header(request_head, "content-length", "114688");
    const auto head = http2_connection_test::headers_frame(std::pmr::get_default_resource(), 1,
        std::uint8_t{0x4},
        std::string_view(request_head.data(), request_head.size()));
    RUVIA_CHECK(connection.feed(std::string_view(head.data(), head.size())) ==
                ruvia::http2_feed_result::accepted);
    std::optional<ruvia::http2_request_head_event> request_lease;
    while (auto event = connection.next_event()) {
        if (auto* value = event->request_head()) {
            request_lease.emplace(std::move(*value));
        }
    }
    RUVIA_CHECK(request_lease.has_value());
    (void)connection.consume_output(connection.pending_output().size());

    counting_memory_resource resource;
    {
        http2_sans_io_body_queue queue(&resource);
        const std::string payload_value(16 * 1024, 'd');
        for (int round = 0; round != 2; ++round) {
            for (int frame_index = 0; frame_index != 3; ++frame_index) {
                const auto frame = http2_connection_test::data_frame(
                    std::pmr::get_default_resource(), 1, 0, payload_value);
                RUVIA_CHECK(connection.feed(std::string_view(frame.data(), frame.size())) ==
                            ruvia::http2_feed_result::accepted);
                bool found_chunk = false;
                while (auto event = connection.next_event()) {
                    if (auto* data = event->message_body_chunk()) {
                        const bool accepted = queue.enqueue_bounded(
                            data->bytes(), data->take_credit(), 2 * payload_value.size());
                        RUVIA_CHECK_EQ(accepted, frame_index != 2);
                        found_chunk = true;
                    }
                }
                RUVIA_CHECK(found_chunk);
            }
            RUVIA_CHECK_EQ(queue.queued_bytes(), std::size_t{2 * payload_value.size()});
            const auto active = queue.pop();
            RUVIA_CHECK_EQ(active.size(), std::size_t{2 * payload_value.size()});
            RUVIA_CHECK(active.front() == 'd' && active.back() == 'd');
            // The view remains valid while its DATA credits are returned.
            queue.release_active_credits();
            RUVIA_CHECK(active.front() == 'd' && active.back() == 'd');
            (void)connection.consume_output(connection.pending_output().size());
        }

        const std::string exception_payload(16 * 1024, 'x');
        const auto frame = http2_connection_test::data_frame(
            std::pmr::get_default_resource(), 1, 0, exception_payload);
        RUVIA_CHECK(connection.feed(std::string_view(frame.data(), frame.size())) ==
                    ruvia::http2_feed_result::accepted);
        asio::io_context& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().handle();
        bool exception_path_ran = false;
        {
            ruvia::detail::http2_sans_io_termination termination;
            ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
            try {
                while (auto event = connection.next_event()) {
                    if (auto* data = event->message_body_chunk()) {
                        http2_sans_io_body_queue discarded(&resource);
                        discarded.enqueue(data->bytes(), data->take_credit());
                        ruvia::detail::http2_sans_io_request_body_reader reader_value(
                            connection, 1, discarded, stream_signal);
                        auto unstarted_read = reader_value.read();
                        (void)unstarted_read;
                        throw std::runtime_error("simulate handler failure after enqueue");
                    }
                }
            } catch (const std::runtime_error&) {
                exception_path_ran = true;
            }
            RUVIA_CHECK(exception_path_ran);
        }
        RUVIA_CHECK(resource.allocations_ > 0);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(http2_web_body_queue_aggregates_one_byte_data_credits) {
    // HTTP/2 deliberately batches WINDOW_UPDATE until half the initial window
    // is consumed. Keep every DATA event's credit in the queue until the test
    // crosses that threshold instead of expecting a frame per returned byte.
    constexpr std::size_t update_threshold = 512 * 1024;
    constexpr std::size_t one_byte_frames = 2048;
    auto connection = ruvia::http2_connection::server();
    handshake(connection);
    std::pmr::string request_head(std::pmr::get_default_resource());
    ruvia::hpack_encoder::encode_header(request_head, ":method", "POST");
    ruvia::hpack_encoder::encode_header(request_head, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(request_head, ":path", "/stream");
    ruvia::hpack_encoder::encode_header(request_head, ":authority", "example.com");
    ruvia::hpack_encoder::encode_header(request_head, "content-length", std::to_string(4 * update_threshold));
    const auto head = http2_connection_test::headers_frame(std::pmr::get_default_resource(), 1,
        std::uint8_t{0x4}, request_head);
    RUVIA_CHECK(connection.feed(head) == ruvia::http2_feed_result::accepted);
    std::optional<ruvia::http2_request_head_event> request_lease;
    while (auto event = connection.next_event()) {
        if (auto* value = event->request_head()) {
            request_lease.emplace(std::move(*value));
        }
    }
    RUVIA_CHECK(request_lease.has_value());
    (void)connection.consume_output(connection.pending_output().size());

    counting_memory_resource resource;
    http2_sans_io_body_queue queue(&resource);
    for (std::size_t i = 0; i < one_byte_frames; ++i) {
        const auto frame = http2_connection_test::data_frame(
            std::pmr::get_default_resource(), 1, 0, "x");
        RUVIA_CHECK(connection.feed(frame) == ruvia::http2_feed_result::accepted);
        bool found_chunk = false;
        while (auto event = connection.next_event()) {
            if (auto* data = event->message_body_chunk()) {
                RUVIA_CHECK(queue.enqueue_bounded(
                    data->bytes(), data->take_credit(), update_threshold));
                found_chunk = true;
            }
        }
        RUVIA_CHECK(found_chunk);
    }
    std::size_t remaining = update_threshold - one_byte_frames;
    const std::string payload_value(16384, 'x');
    while (remaining != 0) {
        const auto count = std::min(remaining, payload_value.size());
        const auto frame = http2_connection_test::data_frame(
            std::pmr::get_default_resource(), 1, 0, std::string_view(payload_value.data(), count));
        RUVIA_CHECK(connection.feed(frame) == ruvia::http2_feed_result::accepted);
        bool found_chunk = false;
        while (auto event = connection.next_event()) {
            if (auto* data = event->message_body_chunk()) {
                RUVIA_CHECK(queue.enqueue_bounded(
                    data->bytes(), data->take_credit(), update_threshold));
                found_chunk = true;
            }
        }
        RUVIA_CHECK(found_chunk);
        remaining -= count;
    }
    RUVIA_CHECK_EQ(queue.queued_bytes(), update_threshold);
    RUVIA_CHECK(resource.allocations_ <= 32);
    RUVIA_CHECK_EQ(connection.pending_output().size(), std::size_t{0});

    const auto active = queue.pop();
    RUVIA_CHECK_EQ(active.size(), update_threshold);
    RUVIA_CHECK_EQ(connection.pending_output().size(), std::size_t{0});
    queue.enqueue("next");
    RUVIA_CHECK_EQ(active.front(), 'x');
    RUVIA_CHECK_EQ(active.back(), 'x');
    RUVIA_CHECK_EQ(connection.pending_output().size(), std::size_t{0});
    (void)queue.pop();
    RUVIA_CHECK(connection.pending_output().size() > 0);
    (void)connection.consume_output(connection.pending_output().size());

    // Discarding queued credit returns it too, but WINDOW_UPDATE is thresholded.
    {
        http2_sans_io_body_queue discarded(&resource);
        const std::string discard_payload(16384, 'y');
        for (std::size_t i = 0; i < update_threshold / discard_payload.size(); ++i) {
            const auto frame = http2_connection_test::data_frame(
                std::pmr::get_default_resource(), 1, 0, discard_payload);
            RUVIA_CHECK(connection.feed(frame) == ruvia::http2_feed_result::accepted);
            bool found_chunk = false;
            while (auto event = connection.next_event()) {
                if (auto* data = event->message_body_chunk()) {
                    discarded.enqueue(data->bytes(), data->take_credit());
                    found_chunk = true;
                }
            }
            RUVIA_CHECK(found_chunk);
        }
        RUVIA_CHECK_EQ(discarded.queued_bytes(), update_threshold);
        RUVIA_CHECK_EQ(connection.pending_output().size(), std::size_t{0});
    }
    RUVIA_CHECK(connection.pending_output().size() > 0);
    (void)connection.consume_output(connection.pending_output().size());

    failing_memory_resource rejecting_resource;
    {
        http2_sans_io_body_queue failed_queue(&rejecting_resource);
        const std::string failed_payload(16384, 'z');
        for (std::size_t i = 0; i < 32; ++i) {
            const auto frame = http2_connection_test::data_frame(
                std::pmr::get_default_resource(), 1, 0, failed_payload);
            RUVIA_CHECK(connection.feed(frame) == ruvia::http2_feed_result::accepted);
            while (auto event = connection.next_event()) {
                if (auto* data = event->message_body_chunk()) {
                    failed_queue.enqueue(data->bytes(), data->take_credit());
                }
            }
        }
        RUVIA_CHECK_EQ(failed_queue.queued_bytes(), update_threshold);

        bool append_failed = false;
        std::size_t retained_bytes = update_threshold;
        rejecting_resource.fail_after(0);
        // Leave failure armed until an append actually needs storage. PMR string
        // growth can retain different spare capacity on different implementations.
        for (std::size_t additional = 0; additional < update_threshold && !append_failed;
            additional += payload_value.size()) {
            const auto failed_frame = http2_connection_test::data_frame(
                std::pmr::get_default_resource(), 1, 0, payload_value);
            RUVIA_CHECK(connection.feed(failed_frame) == ruvia::http2_feed_result::accepted);
            while (auto event = connection.next_event()) {
                if (auto* data = event->message_body_chunk()) {
                    try {
                        failed_queue.enqueue(data->bytes(), data->take_credit());
                        retained_bytes += data->bytes().size();
                    } catch (const std::bad_alloc&) {
                        append_failed = true;
                    }
                }
            }
        }
        RUVIA_CHECK(append_failed);
        RUVIA_CHECK_EQ(failed_queue.queued_bytes(), retained_bytes);
        const auto retained_payload = failed_queue.pop();
        RUVIA_CHECK_EQ(retained_payload.size(), retained_bytes);
        if (retained_payload.size() >= update_threshold) {
            const auto initial_value = retained_payload.substr(0, update_threshold);
            const auto appended = retained_payload.substr(update_threshold);
            RUVIA_CHECK(std::ranges::all_of(initial_value, [](char byte) { return byte == 'z'; }));
            RUVIA_CHECK(std::ranges::all_of(appended, [](char byte) { return byte == 'x'; }));
        }
    }
    RUVIA_CHECK_EQ(rejecting_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK(connection.pending_output().size() > 0);
}

RUVIA_TEST(http2_web_body_queue_commits_backlog_only_after_storage_succeeds) {
    const std::string allocation_sized_chunk(256, 'x');

    failing_memory_resource first_chunk_resource;
    http2_sans_io_body_queue empty_queue(&first_chunk_resource);
    first_chunk_resource.fail_after(0);
    bool first_chunk_rejected = false;
    try {
        empty_queue.enqueue(allocation_sized_chunk);
    } catch (const std::bad_alloc&) {
        first_chunk_rejected = true;
    }
    RUVIA_CHECK(first_chunk_rejected);
    RUVIA_CHECK(empty_queue.empty());
    RUVIA_CHECK_EQ(empty_queue.queued_bytes(), std::size_t{0});

    failing_memory_resource overflow_resource;
    http2_sans_io_body_queue populated_queue(&overflow_resource);
    populated_queue.enqueue("retained");
    overflow_resource.fail_after(0);
    bool overflow_rejected = false;
    try {
        populated_queue.enqueue(allocation_sized_chunk);
    } catch (const std::bad_alloc&) {
        overflow_rejected = true;
    }
    RUVIA_CHECK(overflow_rejected);
    RUVIA_CHECK_EQ(populated_queue.queued_bytes(), std::size_t{8});
    RUVIA_CHECK_EQ(populated_queue.pop(), std::string_view("retained"));
    RUVIA_CHECK(populated_queue.empty());
}

RUVIA_TEST(http2_websocket_transport_empty_end_completes_with_zero_send_window) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::http2_connection::server({.resource_ = &resource});
    handshake(connection);

    // Advertise an empty per-stream send window before admitting the stream.
    char peer_settings[ruvia::http2_frame_header_bytes + 6]{};
    (void)ruvia::encode_http2_frame_header(peer_settings, 6,
        ruvia::http2_frame_type::settings, 0, 0);
    peer_settings[9] = 0;
    peer_settings[10] = 4;  // SETTINGS_INITIAL_WINDOW_SIZE
    RUVIA_CHECK(connection.feed(std::string_view(peer_settings, sizeof(peer_settings))) ==
                ruvia::http2_feed_result::accepted);
    (void)connection.consume_output(connection.pending_output().size());

    [[maybe_unused]] auto request_lease = drive_get_request(connection, &resource);
    const auto window = connection.send_window_state(1);
    RUVIA_CHECK(window.has_value());
    if (window.has_value()) {
        RUVIA_CHECK_EQ(window->available_, std::int32_t{0});
    }
    ruvia::http_response response({.resource_ = &resource});
    const auto head = connection.submit_streaming_response_head(1, std::move(response));
    RUVIA_CHECK(head == ruvia::http2_submit_status::accepted);
    (void)connection.consume_output(connection.pending_output().size());
    RUVIA_CHECK(!connection.has_queued_data(1));

    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal write_signal(worker_value);
    ruvia::detail::http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
    ruvia::detail::http2_sans_io_body_queue body_queue(&resource);
    ruvia::detail::http2_sans_io_ws_transport<asio::any_io_executor> transport(
        connection, 1, body_queue, stream_signal, write_signal, asio::any_io_executor(io.get_executor()));

    // A watchdog makes a regression fail instead of leaving this unit test hung.
    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(1));
    bool timed_out = false;
    bool completed = false;
    std::error_code write_error;
    watchdog.async_wait([&](const std::error_code& error) {
        if (!error) {
            timed_out = true;
            (void)termination.terminate(std::make_error_code(std::errc::timed_out));
            stream_signal.wake();
            io.stop();
        }
    });
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            write_error = co_await ruvia::as_awaitable(transport.write_bytes(
                {}, ruvia::websocket_transport_disposition::end_transport));
            completed = true;
            watchdog.cancel();
            attachment.stop(); }, stop_io_on_completion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(completed);
    RUVIA_CHECK(!timed_out);
    RUVIA_CHECK(!write_error);
    RUVIA_CHECK(!connection.has_queued_data(1));
    const auto output = connection.pending_output();
    const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame.has_value());
    if (frame.has_value()) {
        RUVIA_CHECK_EQ(frame->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
        RUVIA_CHECK_EQ(frame->flags_, std::uint8_t{0x1});
        RUVIA_CHECK_EQ(frame->stream_id_, std::uint32_t{1});
        RUVIA_CHECK_EQ(frame->length_, std::uint32_t{0});
    }
    attachment.stop();
}

RUVIA_TEST(http2_websocket_transport_abort_remains_noexcept_when_reset_output_allocation_fails) {
    failing_memory_resource resource;
    auto connection = ruvia::http2_connection::server({.resource_ = &resource});
    handshake(connection);
    std::pmr::string request_block(&resource);
    http2_connection_test::encode_get_request(request_block);
    const auto request_frame = http2_connection_test::headers_frame(&resource, 1,
        std::uint8_t{0x4},
        std::string_view(request_block.data(), request_block.size()));
    RUVIA_CHECK(connection.feed(std::string_view(request_frame.data(), request_frame.size())) ==
                ruvia::http2_feed_result::accepted);
    std::optional<ruvia::http2_request_head_event> request_lease;
    while (auto event = connection.next_event()) {
        if (auto* request_head = event->request_head()) {
            request_lease.emplace(std::move(*request_head));
        }
    }
    RUVIA_CHECK(request_lease.has_value());
    (void)connection.consume_output(connection.pending_output().size());

    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal write_signal(worker_value);
    ruvia::detail::http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
    ruvia::detail::http2_sans_io_body_queue queue(&resource);
    ruvia::detail::http2_sans_io_ws_transport<asio::any_io_executor> transport(
        connection, 1, queue, stream_signal, write_signal, asio::any_io_executor(io.get_executor()));

    std::pmr::string scratch(&resource);
    connection.take_output(scratch);
    char settings[ruvia::http2_frame_header_bytes];
    (void)ruvia::encode_http2_frame_header(
        settings, 0, ruvia::http2_frame_type::settings, 0, 0);
    RUVIA_CHECK(connection.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(connection.pending_output().size(),
        static_cast<std::size_t>(ruvia::http2_frame_header_bytes));

    resource.fail_after(0);
    bool aborted = false;
    bool read_completed = false;
    std::optional<ruvia::detail::http_stream_read_result> read_result;
    std::pmr::string read_buffer(&resource);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            read_result.emplace(co_await ruvia::as_awaitable(transport.read_more(read_buffer)));
            read_completed = true;
            if (aborted) {
                attachment.stop();
            } }, stop_io_on_completion(io));
    const auto post_result = worker_value.post([&] {
        transport.abort();
        aborted = true;
        if (read_completed) {
            attachment.stop();
        }
    });
    RUVIA_CHECK(post_result.accepted());
    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(1));
    watchdog.async_wait([&](const std::error_code& error) {
        if (!error && !read_completed) {
            io.stop();
        }
    });
    io.run();
    io.restart();
    RUVIA_CHECK(aborted);
    RUVIA_CHECK(read_completed);
    RUVIA_CHECK(read_result.has_value());
    if (read_result.has_value()) {
        RUVIA_CHECK(read_result->failure() != nullptr);
    }
    attachment.stop();
}

RUVIA_TEST(http2_buffered_response_writer_reports_failure_when_reset_output_allocation_fails) {
    failing_memory_resource resource;
    auto connection = ruvia::http2_connection::server({.resource_ = &resource});
    handshake(connection);
    [[maybe_unused]] auto request_lease = drive_get_request(connection, &resource);

    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal write_signal(worker_value);
    ruvia::detail::http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_runtime_table table(
        std::pmr::get_default_resource(), termination);
    ruvia::worker_memory worker_memory;
    ruvia::detail::http2_buffered_response_writer writer(connection, table, worker_memory, write_signal);

    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.header("Connection", "close");
    const auto write_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);

    std::pmr::string scratch(&resource);
    connection.take_output(scratch);
    char settings[ruvia::http2_frame_header_bytes];
    (void)ruvia::encode_http2_frame_header(
        settings, 0, ruvia::http2_frame_type::settings, 0, 0);
    RUVIA_CHECK(connection.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(connection.pending_output().size(),
        static_cast<std::size_t>(ruvia::http2_frame_header_bytes));

    resource.fail_after(0);
    bool threw = false;
    std::optional<ruvia::detail::http2_buffered_response_write_result> result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                result =
                    co_await ruvia::as_awaitable(writer.write(1, response, write_plan));
            } catch (const std::bad_alloc&) {
                threw = true;
            }
        },
        stop_io_on_completion(io));
    io.run();
    io.restart();

    RUVIA_CHECK(!threw);
    RUVIA_CHECK(result.has_value());
    if (result.has_value()) {
        RUVIA_CHECK(result->failed_before_commit() != nullptr ||
                    result->failed_after_commit() != nullptr);
    }
}
RUVIA_TEST(http2_buffered_response_writer_sends_multipart_file_slices_and_ends_stream) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia-http2-multipart-writer.bin";
    const std::string content(50000, '2');
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }
    counting_memory_resource resource;
    {
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        handshake(connection);
        [[maybe_unused]] auto request_lease = drive_get_request(connection, &resource);

        asio::io_context& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
        const auto worker_handle_value = attachment.loop().handle();
        ruvia::worker_signal write_signal(worker_handle_value);
        http2_sans_io_termination termination;
        http2_sans_io_stream_runtime_table table(std::pmr::get_default_resource(), termination);
        auto& runtime = table.ensure_accepted(1);
        RUVIA_CHECK(runtime.select_route(route_resolution{}, request_body_mode::buffered));
        RUVIA_CHECK(table.begin_dispatch(1, worker_handle_value) != nullptr);
        ruvia::worker_memory worker;
        http2_buffered_response_writer writer(connection, table, worker, write_signal);
        const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-19999,30000-49999", content.size());
        auto multipart = ruvia::make_http_multipart_byte_range_plan(
            ranges, content.size(), "text/plain", "h2_writer_boundary", {}, &resource);
        ruvia::http_response response({.resource_ = &resource});
        response.status(ruvia::http_status::partial_content);
        response.header("Content-Type", multipart.content_type());
        response.multipart_file_body(path, content.size(),
            ruvia::http_response_file_identity::unchecked(), std::move(multipart));
        const auto write_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);

        std::optional<ruvia::detail::http2_buffered_response_write_result> result;
        asio::co_spawn(io, [&]() -> asio::awaitable<void> { result.emplace(co_await ruvia::as_awaitable(writer.write(1, response, write_plan))); }, stop_io_on_completion(io));
        io.run();
        io.restart();
        RUVIA_CHECK(result.has_value());
        RUVIA_CHECK(result && result->completed() != nullptr);

        std::pmr::string wire(&resource);
        connection.take_output(wire);
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        std::string body;
        std::string content_length;
        bool saw_headers = false;
        bool saw_data_end_stream = false;
        std::size_t offset = 0;
        while (offset + ruvia::http2_frame_header_bytes <= wire.size()) {
            const auto frame_header = ruvia::parse_http2_frame_header(std::span<const char>(wire.data() + offset, ruvia::http2_frame_header_bytes)).value();
            offset += ruvia::http2_frame_header_bytes;
            RUVIA_CHECK(offset + frame_header.length_ <= wire.size());
            if (offset + frame_header.length_ > wire.size()) {
                break;
            }
            const std::string_view payload_value(wire.data() + offset, frame_header.length_);
            offset += frame_header.length_;
            if (frame_header.stream_id_ != 1) {
                continue;
            }
            if (frame_header.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                saw_headers = true;
                const auto decoded = decoder.decode(payload_value,
                    [&content_length](std::string_view name, std::string_view value) {
                        if (name == "content-length") {
                            content_length.assign(value);
                        }
                        return true;
                    });
                RUVIA_CHECK(decoded.decoded());
            } else if (frame_header.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(payload_value);
                saw_data_end_stream = saw_data_end_stream ||
                                      (frame_header.flags_ & std::uint8_t{0x1}) != 0;
            }
        }
        const std::string expected =
            "--h2_writer_boundary\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-19999/50000\r\n\r\n" +
            content.substr(0, 20000) + "\r\n--h2_writer_boundary\r\nContent-Type: text/plain\r\n" +
            "Content-Range: bytes 30000-49999/50000\r\n\r\n" + content.substr(30000) +
            "\r\n--h2_writer_boundary--\r\n";
        RUVIA_CHECK(saw_headers);
        RUVIA_CHECK(saw_data_end_stream);
        RUVIA_CHECK_EQ(body, expected);
        RUVIA_CHECK_EQ(content_length, std::to_string(expected.size()));
        attachment.stop();
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
    fs::remove(path);
}

RUVIA_TEST(http2_buffered_response_writer_fails_closed_on_multipart_file_identity_mismatch) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia-http2-multipart-identity.bin";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "0123456789";
    }
    counting_memory_resource resource;
    {
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        handshake(connection);
        [[maybe_unused]] auto request_lease = drive_get_request(connection, &resource);
        asio::io_context& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
        const auto worker_handle_value = attachment.loop().handle();
        ruvia::worker_signal write_signal(worker_handle_value);
        http2_sans_io_termination termination;
        http2_sans_io_stream_runtime_table table(std::pmr::get_default_resource(), termination);
        auto& runtime = table.ensure_accepted(1);
        RUVIA_CHECK(runtime.select_route(route_resolution{}, request_body_mode::buffered));
        RUVIA_CHECK(table.begin_dispatch(1, worker_handle_value) != nullptr);
        ruvia::worker_memory worker;
        http2_buffered_response_writer writer(connection, table, worker, write_signal);
        const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
        auto multipart = ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain", "h2_bad_identity", {}, &resource);
        ruvia::http_response response({.resource_ = &resource});
        response.status(ruvia::http_status::partial_content);
        response.header("Content-Type", multipart.content_type());
        response.multipart_file_body(path, 10, ruvia::http_response_file_identity::checked({}),
            std::move(multipart));
        const auto write_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        std::optional<ruvia::detail::http2_buffered_response_write_result> result;
        asio::co_spawn(io, [&]() -> asio::awaitable<void> { result.emplace(co_await ruvia::as_awaitable(writer.write(1, response, write_plan))); }, stop_io_on_completion(io));
        io.run();
        io.restart();
        RUVIA_CHECK(result && result->failed_after_commit() != nullptr);

        std::pmr::string wire(&resource);
        connection.take_output(wire);
        bool saw_reset = false;
        std::size_t offset = 0;
        while (offset + ruvia::http2_frame_header_bytes <= wire.size()) {
            const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(wire.data() + offset, ruvia::http2_frame_header_bytes)).value();
            offset += ruvia::http2_frame_header_bytes;
            RUVIA_CHECK(offset + header_value.length_ <= wire.size());
            if (offset + header_value.length_ > wire.size()) {
                break;
            }
            if (header_value.stream_id_ == 1 &&
                header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                saw_reset = true;
            }
            offset += header_value.length_;
        }
        RUVIA_CHECK(saw_reset);
        attachment.stop();
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
    fs::remove(path);
}

RUVIA_TEST(http2_web_route_selection_owns_exact_body_storage) {
    http2_sans_io_stream_runtime buffered_runtime(1, std::pmr::get_default_resource());
    RUVIA_CHECK(buffered_runtime.selected_route() == nullptr);
    RUVIA_CHECK(buffered_runtime.select_route(route_resolution{}, request_body_mode::buffered));
    auto* buffered_selection = buffered_runtime.selected_route();
    RUVIA_CHECK(buffered_selection != nullptr);
    RUVIA_CHECK(buffered_selection->resolution().not_found() != nullptr);
    auto& buffered_body = buffered_selection->body();
    auto* buffered = buffered_body.buffered();
    RUVIA_CHECK(buffered != nullptr);
    RUVIA_CHECK(buffered_body.streaming() == nullptr);
    RUVIA_CHECK(buffered_body.mode() == request_body_mode::buffered);
    RUVIA_CHECK(!buffered_runtime.select_route(route_resolution{}, request_body_mode::stream));
    const auto buffered_store = buffered_body.store("abc", protocol_byte_limit::limited(3), 0);
    RUVIA_CHECK(buffered_store.stored() != nullptr);
    RUVIA_CHECK_EQ(buffered->bytes(), std::string_view("abc"));
    RUVIA_CHECK_EQ(buffered_body.received_bytes(), std::size_t{3});

    http2_sans_io_stream_runtime streaming_runtime(3, std::pmr::get_default_resource());
    RUVIA_CHECK(streaming_runtime.select_route(route_resolution{}, request_body_mode::stream));
    auto& streaming_body = streaming_runtime.selected_route()->body();
    auto* streaming = streaming_body.streaming();
    RUVIA_CHECK(streaming != nullptr);
    RUVIA_CHECK(streaming_body.buffered() == nullptr);
    const auto first_streaming_store = streaming_body.store("one", protocol_byte_limit::unlimited(), 8);
    RUVIA_CHECK(first_streaming_store.stored() != nullptr);
    const auto second_streaming_store = streaming_body.store("two", protocol_byte_limit::unlimited(), 8);
    RUVIA_CHECK(second_streaming_store.stored() != nullptr);
    RUVIA_CHECK_EQ(streaming->queue().queued_bytes(), std::size_t{6});
}

RUVIA_TEST(http2_web_request_body_runtime_enforces_total_and_backlog_limits) {
    http2_sans_io_stream_runtime buffered_runtime(1, std::pmr::get_default_resource());
    RUVIA_CHECK(buffered_runtime.select_route(route_resolution{}, request_body_mode::buffered));
    auto& buffered_body = buffered_runtime.selected_route()->body();
    auto* buffered = buffered_body.buffered();
    const auto buffered_stored = buffered_body.store("1234", protocol_byte_limit::limited(5), 0);
    RUVIA_CHECK(buffered_stored.stored() != nullptr);
    const auto total_limit_failure = buffered_body.store("67", protocol_byte_limit::limited(5), 0);
    RUVIA_CHECK(total_limit_failure.protocol_failure() != nullptr);
    if (const auto* failure = total_limit_failure.protocol_failure()) {
        RUVIA_CHECK_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
    }
    RUVIA_CHECK_EQ(buffered_body.received_bytes(), std::size_t{4});
    RUVIA_CHECK_EQ(buffered->bytes(), std::string_view("1234"));

    http2_sans_io_stream_runtime streaming_runtime(3, std::pmr::get_default_resource());
    RUVIA_CHECK(streaming_runtime.select_route(route_resolution{}, request_body_mode::stream));
    auto& streaming_body = streaming_runtime.selected_route()->body();
    auto* streaming = streaming_body.streaming();
    const auto streaming_stored = streaming_body.store("1234", protocol_byte_limit::unlimited(), 5);
    RUVIA_CHECK(streaming_stored.stored() != nullptr);
    const auto backlog_overflow = streaming_body.store("67", protocol_byte_limit::unlimited(), 5);
    RUVIA_CHECK(backlog_overflow.backlog_overflow() != nullptr);
    RUVIA_CHECK_EQ(streaming_body.received_bytes(), std::size_t{4});
    RUVIA_CHECK_EQ(streaming->queue().pop(), std::string_view("1234"));
    const auto resumed_store = streaming_body.store("67", protocol_byte_limit::unlimited(), 5);
    RUVIA_CHECK(resumed_store.stored() != nullptr);
}

struct data_output_observation final {
    std::array<std::uint32_t, 8> stream_ids_{};
    std::array<std::size_t, 8> payload_bytes_{};
    std::size_t count_{0};
};

void observe_data_output(void* context_value, std::uint32_t stream_id, std::size_t payload_bytes) noexcept {
    auto& observation_value = *static_cast<data_output_observation*>(context_value);
    if (observation_value.count_ < observation_value.stream_ids_.size()) {
        observation_value.stream_ids_[observation_value.count_] = stream_id;
        observation_value.payload_bytes_[observation_value.count_] = payload_bytes;
        ++observation_value.count_;
    }
}

RUVIA_TEST(http2_data_output_batch_observes_only_successfully_taken_data_frames) {
    auto connection = ruvia::http2_connection::client();
    (void)connection.consume_output(connection.pending_output().size());
    char peer_settings[ruvia::http2_frame_header_bytes];
    (void)ruvia::encode_http2_frame_header(
        peer_settings, 0, ruvia::http2_frame_type::settings, 0, 0);
    RUVIA_CHECK(connection.feed(std::string_view(peer_settings, sizeof(peer_settings))) ==
                ruvia::http2_feed_result::accepted);

    const auto first = connection.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/a", .content_ = ruvia::http2_request_content::streaming()});
    const auto second = connection.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/b", .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(first.submitted() != nullptr);
    RUVIA_CHECK(second.submitted() != nullptr);
    if (!first.submitted() || !second.submitted()) {
        return;
    }
    const auto first_id = first.submitted()->stream_id();
    const auto second_id = second.submitted()->stream_id();
    RUVIA_CHECK(connection.submit_data(first_id, "one", ruvia::http2_end_stream::keep_open) ==
                ruvia::http2_data_submit_status::accepted);
    RUVIA_CHECK(connection.submit_data(second_id, "two!", ruvia::http2_end_stream::keep_open) ==
                ruvia::http2_data_submit_status::accepted);

    std::pmr::string output;
    output = "stale";
    output.clear();  // The Web wrapper clears reused scratch storage before batching.
    data_output_observation observation;
    const auto batch = connection.take_output_batch(1, output, observe_data_output, &observation);
    RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(output.size(), batch.bytes_);
    RUVIA_CHECK(!output.empty());
    RUVIA_CHECK(observation.count_ == 0);  // leading SETTINGS ACK/HEADERS are control output

    while (connection.wants_write()) {
        const auto next_value = connection.take_output_batch(16 * 1024, output, observe_data_output, &observation);
        RUVIA_CHECK(next_value.status_ == ruvia::http2_output_batch_status::taken);
        if (next_value.status_ != ruvia::http2_output_batch_status::taken) {
            break;
        }
    }
    RUVIA_CHECK_EQ(observation.count_, std::size_t{2});
    if (observation.count_ == 2) {
        RUVIA_CHECK_EQ(observation.stream_ids_[0], first_id);
        RUVIA_CHECK_EQ(observation.payload_bytes_[0], std::size_t{3});
        RUVIA_CHECK_EQ(observation.stream_ids_[1], second_id);
        RUVIA_CHECK_EQ(observation.payload_bytes_[1], std::size_t{4});
    }
}

RUVIA_TEST(http2_data_output_budget_caps_slots_and_waits_for_core_drain) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::http2_connection connection = ruvia::http2_connection::server();
    http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
    http2_data_output_budget budget(worker_value);

    // Cold acquisition has no side effect until started. The four accepted
    // reservations each represent at most one 16 KiB DATA frame, independent
    // of the connection's much larger protocol flow-control windows.
    auto cold = budget.acquire(99, stream_signal);
    (void)cold;
    std::size_t acquired = 0;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            for (const auto stream_id : {1U, 3U, 5U, 7U}) {
                if (co_await ruvia::as_awaitable(
                        budget.acquire(stream_id, stream_signal))) {
                    ++acquired;
                }
            }
            // A fifth stream cannot reserve another slot until one is released.
            bool fifth_acquired = false;
            asio::co_spawn(io,
                [&]() -> asio::awaitable<void> {
                    fifth_acquired = co_await ruvia::as_awaitable(
                        budget.acquire(9, stream_signal));
                }, stop_io_on_completion(io));
            (void)io.poll();
            RUVIA_CHECK(!fifth_acquired);

            // Control output is not charged to DATA slots. A server SETTINGS
            // frame remains independently pending while all DATA credits are held.
            RUVIA_CHECK(!connection.pending_output().empty());
            budget.release(3);
            (void)io.poll();
            RUVIA_CHECK(fifth_acquired);
            RUVIA_CHECK_EQ(acquired, std::size_t{4});
            budget.release(1);
            budget.release(5);
            budget.release(7);
            budget.release(9); }, stop_io_on_completion(io));
    io.run();
    io.restart();

    // Exercise the same ownership boundary with real serialized DATA from the
    // public connection API. Releasing a removed stream is not enough: its slot
    // stays held while bytes remain in core output and after they are handed to
    // the socket. Only the corresponding completed socket batch releases it.
    auto wire_connection = ruvia::http2_connection::client();
    (void)wire_connection.consume_output(wire_connection.pending_output().size());
    char peer_settings[ruvia::http2_frame_header_bytes];
    (void)ruvia::encode_http2_frame_header(
        peer_settings, 0, ruvia::http2_frame_type::settings, 0, 0);
    RUVIA_CHECK(wire_connection.feed(std::string_view(peer_settings, sizeof(peer_settings))) ==
                ruvia::http2_feed_result::accepted);
    (void)wire_connection.consume_output(wire_connection.pending_output().size());
    http2_data_output_budget wire_budget(worker_value);
    std::array<std::uint32_t, 4> stream_ids{};
    for (std::size_t i = 0; i < stream_ids.size(); ++i) {
        const auto submitted = wire_connection.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http2_request_content::streaming()});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        if (submitted.submitted() == nullptr) {
            continue;
        }
        stream_ids[i] = submitted.submitted()->stream_id();
        RUVIA_CHECK(wire_connection.submit_data(stream_ids[i], "data", ruvia::http2_end_stream::keep_open) ==
                    ruvia::http2_data_submit_status::accepted);
        wire_budget.note_data_submitted(stream_ids[i], 4);
    }
    bool wire_fifth_acquired = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            for (const auto id : stream_ids) {
                RUVIA_CHECK(co_await ruvia::as_awaitable(
                    wire_budget.acquire(id, stream_signal)));
            }
            asio::co_spawn(io,
                [&]() -> asio::awaitable<void> {
                    wire_fifth_acquired = co_await ruvia::as_awaitable(
                        wire_budget.acquire(99, stream_signal));
                }, stop_io_on_completion(io));
            (void)io.poll();
            RUVIA_CHECK(!wire_fifth_acquired);

            for (const auto id : stream_ids) {
                wire_budget.release(id);
            }
            wire_budget.reconcile(wire_connection, false);
            (void)io.poll();
            RUVIA_CHECK(!wire_fifth_acquired);

            // Consume leading HEADERS, stopping at the first actual DATA frame.
            while (true) {
                const auto pending = wire_connection.pending_output();
                const auto header_value = ruvia::parse_http2_frame_header(
                    std::span<const char>(pending.data(), pending.size()));
                RUVIA_CHECK(header_value.has_value());
                if (!header_value || header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    break;
                }
                RUVIA_CHECK(wire_connection.consume_output(ruvia::http2_frame_header_bytes + header_value->length_) !=
                            ruvia::http2_output_consume_status::out_of_range);
            }

            for (const auto id : stream_ids) {
                const auto pending = wire_connection.pending_output();
                const auto header_value = ruvia::parse_http2_frame_header(
                    std::span<const char>(pending.data(), pending.size()));
                RUVIA_CHECK(header_value.has_value());
                RUVIA_CHECK(header_value && header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
                if (!header_value || header_value->type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    break;
                }
                const auto frame_bytes = ruvia::http2_frame_header_bytes + header_value->length_;
                wire_budget.note_data_output(id, header_value->length_);
                RUVIA_CHECK(wire_connection.consume_output(frame_bytes) !=
                            ruvia::http2_output_consume_status::out_of_range);
                wire_budget.reconcile(wire_connection, false);
                (void)io.poll();
                RUVIA_CHECK(!wire_fifth_acquired);
                wire_budget.reconcile(wire_connection, true);
                (void)io.poll();
                RUVIA_CHECK(wire_fifth_acquired);
                wire_budget.release(99);
                wire_fifth_acquired = false;
                if (id != stream_ids.back()) {
                    asio::co_spawn(io,
                        [&]() -> asio::awaitable<void> {
                            wire_fifth_acquired = co_await ruvia::as_awaitable(
                                wire_budget.acquire(99, stream_signal));
                        }, stop_io_on_completion(io));
                    (void)io.poll();
                    RUVIA_CHECK(!wire_fifth_acquired);
                }
            } }, stop_io_on_completion(io));
    io.run();
    io.restart();
}

RUVIA_TEST(http2_data_output_budget_reconciles_discarded_queued_data_without_socket_output) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    auto connection = ruvia::http2_connection::client();
    (void)connection.consume_output(connection.pending_output().size());
    char peer_settings[ruvia::http2_frame_header_bytes + 6]{};
    (void)ruvia::encode_http2_frame_header(peer_settings, 6,
        ruvia::http2_frame_type::settings, 0, 0);
    peer_settings[9] = 0;
    peer_settings[10] = 4;
    RUVIA_CHECK(connection.feed(std::string_view(peer_settings, sizeof(peer_settings))) ==
                ruvia::http2_feed_result::accepted);
    (void)connection.consume_output(connection.pending_output().size());

    const auto submitted = connection.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const auto stream_id = submitted.submitted() == nullptr ? std::uint32_t{0}
                                                            : submitted.submitted()->stream_id();
    RUVIA_CHECK(connection.submit_data(stream_id, "queued", ruvia::http2_end_stream::keep_open) ==
                ruvia::http2_data_submit_status::queued);
    RUVIA_CHECK(connection.data_queue_state(stream_id) == ruvia::http2_data_queue_state::queued);
    RUVIA_CHECK_EQ(connection.pending_data_output_bytes(stream_id), std::size_t{0});

    http2_data_output_budget budget(worker_value);
    http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
    const std::array<std::uint32_t, 4> held_ids{stream_id, 5U, 7U, 9U};
    std::array<bool, 4> acquired{};
    asio::co_spawn(io, acquire_data_budget_slots(budget, held_ids, stream_signal, acquired), stop_io_on_completion(io));
    io.run();
    io.restart();
    for (const bool value : acquired) {
        RUVIA_CHECK(value);
    }
    for (const auto id : held_ids) {
        budget.note_data_submitted(id, id == stream_id ? 6 : 1);
    }

    bool fifth_acquired = false;
    asio::co_spawn(io, acquire_data_budget_slot(budget, 11, stream_signal, fifth_acquired), stop_io_on_completion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!fifth_acquired);

    char reset[ruvia::http2_frame_header_bytes + 4]{};
    (void)ruvia::encode_http2_frame_header(reset, 4,
        ruvia::http2_frame_type::rst_stream, 0, stream_id);
    RUVIA_CHECK(connection.feed(std::string_view(reset, sizeof(reset))) ==
                ruvia::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(connection.pending_data_output_bytes(stream_id), std::size_t{0});
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.release_and_reconcile(stream_id, connection);
        co_return; }, stop_io_on_completion(io));
    for (std::size_t attempt_value = 0; attempt_value < 32 && !fifth_acquired; ++attempt_value) {
        (void)io.poll();
        io.restart();
    }
    RUVIA_CHECK(fifth_acquired);
    if (!fifth_acquired) {
        (void)termination.terminate(std::make_error_code(std::errc::operation_canceled));
        for (std::size_t attempt_value = 0; attempt_value < 32; ++attempt_value) {
            (void)io.poll();
            io.restart();
        }
        io.stop();
    }
}

RUVIA_TEST(http2_data_output_budget_recovers_after_peer_reset_without_reusing_pending_output) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    auto connection = ruvia::http2_connection::client();
    (void)connection.consume_output(connection.pending_output().size());

    // Restrict the peer's initial stream window to one byte: the first byte is
    // serialized while the remainder of stream 1 stays queued in the core.
    char settings[ruvia::http2_frame_header_bytes + 6]{};
    (void)ruvia::encode_http2_frame_header(settings, 6,
        ruvia::http2_frame_type::settings, 0, 0);
    settings[9] = 0;
    settings[10] = 4;
    settings[14] = 1;
    RUVIA_CHECK(connection.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::http2_feed_result::accepted);
    (void)connection.consume_output(connection.pending_output().size());

    const auto first_result = connection.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http2_request_content::streaming()});
    const auto second_result = connection.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(first_result.submitted() != nullptr);
    RUVIA_CHECK(second_result.submitted() != nullptr);
    const auto first = first_result.submitted() == nullptr ? std::uint32_t{0} : first_result.submitted()->stream_id();
    const auto second = second_result.submitted() == nullptr ? std::uint32_t{0} : second_result.submitted()->stream_id();
    RUVIA_CHECK_EQ(first, std::uint32_t{1});
    RUVIA_CHECK_EQ(second, std::uint32_t{3});
    RUVIA_CHECK(connection.submit_data(first, "ab", ruvia::http2_end_stream::keep_open) ==
                ruvia::http2_data_submit_status::queued);
    RUVIA_CHECK_EQ(connection.pending_data_output_bytes(first), std::size_t{1});
    RUVIA_CHECK(connection.data_queue_state(first) == ruvia::http2_data_queue_state::queued);
    while (true) {
        const auto pending = connection.pending_output();
        const auto header_value = ruvia::parse_http2_frame_header(
            std::span<const char>(pending.data(), pending.size()));
        RUVIA_CHECK(header_value.has_value());
        if (!header_value || header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            break;
        }
        RUVIA_CHECK(connection.consume_output(ruvia::http2_frame_header_bytes + header_value->length_) !=
                    ruvia::http2_output_consume_status::out_of_range);
    }

    http2_data_output_budget budget(worker_value);
    http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
    const std::array<std::uint32_t, 4> held_ids{first, 5U, 7U, 9U};
    std::array<bool, 4> acquired{};
    asio::co_spawn(io, acquire_data_budget_slots(budget, held_ids, stream_signal, acquired), stop_io_on_completion(io));
    io.run();
    io.restart();
    for (std::size_t i = 0; i < acquired.size(); ++i) {
        RUVIA_CHECK(acquired[i]);
        budget.note_data_submitted(held_ids[i], 1);
    }

    budget.note_data_submitted(first, 2);
    bool second_acquired = false;
    std::size_t remaining = 2;
    std::exception_ptr failure;
    const auto stop_when_both_complete = [&](std::exception_ptr error) {
        if (error && !failure) {
            failure = error;
        }
        if (--remaining == 0) {
            asio::post(io, [&io] { io.stop(); });
        }
    };
    asio::co_spawn(io, acquire_data_budget_slot(budget, second, stream_signal, second_acquired),
        stop_when_both_complete);
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!second_acquired);

    // Peer RST discards the still-flow-controlled suffix, but cannot reclaim
    // the credit while the already serialized DATA frame remains in core output.
    char reset[ruvia::http2_frame_header_bytes + 4]{};
    (void)ruvia::encode_http2_frame_header(reset, 4,
        ruvia::http2_frame_type::rst_stream, 0, first);
    RUVIA_CHECK(connection.feed(std::string_view(reset, sizeof(reset))) ==
                ruvia::http2_feed_result::accepted);
    budget.note_data_output(first, 1);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.release(first);
        budget.reconcile(connection, false);
        co_return; }, stop_io_on_completion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!second_acquired);
    RUVIA_CHECK_EQ(connection.pending_data_output_bytes(first), std::size_t{1});

    // Taking a complete batch records DATA as in-flight, but even a partial
    // socket failure must not return its budget. Only whole-batch success does.
    std::pmr::string output;
    const auto taken = connection.take_output_batch(16 * 1024, output, [](void* context_value, std::uint32_t stream_id, std::size_t bytes_value) noexcept { static_cast<http2_data_output_budget*>(context_value)->note_data_output(stream_id, bytes_value); }, &budget);
    RUVIA_CHECK(taken.status_ == ruvia::http2_output_batch_status::taken);
    const auto frame = ruvia::parse_http2_frame_header(
        std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame.has_value());
    RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.reconcile(connection, false);
        co_return; }, stop_io_on_completion(io));
    (void)io.poll();
    io.restart();
    RUVIA_CHECK(!second_acquired);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        budget.reconcile(connection, true);
        co_return; }, stop_when_both_complete);
    io.run();
    io.restart();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(second_acquired);
    RUVIA_CHECK(connection.submit_data(second, "x", ruvia::http2_end_stream::keep_open) ==
                ruvia::http2_data_submit_status::accepted);
    RUVIA_CHECK_EQ(connection.pending_data_output_bytes(second), std::size_t{1});
}

RUVIA_TEST(http2_web_stream_runtime_table_keeps_active_storage_stable) {
    std::pmr::monotonic_buffer_resource resource;
    http2_sans_io_termination termination;
    http2_sans_io_stream_runtime_table table_value(&resource, termination);
    auto& first = ensure_accepted_runtime(table_value, 1, &resource);
    RUVIA_CHECK(first.select_route(route_resolution{}, request_body_mode::buffered));
    auto& first_body_runtime = first.selected_route()->body();
    const auto first_store = first_body_runtime.store("tiny", protocol_byte_limit::limited(16), 0);
    RUVIA_CHECK(first_store.stored() != nullptr);
    const auto first_body = first_body_runtime.buffered()->bytes();
    const auto* first_address = &first;

    // Cross the inline capacity so pointer-vector growth and later compaction are
    // both exercised without moving active runtime objects.
    for (std::uint32_t id = 3; id < 45; id += 2) {
        (void)ensure_accepted_runtime(table_value, id, &resource);
    }
    RUVIA_CHECK(table_value.find(1) == first_address);
    RUVIA_CHECK_EQ(table_value.find(1)->selected_route()->body().buffered()->bytes(), first_body);
    RUVIA_CHECK(table_value.remove(19));
    RUVIA_CHECK(table_value.find(1) == first_address);
    RUVIA_CHECK(!table_value.remove(19));
    RUVIA_CHECK(table_value.size() == 21);
}

RUVIA_TEST(http2_websocket_tunnel_count_follows_stream_runtime_lifetime) {
    std::pmr::monotonic_buffer_resource resource;
    http2_sans_io_termination termination;
    http2_sans_io_stream_runtime_table table_value(&resource, termination);
    auto& runtime = ensure_accepted_runtime(table_value, 1, &resource);
    RUVIA_CHECK(runtime.select_route(route_resolution{}, request_body_mode::stream));
    RUVIA_CHECK_EQ(table_value.tunnel_count(), std::size_t{0});
    RUVIA_CHECK(!table_value.mark_tunnel(3));
    RUVIA_CHECK(table_value.mark_tunnel(1));
    RUVIA_CHECK_EQ(table_value.tunnel_count(), std::size_t{1});
    RUVIA_CHECK(!table_value.mark_tunnel(1));
    RUVIA_CHECK(table_value.remove(1));
    RUVIA_CHECK_EQ(table_value.tunnel_count(), std::size_t{0});
}

RUVIA_TEST(http2_web_stream_runtime_table_owns_dispatch_signal_and_lease) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    std::pmr::monotonic_buffer_resource resource;
    http2_sans_io_termination termination;
    http2_sans_io_stream_runtime_table table_value(&resource, termination);

    RUVIA_CHECK(table_value.begin_dispatch(1, worker_value) == nullptr);
    auto& runtime = ensure_accepted_runtime(table_value, 1, &resource);
    RUVIA_CHECK(!runtime.dispatched());
    RUVIA_CHECK(runtime.signal() == nullptr);
    RUVIA_CHECK_EQ(table_value.dispatched_count(), std::size_t{0});
    RUVIA_CHECK(table_value.begin_dispatch(1, worker_value) == nullptr);
    RUVIA_CHECK(runtime.select_route(route_resolution{}, request_body_mode::buffered));
    auto* selected_route = runtime.selected_route();
    RUVIA_CHECK(selected_route != nullptr);
    RUVIA_CHECK(selected_route->signal() == nullptr);

    auto* signal = table_value.begin_dispatch(1, worker_value);
    RUVIA_CHECK(signal != nullptr);
    RUVIA_CHECK(runtime.selected_route() == selected_route);
    RUVIA_CHECK(selected_route->dispatched());
    RUVIA_CHECK(selected_route->signal() == signal);
    RUVIA_CHECK(runtime.dispatched());
    RUVIA_CHECK(runtime.signal() == signal);
    RUVIA_CHECK_EQ(table_value.dispatched_count(), std::size_t{1});
    RUVIA_CHECK(table_value.begin_dispatch(1, worker_value) == nullptr);
    RUVIA_CHECK_EQ(table_value.dispatched_count(), std::size_t{1});

    std::size_t visited = 0;
    table_value.for_each([&](const auto& entry_value) {
        ++visited;
        RUVIA_CHECK(entry_value.stream_id() == std::uint32_t{1});
        RUVIA_CHECK(entry_value.dispatched());
    });
    RUVIA_CHECK_EQ(visited, std::size_t{1});

    asio::post(io, [&io, signal, &termination] {
        signal->wake();
        (void)termination.terminate(std::make_error_code(std::errc::connection_aborted));
        io.stop();
    });
    io.run();
    io.restart();
    RUVIA_CHECK(signal->terminated());
    RUVIA_CHECK(table_value.remove(1));
    RUVIA_CHECK_EQ(table_value.dispatched_count(), std::size_t{0});
    RUVIA_CHECK_EQ(table_value.size(), std::size_t{0});
}

RUVIA_TEST(http2_web_stream_signal_wakes_concurrent_waiters_without_self_cancel) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    std::pmr::monotonic_buffer_resource resource;
    http2_sans_io_termination termination;
    http2_sans_io_stream_runtime_table table_value(&resource, termination);
    auto& runtime = ensure_accepted_runtime(table_value, 1, &resource);
    RUVIA_CHECK(runtime.select_route(route_resolution{}, request_body_mode::buffered));
    auto* signal = table_value.begin_dispatch(1, worker_value);
    RUVIA_CHECK(signal != nullptr);
    if (signal == nullptr) {
        return;
    }

    std::size_t wake_count = 0;
    const auto wait_once = [&]() -> asio::awaitable<void> {
        co_await ruvia::as_awaitable(signal->wait());
        ++wake_count;
    };
    auto remaining = std::make_shared<std::size_t>(2);
    const auto stop_when_both_complete = [&io, remaining](std::exception_ptr) {
        if (--*remaining == 0) {
            asio::post(io, [&io] { io.stop(); });
        }
    };
    asio::co_spawn(io, wait_once(), stop_when_both_complete);
    asio::co_spawn(io, wait_once(), stop_when_both_complete);
    (void)io.poll();
    RUVIA_CHECK_EQ(wake_count, std::size_t{0});

    io.restart();
    asio::post(io, [signal] { signal->wake(); });
    io.run();
    io.restart();
    RUVIA_CHECK_EQ(wake_count, std::size_t{2});
}

RUVIA_TEST(http2_web_stream_runtime_keeps_overflow_signal_reference_stable) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    std::pmr::monotonic_buffer_resource resource;
    http2_sans_io_termination termination;
    http2_sans_io_stream_runtime_table table_value(&resource, termination);
    for (std::uint32_t id = 1; id <= 33; id += 2) {
        (void)ensure_accepted_runtime(table_value, id, &resource);
    }
    auto* runtime = table_value.find(33);
    RUVIA_CHECK(runtime != nullptr);
    if (runtime == nullptr) {
        return;
    }
    RUVIA_CHECK(runtime->select_route(route_resolution{}, request_body_mode::buffered));
    auto* signal = table_value.begin_dispatch(33, worker_value);
    RUVIA_CHECK(signal != nullptr);
    const auto* runtime_address = runtime;

    for (std::uint32_t id = 35; id <= 99; id += 2) {
        (void)ensure_accepted_runtime(table_value, id, &resource);
    }
    RUVIA_CHECK(table_value.find(33) == runtime_address);
    RUVIA_CHECK(table_value.find(33)->signal() == signal);
    RUVIA_CHECK_EQ(table_value.dispatched_count(), std::size_t{1});
    RUVIA_CHECK(table_value.remove(35));
    RUVIA_CHECK(table_value.find(33) == runtime_address);
    RUVIA_CHECK(table_value.find(33)->signal() == signal);
    RUVIA_CHECK(table_value.remove(33));
    RUVIA_CHECK_EQ(table_value.dispatched_count(), std::size_t{0});
}

RUVIA_TEST(http2_buffered_bodies_share_live_worker_and_connection_allocations) {
    using ruvia::detail::inbound_buffer_resource;
    inbound_buffer_resource worker_value(std::pmr::new_delete_resource(), 800);
    inbound_buffer_resource first_connection(&worker_value, 700);
    inbound_buffer_resource second_connection(&worker_value, 700);
    std::optional<http2_buffered_request_body> first(std::in_place, &first_connection);
    std::optional<http2_buffered_request_body> second(std::in_place, &second_connection);
    const auto second_empty_bytes = second_connection.used();
    const std::string payload_value(512, 'a');
    const auto accepted = first->store(payload_value, protocol_byte_limit::limited(4096));
    RUVIA_CHECK(accepted.stored() != nullptr);
    const auto retained = worker_value.used();
    RUVIA_CHECK(retained >= payload_value.size());
    const auto worker_rejected = second->store(payload_value, protocol_byte_limit::limited(4096));
    RUVIA_CHECK(worker_rejected.backlog_overflow() != nullptr);
    RUVIA_CHECK_EQ(worker_value.used(), retained);
    RUVIA_CHECK_EQ(second->received_bytes(), std::size_t{0});
    RUVIA_CHECK_EQ(first->bytes(), payload_value);
    const auto connection_rejected = first->store(payload_value, protocol_byte_limit::limited(4096));
    RUVIA_CHECK(connection_rejected.backlog_overflow() != nullptr);
    RUVIA_CHECK_EQ(first->bytes(), payload_value);
    first.reset();
    RUVIA_CHECK_EQ(first_connection.used(), std::size_t{0});
    // The second, empty body is still alive and can own a debug iterator proxy.
    RUVIA_CHECK_EQ(worker_value.used(), second_empty_bytes);
    const auto retried = second->store(payload_value, protocol_byte_limit::limited(4096));
    RUVIA_CHECK(retried.stored() != nullptr);
    second.reset();
    RUVIA_CHECK_EQ(worker_value.used(), std::size_t{0});
}

RUVIA_TEST(http2_inbound_reservation_releases_on_runtime_retirement) {
    ruvia::detail::inbound_buffer_resource worker_value(std::pmr::new_delete_resource(), 8192);
    http2_sans_io_termination termination;
    {
        http2_sans_io_stream_runtime_table streams(std::pmr::new_delete_resource(), termination, &worker_value);
        for (unsigned round = 0; round != 8; ++round) {
            auto& stream = streams.ensure_accepted(round * 2 + 1);
            RUVIA_CHECK(stream.select_route(route_resolution{}, request_body_mode::buffered));
            auto& body = stream.selected_route()->body();
            const auto stored = body.store(std::string(1024, 'x'), protocol_byte_limit::limited(4096), 4096);
            RUVIA_CHECK(stored.stored() != nullptr);
            RUVIA_CHECK(worker_value.used() >= 1024);
            RUVIA_CHECK(streams.remove(round * 2 + 1));
            RUVIA_CHECK_EQ(worker_value.used(), std::size_t{0});
        }
    }
    RUVIA_CHECK_EQ(worker_value.used(), std::size_t{0});
}
