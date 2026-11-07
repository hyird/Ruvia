#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/http_client.h"

#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace websocket_client_test {

template <typename value_type>
ruvia::task<bool> rejects_running_operation(ruvia::task<value_type>&& operation) {
    try {
        static_cast<void>(co_await std::move(operation));
    } catch (const std::logic_error&) {
        co_return true;
    }
    co_return false;
}

ruvia::task<void> await_output(ruvia::task<void>&& operation) {
    co_await std::move(operation);
}

ruvia::task<void> receive_payload(ruvia::task<std::optional<ruvia::websocket_message>>&& operation,
    std::string& payload) {
    const auto message = co_await std::move(operation);
    if (!message) {
        throw std::runtime_error("WebSocket client message expected");
    }
    payload.assign(message->payload());
}

}  // namespace websocket_client_test

namespace {

[[nodiscard]] std::string make_server_handshake_response(std::string_view request) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http1_server_request_parse_state parsed;
    parsed.parse_head(request, 0, &resource);
    if (!parsed.head_ready()) {
        throw std::runtime_error("invalid WebSocket handshake request");
    }
    auto handshake = ruvia::make_websocket_server_handshake(parsed.request_, {}, &resource);
    std::string response;
    handshake.for_each_response_part([&response](std::string_view part) {
        response.append(part);
    });
    return response;
}

