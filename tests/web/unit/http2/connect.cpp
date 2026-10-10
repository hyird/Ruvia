#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/next.h"

#include "http2_server_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct connect_observation {
    std::mutex mutex_;
    std::string received_;
    std::atomic<bool> saw_eof_{};
    std::atomic<bool> stable_{true};
    std::atomic<bool> return_early_{};
};
const auto observation = std::make_shared<connect_observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<connect_observation>>([] { return observation; });
    return true;
}();

// Sleep before consuming so peer input (DATA filling the receive window, or a
// request half-close) is fully delivered before the server continues.
ruvia::task<void> hold_off(ruvia::context& context_value) {
    if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(200)) != ruvia::timer_sleep_result::elapsed) {
        throw std::runtime_error("test hold-off sleep interrupted");
    }
}

class hold_off_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        co_await hold_off(context_value);
        co_await next_value();
    }
};

// A rejected expectation is answered before the request content is read. The
// scoped error handler suspends while the peer keeps sending DATA, then reads
// the request body it was handed.
ruvia::task<ruvia::http_response> answer_rejected_expectation(
    ruvia::context& context_value, ruvia::http_error_info error) {
    co_await hold_off(context_value);
    const auto body = co_await context_value.req().text();
    context_value.status(error.status());
    std::pmr::string text(context_value.arena());
    text.append("body=");
    text.append(std::to_string(body.size()));
    co_return context_value.text(std::move(text));
}
[[maybe_unused]] const bool expectation_handler_registered = [] {
    ruvia::app().on_error({.prefix_ = "/expect-rejected", .handler_ = &answer_rejected_expectation});
    return true;
}();

