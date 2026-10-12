#include "ruvia/web/websocket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace http1_websocket_test {
struct websocket_eof_observation {
    std::atomic<bool> first_message_{};
    std::atomic<bool> eof_{};
    std::atomic<bool> read_overlap_rejected_{};
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> close_overlap_rejected_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> inform_rejected_{};
    std::atomic<bool> send_read_rejected_{};
    std::atomic<bool> send_close_rejected_{};
    std::atomic<bool> send_during_read_rejected_{};
    std::atomic<bool> send_echo_finished_{};
};
const auto websocket_eof_observed = std::make_shared<websocket_eof_observation>();
[[maybe_unused]] const bool websocket_eof_observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<websocket_eof_observation>>(
        [] { return websocket_eof_observed; });
    return true;
}();

// Large enough that loopback socket buffers cannot absorb it, so the write
// stays active while the client does not read.
constexpr std::size_t stalled_payload_size = std::size_t{16} << 20;

class http1_websocket_routes final {
    RUVIA_ROUTES_BEGIN(http1_websocket_routes)
    // The longest heartbeat a route accepts; the Pong timeout defaults to it.
    const auto longest_heartbeat = ruvia::websocket_route_config{
        .lifecycle_ = {.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds::max()}}};
    const auto required_subprotocol =
        ruvia::websocket_route_config{.subprotocols_ = {"chat.v1"}, .require_subprotocol_ = true};
    RUVIA_GET_WS("/http1-websocket-eof", read_until_eof);
    RUVIA_GET_WS_OPTIONS("/http1-websocket-longest-heartbeat", write_stalled, longest_heartbeat);
    RUVIA_GET_WS_OPTIONS("/http1-websocket-required-subprotocol", accept_only, required_subprotocol);
    RUVIA_GET_WS("/http1-websocket-upstream-reset", fail_on_upstream_reset);
    RUVIA_GET_WS("/http1-websocket-echo-send", echo_send);
    RUVIA_ROUTES_END

    ruvia::task<void> accept_only(ruvia::context&) {
        co_return;
    }

    // Fails the way an awaited upstream request whose connection was reset
    // does; the inbound websocket itself is healthy.
    ruvia::task<void> fail_on_upstream_reset(ruvia::context& context_value) {
        const auto message = co_await context_value.get_websocket().read();
        if (message.has_value()) {
            throw std::system_error(asio::error::make_error_code(asio::error::connection_reset), "upstream request");
        }
    }

    ruvia::task<void> write_stalled(ruvia::context& context_value) {
        co_await context_value.get_websocket().binary(std::string(stalled_payload_size, 'w'));
    }

    // send(websocket_message&&) borrows the read buffer, so it claims the read
    // lane at the call and holds it until the frame is written or dropped.
    ruvia::task<void> echo_send(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.get_websocket();
        try {
            auto first = co_await websocket_value.read();
            if (!first.has_value()) {
                throw std::runtime_error("missing first message");
            }
            auto echo = websocket_value.send(std::move(*first));
            try {
                auto overlap = websocket_value.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observed_value.send_read_rejected_ = true;
            }
            try {
                co_await websocket_value.close();
            } catch (const std::logic_error&) {
                observed_value.send_close_rejected_ = true;
            }
            co_await std::move(echo);
            {
                // A dropped, never-started send releases the read lane.
                auto dropped = websocket_value.send(
                    ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "dropped"));
                static_cast<void>(dropped);
            }
            std::optional<ruvia::websocket_message> second;
            ruvia::task_scope reads(context_value.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                second = co_await websocket_value.read();
            };
            reads.spawn(receive());
            try {
                static_cast<void>(websocket_value.send(
                    ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "overlap")));
            } catch (const std::logic_error&) {
                observed_value.send_during_read_rejected_ = true;
            }
            co_await reads.join();
            if (!second.has_value()) {
                throw std::runtime_error("missing second message");
            }
            co_await websocket_value.send(std::move(*second));
            const auto end = co_await websocket_value.read();
            observed_value.send_echo_finished_ = !end.has_value();
        } catch (...) {
            observed_value.failed_ = true;
        }
    }

    ruvia::task<void> read_until_eof(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.get_websocket();
        // The 101 head is committed: a 1xx head written now would land inside
        // the frame stream.
        try {
            co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        } catch (const std::logic_error&) {
            observed_value.inform_rejected_ = true;
        }
        try {
            auto first_read = websocket_value.read();
            auto last_read = websocket_value.read();
            std::string payload = "ready";
            auto first_write = websocket_value.text(payload);
            auto last_write = websocket_value.text("ack");
            auto close = websocket_value.close();
            payload.assign("changed");
            {
                auto discarded = websocket_value.text("discarded");
                static_cast<void>(discarded);
            }
            ruvia::task_scope reads(context_value.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                const auto first = co_await std::move(first_read);
                observed_value.first_message_ = first.has_value() && first->payload() == std::string_view("hi");
            };
            reads.spawn(receive());
            try {
                auto overlap = websocket_value.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observed_value.read_overlap_rejected_ = true;
            }
            try {
                co_await std::move(close);
            } catch (const std::logic_error&) {
                observed_value.close_overlap_rejected_ = true;
            }
            ruvia::task_scope writes(context_value.worker());
            const auto send = [&]() -> ruvia::task<void> {
                co_await std::move(first_write);
            };
            writes.spawn(send());
            try {
                auto overlap = websocket_value.ping("overlap");
                co_await std::move(overlap);
            } catch (const std::logic_error&) {
                observed_value.write_overlap_rejected_ = true;
            }
            co_await writes.join();
            co_await reads.join();
            co_await std::move(last_write);
            const auto end = co_await std::move(last_read);
            observed_value.eof_ = !end.has_value();
        } catch (...) {
            observed_value.failed_ = true;
        }
    }
};
}  // namespace http1_websocket_test