void check_peer_exchange(bool reply_close,
    bool truncated_frame = false) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        // The first frame can arrive in the same transport write as the upgrade.
        std::string response = make_server_handshake_response(request);
        if (truncated_frame) {
            response.append("\x81\x05hi", 4);
        } else {
            response.append("\x82\x05hello", 7);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        if (truncated_frame) {
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
            co_return;
        }
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> payload_value{};
        co_await asio::async_read(socket, asio::buffer(payload_value.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        if (reply_close) {
            const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
            co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool succeeded = false;
    bool protocol_error = false;
    bool returned_eof = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
            const auto greeting = co_await websocket.receive();
            if (truncated_frame) {
                returned_eof = !greeting.has_value();
            } else {
                EXPECT_TRUE(greeting.has_value());
                if (greeting) {
                    EXPECT_EQ(greeting->payload(), std::string_view("hello"));
                }
                if (reply_close) {
                    auto close_operation = websocket.close({});
                    ruvia::task_scope closing(client.worker());
                    closing.spawn(websocket_client_test::await_output(std::move(close_operation)));
                    const bool read_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket.receive());
                    const bool write_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket.send("overlap"));
                    const bool close_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket.close({}));
                    co_await closing.join();
                    EXPECT_TRUE(read_rejected && write_rejected && close_rejected);
                } else {
                    co_await websocket.close({});
                }
                succeeded = true;
            }
        } catch (const ruvia::http_client_error& error) {
            protocol_error = error.code() == ruvia::http_client_error::code_type::protocol_error;
            if (!protocol_error) {
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (truncated_frame) {
        EXPECT_TRUE(!returned_eof);
        EXPECT_TRUE(protocol_error);
    } else {
        EXPECT_TRUE(saw_close);
        EXPECT_EQ(succeeded, reply_close);
        EXPECT_EQ(protocol_error, !reply_close);
    }
}

// RFC 6455 section 7.1.1: after the close handshake the server closes the
// TCP connection first; the client waits for that, bounded by
// close_handshake_timeout_, and only then closes without failing.
void check_server_closes_first(bool client_initiates, bool server_closes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool server_closed = false;
    bool client_eof = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response = make_server_handshake_response(request);
        const std::array<unsigned char, 7> close{0x88, 5, 3, 0xe8, 'b', 'y', 'e'};
        if (!client_initiates) {
            response.append(reinterpret_cast<const char*>(close.data()), close.size());
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        std::array<unsigned char, 129> close_payload{};
        co_await asio::async_read(
            socket, asio::buffer(close_payload.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        std::string after_close;
        if (client_initiates) {
            after_close.append(reinterpret_cast<const char*>(close.data()), close.size());
        }
        // Data after Close is ignored by the client.
        after_close.append("\x81\x01x", 3);
        co_await asio::async_write(socket, asio::buffer(after_close), asio::use_awaitable);
        asio::steady_timer delay(io, std::chrono::milliseconds(50));
        co_await delay.async_wait(asio::use_awaitable);
        if (server_closes) {
            server_closed = true;
            // Fails if the client already closed; the order check below reports that.
            std::error_code ignored;
            socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
        std::array<char, 64> sink{};
        std::error_code error;
        while (!error) {
            (void)co_await socket.async_read_some(asio::buffer(sink), asio::redirect_error(asio::use_awaitable, error));
        }
        client_eof = true;
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool completed = false;
    bool completed_after_server_close = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.close_handshake_timeout_ = server_closes ? std::chrono::milliseconds(10000)
                                                                                                                          : std::chrono::milliseconds(100)})
                                 .connect();
            auto& handle = websocket;
            EXPECT_TRUE(handle.connected());
            EXPECT_TRUE(handle.subprotocol().empty());
            if (client_initiates) {
                co_await handle.close({});
            } else {
                const auto message = co_await handle.receive();
                EXPECT_TRUE(!message.has_value());
            }
            EXPECT_TRUE(!handle.connected());
            EXPECT_EQ(handle.peer_close_code(), std::optional<std::uint16_t>(1000));
            EXPECT_EQ(handle.peer_close_reason(), std::string_view("bye"));
            EXPECT_EQ(websocket.peer_close_code(), std::optional<std::uint16_t>(1000));
            EXPECT_EQ(websocket.peer_close_reason(), std::string_view("bye"));
            completed = true;
            completed_after_server_close = server_closed;
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        // Let the peer observe the client's end of the connection.
        for (int i = 0; i < 500 && failure == nullptr && !client_eof && peer_failure == nullptr; ++i) {
            (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(10));
        }
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(client_eof);
    EXPECT_EQ(completed_after_server_close, server_closes);
}

}  // namespace

TEST(Ruvia, websocket_client_negotiates_deflate_and_can_skip_individual_messages) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool offered = false;
    bool compressed = false;
    bool uncompressed = false;
    const std::string payload_value(4096, 'a');
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        offered = request.find("permessage-deflate") != std::string::npos;
        // The server handshake negotiates and serializes the extension once;
        // appending a second offer would make the response invalid.
        const auto response = make_server_handshake_response(request);
        const auto extension = response.find("permessage-deflate");
        EXPECT_TRUE(extension != std::string::npos);
        if (extension != std::string::npos) {
            EXPECT_TRUE(response.find("permessage-deflate", extension + 1) == std::string::npos);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        ruvia::http_io_buffer protocol_input;
        ruvia::websocket_connection protocol(protocol_input, {.compression_ = {.enabled_ = true}});
        std::array<char, 8192> bytes_value{};
        unsigned messages = 0;
        while (messages != 4) {
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            if (messages == 0) {
                compressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) != 0;
            }
            if (messages == 1) {
                uncompressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) == 0;
            }
            protocol_input.append(std::string_view(bytes_value.data(), count));
            while (auto event = protocol.next_event()) {
                if (const auto* message = event->message()) {
                    EXPECT_EQ(message->payload(), payload_value);
                    ++messages;
                    EXPECT_TRUE(protocol.submit_frame(ruvia::websocket_opcode::text, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
                    const auto output = protocol.output_plan();
                    co_await asio::async_write(socket, asio::buffer(output.bytes()), asio::use_awaitable);
                    (void)protocol.consume_output(output.bytes().size());
                }
            }
        }
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        {
            auto cold = client.client().websocket("/").deflate({.compression_ = {.enabled_ = true}}).connect();
        }
        auto websocket = co_await client.client().websocket("/").deflate({.compression_ = {.enabled_ = true}}).connect();
        auto& handle = websocket;
        {
            auto first_write = handle.send("discarded");
            auto second_write = handle.send(std::as_bytes(std::span(std::string_view("discarded"))));
            auto byte_write = handle.send(std::as_bytes(std::span("discarded", 9)));
            auto pmr_text = handle.send(std::pmr::string("discarded"));
            auto pmr_binary = [&] {
                const std::pmr::string source("discarded");
                return handle.send(std::as_bytes(std::span(source)));
            }();
            auto first_read = handle.receive();
            auto second_read = handle.receive();
            auto first_close = handle.close();
            auto second_close = handle.close({});
            auto ping = handle.ping("discarded");
            auto pmr_ping = handle.ping(std::pmr::string("discarded"));
            auto pong = handle.pong("discarded");
            auto default_pong = handle.pong();
            auto pmr_pong = handle.pong(std::pmr::string("discarded"));
            auto send_msg = handle.send(
                ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "discarded"));
        }

        std::string mutable_payload = payload_value;
        auto first = handle.send(mutable_payload);
        auto second = handle.send(std::string(payload_value), {.compress_ = false});
        mutable_payload.assign("changed before starting the write");
        auto first_read = handle.receive();
        auto second_read = handle.receive();
        std::string first_payload;
        {
            ruvia::task_scope writing(client.worker());
            writing.spawn(websocket_client_test::await_output(std::move(first)));
            const bool write_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.send("overlap"));
            const bool close_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.close({}));
            // A failed close releases its read claim and leaves the running write claimed.
            const bool write_still_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.send("overlap"));
            ruvia::task_scope reading(client.worker());
            reading.spawn(websocket_client_test::receive_payload(std::move(first_read), first_payload));
            const bool read_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.receive());
            const bool close_during_read_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.close({}));
            co_await writing.join();
            co_await reading.join();
            EXPECT_TRUE(write_rejected && close_rejected && write_still_rejected && read_rejected &&
                        close_during_read_rejected);
        }
        EXPECT_EQ(first_payload, payload_value);
        co_await std::move(second);
        const auto second_message = co_await std::move(second_read);
        EXPECT_TRUE(second_message.has_value());
        if (second_message) {
            EXPECT_EQ(second_message->payload(), payload_value);
        }
        for (unsigned i = 2; i < 4; ++i) {
            auto operation = [&] {
                const std::string source = payload_value;
                if (i == 2) {
                    return handle.send(
                        ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, source));
                }
                return handle.send(std::as_bytes(std::span(std::string_view(source))));
            }();
            co_await std::move(operation);
            const auto message = co_await handle.receive();
            EXPECT_TRUE(message.has_value());
            if (message) {
                EXPECT_EQ(message->payload(), payload_value);
            }
            EXPECT_TRUE(websocket.connected());
            EXPECT_TRUE(handle.connected());
        }
        client.close();
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    const auto run_to_completion = [&]() -> ruvia::task<void> {
        try {
            co_await run();
        } catch (...) {
            // A rejected handshake must report its error, not leave the test's
            // event-loop work guard running until the whole suite times out.
            peer.close();
            attachment.stop();
            throw;
        }
    };
    auto root = attachment.loop().start(run_to_completion());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(offered);
    EXPECT_TRUE(compressed);
    EXPECT_TRUE(uncompressed);
}

