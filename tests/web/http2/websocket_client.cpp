#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/http_client.h"

#include "output_drain.h"
#include "test_io_context.h"
#include "test_tls_identity.h"

namespace {
void exercise_client(bool reset_peer, bool secure = false) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::optional<ruvia::test::tls_identity> identity;
    if (secure) {
        identity.emplace("localhost");
        SSL_CTX_set_alpn_select_cb(identity->context_.native_handle(), [](SSL*, const unsigned char** selected, unsigned char* selected_size, const unsigned char* offered, unsigned int offered_size, void*) {
                static constexpr unsigned char protocols[]{2, 'h', '2'};
                unsigned char* protocol = nullptr;
                if (SSL_select_next_proto(&protocol, selected_size, protocols, sizeof(protocols),
                        offered, offered_size) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                *selected = protocol;
                return SSL_TLSEXT_ERR_OK; }, nullptr);
    }
    std::exception_ptr peer_failure;
    bool saw_extended_connect = false;
    bool saw_client_end = false;
    unsigned messages = 0;
    const std::string large(2200000, 'a');
    const auto serve_stream = [&](auto& socket) -> asio::awaitable<void> {
        std::pmr::unsynchronized_pool_resource resource;
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        ruvia::http_io_buffer websocket_input(&resource);
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
                const auto result_value = connection.submit_data(1, std::string_view(outbound.data(), count), ruvia::http2_end_stream::keep_open, count);
                EXPECT_TRUE(result_value.status_ == ruvia::http2_data_submit_status::complete ||
                            result_value.status_ == ruvia::http2_data_submit_status::backpressured);
                outbound.erase(0, result_value.consumed_bytes_);
                if (result_value.status_ != ruvia::http2_data_submit_status::complete) {
                    break;
                }
            }
            if (finish_value && outbound.empty() && !ended) {
                const auto result_value = connection.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 0);
                EXPECT_TRUE(result_value.status_ == ruvia::http2_data_submit_status::complete);
                EXPECT_EQ(result_value.consumed_bytes_, std::size_t{0});
                ended = result_value.status_ == ruvia::http2_data_submit_status::complete;
            }
            if (connection.wants_write()) {
                ruvia::testing::drain_output(connection, wire);
                co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            }
            if (saw_client_end || (reset_peer && websocket)) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            const auto input = std::string_view(bytes_value.data(), count);
            for (;;) {
                const auto fed = connection.feed(input);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* request = event->request_head()) {
                        const auto built_request = request->build_request(&resource);
                        EXPECT_TRUE(built_request.header("x-client") == "unified");
                        const auto request_view = request->request_view();
                        saw_extended_connect = request_view.method_ == "CONNECT" &&
                                               request_view.protocol_ == "websocket";
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, request->stream_id(), built_request);
                        const auto submitted = connection.submit_websocket_handshake(request->stream_id(), built_request, validation, {.deflate_ = {.enabled_ = true}});
                        const auto* negotiated = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted);
                        if (negotiated == nullptr) {
                            throw std::runtime_error("HTTP/2 WebSocket handshake failed");
                        }
                        websocket.emplace(websocket_input, ruvia::websocket_connection_options{.compression_ = negotiated->compression()});
                        if (reset_peer) {
                            EXPECT_TRUE(connection.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
                        }
                        request_lease.emplace(std::move(*request));
                    } else if (auto* data = event->tunnel_data()) {
                        websocket_input.append(data->bytes());
                        (void)connection.acknowledge(data->take_credit());
                        while (auto ws_event = websocket->next_event()) {
                            if (auto* message = ws_event->message()) {
                                ++messages;
                                EXPECT_EQ(message->payload(), std::string_view(large));
                                EXPECT_TRUE(websocket->submit_frame(ruvia::websocket_opcode::binary, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
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
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        if (identity) {
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), identity->context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            const unsigned char* selected = nullptr;
            unsigned int selected_size = 0;
            SSL_get0_alpn_selected(stream.native_handle(), &selected, &selected_size);
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(selected), selected_size), std::string_view("h2"));
            co_await serve_stream(stream);
        } else {
            co_await serve_stream(socket);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool reset_observed = false;
    const auto run = [&]() -> ruvia::task<void> {
        const std::array<ruvia::http_header_view, 1> headers{{{"X-Client", "unified"}}};
        const std::string base_url = std::string(secure ? "https://127.0.0.1:" : "http://127.0.0.1:") +
                                     std::to_string(peer.local_endpoint().port());
        ruvia::http_client_config config{
            .base_url_ = base_url,
            .write_timeout_ = std::chrono::milliseconds(2000),
            .read_timeout_ = std::chrono::milliseconds(2000),
            .protocol_ = ruvia::http_client_protocol::http2_only};
        if (identity) {
            config.tls_.ca_file_ = identity->ca_file_;
            config.tls_.server_name_ = "localhost";
        }
        ruvia::http_client client(attachment.loop(), config);
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").headers(headers).deflate({.compression_ = {.enabled_ = true}}).connect();
            auto& client_handle = websocket;
            if (reset_peer) {
                (void)co_await client_handle.receive();
            } else {
                for (const bool compress : {false, true}) {
                    co_await client_handle.send(std::as_bytes(std::span(std::string_view(large))), {.compress_ = compress});
                    const auto echo = co_await client_handle.receive();
                    EXPECT_TRUE(echo.has_value());
                    if (echo) {
                        EXPECT_EQ(echo->payload(), std::string_view(large));
                    }
                }
                co_await client_handle.close({});
            }
        } catch (const ruvia::http_client_error& error) {
            reset_observed = error.code() == ruvia::http_client_error::code_type::protocol_error;
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
    EXPECT_TRUE(saw_extended_connect);
    if (reset_peer) {
        EXPECT_TRUE(reset_observed);
    } else {
        EXPECT_EQ(messages, 2U);
        EXPECT_TRUE(saw_client_end);
    }
}

enum class closing_peer { clean_release,
    no_close,
    no_close_after_end,
    cancel_after_close,
    error_after_close,
    error_after_end,
    end_without_close };

void exercise_closing_peer(closing_peer behavior) {
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
        ruvia::http_io_buffer websocket_input;
        std::optional<ruvia::websocket_connection> websocket;
        std::array<char, 16384> input{};
        std::pmr::string output;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::testing::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        const auto built_request = head->build_request(std::pmr::get_default_resource());
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, head->stream_id(), built_request);
                        const auto submitted = connection.submit_websocket_handshake(head->stream_id(), built_request, validation);
                        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
                        request.emplace(std::move(*head));
                        websocket.emplace(websocket_input);
                    } else if (auto* data = event->tunnel_data()) {
                        websocket_input.append(data->bytes());
                        (void)connection.acknowledge(data->take_credit());
                        while (auto frame = websocket->next_event()) {
                            if (const auto* close = frame->close()) {
                                received_close = true;
                                EXPECT_EQ(close->close_code(), std::uint16_t{1000});
                                EXPECT_EQ(close->reason(), std::string_view("complete"));
                                break;
                            }
                            EXPECT_TRUE(frame->protocol_error() == nullptr);
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
            if (behavior != closing_peer::no_close && behavior != closing_peer::no_close_after_end &&
                behavior != closing_peer::end_without_close) {
                const auto close = websocket->output_plan();
                EXPECT_TRUE(!close.bytes().empty());
                EXPECT_TRUE(connection.submit_data(1, close.bytes(), ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
                (void)websocket->consume_output(close.bytes().size());
            }
            if (behavior == closing_peer::clean_release || behavior == closing_peer::end_without_close ||
                behavior == closing_peer::no_close_after_end || behavior == closing_peer::error_after_end) {
                EXPECT_TRUE(connection.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
            }
            // Publish the DATA/END_STREAM before releasing the real server lease.
            // Its half-open retirement emits NO_ERROR, in the same socket write.
            ruvia::testing::drain_output(connection, output);
            if (behavior == closing_peer::clean_release || behavior == closing_peer::no_close ||
                behavior == closing_peer::no_close_after_end) {
                if (behavior == closing_peer::no_close) {
                    EXPECT_TRUE(connection.submit_reset(1, ruvia::http2_error_code::no_error) == ruvia::http2_submit_status::accepted);
                }
                EXPECT_TRUE(connection.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                released_request = true;
            } else if (behavior != closing_peer::end_without_close) {
                const auto error = behavior == closing_peer::cancel_after_close ? ruvia::http2_error_code::cancel : ruvia::http2_error_code::protocol_error;
                EXPECT_TRUE(connection.submit_reset(1, error) == ruvia::http2_submit_status::accepted);
            }
            std::pmr::string retirement;
            ruvia::testing::drain_output(connection, retirement);
            output.append(retirement);
            for (std::size_t offset = 0; offset < output.size();) {
                const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(output.data() + offset, ruvia::http2_frame_header_bytes));
                EXPECT_TRUE(header_value.has_value());
                if (header_value->stream_id_ == 1) {
                    terminal_frames.push_back(static_cast<ruvia::http2_frame_type>(header_value->type_));
                    if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && (header_value->flags_ & 0x1U) != 0) {
                        received_end_stream = true;
                    }
                    if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        EXPECT_EQ(header_value->length_, std::uint32_t{4});
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
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .protocol_ = ruvia::http_client_protocol::http2_only});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
            auto& client_handle = websocket;
            co_await client_handle.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "complete"});
            close_completed = true;
        } catch (const ruvia::http_client_error& error) {
            protocol_error = error.code() == ruvia::http_client_error::code_type::protocol_error;
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
    EXPECT_TRUE(received_close);
    EXPECT_TRUE(close_completed == (behavior == closing_peer::clean_release));
    EXPECT_TRUE(protocol_error == (behavior != closing_peer::clean_release));
    if (behavior == closing_peer::clean_release) {
        EXPECT_TRUE(released_request);
        EXPECT_EQ(terminal_frames.size(), std::size_t{3});
        EXPECT_TRUE(terminal_frames[0] == ruvia::http2_frame_type::data);
        EXPECT_TRUE(terminal_frames[1] == ruvia::http2_frame_type::data);
        EXPECT_TRUE(terminal_frames[2] == ruvia::http2_frame_type::rst_stream);
        EXPECT_TRUE(received_end_stream);
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::no_error);
    }
    if (behavior == closing_peer::cancel_after_close) {
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::cancel);
    } else if (behavior == closing_peer::error_after_close || behavior == closing_peer::error_after_end) {
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
    } else if (behavior == closing_peer::no_close) {
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::no_error);
        EXPECT_TRUE(!received_end_stream);
    } else if (behavior == closing_peer::no_close_after_end) {
        EXPECT_TRUE(released_request);
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::no_error);
        EXPECT_TRUE(received_end_stream);
    }
    if (behavior == closing_peer::error_after_end) {
        EXPECT_TRUE(received_end_stream);
    }
}

