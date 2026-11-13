#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include <asio.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"

namespace http1_websocket_test {
struct websocket_eof_observation {
    std::atomic<bool> first_message_{};
    std::atomic<bool> eof_{};
    std::atomic<bool> read_overlap_rejected_{};
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> inform_rejected_{};
    std::atomic<bool> send_read_rejected_{};
    std::atomic<bool> send_close_rejected_{};
    std::atomic<bool> send_during_read_rejected_{};
    std::atomic<bool> send_echo_finished_{};
    std::promise<unsigned> close_rejections_;
    std::atomic<bool> close_completed_{};
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
constexpr std::size_t fair_handoff_messages = 33;

class http1_websocket_routes final {
    RUVIA_ROUTES_BEGIN(http1_websocket_routes)
    // Unsigned input beyond the signed millisecond range saturates before
    // normalization; the Pong timeout defaults to the saturated Ping interval.
    const auto longest_heartbeat = ruvia::websocket_route_config{
        .lifecycle_ = {.heartbeat_ = {.ping_interval_ =
                                          std::chrono::duration<std::uint64_t, std::milli>{
                                              (std::numeric_limits<std::uint64_t>::max)()}}}};
    const auto required_subprotocol =
        ruvia::websocket_route_config{.subprotocols_ = {"chat.v1"}, .require_subprotocol_ = true};
    const auto takeover_deflate =
        ruvia::websocket_route_config{.deflate_ = {.context_takeover_ = true, .deflater_ = {.mem_level_ = 4}}};
    // Short enough for a Ping and a lapsed Pong window inside the handler's
    // idle start, long enough that a tick write delayed by a busy CI host is
    // not mistaken for a stalled peer (that budget is interval + timeout).
    const auto short_heartbeat = ruvia::websocket_route_config{
        .lifecycle_ = {.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(100),
                           .pong_timeout_ = std::chrono::milliseconds(150)}}};
    RUVIA_GET_WS("/http1-websocket-eof", read_until_eof);
    RUVIA_GET_WS("/http1-websocket-close-lanes", close_exclusively);
    RUVIA_GET_WS("/http1-websocket-longest-heartbeat", write_stalled, longest_heartbeat);
    RUVIA_GET_WS("/http1-websocket-write-only-heartbeat", write_without_reading, short_heartbeat);
    RUVIA_GET_WS("/http1-websocket-fair-handoff", write_with_pending_read, longest_heartbeat);
    RUVIA_GET_WS("/http1-websocket-required-subprotocol", accept_only, required_subprotocol);
    RUVIA_GET_WS("/http1-websocket-upstream-reset", fail_on_upstream_reset);
    RUVIA_GET_WS("/http1-websocket-echo-send", echo_send);
    RUVIA_GET_WS("/http1-websocket-deflate-echo", echo_all);
    RUVIA_GET_WS("/http1-websocket-deflate-takeover", echo_all, takeover_deflate);
    const auto small_message = ruvia::websocket_route_config{
        .max_message_bytes_ = ruvia::protocol_byte_limit::limited(256)};
    RUVIA_GET_WS("/http1-websocket-small-message", echo_all, small_message);
    RUVIA_ROUTES_END

    ruvia::task<void> echo_all(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        for (;;) {
            auto message = co_await websocket_value.read();
            if (!message.has_value()) {
                co_return;
            }
            co_await websocket_value.send(std::move(*message));
        }
    }

    ruvia::task<void> accept_only(ruvia::context&) {
        co_return;
    }

    // Fails the way an awaited upstream request whose connection was reset
    // does; the inbound websocket itself is healthy.
    ruvia::task<void> fail_on_upstream_reset(ruvia::context& context_value) {
        const auto message = co_await context_value.websocket().read();
        if (message.has_value()) {
            throw std::system_error(asio::error::make_error_code(asio::error::connection_reset), "upstream request");
        }
    }

    ruvia::task<void> write_stalled(ruvia::context& context_value) {
        co_await context_value.websocket().binary(std::string(stalled_payload_size, 'w'));
    }