TEST(Ruvia, websocket_client_event_loop_stop_joins_pending_connect) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::steady_timer gate(io, std::chrono::steady_clock::time_point::max());
    std::promise<void> request_received;
    auto received_value = request_received.get_future();
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        request_received.set_value();
        std::error_code ignored;
        co_await gate.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool connect_failed = false;
    const auto connect_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error&) {
            connect_failed = true;
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(connect_client());
    std::thread driver([&] { attachment.run(); });
    if (received_value.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        asio::post(io, [&gate] { gate.cancel(); });
        driver.join();
        root.get();
        throw std::runtime_error("WebSocket peer did not receive the upgrade request");
    }
    received_value.get();
    attachment.stop();
    asio::post(io, [&gate] { gate.cancel(); });
    driver.join();
    root.get();
    EXPECT_TRUE(connect_failed);
}

TEST(Ruvia, websocket_client_handshake_sends_heads_beyond_the_retained_buffer) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    // Larger than the handshake head buffer first tried, within the limit.
    const std::string large_value(20 * 1024, 'v');
    std::exception_ptr peer_failure;
    bool received_large_header = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        ruvia::http1_server_request_parse_state parsed;
        parsed.parse_head(request);
        received_large_header = parsed.head_ready() && parsed.request_.header("x-large") == large_value;
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) { peer_failure = failure; });

    bool connected = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const ruvia::http_header_view large_headers[]{{"X-Large", large_value}};
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").headers(large_headers).connect();
            connected = websocket.connected();
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received_large_header);
    EXPECT_TRUE(connected);
}