using http1_websocket_test::stalled_payload_size;
using http1_websocket_test::websocket_eof_observed;

// A peer that closes its TCP send direction without a Close frame ends the
// websocket read side (RFC 6455 7.1.5, abnormal closure); read() reports the
// end instead of failing as a transport read error.
RUVIA_TEST(http1_websocket_read_reports_orderly_peer_eof_as_end) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    websocket_eof_observed->first_message_ = false;
    websocket_eof_observed->eof_ = false;
    websocket_eof_observed->read_overlap_rejected_ = false;
    websocket_eof_observed->write_overlap_rejected_ = false;
    websocket_eof_observed->close_overlap_rejected_ = false;
    websocket_eof_observed->failed_ = false;
    websocket_eof_observed->inform_rejected_ = false;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto send = [&](std::string_view bytes_value) -> ruvia::task<void> {
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes_value), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
            };
            co_await send(
                "GET /http1-websocket-eof HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            std::string head;
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            RUVIA_CHECK(head.starts_with("HTTP/1.1 101 "));
            std::array<char, 7> ready{};
            const auto ready_read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read(client, asio::buffer(ready), std::move(handler));
            });
            if (ready_read.error_code()) {
                throw std::system_error(ready_read.error_code());
            }
            RUVIA_CHECK_EQ(std::string_view(ready.data(), ready.size()), std::string_view("\x81\x05ready", 7));
            // One masked text frame (zero masking key), then FIN without Close.
            constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'h', 'i'};
            co_await send(std::string_view(frame.data(), frame.size()));
            client.shutdown(asio::ip::tcp::socket::shutdown_send);
            std::array<char, 256> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client);
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(websocket_eof_observed->first_message_);
    RUVIA_CHECK(websocket_eof_observed->eof_);
    RUVIA_CHECK(websocket_eof_observed->read_overlap_rejected_);
    RUVIA_CHECK(websocket_eof_observed->write_overlap_rejected_);
    RUVIA_CHECK(websocket_eof_observed->close_overlap_rejected_);
    RUVIA_CHECK(!websocket_eof_observed->failed_);
    RUVIA_CHECK(websocket_eof_observed->inform_rejected_);
}