    ruvia::task<void> write_with_pending_read(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        ruvia::task_scope writes(context_value.worker());
        const auto send = [&]() -> ruvia::task<void> {
            co_await websocket_value.binary(std::string(stalled_payload_size, 'w'));
            for (std::size_t index = 1; index != fair_handoff_messages; ++index) {
                co_await websocket_value.text("tick");
            }
        };
        // spawn() starts the first write up to its transport suspension before
        // read() reserves its turn. The peer has not drained any payload yet.
        writes.spawn(send());
        std::exception_ptr failure;
        try {
            while (co_await websocket_value.read()) {
            }
        } catch (...) {
            failure = std::current_exception();
            websocket_value.abort();
        }
        co_await writes.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // Never reads, so no Pong is ever polled. Idle long enough for a Ping to
    // go out and its Pong window to lapse unobserved, then keep writing; the
    // peer answers every Ping.
    ruvia::task<void> write_without_reading(ruvia::context& context_value) {
        auto& websocket_value = context_value.websocket();
        if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(300)) !=
            ruvia::timer_sleep_result::elapsed) {
            co_return;
        }
        for (int i = 0; i < 20; ++i) {
            co_await websocket_value.text("tick");
            if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)) !=
                ruvia::timer_sleep_result::elapsed) {
                co_return;
            }
        }
        co_await websocket_value.close();
    }

    // send(websocket_message&&) borrows the read buffer, so it claims the read
    // lane at the call and holds it until the frame is written or dropped.
    ruvia::task<void> echo_send(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.websocket();
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

    ruvia::task<void> close_exclusively(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.websocket();
        ruvia::task_scope closing(context_value.worker());
        closing.spawn(websocket_value.close());
        unsigned rejected = 0;
        try {
            static_cast<void>(co_await websocket_value.read());
        } catch (const std::logic_error&) {
            ++rejected;
        }
        try {
            co_await websocket_value.text("must not be sent");
        } catch (const std::logic_error&) {
            ++rejected;
        }
        try {
            co_await websocket_value.close();
        } catch (const std::logic_error&) {
            ++rejected;
        }
        observed_value.close_rejections_.set_value(rejected);
        co_await closing.join();
        observed_value.close_completed_ = true;
    }

    ruvia::task<void> read_until_eof(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.websocket();
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
            std::string payload("ready");
            auto first_write = websocket_value.text(payload);
            auto last_write = websocket_value.text("ack");
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

bool zero_mask(void*, ruvia::websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

// Compressible text that is not a single repeated byte.
std::string sample_message(std::size_t size, unsigned seed) {
    std::string message(size, '\0');
    unsigned state_value = seed * 2654435761U + 7U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        message[i] = static_cast<char>('a' + ((state_value >> 16U) % 5U) + ((i / 32U) % 4U));
    }
    return message;
}

// Bytes DEFLATE cannot shrink.
std::string noise_message(std::size_t size) {
    std::string message(size, '\0');
    std::uint32_t state_value = 1234567;
    for (auto& byte : message) {
        state_value ^= state_value << 13;
        state_value ^= state_value >> 17;
        state_value ^= state_value << 5;
        byte = static_cast<char>(state_value);
    }
    return message;
}

void check_fair_handoff(bool peer_close) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::size_t messages = 0;
    std::optional<std::size_t> pong_after;
    std::optional<std::size_t> close_after;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(10));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            const auto send = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto result = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(client, asio::buffer(bytes), std::move(handler));
                });
                if (result.error_code()) {
                    throw std::system_error(result.error_code());
                }
            };
            const auto receive = [&](char* bytes, std::size_t size) -> ruvia::task<void> {
                const auto result = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_read(client, asio::buffer(bytes, size), std::move(handler));
                });
                if (result.error_code()) {
                    throw std::system_error(result.error_code());
                }
            };
            // Send control frames with the upgrade, before draining the stalled
            // first message. Both are masked with the legal all-zero mask.
            constexpr std::array<char, 10> ping{'\x89', '\x84', '\0', '\0', '\0', '\0', 'f', 'a', 'i', 'r'};
            constexpr std::array<char, 8> close{'\x88', '\x82', '\0', '\0', '\0', '\0', '\x03', '\xe8'};
            std::string upgrade(
                "GET /http1-websocket-fair-handoff HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            upgrade.append(ping.data(), ping.size());
            if (peer_close) {
                upgrade.append(close.data(), close.size());
            }
            co_await send(upgrade);
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                co_await receive(&byte, 1);
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            std::array<char, 65536> input{};
            bool close_sent = peer_close;
            while (!close_after.has_value()) {
                std::array<unsigned char, 2> frame{};
                co_await receive(reinterpret_cast<char*>(frame.data()), frame.size());
                EXPECT_TRUE((frame[0] & 0x80U) != 0);
                EXPECT_TRUE((frame[1] & 0x80U) == 0);
                std::uint64_t length = frame[1] & 0x7fU;
                if (length == 126 || length == 127) {
                    const std::size_t size = length == 126 ? 2 : 8;
                    std::array<unsigned char, 8> extended{};
                    co_await receive(reinterpret_cast<char*>(extended.data()), size);
                    length = 0;
                    for (std::size_t index = 0; index != size; ++index) {
                        length = (length << 8U) | extended[index];
                    }
                }
                const auto opcode = frame[0] & 0x0fU;
                if (opcode == 2) {
                    EXPECT_EQ(messages, std::size_t{0});
                    EXPECT_EQ(length, stalled_payload_size);
                    while (length != 0) {
                        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), length));
                        co_await receive(input.data(), size);
                        EXPECT_TRUE(std::string_view(input.data(), size).find_first_not_of('w') == std::string_view::npos);
                        length -= size;
                    }
                    ++messages;
                } else {
                    if (length > input.size()) {
                        throw std::runtime_error("unexpected WebSocket frame size");
                    }
                    co_await receive(input.data(), static_cast<std::size_t>(length));
                    const std::string_view payload(input.data(), static_cast<std::size_t>(length));
                    if (opcode == 1) {
                        EXPECT_EQ(payload, std::string_view("tick"));
                        ++messages;
                    } else if (opcode == 10) {
                        EXPECT_EQ(payload, std::string_view("fair"));
                        pong_after = messages;
                    } else if (opcode == 8) {
                        EXPECT_EQ(payload, std::string_view("\x03\xe8", 2));
                        close_after = messages;
                    } else {
                        throw std::runtime_error("unexpected WebSocket opcode");
                    }
                }
                if (!close_sent && messages == fair_handoff_messages && pong_after.has_value()) {
                    co_await send(std::string_view(close.data(), close.size()));
                    close_sent = true;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(client);
        (void)watchdog.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(pong_after.has_value());
    if (pong_after.has_value()) {
        EXPECT_TRUE(*pong_after < fair_handoff_messages);
    }
    EXPECT_TRUE(close_after.has_value());
    if (peer_close && close_after.has_value()) {
        EXPECT_TRUE(*close_after < fair_handoff_messages);
    } else if (!peer_close) {
        EXPECT_EQ(messages, fair_handoff_messages);
    }
}
}  // namespace http1_websocket_test

using http1_websocket_test::stalled_payload_size;
using http1_websocket_test::websocket_eof_observed;

// A reader blocked behind one complete application message gets a protocol
// poll and automatic Pong/Close flush before a sequential writer drains its
// entire finite burst. The peer keeps reading; there is no timer-based yield.
TEST(Ruvia, http1_websocket_fair_handoff_polls_ping_between_sequential_writes) {
    http1_websocket_test::check_fair_handoff(false);
}

TEST(Ruvia, http1_websocket_fair_handoff_flushes_peer_close_between_sequential_writes) {
    http1_websocket_test::check_fair_handoff(true);
}

// A peer that closes its TCP send direction without a Close frame ends the
// websocket read side (RFC 6455 7.1.5, abnormal closure); read() reports the
// end instead of failing as a transport read error.
TEST(Ruvia, http1_websocket_read_reports_orderly_peer_eof_as_end) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    websocket_eof_observed->first_message_ = false;
    websocket_eof_observed->eof_ = false;
    websocket_eof_observed->read_overlap_rejected_ = false;
    websocket_eof_observed->write_overlap_rejected_ = false;
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
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            std::array<char, 7> ready{};
            const auto ready_read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read(client, asio::buffer(ready), std::move(handler));
            });
            if (ready_read.error_code()) {
                throw std::system_error(ready_read.error_code());
            }
            EXPECT_EQ(std::string_view(ready.data(), ready.size()), std::string_view("\x81\x05ready", 7));
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
    std::move(root).get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(websocket_eof_observed->first_message_);
    EXPECT_TRUE(websocket_eof_observed->eof_);
    EXPECT_TRUE(websocket_eof_observed->read_overlap_rejected_);
    EXPECT_TRUE(websocket_eof_observed->write_overlap_rejected_);
    EXPECT_TRUE(!websocket_eof_observed->failed_);
    EXPECT_TRUE(websocket_eof_observed->inform_rejected_);
}