// One User-Agent rule for every client: the configured agent (default "Ruvia")
// unless the application headers carry their own field, which replaces it; an
// empty agent sends none.
TEST(Ruvia, websocket_client_handshake_sends_the_default_user_agent_unless_headers_supply_one) {
    struct case_type final {
        std::span<const ruvia::http_header_view> headers_;
        std::optional<std::string_view> user_agent_;
        std::string_view expected_;
    };
    const ruvia::http_header_view custom[]{{"User-Agent", "custom/1"}};
    const case_type cases[]{
        {.expected_ = "Ruvia"},
        {.headers_ = custom, .expected_ = "custom/1"},
        {.user_agent_ = "configured/2", .expected_ = "configured/2"},
        {.headers_ = custom, .user_agent_ = "configured/2", .expected_ = "custom/1"},
        {.user_agent_ = "", .expected_ = {}},
    };
    for (const auto& test_case : cases) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        std::string request;
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await peer.async_accept(asio::use_awaitable);
            co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            const auto response = make_server_handshake_response(request);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
            }
        };
        asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) { peer_failure = failure; });

        const auto run_client = [&]() -> ruvia::task<void> {
            const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
            ruvia::http_client_config config{.base_url_ = base_url};
            if (test_case.user_agent_) {
                config.user_agent_ = *test_case.user_agent_;
            }
            ruvia::http_client client(attachment.loop(), config);
            std::exception_ptr failure;
            try {
                auto websocket = co_await client.client().websocket("/").headers(test_case.headers_).connect();
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
        auto root = attachment.loop().start(run_client());
        attachment.run();
        root.get();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        ruvia::http1_server_request_parse_state parsed;
        parsed.parse_head(request);
        EXPECT_TRUE(parsed.head_ready());
        std::size_t user_agents = 0;
        for (const auto& field : parsed.request_.headers()) {
            if (ruvia::http_ascii_equals_ignore_case(field.name(), "user-agent")) {
                ++user_agents;
                EXPECT_EQ(field.value(), test_case.expected_);
            }
        }
        EXPECT_EQ(user_agents, test_case.expected_.empty() ? 0U : 1U);
    }
}

TEST(Ruvia, websocket_client_event_loop_stop_joins_connected_heartbeat_and_pending_read) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    bool observed_client_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        const auto payload_bytes = static_cast<std::size_t>(header_value[1] & 0x7f);
        if (header_value[0] != 0x89 || (header_value[1] & 0x80) == 0 || payload_bytes > 125) {
            throw std::runtime_error("expected a masked client heartbeat ping");
        }
        std::array<unsigned char, 4> mask{};
        co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
        std::array<unsigned char, 125> payload_value{};
        co_await asio::async_read(
            socket, asio::buffer(payload_value.data(), payload_bytes), asio::use_awaitable);
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
        observed_client_close = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool connected = false;
    bool read_cancelled = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        auto websocket = co_await client.client().websocket("/").lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20),
                                                                                .pong_timeout_ = std::chrono::seconds(5)}})
                             .connect();
        connected = websocket.connected();
        try {
            (void)co_await websocket.receive();
        } catch (const ruvia::http_client_error&) {
            read_cancelled = true;
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        root.get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not send its heartbeat ping");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    root.get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(connected);
    EXPECT_TRUE(read_cancelled);
    EXPECT_TRUE(observed_client_close);
}