class connect_routes final : public ruvia::controller<connect_routes> {
    RUVIA_ROUTES_BEGIN
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::milliseconds(40)};
    RUVIA_CONNECT_OPTIONS("target.test:443", echo, options);
    RUVIA_CONNECT_PROTOCOL("test-tunnel", "/udp/:host/:port", echo);
    RUVIA_CONNECT_PROTOCOL("slow-tunnel", "/slow-tunnel", slow_tunnel);
    RUVIA_POST_STREAM("/slow-upload", slow_upload);
    RUVIA_GET("/after", after);
    RUVIA_POST("/expect-rejected", expect_rejected);
    RUVIA_GET_WS("/late-fin-websocket", late_fin_websocket, hold_off_middleware);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& context_value);
    ruvia::task<void> slow_tunnel(ruvia::context& context_value);
    ruvia::task<ruvia::http_response> slow_upload(ruvia::context& context_value);
    ruvia::task<ruvia::http_response> after(ruvia::context& context_value) {
        co_return context_value.text("sibling");
    }
    ruvia::task<ruvia::http_response> expect_rejected(ruvia::context& context_value) {
        co_return context_value.text("unreachable");
    }
    ruvia::task<void> late_fin_websocket(ruvia::context&) {
        co_return;
    }
};
ruvia::task<void> connect_routes::echo(ruvia::context& context_value) {
    auto& observation_value = *context_value.worker_state<std::shared_ptr<connect_observation>>();
    if (observation_value.return_early_) {
        co_return;
    }
    std::optional<std::pmr::string> retained;
    auto& tunnel = context_value.tunnel();
    {
        auto cold = tunnel.read();
    }
    while (auto chunk = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.received_.append(*chunk);
        }
        if (!retained) {
            retained.emplace(*chunk, context_value.pool());
        }
        auto output = tunnel.write(std::string_view(*chunk));
        chunk->assign("changed-input");
        co_await std::move(output);
        observation_value.stable_ = observation_value.stable_ && retained->find_first_not_of('t') == std::string_view::npos;
    }
    observation_value.saw_eof_ = true;
    co_await tunnel.finish();
}
ruvia::task<void> connect_routes::slow_tunnel(ruvia::context& context_value) {
    co_await hold_off(context_value);
    auto& tunnel = context_value.tunnel();
    std::size_t total = 0;
    while (auto chunk = co_await tunnel.read()) {
        total += chunk->size();
    }
    const auto total_text = std::to_string(total);
    co_await tunnel.write(std::string_view(total_text));
    co_await tunnel.finish();
}
ruvia::task<ruvia::http_response> connect_routes::slow_upload(ruvia::context& context_value) {
    co_await hold_off(context_value);
    std::size_t total = 0;
    auto& reader = context_value.req().get_body_reader();
    while (auto chunk = co_await reader.text()) {
        total += chunk->size();
    }
    std::pmr::string body(context_value.pool());
    body = std::to_string(total);
    co_return context_value.text(std::move(body));
}
ruvia::task<void> flush_peer(asio::ip::tcp::socket& socket, ruvia::http2_connection& connection, std::pmr::memory_resource* resource) {
    std::pmr::string bytes(resource);
    while (connection.wants_write()) {
        bytes.clear();
        (void)connection.take_output_batch(16384, bytes);
        const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(bytes), std::move(handler)); });
        if (completion.error_code()) {
            throw std::system_error(completion.error_code());
        }
    }
}
}  // namespace
RUVIA_TEST(http2_connect_routes_echo_large_duplex_streams_and_leave_sibling_requests_available) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource allocations;
    std::exception_ptr failure;
    ruvia::test::http2_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        auto& observation_value = *observation;
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        ruvia::worker_memory memory(allocations);
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client({.resource_ = memory.resource()});
            std::array<char, 16384> input{};
            std::string received_wire;
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received_wire.append(input.data(), read.result());
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("CONNECT peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer, memory.resource());
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            for (unsigned round = 0; round != 3; ++round) {
                observation_value.return_early_ = round == 2;
                const auto request = round != 1 ? peer.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "TARGET.TEST:443"})
                                                : peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "test-tunnel", .scheme_ = "https", .authority_ = "proxy.test", .target_ = "/udp/target.test/443"});
                if (!request.submitted()) {
                    throw std::runtime_error("CONNECT request submission rejected");
                }
                const auto id = request.submitted()->stream_id();
                co_await flush_peer(socket, peer, memory.resource());
                bool established = false;
                while (!established) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (const auto* head = event->response_head()) {
                            RUVIA_CHECK(head->stream_id() == id);
                            RUVIA_CHECK(head->head().status() == ruvia::http_status::ok);
                            RUVIA_CHECK(std::ranges::none_of(head->head().headers(), [](const auto& header_value) { return header_value.name() == "content-length"; }));
                            established = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT handshake reset");
                        }
                    }
                    co_await flush_peer(socket, peer, memory.resource());
                }
                const std::string payload_value(100003, 't');
                if (round == 2) {
                    bool reset = false;
                    while (!reset) {
                        co_await pump();
                        while (auto event = peer.next_event()) {
                            if (event->stream_closed()) {
                                reset = true;
                            }
                            RUVIA_CHECK(!peer.connection_error());
                        }
                        co_await flush_peer(socket, peer, memory.resource());
                    }
                    bool sent_fin = false;
                    for (std::size_t offset = 0; offset + ruvia::http2_frame_header_bytes <= received_wire.size();) {
                        const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(received_wire.data() + offset, received_wire.size() - offset));
                        if (!header_value || header_value->length_ > received_wire.size() - offset - ruvia::http2_frame_header_bytes) {
                            break;
                        }
                        sent_fin = sent_fin || (header_value->stream_id_ == id && header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && (header_value->flags_ & 1U) != 0);
                        offset += ruvia::http2_frame_header_bytes + header_value->length_;
                    }
                    RUVIA_CHECK(sent_fin);
                    continue;
                }
                const auto submitted = peer.submit_data(id, payload_value, ruvia::http2_end_stream::end_stream);
                RUVIA_CHECK(submitted == ruvia::http2_data_submit_status::accepted || submitted == ruvia::http2_data_submit_status::queued);
                co_await flush_peer(socket, peer, memory.resource());
                std::string echoed;
                bool ended = false;
                while (!ended) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (auto* data = event->tunnel_data()) {
                            RUVIA_CHECK(data->stream_id() == id);
                            echoed.append(data->bytes());
                            (void)peer.acknowledge(data->take_credit());
                        }
                        if (const auto* end = event->tunnel_end()) {
                            RUVIA_CHECK(end->stream_id() == id);
                            ended = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT stream reset");
                        }
                    }
                    co_await flush_peer(socket, peer, memory.resource());
                }
                RUVIA_CHECK(echoed == payload_value);
                RUVIA_CHECK(observation_value.saw_eof_ && observation_value.stable_);
                {
                    std::lock_guard lock(observation_value.mutex_);
                    RUVIA_CHECK(observation_value.received_ == payload_value);
                    observation_value.received_.clear();
                }
                observation_value.saw_eof_ = false;
            }
            auto sibling = peer.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "proxy.test", .target_ = "/after"});
            if (!sibling.submitted()) {
                throw std::runtime_error("sibling request failed");
            }
            co_await flush_peer(socket, peer, memory.resource());
            bool ended = false;
            std::string body;
            while (!ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (auto* bytes = event->message_body_chunk()) {
                        body.append(bytes->bytes());
                    }
                    if (event->message_end()) {
                        ended = true;
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            RUVIA_CHECK(body == "sibling");
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        RUVIA_CHECK(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
}
RUVIA_TEST(http2_slow_body_and_tunnel_consumers_return_receive_window_without_peer_input) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource allocations;
    std::exception_ptr failure;
    ruvia::test::http2_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        ruvia::worker_memory memory(allocations);
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client({.resource_ = memory.resource()});
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("slow consumer peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer, memory.resource());
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("slow consumer SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            // Three times the server's 1 MiB stream/connection receive window.
            const std::string payload_value(std::size_t{3} * 1024 * 1024 + 7, 'w');
            const auto expected = std::to_string(payload_value.size());

            const auto tunnel = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "slow-tunnel", .scheme_ = "https", .authority_ = "proxy.test", .target_ = "/slow-tunnel"});
            if (!tunnel.submitted()) {
                throw std::runtime_error("slow tunnel request submission rejected");
            }
            const auto tunnel_id = tunnel.submitted()->stream_id();
            co_await flush_peer(socket, peer, memory.resource());
            bool established = false;
            while (!established) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        RUVIA_CHECK(head->head().status() == ruvia::http_status::ok);
                        established = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow tunnel handshake reset");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            const auto tunnel_submitted = peer.submit_data(tunnel_id, payload_value, ruvia::http2_end_stream::end_stream);
            RUVIA_CHECK(tunnel_submitted == ruvia::http2_data_submit_status::accepted || tunnel_submitted == ruvia::http2_data_submit_status::queued);
            co_await flush_peer(socket, peer, memory.resource());
            std::string tunnel_reply;
            bool tunnel_ended = false;
            while (!tunnel_ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (auto* data = event->tunnel_data()) {
                        tunnel_reply.append(data->bytes());
                        (void)peer.acknowledge(data->take_credit());
                    }
                    if (event->tunnel_end()) {
                        tunnel_ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow tunnel stream reset");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            RUVIA_CHECK(tunnel_reply == expected);

            const auto upload = peer.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "proxy.test", .target_ = "/slow-upload", .content_ = ruvia::http2_request_content::streaming()});
            if (!upload.submitted()) {
                throw std::runtime_error("slow upload request submission rejected");
            }
            const auto upload_id = upload.submitted()->stream_id();
            const auto upload_submitted = peer.submit_data(upload_id, payload_value, ruvia::http2_end_stream::end_stream);
            RUVIA_CHECK(upload_submitted == ruvia::http2_data_submit_status::accepted || upload_submitted == ruvia::http2_data_submit_status::queued);
            co_await flush_peer(socket, peer, memory.resource());
            std::string upload_reply;
            bool upload_ended = false;
            while (!upload_ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        RUVIA_CHECK(head->head().status() == ruvia::http_status::ok);
                    }
                    if (auto* bytes = event->message_body_chunk()) {
                        upload_reply.append(bytes->bytes());
                        (void)peer.acknowledge(bytes->take_credit());
                    }
                    if (event->message_end()) {
                        upload_ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow upload stream reset");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            RUVIA_CHECK(upload_reply == expected);
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        RUVIA_CHECK(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
}
RUVIA_TEST(http2_websocket_request_half_closed_before_handshake_receives_error_response) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource allocations;
    std::exception_ptr failure;
    ruvia::test::http2_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        ruvia::worker_memory memory(allocations);
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client({.resource_ = memory.resource()});
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("websocket peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer, memory.resource());
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("websocket SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            const std::array<ruvia::http_header_view, 1> headers{ruvia::http_header_view("sec-websocket-version", "13")};
            const auto request = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "websocket", .scheme_ = "https", .authority_ = "proxy.test", .target_ = "/late-fin-websocket", .headers_ = headers});
            if (!request.submitted()) {
                throw std::runtime_error("websocket request submission rejected");
            }
            const auto id = request.submitted()->stream_id();
            co_await flush_peer(socket, peer, memory.resource());
            // The client API cannot half-close before the handshake decision, so
            // write the empty DATA(END_STREAM) frame directly. The route
            // middleware delays the handshake until this half-close has arrived.
            const std::array<char, 9> half_close{0, 0, 0, 0, 1, static_cast<char>((id >> 24) & 0x7fU), static_cast<char>((id >> 16) & 0xffU), static_cast<char>((id >> 8) & 0xffU), static_cast<char>(id & 0xffU)};
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(half_close), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            bool answered = false;
            while (!answered) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        RUVIA_CHECK(head->stream_id() == id);
                        RUVIA_CHECK(head->head().status() != ruvia::http_status::ok);
                        answered = true;
                    }
                    if (peer.connection_error()) {
                        throw std::runtime_error("websocket connection failure");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        RUVIA_CHECK(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
}
// RFC 9110 §10.1.1: the server answers an unsupported expectation without
// processing the request content. DATA arriving while the suspended error
// handler runs is discarded, so the request it reads stays stable and empty.
RUVIA_TEST(http2_rejected_expectation_discards_request_content_received_during_error_handler) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http2_server_fixture server(io);
    std::string body;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            // The client API generates Expect only for 100-continue, so the
            // request is framed directly: static-index and literal HPACK fields
            // without indexing, then DATA that keeps arriving after dispatch.
            const auto append_frame = [](std::string& wire, char type, char flags, char stream_id, std::string_view payload) {
                const auto size = payload.size();
                wire.push_back(static_cast<char>((size >> 16) & 0xffU));
                wire.push_back(static_cast<char>((size >> 8) & 0xffU));
                wire.push_back(static_cast<char>(size & 0xffU));
                wire.push_back(type);
                wire.push_back(flags);
                wire.append(3, '\0');
                wire.push_back(stream_id);
                wire.append(payload);
            };
            const auto literal = [](std::string& block, std::string_view value) {
                block.push_back(static_cast<char>(value.size()));
                block.append(value);
            };
            std::string block("\x83\x86", 2);  // :method POST, :scheme http
            block.push_back('\x04');           // :path, literal without indexing
            literal(block, "/expect-rejected");
            block.push_back('\x01');  // :authority, literal without indexing
            literal(block, "localhost");
            block.push_back('\0');  // new-name literal without indexing
            literal(block, "expect");
            literal(block, "x-unknown");
            std::string wire("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
            append_frame(wire, 0x4, 0x0, 0, {});     // SETTINGS
            append_frame(wire, 0x1, 0x4, 1, block);  // HEADERS, END_HEADERS
            const std::string chunk(16384, 'd');
            for (int index = 0; index < 3; ++index) {
                append_frame(wire, 0x0, 0x0, 1, chunk);  // DATA within the initial window
            }
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(wire), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            std::string received;
            std::array<char, 16384> input{};
            bool ended = false;
            while (!ended) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received.append(input.data(), read.result());
                while (!ended && received.size() >= 9) {
                    const auto size = (static_cast<std::size_t>(static_cast<unsigned char>(received[0])) << 16) |
                                      (static_cast<std::size_t>(static_cast<unsigned char>(received[1])) << 8) |
                                      static_cast<std::size_t>(static_cast<unsigned char>(received[2]));
                    if (received.size() < 9 + size) {
                        break;
                    }
                    const auto type = received[3];
                    const auto flags = static_cast<unsigned char>(received[4]);
                    const auto stream_id = (static_cast<std::uint32_t>(static_cast<unsigned char>(received[5]) & 0x7fU) << 24) |
                                           (static_cast<std::uint32_t>(static_cast<unsigned char>(received[6])) << 16) |
                                           (static_cast<std::uint32_t>(static_cast<unsigned char>(received[7])) << 8) |
                                           static_cast<std::uint32_t>(static_cast<unsigned char>(received[8]));
                    if (stream_id == 1 && type == 0x0) {
                        body.append(received, 9, size);
                        ended = (flags & 0x1U) != 0;
                    } else if ((stream_id == 1 && type == 0x3) || type == 0x7) {
                        throw std::runtime_error("rejected expectation stream was reset before its response");
                    }
                    received.erase(0, 9 + size);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        RUVIA_CHECK(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(body, std::string("body=0"));
}