TEST(Ruvia, http1_websocket_close_excludes_reads_writes_and_another_close) {
    auto& io = ruvia::test::new_test_io_context();
    websocket_eof_observed->close_rejections_ = std::promise<unsigned>();
    auto rejections = websocket_eof_observed->close_rejections_.get_future();
    websocket_eof_observed->close_completed_ = false;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /http1-websocket-close-lanes HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(client, asio::buffer(request));
    std::string response{};
    asio::read_until(client, asio::dynamic_buffer(response), "\r\n\r\n");
    ASSERT_TRUE(response.starts_with("HTTP/1.1 101 "));
    const auto head_end = response.find("\r\n\r\n") + 4;
    // Keep the handshake pending until all overlap attempts have completed.
    ASSERT_EQ(rejections.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(rejections.get(), 3U);
    constexpr std::array<char, 8> close_frame{'\x88', '\x82', '\0', '\0', '\0', '\0', '\x03', '\xe8'};
    asio::write(client, asio::buffer(close_frame));
    client.shutdown(asio::ip::tcp::socket::shutdown_send);
    std::error_code error;
    asio::read(client, asio::dynamic_buffer(response), error);
    EXPECT_EQ(error, asio::error::eof);
    EXPECT_EQ(std::string_view(response).substr(head_end), std::string_view("\x88\x02\x03\xe8", 4));
    ruvia::close_socket(client);
    server.finish();
    EXPECT_TRUE(websocket_eof_observed->close_completed_);
}

// send(websocket_message&&) echoes the borrowed read payload with its own
// opcode, and no read or close can run until that frame is written.
TEST(Ruvia, http1_websocket_send_echoes_borrowed_message_while_holding_read_lane) {
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
                EXPECT_EQ(received, expected);
            };
            co_await send(
                "GET /http1-websocket-echo-send HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
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
    std::move(root).get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(websocket_eof_observed->send_read_rejected_);
    EXPECT_TRUE(websocket_eof_observed->send_close_rejected_);
    EXPECT_TRUE(websocket_eof_observed->send_during_read_rejected_);
    EXPECT_TRUE(websocket_eof_observed->send_echo_finished_);
    EXPECT_TRUE(!websocket_eof_observed->failed_);
}

// The stalled-write budget is the ping interval plus the Pong timeout. At the
// longest configurable heartbeat that budget is effectively unbounded, so a
// write the client takes a while to drain is never aborted.
TEST(Ruvia, http1_websocket_longest_heartbeat_never_aborts_a_slow_write) {
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
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
            // Leave the server's write blocked across many connection scans.
            asio::steady_timer stall(io, std::chrono::milliseconds(300));
            (void)co_await ruvia::async_asio([&](auto handler) { stall.async_wait(std::move(handler)); });
            std::array<unsigned char, 10> frame_head{};
            const auto head_read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_read(client, asio::buffer(frame_head), std::move(handler)); });
            if (head_read.error_code()) {
                throw std::system_error(head_read.error_code());
            }
            EXPECT_EQ(frame_head[0], static_cast<unsigned char>(0x82));
            EXPECT_EQ(frame_head[1], static_cast<unsigned char>(127));
            std::size_t length = 0;
            for (std::size_t index = 2; index != frame_head.size(); ++index) {
                length = (length << 8) | frame_head[index];
            }
            EXPECT_EQ(length, stalled_payload_size);
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
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(received, stalled_payload_size);
    EXPECT_TRUE(payload_intact);
}

// A handler that only writes never polls the Pong the peer sends. The Pong
// window counts only while a read can observe it, so a responsive peer keeps
// the connection until the handler's own Close instead of being aborted once
// the ping interval plus Pong timeout have elapsed.
TEST(Ruvia, http1_websocket_heartbeat_does_not_abort_an_unread_responsive_peer) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view upgrade =
        "GET /http1-websocket-write-only-heartbeat HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(client, asio::buffer(upgrade));
    std::string head{};
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        asio::read(client, asio::buffer(&byte, 1));
        head.push_back(byte);
    }
    EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
    ruvia::http_io_buffer peer_input;
    ruvia::websocket_connection peer(peer_input, {.role_ = ruvia::websocket_connection_role::client,
                                                     .mask_key_generator_ = &http1_websocket_test::zero_mask});
    const auto flush = [&] {
        const std::string bytes_value(peer.output_plan().bytes());
        if (!bytes_value.empty()) {
            asio::write(client, asio::buffer(bytes_value));
        }
        EXPECT_TRUE(peer.consume_output(bytes_value.size()) == ruvia::websocket_output_consume_status::drained);
    };
    std::size_t messages = 0;
    std::size_t pings = 0;
    std::optional<std::uint16_t> close_code;
    std::array<char, 4096> input{};
    asio::error_code end;
    while (!close_code.has_value() && !end) {
        if (const auto event = peer.next_event()) {
            if (event->message() != nullptr) {
                ++messages;
            } else if (event->ping() != nullptr) {
                // The core queued the Pong; the peer answers promptly.
                ++pings;
                flush();
            } else if (const auto* close = event->close()) {
                close_code = close->close_code();
                flush();
            } else {
                break;
            }
            continue;
        }
        const auto read = client.read_some(asio::buffer(input), end);
        if (!end) {
            peer_input.append(std::string_view(input.data(), read));
        }
    }
    // A Ping went out during the 300 ms idle start and was answered, and
    // the server still ended the session with its own Close, not an abort.
    EXPECT_EQ(messages, std::size_t{20});
    EXPECT_TRUE(pings != 0);
    EXPECT_TRUE(close_code.has_value());
    if (close_code.has_value()) {
        EXPECT_EQ(*close_code, std::uint16_t{1000});
    }
    client.close();
    server.finish();
}