TEST(Ruvia, websocket_client_heartbeat_pings_after_a_blocked_message_write) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    constexpr std::size_t payload_size = std::size_t{16} * 1024 * 1024;
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    std::uint64_t message_bytes = 0;
    bool message_complete = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        // Stop reading for many ping intervals so the large client write stays
        // blocked while its heartbeat becomes due.
        asio::steady_timer pause(socket.get_executor(), std::chrono::milliseconds(200));
        co_await pause.async_wait(asio::use_awaitable);

        std::vector<char> discard(64 * 1024);
        for (;;) {
            std::array<unsigned char, 2> header_value{};
            co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
            if ((header_value[1] & 0x80) == 0) {
                throw std::runtime_error("expected masked client frames");
            }
            std::uint64_t payload_bytes = header_value[1] & 0x7f;
            if (payload_bytes >= 126) {
                std::array<unsigned char, 8> length_value{};
                const std::size_t length_size = payload_bytes == 126 ? 2 : 8;
                co_await asio::async_read(socket, asio::buffer(length_value.data(), length_size), asio::use_awaitable);
                payload_bytes = 0;
                for (std::size_t i = 0; i < length_size; ++i) {
                    payload_bytes = (payload_bytes << 8) | length_value[i];
                }
            }
            std::array<unsigned char, 4> mask{};
            co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
            for (auto remaining = payload_bytes; remaining != 0;) {
                const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, discard.size()));
                co_await asio::async_read(socket, asio::buffer(discard.data(), chunk), asio::use_awaitable);
                remaining -= chunk;
            }
            const auto opcode = header_value[0] & 0x0f;
            if (opcode == 0x9) {
                if (payload_bytes > 125) {
                    throw std::runtime_error("expected a client heartbeat ping");
                }
                break;
            }
            if (opcode != 0x2 && opcode != 0x0) {
                throw std::runtime_error("unexpected client frame before the heartbeat ping");
            }
            message_bytes += payload_bytes;
            message_complete = (header_value[0] & 0x80) != 0;
        }
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool wrote = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_size).lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20), .pong_timeout_ = std::chrono::seconds(5)}}).connect();
        const std::string payload(payload_size, 'x');
        co_await websocket.send(std::as_bytes(std::span(std::string_view(payload))));
        wrote = true;
        try {
            (void)co_await websocket.receive();
        } catch (const ruvia::http_client_error&) {
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        root.get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not ping after its blocked write");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    root.get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(wrote);
    EXPECT_TRUE(message_complete);
    EXPECT_EQ(message_bytes, std::uint64_t{payload_size});
}

// A peer that stops reading blocks the application write and defers the
// heartbeat Ping. The liveness budget (Ping interval plus Pong timeout) still
// bounds that stall, independently of any transport write timeout.
TEST(Ruvia, websocket_client_heartbeat_aborts_a_write_stalled_by_a_non_reading_peer) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    constexpr std::size_t payload_size = std::size_t{16} * 1024 * 1024;
    // Held open without reading, so the client's large write fills the
    // socket buffers and stalls until the client gives up.
    std::optional<asio::ip::tcp::socket> stalled;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        stalled.emplace(std::move(socket));
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    std::optional<ruvia::http_client_error::code_type> write_error;
    std::promise<void> write_finished;
    auto write_done = write_finished.get_future();
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .write_timeout_ = std::nullopt,
                                                         .request_timeout_ = std::nullopt});
        auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_size).lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(50), .pong_timeout_ = std::chrono::milliseconds(100)}}).connect();
        const std::string payload(payload_size, 'x');
        try {
            co_await websocket.send(std::as_bytes(std::span(std::string_view(payload))));
        } catch (const ruvia::http_client_error& error) {
            write_error = error.code();
        }
        co_await client.shutdown();
        write_finished.set_value();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    // Well under the 30 s default write timeout the configuration disabled.
    const bool finished = write_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    attachment.stop();
    driver.join();
    root.get();
    if (!finished) {
        throw std::runtime_error("WebSocket client write stalled by a non-reading peer never timed out");
    }
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(write_error.has_value());
    if (write_error) {
        EXPECT_TRUE(*write_error == ruvia::http_client_error::code_type::timeout);
    }
}

TEST(Ruvia, websocket_client_close_requires_peer_close) {
    check_peer_exchange(false);
    check_peer_exchange(true);
}

TEST(Ruvia, websocket_client_read_rejects_eof_during_incomplete_frame) {
    check_peer_exchange(false, true);
}

TEST(Ruvia, websocket_client_lets_the_server_close_the_connection_first) {
    for (const bool client_initiates : {false, true}) {
        check_server_closes_first(client_initiates, true);
        check_server_closes_first(client_initiates, false);
    }
}

TEST(Ruvia, websocket_client_reads_frame_above_default_message_limit) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    std::string payload_value(ruvia::default_max_websocket_message_bytes + 1, '\0');
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>('a' + i % 26);
    }
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response = make_server_handshake_response(request);
        response.push_back('\x82');
        response.push_back('\x7f');
        const auto length = static_cast<std::uint64_t>(payload_value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            response.push_back(static_cast<char>((length >> shift) & 0xffU));
        }
        response.append(payload_value);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> close_payload{};
        co_await asio::async_read(socket, asio::buffer(close_payload.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
        co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool matched = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        // A single frame larger than the protocol's default input bound must be
        // delivered intact when the configured message limit admits it.
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url, .read_timeout_ = std::chrono::seconds(10)});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_value.size()).connect();
            const auto message = co_await websocket.receive();
            matched = message.has_value() && message->payload() == payload_value;
            co_await websocket.close({});
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(matched);
    EXPECT_TRUE(saw_close);
}