// send(websocket_message&&) echoes the borrowed read payload with its own
// opcode, and no read or close can run until that frame is written.
RUVIA_TEST(http1_websocket_send_echoes_borrowed_message_while_holding_read_lane) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    websocket_eof_observed->send_read_rejected_ = false;
    websocket_eof_observed->send_close_rejected_ = false;
    websocket_eof_observed->send_during_read_rejected_ = false;
    websocket_eof_observed->send_echo_finished_ = false;
    websocket_eof_observed->failed_ = false;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto send = [&](std::string_view bytes_value) -> ruvia::task<void> {
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes_value), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
            };
            const auto expect = [&](std::string_view expected) -> ruvia::task<void> {
                std::string received(expected.size(), '\0');
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_read(client, asio::buffer(received), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
                RUVIA_CHECK_EQ(received, expected);
            };
            co_await send(
                "GET /http1-websocket-echo-send HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            std::string head;
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            RUVIA_CHECK(head.starts_with("HTTP/1.1 101 "));
            // Masked frames with a zero masking key: text "hi", then binary "abc".
            constexpr std::array<char, 8> text_frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'h', 'i'};
            co_await send(std::string_view(text_frame.data(), text_frame.size()));
            co_await expect(std::string_view("\x81\x02hi", 4));
            constexpr std::array<char, 9> binary_frame{'\x82', '\x83', '\0', '\0', '\0', '\0', 'a', 'b', 'c'};
            co_await send(std::string_view(binary_frame.data(), binary_frame.size()));
            co_await expect(std::string_view(
                "\x82\x03"
                "abc",
                5));
            client.shutdown(asio::ip::tcp::socket::shutdown_send);
            std::array<char, 256> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client);
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(websocket_eof_observed->send_read_rejected_);
    RUVIA_CHECK(websocket_eof_observed->send_close_rejected_);
    RUVIA_CHECK(websocket_eof_observed->send_during_read_rejected_);
    RUVIA_CHECK(websocket_eof_observed->send_echo_finished_);
    RUVIA_CHECK(!websocket_eof_observed->failed_);
}

// The stalled-write budget is the ping interval plus the Pong timeout. At the
// longest configurable heartbeat that budget is effectively unbounded, so a
// write the client takes a while to drain is never aborted.
RUVIA_TEST(http1_websocket_longest_heartbeat_never_aborts_a_slow_write) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::size_t received = 0;
    bool payload_intact = true;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(10));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            constexpr std::string_view upgrade =
                "GET /http1-websocket-longest-heartbeat HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(upgrade), std::move(handler)); });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::string head;
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            RUVIA_CHECK(head.starts_with("HTTP/1.1 101 "));
            // Leave the server's write blocked across many connection scans.
            asio::steady_timer stall(io, std::chrono::milliseconds(300));
            (void)co_await ruvia::async_asio([&](auto handler) { stall.async_wait(std::move(handler)); });
            std::array<unsigned char, 10> frame_head{};
            const auto head_read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_read(client, asio::buffer(frame_head), std::move(handler)); });
            if (head_read.error_code()) {
                throw std::system_error(head_read.error_code());
            }
            RUVIA_CHECK_EQ(frame_head[0], static_cast<unsigned char>(0x82));
            RUVIA_CHECK_EQ(frame_head[1], static_cast<unsigned char>(127));
            std::size_t length = 0;
            for (std::size_t index = 2; index != frame_head.size(); ++index) {
                length = (length << 8) | frame_head[index];
            }
            RUVIA_CHECK_EQ(length, stalled_payload_size);
            std::array<char, 65536> input{};
            while (received < stalled_payload_size) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(input.data(), std::min(input.size(), stalled_payload_size - received)), std::move(handler));
                });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                payload_intact = payload_intact && std::string_view(input.data(), read.result()).find_first_not_of('w') == std::string_view::npos;
                received += read.result();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(client);
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(received, stalled_payload_size);
    RUVIA_CHECK(payload_intact);
}