// A route that requires a subprotocol refuses an upgrade offering none of its
// own with 400 instead of switching protocols, and still selects one that is
// offered alongside unknown entries.
TEST(Ruvia, http1_websocket_required_subprotocol_refuses_offers_without_one) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    std::array<std::string, 3> heads{std::string{}, std::string{}, std::string{}};
    auto run = [&]() -> ruvia::task<void> {
        constexpr std::array<std::string_view, 3> offers{
            "", "Sec-WebSocket-Protocol: other\r\n", "Sec-WebSocket-Protocol: other, chat.v1\r\n"};
        for (std::size_t index = 0; index != offers.size(); ++index) {
            asio::ip::tcp::socket client(io);
            asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
            watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
            try {
                client.connect(server.endpoint());
                const std::string upgrade(std::string(
                                              "GET /http1-websocket-required-subprotocol HTTP/1.1\r\n"
                                              "Host: 127.0.0.1\r\n"
                                              "Connection: Upgrade\r\n"
                                              "Upgrade: websocket\r\n"
                                              "Sec-WebSocket-Version: 13\r\n"
                                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") +
                                          std::string(offers[index]) + "\r\n");
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
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(heads[0].starts_with("HTTP/1.1 400 "));
    EXPECT_TRUE(heads[1].starts_with("HTTP/1.1 400 "));
    EXPECT_TRUE(heads[0].find("Sec-WebSocket-Accept") == std::string::npos);
    EXPECT_TRUE(heads[2].starts_with("HTTP/1.1 101 "));
    EXPECT_TRUE(heads[2].find("\r\nSec-WebSocket-Protocol: chat.v1\r\n") != std::string::npos);
}

// RFC 9110 section 7.8: a server that receives Upgrade together with
// Expect: 100-continue MUST send 100 (Continue) before 101 (Switching Protocols).
TEST(Ruvia, http1_websocket_upgrade_with_continue_expectation_sends_100_before_101) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    std::array<std::string, 2> heads{std::string{}, std::string{}};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket client(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            client.connect(server.endpoint());
            constexpr std::string_view upgrade =
                "GET /http1-websocket-required-subprotocol HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Expect: 100-continue\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Protocol: chat.v1\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(upgrade), std::move(handler)); });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            for (auto& head : heads) {
                char byte{};
                while (!head.ends_with("\r\n\r\n")) {
                    const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                    if (read.error_code()) {
                        throw std::system_error(read.error_code());
                    }
                    head.push_back(byte);
                }
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
    std::move(root).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(heads[0], "HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(heads[1].starts_with("HTTP/1.1 101 "));
}

// A websocket handler failure is reported even when it carries a code an
// ending inbound connection also produces, while a peer that resets instead of
// answering the resulting 1011 Close ends the connection normally: exactly one
// failure is counted.
TEST(Ruvia, http1_websocket_handler_failure_is_reported_and_a_close_reset_is_not) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto failures_before = ruvia::app().stats().connection_failures_;
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
        std::string head{};
        char byte{};
        while (!head.ends_with("\r\n\r\n")) {
            asio::read(client, asio::buffer(&byte, 1));
            head.push_back(byte);
        }
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
        // One masked text frame (zero masking key).
        constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'g', 'o'};
        asio::write(client, asio::buffer(frame));
        // The server's Close: code 1011 followed by its reason.
        std::array<char, 4> close_head{};
        asio::read(client, asio::buffer(close_head));
        EXPECT_EQ(close_head[0], '\x88');
        EXPECT_EQ(close_head[2], '\x03');
        EXPECT_EQ(close_head[3], '\xf3');
        // SO_LINGER 0 turns close() into a reset instead of a Close answer.
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before + 1);
}

// Server permessage-deflate echoes messages of every size. Without context
// takeover the connection borrows the worker's shared codecs, so a repeated
// message compresses to the same frame; with opt-in takeover each direction
// keeps its own dictionary, so the repeat shrinks.
TEST(Ruvia, http1_websocket_deflate_echoes_with_shared_and_takeover_codecs) {
    using http1_websocket_test::noise_message;
    using http1_websocket_test::sample_message;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const bool takeover : {false, true}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string upgrade(std::string("GET ") +
                                  (takeover ? "/http1-websocket-deflate-takeover" : "/http1-websocket-deflate-echo") +
                                  " HTTP/1.1\r\n"
                                  "Host: 127.0.0.1\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Sec-WebSocket-Version: 13\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                  "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n");
        asio::write(client, asio::buffer(upgrade));
        std::string head{};
        char byte{};
        while (!head.ends_with("\r\n\r\n")) {
            asio::read(client, asio::buffer(&byte, 1));
            head.push_back(byte);
        }
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(head.find("permessage-deflate") != std::string::npos);
        EXPECT_TRUE((head.find("server_no_context_takeover") == std::string::npos) == takeover);
        ruvia::http_io_buffer peer_input;
        ruvia::websocket_connection peer(peer_input, {.message_limit_ = ruvia::protocol_byte_limit::unlimited(),
                                                         .compression_ = {.enabled_ = true,
                                                             .server_no_context_takeover_ = !takeover,
                                                             .client_no_context_takeover_ = !takeover},
                                                         .role_ = ruvia::websocket_connection_role::client,
                                                         .mask_key_generator_ = &http1_websocket_test::zero_mask});
        const auto flush = [&] {
            const std::string bytes_value(peer.output_plan().bytes());
            asio::write(client, asio::buffer(bytes_value));
            EXPECT_TRUE(peer.consume_output(bytes_value.size()) == ruvia::websocket_output_consume_status::drained);
        };
        // Returns the wire bytes of the echoed frame.
        const auto echo = [&](const std::string& payload) {
            EXPECT_TRUE(peer.submit_frame(ruvia::websocket_opcode::binary, payload) == ruvia::websocket_frame_submit_status::accepted);
            flush();
            std::size_t wire_bytes = 0;
            std::array<char, 16384> input{};
            for (;;) {
                if (const auto event = peer.next_event()) {
                    if (const auto* message = event->message()) {
                        EXPECT_TRUE(message->payload() == payload);
                        return wire_bytes;
                    }
                    // A heartbeat Ping queues its Pong.
                    flush();
                    continue;
                }
                const auto read = client.read_some(asio::buffer(input));
                wire_bytes += read;
                peer_input.append(std::string_view(input.data(), read));
            }
        };
        // Small, mid-size, past 64 KiB, incompressible, then small again.
        for (const auto& payload : {sample_message(32, 1), sample_message(4096, 2), sample_message(70'000, 3),
                 noise_message(70'000), sample_message(32, 4)}) {
            static_cast<void>(echo(payload));
        }
        const auto repeated = sample_message(2000, 5);
        const auto first = echo(repeated);
        const auto second = echo(repeated);
        EXPECT_TRUE(takeover ? second < first : second == first);
        client.shutdown(asio::ip::tcp::socket::shutdown_send);
        std::array<char, 256> rest{};
        asio::error_code end;
        while (!end) {
            static_cast<void>(client.read_some(asio::buffer(rest), end));
        }
    }
    server.finish();
}

TEST(Ruvia, websocket_route_receive_limit_rejects_before_payload) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view upgrade =
        "GET /http1-websocket-small-message HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(client, asio::buffer(upgrade));
    std::string head;
    char byte;
    while (!head.ends_with("\r\n\r\n")) {
        asio::read(client, asio::buffer(&byte, 1));
        head.push_back(byte);
    }
    EXPECT_TRUE(head.starts_with("HTTP/1.1 101 "));
    const std::string declared("\x82\xfe\x01\x01\0\0\0\0", 8);
    asio::write(client, asio::buffer(declared));
    std::array<unsigned char, 4> close{};
    asio::read(client, asio::buffer(close));
    EXPECT_EQ(close[0], 0x88);
    EXPECT_EQ(close[1], 2);
    EXPECT_EQ((static_cast<unsigned>(close[2]) << 8) | close[3], 1009U);
    client.close();
    server.finish();
}

namespace {
struct observation {
    std::mutex mutex_;
    std::string bytes_{};
    std::atomic<bool> finish_first_{};
    std::atomic<bool> ended_{};
    std::atomic<bool> retained_stable_{true};
};
const auto observed = std::make_shared<observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<observation>>([] { return observed; });
    return true;
}();

class http1_tunnel_routes final {
    RUVIA_ROUTES_BEGIN(http1_tunnel_routes)
    RUVIA_CONNECT("http1-target.test:443", tunnel);
    RUVIA_ROUTES_END

    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<observation>>();
        auto& tunnel_value = context_value.tunnel();
        if (observed_value.finish_first_) {
            co_await tunnel_value.finish();
        }
        std::optional<std::string> retained;
        while (auto bytes = co_await tunnel_value.read()) {
            {
                std::lock_guard lock(observed_value.mutex_);
                observed_value.bytes_.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            }
            if (!retained) {
                retained.emplace(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            }
            if (!observed_value.finish_first_) {
                auto output = tunnel_value.write(*bytes);
                co_await std::move(output);
            }
            observed_value.retained_stable_ = observed_value.retained_stable_ && retained->find_first_not_of('t') == std::string_view::npos;
        }
        observed_value.ended_ = true;
        co_await tunnel_value.finish();
    }
};

void exercise_connect(bool finish_first) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    {
        std::lock_guard lock(observed->mutex_);
        observed->bytes_.clear();
    }
    observed->finish_first_ = finish_first;
    observed->ended_ = false;
    observed->retained_stable_ = true;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const std::string initial_value(5007, 't');
    const std::string additional(100003, 't');
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
            const std::string request(std::string("CONNECT HTTP1-TARGET.TEST:0443 HTTP/1.1\r\nHost: HTTP1-TARGET.TEST:0443\r\n\r\n") + initial_value);
            co_await send(request);
            std::string head{};
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
            EXPECT_TRUE(head.find("Content-Length:") == std::string::npos);
            EXPECT_TRUE(head.find("Transfer-Encoding:") == std::string::npos);
            std::array<char, 4096> input{};
            std::string echoed{};
            if (!finish_first) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code() == asio::error::eof) {
                    break;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                echoed.append(input.data(), read.result());
            }
            EXPECT_TRUE(echoed == (finish_first ? std::string{} : std::string(initial_value) + additional));
            if (finish_first) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
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
    std::move(root).get();
    server.finish();
    ruvia::close_socket(client);
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(observed->ended_ && observed->retained_stable_);
    {
        std::lock_guard lock(observed->mutex_);
        EXPECT_TRUE(observed->bytes_ == std::string(initial_value) + additional);
    }
}

class http1_interim_routes final {
    RUVIA_ROUTES_BEGIN(http1_interim_routes)
    RUVIA_GET("/http1-early-hints", hints);
    RUVIA_GET("/http1-cold-hints", cold_hints);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hints(ruvia::context& context_value) {
        std::string link_value("</asset>; rel=preload");
        const std::array<ruvia::http_header_view, 1> links{{{"link", link_value}}};
        auto first = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, links));
        auto second = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
        link_value.assign("changed before starting the operation");
        if (context_value.req().protocol_version() == ruvia::http_protocol_version::http11) {
            ruvia::task_scope output(context_value.worker());
            const auto send_first = [&]() -> ruvia::task<void> { co_await std::move(first); };
            output.spawn(send_first());
            unsigned rejected = 0;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                try {
                    co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
                } catch (const std::logic_error&) {
                    ++rejected;
                }
            }
            co_await output.join();
            if (rejected != 2) {
                throw std::logic_error("interim response output accepted concurrent operations");
            }
        } else {
            co_await std::move(first);
        }
        co_await std::move(second);
        co_return context_value.text("final");
    }

    ruvia::task<void> cold_hints(ruvia::context& context_value) {
        auto cold = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        auto another = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("cold-final");
        bool rejected_after_final = false;
        try {
            co_await std::move(cold);
        } catch (const std::logic_error&) {
            rejected_after_final = true;
        }
        if (!rejected_after_final) {
            throw std::logic_error("interim response started after final response commit");
        }
    }
};

