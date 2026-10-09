#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "http2/http2_sans_io_ws_transport.h"
#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(http2_websocket_transport_abort_wakes_budget_waiter) {
    std::pmr::monotonic_buffer_resource resource;
    auto connection = ruvia::http2_connection::server({.resource_ = &resource});
    RUVIA_CHECK(connection.feed(ruvia::http2_client_preface) ==
                ruvia::http2_feed_result::accepted);
    std::array<char, ruvia::http2_frame_header_bytes> handshake_settings{};
    RUVIA_CHECK(ruvia::encode_http2_frame_header(handshake_settings, 0,
        ruvia::http2_frame_type::settings, 0, 0));
    RUVIA_CHECK(connection.feed(std::string_view(handshake_settings.data(), handshake_settings.size())) ==
                ruvia::http2_feed_result::accepted);
    (void)connection.consume_output(connection.pending_output().size());
    std::pmr::string request_block(&resource);
    ruvia::hpack_encoder::encode_header(request_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(request_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(request_block, ":path", "/");
    ruvia::hpack_encoder::encode_header(request_block, ":authority", "example.com");
    std::array<char, ruvia::http2_frame_header_bytes> request_header{};
    RUVIA_CHECK(ruvia::encode_http2_frame_header(request_header,
        static_cast<std::uint32_t>(request_block.size()), ruvia::http2_frame_type::headers,
        0x5, 1));  // END_HEADERS | END_STREAM
    std::pmr::string request_frame(&resource);
    request_frame.append(request_header.data(), request_header.size());
    request_frame.append(request_block);
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

    std::array<char, ruvia::http2_frame_header_bytes + 6> peer_settings{};
    RUVIA_CHECK(ruvia::encode_http2_frame_header(peer_settings, 6,
        ruvia::http2_frame_type::settings, 0, 0));
    peer_settings[9] = 0;
    peer_settings[10] = 4;
    RUVIA_CHECK(connection.feed(std::string_view(peer_settings.data(), peer_settings.size())) ==
                ruvia::http2_feed_result::accepted);
    (void)connection.consume_output(connection.pending_output().size());
    ruvia::http_response response({.resource_ = &resource});
    RUVIA_CHECK(connection.submit_streaming_response_head(1, std::move(response)) ==
                ruvia::http2_submit_status::accepted);
    (void)connection.consume_output(connection.pending_output().size());

    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal write_signal(worker_value);
    ruvia::detail::http2_data_output_budget budget(worker_value);
    ruvia::detail::http2_sans_io_termination termination;
    ruvia::detail::http2_sans_io_stream_signal stream_signal(worker_value, termination);
    ruvia::detail::http2_sans_io_body_queue queue(&resource);
    ruvia::detail::http2_sans_io_ws_transport<asio::any_io_executor> transport(
        connection, 1, queue, stream_signal, write_signal, budget,
        asio::any_io_executor(io.get_executor()));

    bool completed = false;
    std::error_code write_error;
    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(1));
    watchdog.async_wait([&](const std::error_code& error) {
        if (!error) {
            io.stop();
        }
    });
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            write_error = co_await ruvia::as_awaitable(transport.write_bytes(
                "blocked", ruvia::websocket_transport_disposition::keep_open));
            completed = true;
            watchdog.cancel();
            attachment.stop(); }, asio::detached);
    const auto post_result = worker_value.post([&] { transport.abort(); });
    RUVIA_CHECK(post_result.accepted());
    io.run();
    RUVIA_CHECK(completed);
    RUVIA_CHECK(write_error == std::make_error_code(std::errc::operation_canceled));
    attachment.stop();
}