enum class handshake_end { close_on_data,
    close_without_end_stream,
    end_on_headers };

// The peer answers the Extended CONNECT in one write: either 200 HEADERS
// followed by DATA carrying its Close frame and END_STREAM (RFC 8441 section 5
// orderly close), or 200 HEADERS that themselves carry END_STREAM. It then
// waits for the client connection to retire after shutdown.
void exercise_handshake_end(handshake_end mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto worker = attachment.loop().handle();
    ruvia::worker_signal peer_done(worker);
    bool reached_eof = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::pmr::monotonic_buffer_resource resource;
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::pmr::string output(&resource);
        for (;;) {
            if (connection.wants_write()) {
                ruvia::testing::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                reached_eof = true;
                co_return;
            }
            if (count == 0) {
                reached_eof = true;
                co_return;
            }
            if (request) {
                continue;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    auto* head = event->request_head();
                    if (head == nullptr) {
                        continue;
                    }
                    if (mode == handshake_end::end_on_headers) {
                        ruvia::testing::drain_output(connection, output);
                        std::pmr::string block(&resource);
                        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
                        std::array<char, ruvia::http2_frame_header_bytes> header{};
                        EXPECT_TRUE(ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(block.size()),
                            ruvia::http2_frame_type::headers, 0x5, head->stream_id()));
                        output.append(header.data(), header.size());
                        output.append(block);
                        co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
                    } else {
                        const auto built_request = head->build_request(&resource);
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, head->stream_id(), built_request);
                        const auto submitted = connection.submit_websocket_handshake(head->stream_id(), built_request, validation);
                        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
                        ruvia::http_io_buffer websocket_input;
                        ruvia::websocket_connection websocket(websocket_input);
                        EXPECT_TRUE(websocket.submit_close(ruvia::websocket_close_status::normal_closure, "bye") == ruvia::websocket_close_submit_status::accepted);
                        const auto close = websocket.output_plan();
                        const auto end_flag = mode == handshake_end::close_without_end_stream
                                                  ? ruvia::http2_end_stream::keep_open
                                                  : ruvia::http2_end_stream::end_stream;
                        EXPECT_TRUE(connection.submit_data(head->stream_id(), close.bytes(), end_flag, 65535).status_ ==
                                    ruvia::http2_data_submit_status::complete);
                    }
                    request.emplace(std::move(*head));
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    bool connected = false;
    bool closed_by_peer = false;
    std::optional<ruvia::http_client_error::code_type> connect_error;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .protocol_ = ruvia::http_client_protocol::http2_only});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.close_handshake_timeout_ = std::chrono::milliseconds(50)}).connect();
            connected = true;
            auto& client_handle = websocket;
            closed_by_peer = !(co_await client_handle.receive()) &&
                             client_handle.peer_close_code() == std::uint16_t{1000} &&
                             client_handle.peer_close_reason() == std::string_view("bye") &&
                             websocket.peer_close_code() == std::uint16_t{1000} &&
                             websocket.peer_close_reason() == std::string_view("bye");
        } catch (const ruvia::http_client_error& error) {
            connect_error = error.code();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await peer_done.wait();
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
    if (mode != handshake_end::end_on_headers) {
        EXPECT_TRUE(connected);
        EXPECT_TRUE(closed_by_peer);
        EXPECT_TRUE(!connect_error.has_value());
    } else {
        EXPECT_TRUE(!connected);
        EXPECT_TRUE(connect_error == ruvia::http_client_error::code_type::handshake_rejected);
    }
    EXPECT_TRUE(reached_eof);
}
}  // namespace