// Sends one closing request and returns every byte the server wrote until EOF.
std::string exchange_closing_request(std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::string received{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(request), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            std::array<char, 4096> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code() == asio::error::eof) {
                    break;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received.append(input.data(), read.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return received;
}
}  // namespace

TEST(Ruvia, http1_connect_routes_transfer_buffered_bytes_and_keep_receive_direction_after_send_fin) {
    exercise_connect(false);
    exercise_connect(true);
}

TEST(Ruvia, http1_interim_responses_reach_http11_clients_but_never_http10_clients) {
    const auto http11 = exchange_closing_request("GET /http1-early-hints HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(http11.starts_with("HTTP/1.1 103 "));
    EXPECT_TRUE(http11.find("link: </asset>; rel=preload\r\n") != std::string::npos);
    EXPECT_TRUE(http11.find("\r\n\r\nHTTP/1.1 102 ") != std::string::npos);
    EXPECT_TRUE(http11.find("\r\n\r\nHTTP/1.1 200 ") != std::string::npos);
    EXPECT_TRUE(http11.ends_with("final"));

    // RFC 9110 15.2: an HTTP/1.0 client would take the 1xx head as the final
    // response, so the server omits it and sends only the final response.
    const auto http10 = exchange_closing_request("GET /http1-early-hints HTTP/1.0\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(http10.find(" 103 ") == std::string::npos);
    EXPECT_TRUE(http10.find(" 102 ") == std::string::npos);
    EXPECT_TRUE(std::string_view(http10).substr(0, http10.find("\r\n")).find(" 200 ") != std::string::npos);
    EXPECT_TRUE(http10.ends_with("final"));
}

TEST(Ruvia, http1_unstarted_interim_responses_do_not_block_the_final_response) {
    const auto response = exchange_closing_request("GET /http1-cold-hints HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response.find(" 103 ") == std::string::npos);
    EXPECT_TRUE(response.find(" 102 ") == std::string::npos);
    EXPECT_TRUE(response.find("cold-final") != std::string::npos);
}

namespace udp_tunnel_test {
struct udp_tunnel_observation {
    std::atomic<bool> eof_{};
    std::atomic<bool> read_overlap_rejected_{};
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> capsule_bound_rejected_{};
    std::atomic<bool> failed_{};
    std::atomic<bool> dynamic_invoked_{};
    std::atomic<bool> ws_subprotocol_matched_{};
    std::atomic<bool> ws_peer_close_matched_{};
};

struct retained_tunnel_results {
    std::optional<ruvia::http_capsule> capsule_;
    std::optional<ruvia::http_datagram> datagram_;
    bool cold_operations_expired_{};
};

const auto udp_observation = std::make_shared<udp_tunnel_observation>();
[[maybe_unused]] const bool udp_state_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<udp_tunnel_observation>>(
        [] { return udp_observation; });
    ruvia::app().use_worker_state<retained_tunnel_results>(
        [] { return retained_tunnel_results{}; });
    return true;
}();

class udp_tunnel_routes final {
    RUVIA_ROUTES_BEGIN(udp_tunnel_routes)
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/target.test/53/", echo);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/cancel.test/53/", delayed);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/retained-capsule.test/53/", retain_capsule);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/retained-datagram.test/53/", retain_datagram);
    RUVIA_GET("/udp-tunnel-retained", take_retained);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/udp-tunnel/:host/:port", dynamic);
    RUVIA_GET("/udp-tunnel-sibling", sibling);
    const ruvia::websocket_route_config ws_config{
        .subprotocols_ = {"chat.v2", "chat.v1"}, .require_subprotocol_ = true};
    RUVIA_GET_WS("/udp-tunnel-ws", ws_inspect, ws_config);
    RUVIA_ROUTES_END

    ruvia::task<void> retain_capsule(ruvia::context& c) {
        auto& retained = c.worker_state<retained_tunnel_results>();
        auto& tunnel = c.tunnel();
        std::optional stream(tunnel.capsules());
        unsigned adapted_rejected{};
        try {
            (void)tunnel.capsules();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.datagrams();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.udp();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.read();
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        try {
            (void)tunnel.write("raw");
        } catch (const std::logic_error&) {
            ++adapted_rejected;
        }
        retained.capsule_ = co_await stream->read();
        auto read = stream->read();
        auto write = stream->write(42, "unstarted");
        auto finish = stream->finish();
        co_await stream->finish();
        stream.reset();
        unsigned expired{};
        try {
            (void)co_await std::move(read);
        } catch (const std::logic_error&) {
            ++expired;
        }
        try {
            co_await std::move(write);
        } catch (const std::logic_error&) {
            ++expired;
        }
        try {
            co_await std::move(finish);
        } catch (const std::logic_error&) {
            ++expired;
        }
        retained.cold_operations_expired_ = expired == 3 && adapted_rejected == 5;
    }

    ruvia::task<void> retain_datagram(ruvia::context& c) {
        auto& retained = c.worker_state<retained_tunnel_results>();
        auto tunnel = c.tunnel().udp();
        retained.datagram_ = co_await tunnel.read();
        co_await tunnel.finish();
    }

    ruvia::task<ruvia::http_response> take_retained(ruvia::context& c) {
        auto& retained = c.worker_state<retained_tunnel_results>();
        if (retained.capsule_) {
            auto response = c.text(retained.cold_operations_expired_ && retained.capsule_->type() == 42 &&
                                           retained.capsule_->bytes().size() == retained.capsule_->text().size()
                                       ? retained.capsule_->text()
                                       : std::string_view("invalid-capsule-state"));
            retained.capsule_.reset();
            co_return response;
        }
        if (retained.datagram_) {
            auto response = c.text(retained.datagram_->bytes().size() == retained.datagram_->text().size()
                                       ? retained.datagram_->text()
                                       : std::string_view("invalid-datagram-state"));
            retained.datagram_.reset();
            co_return response;
        }
        co_return c.text("empty");
    }

    ruvia::task<void> dynamic(ruvia::context& c) {
        c.worker_state<std::shared_ptr<udp_tunnel_observation>>()->dynamic_invoked_ = true;
        auto tunnel = c.tunnel().udp();
        co_await tunnel.finish();
    }

    ruvia::task<void> echo(ruvia::context& c) {
        auto& observation = *c.worker_state<std::shared_ptr<udp_tunnel_observation>>();
        ruvia::http_capsule_config config{.max_capsule_length_ = 13};
        auto original = c.tunnel().udp(config);
        try {
            try {
                co_await original.send(std::string(13, 'x'));
            } catch (const std::length_error&) {
                observation.capsule_bound_rejected_ = true;
            }
            auto first_read = original.read();
            auto last_read = original.read();
            auto first_write = original.send(std::string("ready"));
            auto last_write = original.send("second-ready");
            auto finish = original.finish();
            auto tunnel = std::move(original);
            {
                auto discarded = tunnel.send("discarded");
                static_cast<void>(discarded);
            }
            std::optional<ruvia::http_datagram> datagram;
            ruvia::task_scope reads(c.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                datagram = co_await std::move(first_read);
            };
            reads.spawn(receive());
            try {
                auto overlap = tunnel.read();
                static_cast<void>(co_await std::move(overlap));
            } catch (const std::logic_error&) {
                observation.read_overlap_rejected_ = true;
            }
            ruvia::task_scope writes(c.worker());
            const auto send = [&]() -> ruvia::task<void> {
                co_await std::move(first_write);
            };
            writes.spawn(send());
            try {
                auto overlap = tunnel.send("overlap");
                co_await std::move(overlap);
            } catch (const std::logic_error&) {
                observation.write_overlap_rejected_ = true;
            }
            co_await writes.join();
            co_await std::move(last_write);
            co_await reads.join();
            observation.eof_ = !(co_await std::move(last_read));
            if (datagram) {
                co_await tunnel.send(datagram->payload());
            }
            co_await std::move(finish);
        } catch (...) {
            observation.failed_ = true;
            throw;
        }
    }

    ruvia::task<void> delayed(ruvia::context& c) {
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(30));
        auto tunnel = c.tunnel().udp();
        co_await tunnel.finish();
    }

    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
    ruvia::task<void> ws_inspect(ruvia::context& c) {
        auto& observation = *c.worker_state<std::shared_ptr<udp_tunnel_observation>>();
        auto& ws = c.websocket();
        observation.ws_subprotocol_matched_ = ws.subprotocol() == "chat.v2";
        co_await ws.pong();
        while (auto message = co_await ws.read()) {
            co_await ws.send(std::move(*message));
        }
        observation.ws_peer_close_matched_ =
            ws.peer_close_code() == std::uint16_t{4001} && ws.peer_close_reason() == "client-done";
    }
};
// A raw HTTP/1.1 peer: the response head and any input already read after it.
struct upgraded_connection {
    asio::ip::tcp::socket socket_;
    std::string head_{};
    std::string input_{};
};

[[nodiscard]] upgraded_connection open_upgrade(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target, std::string_view fields) {
    upgraded_connection connection{asio::ip::tcp::socket(io)};
    connection.socket_.connect(endpoint);
    std::string request{"GET "};
    request.append(target).append(" HTTP/1.1\r\nHost: localhost\r\n").append(fields).append("\r\n");
    asio::write(connection.socket_, asio::buffer(request));
    const auto head_size = asio::read_until(connection.socket_, asio::dynamic_buffer(connection.input_), "\r\n\r\n");
    connection.head_.assign(connection.input_.data(), head_size);
    connection.input_.erase(0, head_size);
    return connection;
}

[[nodiscard]] upgraded_connection open_udp_tunnel(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    return open_upgrade(io, endpoint, target, "Connection: Upgrade\r\nUpgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n");
}

// Appends a capsule; a DATAGRAM capsule (type 0) carries context ID 0 first.
void append_capsule(std::string& output, std::uint64_t type, std::string_view payload) {
    const bool datagram = type == 0;
    std::array<char, 16> header{};
    const auto encoded = ruvia::encode_http_capsule_header(header, type, payload.size() + (datagram ? 1 : 0));
    output.append(header.data(), std::get<std::size_t>(encoded));
    if (datagram) {
        output.push_back('\0');
    }
    output.append(payload);
}

[[nodiscard]] std::uint64_t read_varint(std::string_view& input) {
    const auto length = std::size_t{1} << (static_cast<unsigned char>(input.front()) >> 6);
    std::uint64_t value = static_cast<unsigned char>(input.front()) & 0x3f;
    for (std::size_t index = 1; index != length; ++index) {
        value = (value << 8) | static_cast<unsigned char>(input[index]);
    }
    input.remove_prefix(length);
    return value;
}

[[nodiscard]] bool has_varint(std::string_view input) {
    return !input.empty() && input.size() >= (std::size_t{1} << (static_cast<unsigned char>(input.front()) >> 6));
}

// The payload of the next UDP datagram the server tunnels, or nullopt at EOF.
[[nodiscard]] std::optional<std::string> read_datagram(upgraded_connection& connection) {
    for (;;) {
        std::string_view view = connection.input_;
        if (has_varint(view)) {
            const auto type = read_varint(view);
            if (has_varint(view)) {
                const auto length = read_varint(view);
                if (view.size() >= length) {
                    if (type != 0 || length == 0 || view.front() != '\0') {
                        throw std::runtime_error("unexpected capsule");
                    }
                    std::string payload(view.substr(1, length - 1));
                    connection.input_.erase(0, connection.input_.size() - view.size() + length);
                    return payload;
                }
            }
        }
        std::array<char, 4096> input{};
        std::error_code ec;
        const auto read = connection.socket_.read_some(asio::buffer(input), ec);
        if (ec == asio::error::eof) {
            if (!connection.input_.empty()) {
                throw std::runtime_error("truncated capsule");
            }
            return std::nullopt;
        }
        if (ec) {
            throw std::system_error(ec);
        }
        connection.input_.append(input.data(), read);
    }
}

// One GET on its own connection; returns the whole response.
[[nodiscard]] std::string get_response(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    auto connection = open_upgrade(io, endpoint, target, "Connection: close\r\n");
    std::error_code ec;
    asio::read(connection.socket_, asio::dynamic_buffer(connection.input_), ec);
    connection.head_.append(connection.input_);
    return std::move(connection.head_);
}

// A masked client WebSocket frame (RFC 6455 section 5.3) with a short payload.
[[nodiscard]] std::string client_frame(std::uint8_t opcode, std::string_view payload) {
    constexpr std::array<char, 4> mask{'\x12', '\x34', '\x56', '\x78'};
    std::string frame{};
    frame.push_back(static_cast<char>(0x80 | opcode));
    frame.push_back(static_cast<char>(0x80 | payload.size()));
    frame.append(mask.data(), mask.size());
    for (std::size_t index = 0; index != payload.size(); ++index) {
        frame.push_back(static_cast<char>(payload[index] ^ mask[index % mask.size()]));
    }
    return frame;
}

struct server_frame {
    std::uint8_t opcode_{};
    std::string payload_{};
};

[[nodiscard]] server_frame read_frame(upgraded_connection& connection) {
    while (connection.input_.size() < 2 ||
           connection.input_.size() < 2 + (static_cast<unsigned char>(connection.input_[1]) & 0x7f)) {
        std::array<char, 1024> input{};
        connection.input_.append(input.data(), connection.socket_.read_some(asio::buffer(input)));
    }
    const auto length = static_cast<std::size_t>(static_cast<unsigned char>(connection.input_[1]) & 0x7f);
    server_frame frame{static_cast<std::uint8_t>(connection.input_[0] & 0x0f), std::string(std::string_view(connection.input_).substr(2, length))};
    connection.input_.erase(0, 2 + length);
    return frame;
}

[[nodiscard]] bool eventually(const std::atomic<bool>& flag) {
    ruvia::test::wait_until([&flag] { return flag.load(); }, std::chrono::seconds(5));
    return true;
}
}  // namespace udp_tunnel_test

using udp_tunnel_test::udp_observation;

TEST(Ruvia, http1_udp_tunnel_capsules_echo_preserve_siblings_and_owned_lifetime) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    const auto& observation = udp_observation;
    observation->eof_ = false;
    observation->read_overlap_rejected_ = false;
    observation->write_overlap_rejected_ = false;
    observation->capsule_bound_rejected_ = false;
    observation->failed_ = false;
    ruvia::test::http_server_fixture server(io);
    {
        auto tunnel = open_udp_tunnel(io, server.endpoint(), "/.well-known/masque/udp/target.test/53/");
        EXPECT_TRUE(tunnel.head_.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(tunnel.head_.find("\r\ncapsule-protocol: ?1\r\n") != std::string::npos ||
                    tunnel.head_.find("\r\nCapsule-Protocol: ?1\r\n") != std::string::npos);
        for (const auto expected : {std::string_view("ready"), std::string_view("second-ready")}) {
            const auto ready = read_datagram(tunnel);
            EXPECT_TRUE(ready.has_value() && *ready == expected);
        }
        std::string query{};
        append_capsule(query, 0, "dns-query");
        asio::write(tunnel.socket_, asio::buffer(query));
        EXPECT_TRUE(get_response(io, server.endpoint(), "/udp-tunnel-sibling").ends_with("\r\n\r\nsibling"));
        tunnel.socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
        const auto echoed = read_datagram(tunnel);
        EXPECT_TRUE(echoed.has_value() && *echoed == "dns-query");
        EXPECT_TRUE(!read_datagram(tunnel));
    }
    EXPECT_TRUE(eventually(observation->eof_));
    EXPECT_TRUE(observation->capsule_bound_rejected_);
    EXPECT_TRUE(observation->read_overlap_rejected_);
    EXPECT_TRUE(observation->write_overlap_rejected_);
    EXPECT_TRUE(!observation->failed_);
    EXPECT_TRUE(get_response(io, server.endpoint(), "/udp-tunnel-sibling").ends_with("\r\n\r\nsibling"));
    {
        auto delayed = open_udp_tunnel(io, server.endpoint(), "/.well-known/masque/udp/cancel.test/53/");
        EXPECT_TRUE(delayed.head_.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(!read_datagram(delayed));
    }
    server.finish();
}

// A connect-udp upgrade target is an origin path like any other: dot segments
// are refused before any tunnel route can capture them, and a path with no
// connect-udp route serves its GET route with the Upgrade ignored (RFC 9110 §7.8).
TEST(Ruvia, http1_udp_tunnel_upgrade_routes_only_normalized_tunnel_paths) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    udp_observation->dynamic_invoked_ = false;
    const auto exchange = [&](std::string_view path) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto request = std::string("GET ") + std::string(path) +
                             " HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n"
                             "Upgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::string response{};
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        return response;
    };
    EXPECT_TRUE(exchange("/udp-tunnel/../53").starts_with("HTTP/1.1 404 "));
    EXPECT_TRUE(exchange("/udp-tunnel/%2E%2E/53").starts_with("HTTP/1.1 404 "));
    EXPECT_TRUE(!udp_observation->dynamic_invoked_);
    const auto sibling = exchange("/udp-tunnel-sibling");
    EXPECT_TRUE(sibling.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(sibling.ends_with("\r\n\r\nsibling"));
    EXPECT_TRUE(exchange("/udp-tunnel/target.test/53").starts_with("HTTP/1.1 101 "));
    EXPECT_TRUE(udp_observation->dynamic_invoked_);
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view h10_request =
            "GET /udp-tunnel/target.test/53 HTTP/1.0\r\nHost: localhost\r\n"
            "Connection: Upgrade, close\r\nUpgrade: connect-udp\r\nCapsule-Protocol: ?1\r\n\r\n";
        asio::write(client, asio::buffer(h10_request));
        std::string response{};
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        EXPECT_TRUE(response.starts_with("HTTP/1.1 404 "));
    }
    server.finish();
}