TEST(Ruvia, websocket_client_rejects_untrusted_tls_peer) {
    asio::ssl::context tls(asio::ssl::context::tls_server);
    const auto require = [](bool success) {
        if (!success) {
            throw std::runtime_error("could not generate test certificate");
        }
    };
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
    require(generator != nullptr);
    require(EVP_PKEY_keygen_init(generator.get()) == 1);
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
    EVP_PKEY* raw_key = nullptr;
    require(EVP_PKEY_generate(generator.get(), &raw_key) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
    require(certificate != nullptr);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    require(name != nullptr);
    require(X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_subject_name(certificate.get(), name.get()) == 1);
    require(X509_set_issuer_name(certificate.get(), name.get()) == 1);
    require(ruvia::test::sign_tls_certificate(certificate.get(), key.get()) > 0);
    require(SSL_CTX_use_certificate(tls.native_handle(), certificate.get()) == 1);
    require(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()) == 1);

    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool handshake_rejected = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), tls);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server,
            asio::redirect_error(asio::use_awaitable, error));
        handshake_rejected = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool tls_failure = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "https://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error& error) {
            tls_failure = error.code() == ruvia::http_client_error::code_type::tls_failed;
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(tls_failure);
    EXPECT_TRUE(handshake_rejected);
}

namespace {

struct client_frame final {
    unsigned char opcode_;
    std::string payload_;
};

asio::awaitable<client_frame> read_client_frame(asio::ip::tcp::socket& socket) {
    std::array<unsigned char, 2> header_value{};
    co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
    if ((header_value[1] & 0x80) == 0) {
        throw std::runtime_error("expected masked client frames");
    }
    std::uint64_t payload_bytes = header_value[1] & 0x7f;
    if (payload_bytes >= 126) {
        std::array<unsigned char, 8> length_value{};
        const std::size_t length_size = payload_bytes == 126 ? 2 : 8;
        co_await asio::async_read(socket, asio::buffer(length_value.data(), length_size), asio::use_awaitable);
        payload_bytes = 0;
        for (std::size_t i = 0; i < length_size; ++i) {
            payload_bytes = (payload_bytes << 8) | length_value[i];
        }
    }
    std::array<unsigned char, 4> mask{};
    co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
    std::string payload_value(static_cast<std::size_t>(payload_bytes), '\0');
    co_await asio::async_read(socket, asio::buffer(payload_value), asio::use_awaitable);
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>(payload_value[i] ^ static_cast<char>(mask[i % mask.size()]));
    }
    co_return client_frame{static_cast<unsigned char>(header_value[0] & 0x0f), std::move(payload_value)};
}

// Answers the client's Close and ends the peer.
asio::awaitable<void> answer_client_close(asio::ip::tcp::socket& socket) {
    constexpr std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
    co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
    socket.shutdown(asio::ip::tcp::socket::shutdown_send);
}

