#include "ruvia/web/websocket_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/websocket_connection.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {
void exercise_client(ruvia::testing::test_context& ruvia_ctx, bool reset_peer) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool saw_extended_connect = false;
    bool saw_client_end = false;
    unsigned messages = 0;
    const std::string large(2200000, 'a');
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::pmr::unsynchronized_pool_resource resource;
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        std::optional<ruvia::websocket_connection> websocket;
        std::optional<ruvia::http2_request_head_event> request_lease;
        std::string outbound;
        bool finish_value = false;
        bool ended = false;
        std::array<char, 16384> bytes_value{};
        std::pmr::string wire(&resource);
        for (;;) {
            while (!outbound.empty()) {
                const auto count = std::min<std::size_t>(outbound.size(), 16384);
                const auto result_value = connection.submit_data(1, std::string_view(outbound.data(), count), ruvia::http2_end_stream::keep_open);
                if (result_value == ruvia::http2_data_submit_status::backpressured) {
                    break;
                }
                RUVIA_CHECK(result_value == ruvia::http2_data_submit_status::accepted || result_value == ruvia::http2_data_submit_status::queued);
                outbound.erase(0, count);
                if (result_value == ruvia::http2_data_submit_status::queued) {
                    break;
                }
            }
            if (finish_value && outbound.empty() && !ended) {
                const auto result_value = connection.submit_data(1, {}, ruvia::http2_end_stream::end_stream);
                RUVIA_CHECK(result_value == ruvia::http2_data_submit_status::accepted || result_value == ruvia::http2_data_submit_status::queued);
                ended = true;
            }
            if (connection.wants_write()) {
                wire.clear();
                connection.take_output(wire);
                co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            }
            if (saw_client_end || (reset_peer && websocket)) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            const auto input = std::string_view(bytes_value.data(), count);
            for (;;) {
                const auto fed = connection.feed(input);
                RUVIA_CHECK(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* request = event->request_head()) {
                        const auto request_view = connection.server_request_view(request->stream_id());
                        saw_extended_connect = request_view && request_view->method_ == "CONNECT" &&
                                               request_view->protocol_ == "websocket";
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, request->stream_id(), request->request());
                        const auto submitted = connection.submit_websocket_handshake(request->stream_id(), request->request(), validation);
                        if (!submitted.submitted()) {
                            throw std::runtime_error("HTTP/2 WebSocket handshake failed");
                        }
                        websocket.emplace(ruvia::websocket_connection_options{.resource_ = &resource,
                            .compression_ = submitted.submitted()->compression()});
                        if (reset_peer) {
                            RUVIA_CHECK(connection.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
                        }
                        request_lease.emplace(std::move(*request));
                    } else if (auto* data = event->tunnel_data()) {
                        (void)websocket->feed(data->bytes());
                        (void)connection.acknowledge(data->take_credit());
                        while (auto ws_event = websocket->next_event()) {
                            if (auto* message = ws_event->message()) {
                                ++messages;
                                RUVIA_CHECK_EQ(message->payload(), std::string_view(large));
                                RUVIA_CHECK(websocket->submit_frame(ruvia::websocket_opcode::binary, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
                            } else if (ws_event->close()) {
                                finish_value = true;
                            } else if (ws_event->protocol_error()) {
                                throw std::runtime_error("invalid tunneled WebSocket frame");
                            }
                            const auto output = websocket->output_plan();
                            outbound.append(output.bytes());
                            (void)websocket->consume_output(output.bytes().size());
                            if (finish_value) {
                                break;
                            }
                        }
                    } else if (event->tunnel_end()) {
                        saw_client_end = true;
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool reset_observed = false;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::ws,
                                                              .protocol_ = ruvia::websocket_client_protocol::http2,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port(),
                                                              .deflate_ = {.enabled_ = true},
                                                              .read_timeout_ = std::chrono::milliseconds(2000),
                                                              .write_timeout_ = std::chrono::milliseconds(2000)});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            if (reset_peer) {
                (void)co_await client.read();
            } else {
                for (const bool compress : {false, true}) {
                    co_await client.binary(large, {.compress_ = compress});
                    const auto echo = co_await client.read();
                    RUVIA_CHECK(echo.has_value());
                    if (echo) {
                        RUVIA_CHECK_EQ(echo->payload(), std::string_view(large));
                    }
                }
                co_await client.close({});
            }
        } catch (const ruvia::websocket_client_error& error) {
            reset_observed = error.code() == ruvia::websocket_client_error::code_type::protocol_error;
            if (!reset_peer || !reset_observed) {
                failure = std::current_exception();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK(saw_extended_connect);
    if (reset_peer) {
        RUVIA_CHECK(reset_observed);
    } else {
        RUVIA_CHECK_EQ(messages, 2U);
        RUVIA_CHECK(saw_client_end);
    }
}

enum class closing_peer { clean_release,
    no_close,
    cancel_after_close,
    error_after_close,
    end_without_close };

void exercise_closing_peer(ruvia::testing::test_context& ruvia_ctx, closing_peer behavior) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool received_close = false;
    bool released_request = false;
    std::vector<ruvia::http2_frame_type> terminal_frames;
    bool received_end_stream = false;
    std::optional<ruvia::http2_error_code> reset_error;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        std::optional<ruvia::websocket_connection> websocket;
        std::array<char, 16384> input{};
        std::pmr::string output;
        for (;;) {
            if (connection.wants_write()) {
                output.clear();
                connection.take_output(output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                RUVIA_CHECK(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, head->stream_id(), head->request());
                        const auto submitted = connection.submit_websocket_handshake(head->stream_id(), head->request(), validation);
                        RUVIA_CHECK(submitted.submitted() != nullptr);
                        request.emplace(std::move(*head));
                        websocket.emplace();
                    } else if (auto* data = event->tunnel_data()) {
                        (void)websocket->feed(data->bytes());
                        (void)connection.acknowledge(data->take_credit());
                        while (auto frame = websocket->next_event()) {
                            if (const auto* close = frame->close()) {
                                received_close = true;
                                RUVIA_CHECK_EQ(close->close_code(), std::uint16_t{1000});
                                RUVIA_CHECK_EQ(close->reason(), std::string_view("complete"));
                                break;
                            }
                            RUVIA_CHECK(frame->protocol_error() == nullptr);
                        }
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
            if (!received_close) {
                continue;
            }
            if (behavior != closing_peer::no_close && behavior != closing_peer::end_without_close) {
                const auto close = websocket->output_plan();
                RUVIA_CHECK(!close.bytes().empty());
                RUVIA_CHECK(connection.submit_data(1, close.bytes(), ruvia::http2_end_stream::keep_open) == ruvia::http2_data_submit_status::accepted);
                (void)websocket->consume_output(close.bytes().size());
            }
            if (behavior == closing_peer::clean_release || behavior == closing_peer::end_without_close) {
                RUVIA_CHECK(connection.submit_data(1, {}, ruvia::http2_end_stream::end_stream) == ruvia::http2_data_submit_status::accepted);
            }
            // Publish the DATA/END_STREAM before releasing the real server lease.
            // Its half-open retirement emits NO_ERROR, in the same socket write.
            output.clear();
            connection.take_output(output);
            if (behavior == closing_peer::clean_release || behavior == closing_peer::no_close) {
                if (behavior == closing_peer::no_close) {
                    RUVIA_CHECK(connection.submit_reset(1, ruvia::http2_error_code::no_error) == ruvia::http2_submit_status::accepted);
                }
                RUVIA_CHECK(connection.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                released_request = true;
            } else if (behavior != closing_peer::end_without_close) {
                const auto error = behavior == closing_peer::cancel_after_close ? ruvia::http2_error_code::cancel : ruvia::http2_error_code::protocol_error;
                RUVIA_CHECK(connection.submit_reset(1, error) == ruvia::http2_submit_status::accepted);
            }
            std::pmr::string retirement;
            connection.take_output(retirement);
            output.append(retirement);
            for (std::size_t offset = 0; offset < output.size();) {
                const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(output.data() + offset, ruvia::http2_frame_header_bytes));
                RUVIA_CHECK(header_value.has_value());
                if (header_value->stream_id_ == 1) {
                    terminal_frames.push_back(static_cast<ruvia::http2_frame_type>(header_value->type_));
                    if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && (header_value->flags_ & 0x1U) != 0) {
                        received_end_stream = true;
                    }
                    if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        RUVIA_CHECK_EQ(header_value->length_, std::uint32_t{4});
                        const auto* code = output.data() + offset + ruvia::http2_frame_header_bytes;
                        const auto error = (std::uint32_t(static_cast<unsigned char>(code[0])) << 24) |
                                           (std::uint32_t(static_cast<unsigned char>(code[1])) << 16) |
                                           (std::uint32_t(static_cast<unsigned char>(code[2])) << 8) |
                                           std::uint32_t(static_cast<unsigned char>(code[3]));
                        reset_error = static_cast<ruvia::http2_error_code>(error);
                    }
                }
                offset += ruvia::http2_frame_header_bytes + header_value->length_;
            }
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            co_return;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool protocol_error = false;
    bool close_completed = false;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::ws,
                                                              .protocol_ = ruvia::websocket_client_protocol::http2,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            co_await client.close({.code_ = 1000, .reason_ = "complete"});
            close_completed = true;
        } catch (const ruvia::websocket_client_error& error) {
            protocol_error = error.code() == ruvia::websocket_client_error::code_type::protocol_error;
            if (!protocol_error) {
                failure = std::current_exception();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK(received_close);
    RUVIA_CHECK(close_completed == (behavior == closing_peer::clean_release));
    RUVIA_CHECK(protocol_error == (behavior != closing_peer::clean_release));
    if (behavior == closing_peer::clean_release) {
        RUVIA_CHECK(released_request);
        RUVIA_CHECK_EQ(terminal_frames.size(), std::size_t{3});
        RUVIA_CHECK(terminal_frames[0] == ruvia::http2_frame_type::data);
        RUVIA_CHECK(terminal_frames[1] == ruvia::http2_frame_type::data);
        RUVIA_CHECK(terminal_frames[2] == ruvia::http2_frame_type::rst_stream);
        RUVIA_CHECK(received_end_stream);
        RUVIA_CHECK(reset_error == ruvia::http2_error_code::no_error);
    }
    if (behavior == closing_peer::cancel_after_close) {
        RUVIA_CHECK(reset_error == ruvia::http2_error_code::cancel);
    } else if (behavior == closing_peer::error_after_close) {
        RUVIA_CHECK(reset_error == ruvia::http2_error_code::protocol_error);
    } else if (behavior == closing_peer::no_close) {
        RUVIA_CHECK(reset_error == ruvia::http2_error_code::no_error);
        RUVIA_CHECK(!received_end_stream);
    }
}
}  // namespace

RUVIA_TEST(http2_websocket_client_drives_flow_control_deflate_and_end_stream) {
    exercise_client(ruvia_ctx, false);
}

RUVIA_TEST(http2_websocket_client_observes_peer_reset_and_joins_drivers) {
    exercise_client(ruvia_ctx, true);
}

RUVIA_TEST(http2_websocket_normal_close_consumes_peer_close_before_clean_stream_retirement) {
    exercise_closing_peer(ruvia_ctx, closing_peer::clean_release);
}

RUVIA_TEST(http2_websocket_no_error_reset_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(ruvia_ctx, closing_peer::no_close);
}

RUVIA_TEST(http2_websocket_cancel_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(ruvia_ctx, closing_peer::cancel_after_close);
}

RUVIA_TEST(http2_websocket_protocol_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(ruvia_ctx, closing_peer::error_after_close);
}

RUVIA_TEST(http2_websocket_end_stream_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(ruvia_ctx, closing_peer::end_without_close);
}