// RFC 9110 section 7.8: Upgrade together with Expect: 100-continue gets a
// 100 (Continue) before the 101 (Switching Protocols), for a CONNECT-UDP
// upgrade exactly as for a WebSocket one, even though no content follows.
TEST(Ruvia, http1_udp_tunnel_upgrade_with_continue_expectation_sends_100_before_101) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /udp-tunnel/target.test/53 HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n"
        "Upgrade: connect-udp\r\nCapsule-Protocol: ?1\r\nExpect: 100-continue\r\n\r\n";
    asio::write(client, asio::buffer(request));
    std::string response{};
    std::array<char, 1024> input{};
    std::error_code ec;
    while (!ec) {
        const auto read = client.read_some(asio::buffer(input), ec);
        response.append(input.data(), read);
    }
    EXPECT_TRUE(response.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 101 "));
    client.close();
    server.finish();
}

TEST(Ruvia, server_capsule_and_udp_results_outlive_unread_input_and_connections) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const bool capsule : {true, false}) {
        {
            auto tunnel = open_udp_tunnel(io, server.endpoint(),
                capsule ? "/.well-known/masque/udp/retained-capsule.test/53/"
                        : "/.well-known/masque/udp/retained-datagram.test/53/");
            EXPECT_TRUE(tunnel.head_.starts_with("HTTP/1.1 101 "));
            // One transport write includes a second complete capsule. The
            // handler reads only the first, never advancing to EOF.
            std::string batch{};
            for (const char fill : {'r', 's'}) {
                append_capsule(batch, capsule ? 42 : 0, std::string(1024, fill));
            }
            asio::write(tunnel.socket_, asio::buffer(batch));
            tunnel.socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
            EXPECT_TRUE(!read_datagram(tunnel));
        }
        // Wait for actual connection retirement, not just the peer FIN.
        // Worker state must keep the result alive through this barrier.
        server.finish();
        for (const auto& expected : {std::string(1024, 'r'), std::string("empty")}) {
            EXPECT_TRUE(get_response(io, server.endpoint(), "/udp-tunnel-retained").ends_with(std::string("\r\n\r\n") + expected));
        }
        server.finish();
    }
}