// A route that requires a subprotocol refuses an upgrade offering none of its
// own with 400 instead of switching protocols, and still selects one that is
// offered alongside unknown entries.
RUVIA_TEST(http1_websocket_required_subprotocol_refuses_offers_without_one) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    std::array<std::string, 3> heads;
    auto run = [&]() -> ruvia::task<void> {
        constexpr std::array<std::string_view, 3> offers{
            "", "Sec-WebSocket-Protocol: other\r\n", "Sec-WebSocket-Protocol: other, chat.v1\r\n"};
        for (std::size_t index = 0; index != offers.size(); ++index) {
            asio::ip::tcp::socket client(io);
            asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
            watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
            try {
                client.connect(server.endpoint());
                const std::string upgrade = std::string(
                                                "GET /http1-websocket-required-subprotocol HTTP/1.1\r\n"
                                                "Host: 127.0.0.1\r\n"
                                                "Connection: Upgrade\r\n"
                                                "Upgrade: websocket\r\n"
                                                "Sec-WebSocket-Version: 13\r\n"
                                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") +
                                            std::string(offers[index]) + "\r\n";
                const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(upgrade), std::move(handler)); });
                if (sent.error_code()) {
                    throw std::system_error(sent.error_code());
                }
                char byte{};
                while (!heads[index].ends_with("\r\n\r\n")) {
                    const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                    if (read.error_code()) {
                        throw std::system_error(read.error_code());
                    }
                    heads[index].push_back(byte);
                }
            } catch (...) {
                failure = std::current_exception();
            }
            ruvia::close_socket(client);
            (void)watchdog_value.cancel();
            if (failure) {
                break;
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(heads[0].starts_with("HTTP/1.1 400 "));
    RUVIA_CHECK(heads[1].starts_with("HTTP/1.1 400 "));
    RUVIA_CHECK(heads[0].find("Sec-WebSocket-Accept") == std::string::npos);
    RUVIA_CHECK(heads[2].starts_with("HTTP/1.1 101 "));
    RUVIA_CHECK(heads[2].find("\r\nSec-WebSocket-Protocol: chat.v1\r\n") != std::string::npos);
}

// A websocket handler failure is reported even when it carries a code an
// ending inbound connection also produces, while a peer that resets instead of
// answering the resulting 1011 Close ends the connection normally: exactly one
// failure is counted.
RUVIA_TEST(http1_websocket_handler_failure_is_reported_and_a_close_reset_is_not) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto failures_before = ruvia::app().http_stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view upgrade =
            "GET /http1-websocket-upstream-reset HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        asio::write(client, asio::buffer(upgrade));
        std::string head;
        char byte{};
        while (!head.ends_with("\r\n\r\n")) {
            asio::read(client, asio::buffer(&byte, 1));
            head.push_back(byte);
        }
        RUVIA_CHECK(head.starts_with("HTTP/1.1 101 "));
        // One masked text frame (zero masking key).
        constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'g', 'o'};
        asio::write(client, asio::buffer(frame));
        // The server's Close: code 1011 followed by its reason.
        std::array<char, 4> close_head{};
        asio::read(client, asio::buffer(close_head));
        RUVIA_CHECK_EQ(close_head[0], '\x88');
        RUVIA_CHECK_EQ(close_head[2], '\x03');
        RUVIA_CHECK_EQ(close_head[3], '\xf3');
        // SO_LINGER 0 turns close() into a reset instead of a Close answer.
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    server.finish();
    RUVIA_CHECK_EQ(ruvia::app().http_stats().connection_failures_, failures_before + 1);
}