TEST(Ruvia, http2_websocket_client_drives_flow_control_deflate_and_end_stream) {
    exercise_client(false);
}

TEST(Ruvia, http2_websocket_wss_authenticates_and_exchanges_deflated_messages) {
    exercise_client(false, true);
}

TEST(Ruvia, http2_websocket_client_observes_peer_reset_and_joins_drivers) {
    exercise_client(true);
}

TEST(Ruvia, http2_websocket_normal_close_consumes_peer_close_before_clean_stream_retirement) {
    exercise_closing_peer(closing_peer::clean_release);
}

TEST(Ruvia, http2_websocket_no_error_reset_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(closing_peer::no_close);
}

TEST(Ruvia, http2_websocket_no_error_reset_after_end_stream_still_requires_peer_close) {
    exercise_closing_peer(closing_peer::no_close_after_end);
}

TEST(Ruvia, http2_websocket_cancel_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(closing_peer::cancel_after_close);
}

TEST(Ruvia, http2_websocket_protocol_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(closing_peer::error_after_close);
}

TEST(Ruvia, http2_websocket_protocol_reset_after_close_and_end_stream_remains_an_error) {
    exercise_closing_peer(closing_peer::error_after_end);
}

TEST(Ruvia, http2_websocket_end_stream_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(closing_peer::end_without_close);
}

TEST(Ruvia, http2_websocket_handshake_accepts_close_and_end_stream_in_the_same_read) {
    exercise_handshake_end(handshake_end::close_on_data);
}

TEST(Ruvia, http2_websocket_handshake_rejects_end_stream_on_response_headers_and_sends_goaway) {
    exercise_handshake_end(handshake_end::end_on_headers);
}

TEST(Ruvia, http2_websocket_close_handshake_timeout_sends_end_stream_before_goaway) {
    exercise_handshake_end(handshake_end::close_without_end_stream);
}