TEST(Ruvia, server_websocket_exposes_negotiated_subprotocol_and_peer_close) {
    using namespace udp_tunnel_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    udp_observation->ws_subprotocol_matched_ = false;
    udp_observation->ws_peer_close_matched_ = false;
    {
        auto ws = open_upgrade(io, server.endpoint(), "/udp-tunnel-ws",
            "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: chat.v1, chat.v2\r\n");
        EXPECT_TRUE(ws.head_.starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(ws.head_.find(": chat.v2\r\n") != std::string::npos);
        const auto text = client_frame(0x1, "ping-echo");
        asio::write(ws.socket_, asio::buffer(text));
        auto frame = read_frame(ws);
        if (frame.opcode_ == 0xA) {
            frame = read_frame(ws);
        }
        EXPECT_TRUE(frame.opcode_ == 0x1 && frame.payload_ == "ping-echo");
        const auto close_frame = client_frame(0x8, std::string("\x0f\xa1", 2) + "client-done");
        asio::write(ws.socket_, asio::buffer(close_frame));
        const auto close_reply = read_frame(ws);
        EXPECT_TRUE(close_reply.opcode_ == 0x8);
    }
    EXPECT_TRUE(eventually(udp_observation->ws_subprotocol_matched_));
    EXPECT_TRUE(eventually(udp_observation->ws_peer_close_matched_));
    server.finish();
}