void check_heartbeat_policy(bool enabled) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto worker = attachment.loop().handle();
    ruvia::worker_signal peer_done(worker);
    std::exception_ptr peer_failure;
    std::size_t pings = 0;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        try {
            for (;;) {
                const auto frame = co_await read_client_frame(socket);
                if (frame.opcode_ == 0x8) {
                    co_await answer_client_close(socket);
                    co_return;
                }
                if (frame.opcode_ == 0x9) {
                    ++pings;
                    continue;  // Deliberately never acknowledge heartbeat Pings.
                }
                if (frame.opcode_ == 0x1) {
                    constexpr std::string_view ready = "\x81\x05ready";
                    co_await asio::async_write(socket, asio::buffer(ready), asio::use_awaitable);
                }
            }
        } catch (const std::system_error& error) {
            if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset) {
                throw;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .read_timeout_ = std::chrono::seconds(5),
                                                         .request_timeout_ = std::nullopt});
        std::exception_ptr failure;
        try {
            ruvia::websocket_lifecycle_options lifecycle;
            lifecycle.heartbeat_.ping_interval_ =
                enabled ? std::optional(std::chrono::milliseconds(20)) : std::nullopt;
            lifecycle.heartbeat_.pong_timeout_ =
                enabled ? std::optional(std::chrono::milliseconds(50)) : std::nullopt;
            auto websocket = co_await client.client().websocket("/").lifecycle(lifecycle).connect();
            if (!enabled) {
                (void)co_await ruvia::sleep_for(websocket.worker(), std::chrono::milliseconds(200));
                EXPECT_TRUE(websocket.connected());
            }
            co_await websocket.send("check");
            const auto ready = co_await websocket.receive();
            EXPECT_TRUE(ready && ready->payload() == "ready");
            if (enabled) {
                bool timed_out = false;
                const auto started = std::chrono::steady_clock::now();
                try {
                    (void)co_await websocket.receive();
                } catch (const ruvia::http_client_error& error) {
                    timed_out = error.code() == ruvia::http_client_error::code_type::timeout;
                }
                EXPECT_TRUE(timed_out);
                EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
                EXPECT_TRUE(!websocket.connected());
            } else {
                co_await websocket.close();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        // Opening can fail before the peer accepts. Cancel that pending accept
        // before joining its completion so the original exception is reported.
        peer.close();
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
    EXPECT_TRUE(enabled ? pings != 0 : pings == 0);
}

}  // namespace

TEST(Ruvia, websocket_client_active_receive_times_out_an_unanswered_heartbeat) {
    check_heartbeat_policy(true);
}

TEST(Ruvia, websocket_client_absent_ping_interval_disables_heartbeat) {
    check_heartbeat_policy(false);
}

TEST(Ruvia, websocket_client_keeps_a_peer_whose_pong_waits_for_the_next_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        bool answered = false;
        for (;;) {
            const auto frame = co_await read_client_frame(socket);
            if (frame.opcode_ == 0x8) {
                co_await answer_client_close(socket);
                co_return;
            }
            if (frame.opcode_ != 0x9) {
                continue;
            }
            std::string pong{static_cast<char>(0x8a), static_cast<char>(frame.payload_.size())};
            pong.append(frame.payload_);
            if (!answered) {
                pong.append("\x81\x05ready", 7);
                answered = true;
            }
            co_await asio::async_write(socket, asio::buffer(pong), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool connected_before_read = false;
    bool received = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20),
                                                                                    .pong_timeout_ = std::chrono::milliseconds(50)}})
                                 .connect();
            // The answered Ping's Pong stays unread far past its timeout.
            (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(300));
            connected_before_read = websocket.connected();
            const auto message = co_await websocket.receive();
            received = message.has_value() && message->payload() == std::string_view("ready");
            co_await websocket.close({});
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(connected_before_read);
    EXPECT_TRUE(received);
}

TEST(Ruvia, websocket_client_read_progresses_between_back_to_back_writes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        bool answered = false;
        for (;;) {
            const auto frame = co_await read_client_frame(socket);
            if (frame.opcode_ == 0x8) {
                co_await answer_client_close(socket);
                co_return;
            }
            if (!answered) {
                // Arrives while the client keeps writing.
                constexpr std::string_view hello = "\x81\x05hello";
                co_await asio::async_write(socket, asio::buffer(hello), asio::use_awaitable);
                answered = true;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    constexpr std::size_t write_count = 1000;
    std::size_t writes = 0;
    std::size_t writes_before_read = write_count;
    bool received = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
            ruvia::task_scope reader(client.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                const auto message = co_await websocket.receive();
                writes_before_read = writes;
                received = message.has_value() && message->payload() == std::string_view("hello");
            };
            reader.spawn(receive());
            for (; writes != write_count; ++writes) {
                co_await websocket.send(std::as_bytes(std::span(std::string_view("x"))));
            }
            co_await reader.join();
            co_await websocket.close({});
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received);
    EXPECT_TRUE(writes_before_read < write_count);
}

TEST(Ruvia, websocket_client_handshake_rejection_records_upstream_http_status) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view rejected =
            "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(rejected), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    std::optional<ruvia::http_client_error::code_type> error_code;
    std::optional<ruvia::http_status_code> status_code;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error& error) {
            error_code = error.code();
            status_code = error.status();
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
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(error_code == ruvia::http_client_error::code_type::handshake_rejected);
    EXPECT_TRUE(status_code == ruvia::http_status::unauthorized);
}
