#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif
#include <asio/co_spawn.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <openssl/ssl.h>

#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"

#include "content_decoding_fixture.h"
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
    ruvia::test::reset_app_callbacks(ruvia::app());
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

#if defined(RUVIA_ENABLE_HTTP2)

namespace {
struct connect_observation {
    std::mutex mutex_;
    std::string received_{};
    std::atomic<bool> saw_eof_{};
    std::atomic<bool> stable_{true};
    std::atomic<bool> return_early_{};
    std::atomic<bool> halfclose_capsules_{};
    std::atomic<int> seen_urgency_{-1};
    std::atomic<bool> hold_push_{};
};
const auto observation = std::make_shared<connect_observation>();
[[maybe_unused]] const bool connect_observation_registered = [] {
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

class hold_off_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
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
    context_value.draft().status(error.status());
    std::string text;
    text.append("body=");
    text.append(std::to_string(body.size()));
    co_return context_value.text(std::move(text));
}
[[maybe_unused]] const bool expectation_handler_registered = [] {
    ruvia::app().on_error({.prefix_ = "/expect-rejected", .handler_ = &answer_rejected_expectation});
    return true;
}();

class connect_routes final {
public:
    RUVIA_ROUTES_BEGIN(connect_routes)
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::milliseconds(40)};
    RUVIA_CONNECT("target.test:443", echo, options);
    RUVIA_CONNECT_PROTOCOL("test-tunnel", "/udp/:host/:port", echo);
    RUVIA_CONNECT_PROTOCOL("slow-tunnel", "/slow-tunnel", slow_tunnel);
    const ruvia::http_tunnel_route_config capsule_options{.datagrams_ = true};
    RUVIA_CONNECT_PROTOCOL("capsule-tunnel", "/capsules", capsules, capsule_options);
    RUVIA_POST("/slow-upload", slow_upload, ruvia::stream_body);
    RUVIA_GET("/after", after);
    RUVIA_POST("/expect-rejected", expect_rejected);
    RUVIA_GET_WS("/late-fin-websocket", late_fin_websocket, hold_off_middleware);
    RUVIA_GET("/prio-idle", prio_idle);
    RUVIA_GET("/push-parent-cancel", push_parent_cancel);
    RUVIA_GET("/pushed-buffered-cancel", pushed_buffered_cancel);
    RUVIA_ROUTES_END
    ruvia::task<void> echo(ruvia::context& context_value);
    ruvia::task<void> slow_tunnel(ruvia::context& context_value);
    ruvia::task<void> capsules(ruvia::context& context_value);
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
    ruvia::task<ruvia::http_response> prio_idle(ruvia::context& context_value) {
        context_value.worker_state<std::shared_ptr<connect_observation>>()->seen_urgency_ =
            static_cast<int>(context_value.req().priority().urgency_.value());
        co_return context_value.text("ok");
    }
    ruvia::task<ruvia::http_response> push_parent_cancel(ruvia::context& context_value) {
        (void)context_value.push({.method_ = "GET", .scheme_ = "http", .authority_ = "localhost", .path_ = "/pushed-buffered-cancel"});
        co_return context_value.text("parent");
    }
    ruvia::task<ruvia::http_response> pushed_buffered_cancel(ruvia::context& context_value) {
        auto& obs = *context_value.worker_state<std::shared_ptr<connect_observation>>();
        while (obs.hold_push_) {
            (void)co_await ruvia::sleep_for(
                context_value.worker(), std::chrono::milliseconds(1), context_value.stop_token());
        }
        co_return context_value.text("pushed-body");
    }
};
ruvia::task<void> connect_routes::echo(ruvia::context& context_value) {
    auto& observation_value = *context_value.worker_state<std::shared_ptr<connect_observation>>();
    if (observation_value.return_early_) {
        co_return;
    }
    std::optional<std::string> retained;
    auto& tunnel = context_value.tunnel();
    {
        auto cold = tunnel.read();
    }
    while (auto chunk = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.received_.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        if (!retained) {
            retained.emplace(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        auto output = tunnel.write(*chunk);
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
    const auto total_text = std::string(std::to_string(total));
    co_await tunnel.write(std::string_view(total_text));
    co_await tunnel.finish();
}
// Reads capsules until the malformed input aborts the stream.
ruvia::task<void> connect_routes::capsules(ruvia::context& context_value) {
    auto& obs = *context_value.worker_state<std::shared_ptr<connect_observation>>();
    auto stream = context_value.tunnel().capsules();
    if (obs.halfclose_capsules_) {
        co_await stream.finish();
    }
    try {
        while (co_await stream.read()) {
        }
    } catch (const std::runtime_error&) {
    }
}
ruvia::task<ruvia::http_response> connect_routes::slow_upload(ruvia::context& context_value) {
    co_await hold_off(context_value);
    std::size_t total = 0;
    auto& reader = context_value.req().body_reader();
    while (auto chunk = co_await reader.text()) {
        total += chunk->size();
    }
    std::string body;
    body = std::string(std::to_string(total));
    co_return context_value.text(std::move(body));
}
ruvia::task<void> flush_peer(asio::ip::tcp::socket& socket, ruvia::http2_connection& connection) {
    std::string bytes;
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
TEST(Ruvia, http2_connect_routes_echo_large_duplex_streams_and_leave_sibling_requests_available) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        auto& observation_value = *observation;
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            std::string received_wire{};
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
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            for (unsigned round = 0; round != 3; ++round) {
                observation_value.return_early_ = round == 2;
                const auto request = round != 1 ? peer.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "TARGET.TEST:443"})
                                                : peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "test-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/udp/target.test/443"});
                const auto* id_ptr = std::get_if<std::uint32_t>(&request);
                if (id_ptr == nullptr) {
                    throw std::runtime_error("CONNECT request submission rejected");
                }
                const auto id = *id_ptr;
                co_await flush_peer(socket, peer);
                bool established = false;
                while (!established) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (const auto* head = event->response_head()) {
                            EXPECT_TRUE(head->stream_id() == id);
                            EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                            EXPECT_TRUE(std::ranges::none_of(head->head().headers(), [](const auto& header_value) { return header_value.name() == "content-length"; }));
                            established = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT handshake reset");
                        }
                    }
                    co_await flush_peer(socket, peer);
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
                            EXPECT_TRUE(!peer.connection_error());
                        }
                        co_await flush_peer(socket, peer);
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
                    EXPECT_TRUE(sent_fin);
                    continue;
                }
                const auto submitted = peer.submit_data(id, payload_value, ruvia::http2_end_stream::end_stream, payload_value.size());
                EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete || submitted.status_ == ruvia::http2_data_submit_status::backpressured);
                auto consumed = submitted.consumed_bytes_;
                co_await flush_peer(socket, peer);
                std::string echoed{};
                bool ended = false;
                while (!ended) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (auto* data = event->tunnel_data()) {
                            EXPECT_TRUE(data->stream_id() == id);
                            echoed.append(data->bytes());
                            (void)data->take_credit();
                        }
                        if (const auto* end = event->tunnel_end()) {
                            EXPECT_TRUE(end->stream_id() == id);
                            ended = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT stream reset");
                        }
                    }
                    if (consumed != payload_value.size()) {
                        const auto suffix = std::string_view(payload_value).substr(consumed);
                        const auto retried = peer.submit_data(id, suffix, ruvia::http2_end_stream::end_stream, suffix.size());
                        EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete || retried.status_ == ruvia::http2_data_submit_status::backpressured);
                        consumed += retried.consumed_bytes_;
                    }
                    co_await flush_peer(socket, peer);
                }
                EXPECT_EQ(consumed, payload_value.size());
                EXPECT_TRUE(echoed == payload_value);
                EXPECT_TRUE(observation_value.saw_eof_ && observation_value.stable_);
                {
                    std::lock_guard lock(observation_value.mutex_);
                    EXPECT_TRUE(observation_value.received_ == payload_value);
                    observation_value.received_.clear();
                }
                observation_value.saw_eof_ = false;
            }
            auto sibling = peer.submit_request_head(ruvia::http2_regular_request_head_view{.scheme_ = "http", .authority_ = "proxy.test", .target_ = "/after"});
            if (!std::holds_alternative<std::uint32_t>(sibling)) {
                throw std::runtime_error("sibling request failed");
            }
            co_await flush_peer(socket, peer);
            bool ended = false;
            std::string body{};
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
                co_await flush_peer(socket, peer);
            }
            EXPECT_TRUE(body == "sibling");
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
// RFC 9113 section 8.5 / RFC 9298 section 5: DATA a client sends before the 2xx
// is tunnel data. The route consumes it once the tunnel is accepted.
TEST(Ruvia, http2_connect_route_receives_data_sent_before_the_response) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        auto& observation_value = *observation;
        observation_value.return_early_ = false;
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("CONNECT peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const auto request = peer.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "target.test:443"});
            const auto* id_ptr = std::get_if<std::uint32_t>(&request);
            if (id_ptr == nullptr) {
                throw std::runtime_error("CONNECT request submission rejected");
            }
            const auto id = *id_ptr;
            co_await flush_peer(socket, peer);
            // The client core only sends after the 2xx; write the optimistic
            // DATA frame directly behind its HEADERS.
            const std::string early(64, 't');
            std::string raw{};
            ruvia::test::append_frame(raw, ruvia::http2_frame_type::data, 0, id, early);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(raw), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            bool established = false;
            bool finished = false;
            bool ended = false;
            std::string echoed{};
            while (!ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                        established = true;
                    }
                    if (auto* data = event->tunnel_data()) {
                        echoed.append(data->bytes());
                        (void)data->take_credit();
                    }
                    if (event->tunnel_end()) {
                        ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("CONNECT stream reset");
                    }
                }
                if (established && !finished && echoed.size() == early.size()) {
                    finished = true;
                    const auto submitted = peer.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
                    EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete);
                    EXPECT_EQ(submitted.consumed_bytes_, std::size_t{0});
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_TRUE(echoed == early);
            EXPECT_TRUE(observation_value.saw_eof_ && observation_value.stable_);
            {
                std::lock_guard lock(observation_value.mutex_);
                EXPECT_TRUE(observation_value.received_ == early);
                observation_value.received_.clear();
            }
            observation_value.saw_eof_ = false;
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
TEST(Ruvia, http2_slow_body_and_tunnel_consumers_return_receive_window_without_peer_input) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
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
            auto peer = ruvia::http2_connection::client();
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
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("slow consumer SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            // Three times the server's 1 MiB stream/connection receive window.
            const std::string payload_value(std::size_t{3} * 1024 * 1024 + 7, 'w');
            const auto expected = std::string(std::to_string(payload_value.size()));

            const auto tunnel = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "slow-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/slow-tunnel"});
            const auto* tunnel_id_ptr = std::get_if<std::uint32_t>(&tunnel);
            if (tunnel_id_ptr == nullptr) {
                throw std::runtime_error("slow tunnel request submission rejected");
            }
            const auto tunnel_id = *tunnel_id_ptr;
            co_await flush_peer(socket, peer);
            bool established = false;
            while (!established) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                        established = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow tunnel handshake reset");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const auto tunnel_submitted = peer.submit_data(tunnel_id, payload_value, ruvia::http2_end_stream::end_stream, payload_value.size());
            EXPECT_TRUE(tunnel_submitted.status_ == ruvia::http2_data_submit_status::complete || tunnel_submitted.status_ == ruvia::http2_data_submit_status::backpressured);
            auto tunnel_consumed = tunnel_submitted.consumed_bytes_;
            co_await flush_peer(socket, peer);
            std::string tunnel_reply{};
            bool tunnel_ended = false;
            while (!tunnel_ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (auto* data = event->tunnel_data()) {
                        tunnel_reply.append(data->bytes());
                        (void)data->take_credit();
                    }
                    if (event->tunnel_end()) {
                        tunnel_ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow tunnel stream reset");
                    }
                }
                if (tunnel_consumed != payload_value.size()) {
                    const auto suffix = std::string_view(payload_value).substr(tunnel_consumed);
                    const auto retried = peer.submit_data(tunnel_id, suffix, ruvia::http2_end_stream::end_stream, suffix.size());
                    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete || retried.status_ == ruvia::http2_data_submit_status::backpressured);
                    tunnel_consumed += retried.consumed_bytes_;
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_EQ(tunnel_consumed, payload_value.size());
            EXPECT_TRUE(tunnel_reply == expected);

            const auto upload = peer.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/slow-upload", .content_ = ruvia::http_request_content_plan::streaming()});
            const auto* upload_id_ptr = std::get_if<std::uint32_t>(&upload);
            if (upload_id_ptr == nullptr) {
                throw std::runtime_error("slow upload request submission rejected");
            }
            const auto upload_id = *upload_id_ptr;
            const auto upload_submitted = peer.submit_data(upload_id, payload_value, ruvia::http2_end_stream::end_stream, payload_value.size());
            EXPECT_TRUE(upload_submitted.status_ == ruvia::http2_data_submit_status::complete || upload_submitted.status_ == ruvia::http2_data_submit_status::backpressured);
            auto upload_consumed = upload_submitted.consumed_bytes_;
            co_await flush_peer(socket, peer);
            std::string upload_reply{};
            bool upload_ended = false;
            while (!upload_ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                    }
                    if (auto* bytes = event->message_body_chunk()) {
                        upload_reply.append(bytes->bytes());
                        (void)bytes->take_credit();
                    }
                    if (event->message_end()) {
                        upload_ended = true;
                    }
                    if (event->stream_closed() || peer.connection_error()) {
                        throw std::runtime_error("slow upload stream reset");
                    }
                }
                if (upload_consumed != payload_value.size()) {
                    const auto suffix = std::string_view(payload_value).substr(upload_consumed);
                    const auto retried = peer.submit_data(upload_id, suffix, ruvia::http2_end_stream::end_stream, suffix.size());
                    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete || retried.status_ == ruvia::http2_data_submit_status::backpressured);
                    upload_consumed += retried.consumed_bytes_;
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_EQ(upload_consumed, payload_value.size());
            EXPECT_TRUE(upload_reply == expected);
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
TEST(Ruvia, http2_websocket_request_half_closed_before_handshake_receives_error_response) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
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
            auto peer = ruvia::http2_connection::client();
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
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("websocket SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const std::array<ruvia::http_header_view, 1> headers{ruvia::http_header_view("sec-websocket-version", "13")};
            const auto request = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "websocket", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/late-fin-websocket", .headers_ = headers});
            const auto* id_ptr = std::get_if<std::uint32_t>(&request);
            if (id_ptr == nullptr) {
                throw std::runtime_error("websocket request submission rejected");
            }
            const auto id = *id_ptr;
            co_await flush_peer(socket, peer);
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
                        EXPECT_TRUE(head->stream_id() == id);
                        EXPECT_TRUE(head->head().status() != ruvia::http_status::ok);
                        answered = true;
                    }
                    if (peer.connection_error()) {
                        throw std::runtime_error("websocket connection failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
// RFC 9110 §10.1.1: the server answers an unsupported expectation without
// processing the request content. DATA arriving while the suspended error
// handler runs is discarded, so the request it reads stays stable and empty.
TEST(Ruvia, http2_rejected_expectation_discards_request_content_received_during_error_handler) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    std::string body{};
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
            std::string wire(ruvia::http2_client_preface);
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
            const std::string chunk(16384, 'd');
            for (int index = 0; index < 3; ++index) {
                ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 0, 1, chunk);
            }
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(wire), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            std::string received{};
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
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(body, std::string("body=0"));
}
// RFC 9297 section 3.3 / RFC 9113 section 8.1.1: a truncated capsule makes the
// tunnel request malformed, so the server resets its stream with PROTOCOL_ERROR.
TEST(Ruvia, http2_capsule_route_resets_truncated_capsule_with_protocol_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http2_error_code> reset_error;
    ruvia::test::http_server_fixture server(io);
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
            auto peer = ruvia::http2_connection::client();
            std::array<char, 16384> input{};
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("capsule peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer);
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("capsule SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer);
            }
            const auto request = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "capsule-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/capsules"});
            const auto* id_ptr = std::get_if<std::uint32_t>(&request);
            if (id_ptr == nullptr) {
                throw std::runtime_error("capsule request submission rejected");
            }
            const auto id = *id_ptr;
            co_await flush_peer(socket, peer);
            bool sent = false;
            while (!reset_error) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (const auto* head = event->response_head()) {
                        EXPECT_TRUE(head->head().status() == ruvia::http_status::ok);
                        // DATAGRAM capsule declaring 10 payload bytes, ended after 2.
                        const auto submitted = peer.submit_data(id, std::string_view("\x00\x0a"
                                                                                     "ab",
                                                                        4),
                            ruvia::http2_end_stream::end_stream, 65535);
                        EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete);
                        EXPECT_EQ(submitted.consumed_bytes_, std::size_t{4});
                        sent = true;
                    }
                    if (const auto* closed = event->stream_closed()) {
                        EXPECT_TRUE(closed->stream_id() == id);
                        reset_error = closed->error();
                    }
                    EXPECT_TRUE(!peer.connection_error());
                }
                co_await flush_peer(socket, peer);
            }
            EXPECT_TRUE(sent);
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog_value.cancel();
        server.finish();
        EXPECT_TRUE(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
}

// WST-01 (server): When the server half-closes its capsule stream first and the
// client subsequently sends a truncated capsule on DATA(END_STREAM), the server
// rejects the capsule. Both END_STREAM flags closed the stream, so no
// RST_STREAM follows (RFC 9113 section 5.1 forbids frames on a closed stream)
// and the connection keeps serving.
TEST(Ruvia, http2_capsule_route_closed_stream_rejects_truncated_capsule_without_reset) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    observation->halfclose_capsules_ = true;
    ruvia::test::http_server_fixture server(io);
    std::string raw_from_server{};
    // The frames read from the server, as (type, flags, payload).
    const auto server_frames = [&] {
        std::vector<std::tuple<std::uint8_t, std::uint8_t, std::string>> frames{};
        for (std::size_t off = 0; off + ruvia::http2_frame_header_bytes <= raw_from_server.size();) {
            const auto hdr = ruvia::parse_http2_frame_header(
                std::span<const char>(raw_from_server.data() + off, ruvia::http2_frame_header_bytes));
            if (!hdr || off + ruvia::http2_frame_header_bytes + hdr->length_ > raw_from_server.size()) {
                break;
            }
            frames.emplace_back(hdr->type_, hdr->flags_,
                std::string_view(raw_from_server).substr(off + ruvia::http2_frame_header_bytes, hdr->length_));
            off += ruvia::http2_frame_header_bytes + hdr->length_;
        }
        return frames;
    };
    const auto ping_acknowledged = [&] {
        return std::ranges::any_of(server_frames(), [](const auto& frame) {
            return std::get<0>(frame) == static_cast<std::uint8_t>(ruvia::http2_frame_type::ping) &&
                   (std::get<1>(frame) & 0x1) != 0 && std::get<2>(frame) == "12345678";
        });
    };
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        (void)co_await ruvia::async_asio([&](auto h) { socket.async_connect(server.endpoint(), std::move(h)); });
        auto peer = ruvia::http2_connection::client();
        std::array<char, 16384> input{};
        auto pump = [&]() -> ruvia::task<bool> {
            auto r = co_await ruvia::async_asio<std::size_t>([&](auto h) {
                socket.async_read_some(asio::buffer(input), std::move(h));
            });
            if (r.error_code() || r.result() == 0) {
                co_return false;
            }
            raw_from_server.append(input.data(), r.result());
            (void)peer.feed(std::string_view(input.data(), r.result()));
            co_return true;
        };
        co_await flush_peer(socket, peer);
        while (!peer.received_peer_settings()) {
            co_await pump();
            while (peer.next_event()) {
            }
            co_await flush_peer(socket, peer);
        }
        const auto req = peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{
            .protocol_ = "capsule-tunnel", .scheme_ = "http", .authority_ = "proxy.test", .target_ = "/capsules"});
        const auto id = std::get<std::uint32_t>(req);
        co_await flush_peer(socket, peer);
        bool saw_server_end = false;
        while (!saw_server_end) {
            co_await pump();
            while (auto ev = peer.next_event()) {
                if (ev->tunnel_end()) {
                    saw_server_end = true;
                }
            }
            co_await flush_peer(socket, peer);
        }
        (void)peer.submit_data(id, std::string_view("\x00\x0a"
                                                    "ab",
                                       4),
            ruvia::http2_end_stream::end_stream, 65535);
        co_await flush_peer(socket, peer);
        const auto& worker_value = attachment.loop().worker();
        (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(30));
        std::string ping_frame{};
        ruvia::test::append_frame(ping_frame, ruvia::http2_frame_type::ping, 0, 0, "12345678");
        (void)co_await ruvia::async_asio<std::size_t>([&](auto h) {
            asio::async_write(socket, asio::buffer(ping_frame), std::move(h));
        });
        while (!ping_acknowledged() && co_await pump()) {
        }
        ruvia::close_socket(socket);
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    observation->halfclose_capsules_ = false;

    EXPECT_TRUE(ping_acknowledged());
    for (const auto& [type, flags, payload] : server_frames()) {
        EXPECT_TRUE(type != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        EXPECT_TRUE(type != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    }
}

// H2-07 & H2-09: PRIORITY_UPDATE on an idle stream is observed by the route handler
// once HEADERS opens the stream, and a buffered push stream resets with CANCEL when
// the client lowers SETTINGS_MAX_CONCURRENT_STREAMS to 0.
TEST(Ruvia, http2_web_idle_priority_update_and_buffered_push_stream_limit_cancel) {
    auto& io = ruvia::test::new_test_io_context();
    observation->seen_urgency_ = -1;
    observation->hold_push_ = true;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket sock(io);
    sock.connect(server.endpoint());

    std::string wire(ruvia::http2_client_preface);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view("\x00\x02\x00\x00\x00\x01\x00\x03\x00\x00\x00\x01", 12));
    std::array<char, 32> prio_buf{};
    const auto prio_sz =
        std::get<0>(ruvia::encode_http2_priority_update(prio_buf, 1, {.urgency_ = 1, .incremental_ = false}));
    wire.append(prio_buf.data(), prio_sz);
    std::string get1;
    ruvia::hpack_encoder::encode_header(get1, ":method", "GET");
    ruvia::hpack_encoder::encode_header(get1, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(get1, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(get1, ":path", "/prio-idle");
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, get1);

    std::string get3;
    ruvia::hpack_encoder::encode_header(get3, ":method", "GET");
    ruvia::hpack_encoder::encode_header(get3, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(get3, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(get3, ":path", "/push-parent-cancel");
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3, get3);
    asio::write(sock, asio::buffer(wire));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (observation->seen_urgency_ == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(observation->seen_urgency_.load(), 1);

    std::string wire2{};
    ruvia::test::append_frame(wire2, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view("\x00\x03\x00\x00\x00\x00", 6));
    asio::write(sock, asio::buffer(wire2));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    observation->hold_push_ = false;

    sock.non_blocking(true);
    std::string in_buf{};
    std::uint32_t push_rst_code = 0xffffffffU;
    const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (push_rst_code == 0xffffffffU && std::chrono::steady_clock::now() < read_deadline) {
        std::array<char, 4096> tmp{};
        asio::error_code ec;
        const auto n = sock.read_some(asio::buffer(tmp), ec);
        if (n > 0) {
            in_buf.append(tmp.data(), n);
            std::size_t off = 0;
            while (off + 9 <= in_buf.size()) {
                const auto len = (static_cast<unsigned char>(in_buf[off]) << 16) |
                                 (static_cast<unsigned char>(in_buf[off + 1]) << 8) |
                                 static_cast<unsigned char>(in_buf[off + 2]);
                const auto type = static_cast<unsigned char>(in_buf[off + 3]);
                const auto sid = ((static_cast<unsigned char>(in_buf[off + 5]) & 0x7fU) << 24) |
                                 (static_cast<unsigned char>(in_buf[off + 6]) << 16) |
                                 (static_cast<unsigned char>(in_buf[off + 7]) << 8) |
                                 static_cast<unsigned char>(in_buf[off + 8]);
                if (off + 9 + len > in_buf.size()) {
                    break;
                }
                if (type == static_cast<unsigned char>(ruvia::http2_frame_type::rst_stream) &&
                    sid == 2 && len == 4) {
                    push_rst_code = (static_cast<unsigned char>(in_buf[off + 9]) << 24) |
                                    (static_cast<unsigned char>(in_buf[off + 10]) << 16) |
                                    (static_cast<unsigned char>(in_buf[off + 11]) << 8) |
                                    static_cast<unsigned char>(in_buf[off + 12]);
                }
                off += 9 + len;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_EQ(push_rst_code, static_cast<std::uint32_t>(ruvia::http2_error_code::cancel));
    sock.close();
    server.finish();
}

namespace {
struct tunnel_observation {
    std::mutex mutex_;
    std::string bytes_{};
    std::atomic<bool> finish_first_{};
    std::atomic<bool> eof_{};
    [[nodiscard]] std::string received() {
        std::lock_guard lock(mutex_);
        return std::string(bytes_);
    }
};
const auto tunnel_obs = std::make_shared<tunnel_observation>();
[[maybe_unused]] const bool tunnel_obs_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<tunnel_observation>>([] { return tunnel_obs; });
    return true;
}();

class client_tunnel_routes final {
public:
    RUVIA_ROUTES_BEGIN(client_tunnel_routes)
    RUVIA_CONNECT("client-target.test:443", echo);
    RUVIA_CONNECT_PROTOCOL("test-protocol", "/tunnel", echo);
    const ruvia::http_tunnel_route_config capsule_route{.datagrams_ = true};
    RUVIA_CONNECT_PROTOCOL("test-capsules", "/capsules", capsules, capsule_route);
    RUVIA_CONNECT_PROTOCOL("test-no-datagrams", "/no-datagrams", no_datagrams);
    RUVIA_GET("/sibling", sibling);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& c);
    ruvia::task<void> capsules(ruvia::context& c);
    ruvia::task<void> no_datagrams(ruvia::context& c);
    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
};
ruvia::task<void> client_tunnel_routes::echo(ruvia::context& c) {
    auto& observation_value = *c.worker_state<std::shared_ptr<tunnel_observation>>();
    {
        std::lock_guard lock(observation_value.mutex_);
        observation_value.bytes_.clear();
    }
    observation_value.eof_ = false;
    auto& tunnel = c.tunnel();
    if (observation_value.finish_first_) {
        co_await tunnel.write("greeting");
        co_await tunnel.finish();
    }
    while (auto bytes = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.bytes_.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        }
        if (!observation_value.finish_first_) {
            co_await tunnel.write(*bytes);
        }
    }
    observation_value.eof_ = true;
    co_await tunnel.finish();
}
}  // namespace
TEST(Ruvia, http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http_client_tunnel> retained_tunnel;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        auto& observation_value = *tunnel_obs;
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .versions_ = ruvia::http_client_versions{.http1_ = false}});
        const auto client_origin = client.client();
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation_value.finish_first_ = round >= 2;
                std::string authority(round == 1 ? "proxy.test" : "client-target.test:443");
                auto cold = client_origin.connect(authority).protocol(round == 1 ? "test-protocol" : "").target(round == 1 ? "/tunnel" : "").open();
                authority.assign("mutated");
                auto result_value = co_await std::move(cold);
                EXPECT_TRUE(result_value.response() == nullptr && result_value.tunnel() != nullptr);
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                EXPECT_TRUE(tunnel.status() == ruvia::http_status::ok);
                EXPECT_TRUE(!tunnel.header("content-length"));
                std::string echoed{};
                const auto receive = [&]() -> ruvia::task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation_value.finish_first_) {
                    co_await receive();
                    EXPECT_TRUE(echoed == "greeting");
                }
                ruvia::task_scope readers(worker_value);
                if (round == 3) {
                    tunnel.abort();
                    bool half_closed_write_rejected = false;
                    try {
                        auto late_write = tunnel.write("late");
                        static_cast<void>(late_write);
                    } catch (const ruvia::http_client_error& error) {
                        half_closed_write_rejected = error.code() == std::make_error_code(std::errc::operation_canceled);
                    }
                    EXPECT_TRUE(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client_origin.get("/sibling").send();
                    std::string sibling_body{};
                    while (auto bytes = co_await sibling.stream().text()) {
                        sibling_body.append(*bytes);
                    }
                    EXPECT_TRUE(sibling_body == "sibling");
                    retained_tunnel.emplace(std::move(tunnel));
                    continue;
                }
                if (!observation_value.finish_first_) {
                    readers.spawn(receive());
                }
                std::string payload_value(100003, 't');
                for (std::size_t offset = 0; offset != payload_value.size();) {
                    const auto count = std::min<std::size_t>(16384, payload_value.size() - offset);
                    auto output = tunnel.write(std::string_view(payload_value).substr(offset, count));
                    {
                        // Discarding a cold finish does not claim the write lane.
                        auto unused_finish = tunnel.finish();
                        static_cast<void>(unused_finish);
                    }
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await readers.join();
                if (!observation_value.finish_first_) {
                    EXPECT_TRUE(echoed == payload_value);
                }
                auto sibling = co_await client_origin.get("/sibling").send();
                std::string sibling_body{};
                while (auto bytes = co_await sibling.stream().text()) {
                    sibling_body.append(*bytes);
                }
                EXPECT_TRUE(sibling_body == "sibling");
                EXPECT_TRUE(observation_value.received() == payload_value && observation_value.eof_);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (retained_tunnel) {
            retained_tunnel->abort();
            retained_tunnel->abort();
            bool late_write_rejected = false;
            try {
                auto late_write = retained_tunnel->write("late");
                static_cast<void>(late_write);
            } catch (const ruvia::http_client_error&) {
                late_write_rejected = true;
            }
            EXPECT_TRUE(late_write_rejected);
            retained_tunnel.reset();
        }
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace {
ruvia::task<void> client_tunnel_routes::capsules(ruvia::context& c) {
    auto stream = c.tunnel().capsules();
    while (auto capsule = co_await stream.read()) {
        co_await stream.write(capsule->type(), capsule->payload());
    }
    co_await stream.finish();
}

// The route protocol owns Capsule Protocol negotiation (RFC 9297 section 3.4):
// a route that did not declare datagrams cannot open a capsule facade, and the
// raw tunnel stays usable.
ruvia::task<void> client_tunnel_routes::no_datagrams(ruvia::context& c) {
    auto& tunnel = c.tunnel();
    unsigned rejected = 0;
    try {
        static_cast<void>(tunnel.datagrams());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    try {
        static_cast<void>(tunnel.udp());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    try {
        static_cast<void>(tunnel.capsules());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    co_await tunnel.write(rejected == 3 ? std::string_view("rejected") : std::string_view("accepted"));
    co_await tunnel.finish();
}
}  // namespace
TEST(Ruvia, http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_capsule> retained;
        std::unique_ptr<ruvia::task<void>> cold;
        {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .versions_ = ruvia::http_client_versions{.http1_ = false}});
            const auto client_origin = client.client();
            try {
                auto result_value = co_await client_origin.connect("proxy.test").protocol("test-capsules").target("/capsules").open({.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                // RFC 9297 section 3.4: the datagram route's 2xx advertises it.
                EXPECT_EQ(result_value.tunnel()->header("Capsule-Protocol"), std::string_view("?1"));
                {
                    auto stream = std::move(*result_value.tunnel()).capsules();
                    std::string bytes_value(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes_value);
                    bytes_value.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    EXPECT_TRUE(retained && retained->type() == 0x123456789ULL && retained->text() == std::string(4099, 'c') && retained->bytes().size() == 4099);
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    EXPECT_TRUE(empty && empty->type() == 0 && empty->text().empty() && empty->bytes().empty());
                    co_await stream.finish();
                    EXPECT_TRUE(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                {
                    // Without declared datagrams: no Capsule-Protocol, and the
                    // handler's datagrams()/udp()/capsules() throw std::logic_error.
                    auto plain_result = co_await client_origin.connect("proxy.test").protocol("test-no-datagrams").target("/no-datagrams").open();
                    if (!plain_result.tunnel()) {
                        throw std::runtime_error("protocol CONNECT without datagrams rejected");
                    }
                    auto plain = std::move(*plain_result.tunnel());
                    EXPECT_TRUE(!plain.header("Capsule-Protocol"));
                    std::string answer{};
                    while (auto text_chunk = co_await plain.text()) {
                        answer.append(*text_chunk);
                    }
                    EXPECT_EQ(answer, std::string("rejected"));
                    co_await plain.finish();
                }
                auto second = co_await client_origin.connect("proxy.test").protocol("test-capsules").target("/capsules").open();
                if (!second.tunnel()) {
                    throw std::runtime_error("second capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*second.tunnel()).capsules();
                    cold.reset(new ruvia::task<void>(stream.write(12, std::string(4099, 'x'))));
                }
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        EXPECT_TRUE(retained && retained->payload() == std::string(4099, 'c'));
        cold.reset();
        retained.reset();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// RFC 9297 section 3.2: an unknown capsule type and a DATAGRAM with an
// unknown Context ID (RFC 9298 section 4) are skipped even beyond the reader's
// max_capsule_length_. A malformed DATAGRAM then fails the tunnel as a
// malformed message (RFC 9297 section 3.3): RST_STREAM(PROTOCOL_ERROR) per
// RFC 9113 section 8.1.1, not CANCEL.
TEST(Ruvia, http2_client_udp_tunnel_skips_dropped_capsules_and_resets_malformed_input_with_protocol_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    std::optional<ruvia::http2_error_code> reset_error;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::string output;
        while (!reset_error) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                break;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        EXPECT_TRUE(connection.submit_connect_response_head(head->stream_id(), accepted) ==
                                    ruvia::http2_submit_status::accepted);
                        std::string capsules{};
                        capsules.append("\x17\x40\xc8", 3).append(200, 'u');
                        capsules.append("\x00\x40\xc9\x03", 4).append(200, 'c');
                        capsules.append(
                            "\x00\x06\x00"
                            "after",
                            8);
                        capsules.append("\x00\x00", 2);
                        EXPECT_TRUE(connection.submit_data(head->stream_id(), capsules, ruvia::http2_end_stream::keep_open, 65535).status_ ==
                                    ruvia::http2_data_submit_status::complete);
                        request.emplace(std::move(*head));
                    } else if (auto* closed = event->stream_closed()) {
                        if (closed->source() == ruvia::http2_stream_close_source::peer) {
                            reset_error = closed->error();
                        }
                    } else if (auto* data = event->tunnel_data()) {
                        (void)data->take_credit();
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
        request.reset();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    std::string first{};
    bool malformed_rejected = false;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5), .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            auto opened = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
            if (!opened.tunnel()) {
                throw std::runtime_error("CONNECT-UDP rejected");
            }
            auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
            auto datagram = co_await udp.read();
            if (datagram) {
                first.assign(reinterpret_cast<const char*>(datagram->payload().data()), datagram->payload().size());
            }
            try {
                (void)co_await udp.read();
            } catch (const std::runtime_error&) {
                malformed_rejected = true;
            }
            // The reset is queued with the abort; let the session write it
            // before shutdown closes the connection.
            const auto& worker_value = attachment.loop().worker();
            for (unsigned attempt = 0; !reset_error && attempt != 200; ++attempt) {
                (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_EQ(first, std::string("after"));
    EXPECT_TRUE(malformed_rejected);
    EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
}

// A truncated capsule remains an application error after both HTTP/2 stream
// directions close; a closed stream need not emit another reset frame.
TEST(Ruvia, http2_client_half_closed_tunnel_rejects_truncated_capsule_on_end_stream) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool server_done = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::string output;
        while (!server_done) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                break;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)connection.submit_connect_response_head(head->stream_id(), accepted);
                        request.emplace(std::move(*head));
                    } else if (auto* end = event->tunnel_end()) {
                        std::string truncated(
                            "\x00\x0a"
                            "ab",
                            4);
                        (void)connection.submit_data(
                            end->stream_id(), truncated, ruvia::http2_end_stream::end_stream, 65535);
                    } else if (auto* data = event->tunnel_data()) {
                        (void)data->take_credit();
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
        request.reset();
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool client_threw = false;

    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        auto opened = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened.tunnel() != nullptr);
        auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp.finish();
        try {
            (void)co_await udp.read();
        } catch (const std::runtime_error&) {
            client_threw = true;
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_TRUE(client_threw);
}

// A5H2-04: A malformed capsule detected on a completed tunnel from an earlier
// HTTP/2 connection generation must not submit RST_STREAM(PROTOCOL_ERROR) on
// the same stream ID of a newly reconnected HTTP/2 session.
TEST(Ruvia, http2_client_stale_tunnel_malformed_abort_does_not_reset_reconnected_session_stream) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::uint32_t second_conn_rst_stream_id = 0;
    std::uint32_t second_conn_rst_code = 0;
    bool server_done = false;

    const auto serve = [&]() -> asio::awaitable<void> {
        {
            auto socket1 = co_await peer.async_accept(asio::use_awaitable);
            auto conn1 = ruvia::http2_connection::server();
            std::optional<ruvia::http2_request_head_event> req1;
            std::array<char, 16384> input{};
            std::string output;
            bool sent_truncated = false;
            while (!sent_truncated) {
                if (conn1.wants_write()) {
                    ruvia::test::drain_output(conn1, output);
                    co_await asio::async_write(socket1, asio::buffer(output), asio::use_awaitable);
                }
                const auto count = co_await socket1.async_read_some(asio::buffer(input), asio::use_awaitable);
                (void)conn1.feed(std::string_view(input.data(), count));
                while (auto ev = conn1.next_event()) {
                    if (auto* head = ev->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)conn1.submit_connect_response_head(head->stream_id(), accepted);
                        req1.emplace(std::move(*head));
                    } else if (auto* end = ev->tunnel_end()) {
                        (void)conn1.submit_data(
                            end->stream_id(), std::string_view("\x00\x0a"
                                                               "ab",
                                                  4),
                            ruvia::http2_end_stream::end_stream, 65535);
                        sent_truncated = true;
                    }
                }
            }
            if (conn1.wants_write()) {
                ruvia::test::drain_output(conn1, output);
                co_await asio::async_write(socket1, asio::buffer(output), asio::use_awaitable);
            }
            req1.reset();
            std::error_code ec;
            socket1.close(ec);
        }

        {
            auto socket2 = co_await peer.async_accept(asio::use_awaitable);
            auto conn2 = ruvia::http2_connection::server();
            std::optional<ruvia::http2_request_head_event> req2;
            std::array<char, 16384> input{};
            std::string output;
            while (!server_done) {
                if (conn2.wants_write()) {
                    ruvia::test::drain_output(conn2, output);
                    co_await asio::async_write(socket2, asio::buffer(output), asio::use_awaitable);
                }
                std::size_t count = 0;
                try {
                    count = co_await socket2.async_read_some(asio::buffer(input), asio::use_awaitable);
                } catch (const std::system_error&) {
                    break;
                }
                (void)conn2.feed(std::string_view(input.data(), count));
                while (auto ev = conn2.next_event()) {
                    if (auto* head = ev->request_head()) {
                        ruvia::http_response accepted;
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)conn2.submit_connect_response_head(head->stream_id(), accepted);
                        req2.emplace(std::move(*head));
                    } else if (auto* closed = ev->stream_closed()) {
                        second_conn_rst_stream_id = closed->stream_id();
                        second_conn_rst_code = static_cast<std::uint32_t>(closed->error());
                    }
                }
            }
            req2.reset();
        }
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool udp1_threw = false;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        auto opened1 = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened1.tunnel() != nullptr);
        auto udp1 = std::move(*opened1.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp1.finish();
        (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(30));

        auto opened2 = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened2.tunnel() != nullptr);
        auto udp2 = std::move(*opened2.tunnel()).udp({.max_capsule_length_ = 100});
        (void)udp2;

        try {
            (void)co_await udp1.read();
        } catch (const std::runtime_error&) {
            udp1_threw = true;
        }
        for (unsigned attempt = 0; second_conn_rst_stream_id == 0 && attempt != 30; ++attempt) {
            (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_TRUE(udp1_threw);
    EXPECT_EQ(second_conn_rst_stream_id, std::uint32_t{0});
    EXPECT_EQ(second_conn_rst_code, std::uint32_t{0});
}

namespace http2_streaming_test {

struct sleeping_stream_observation final {
    std::atomic<bool> sleep_failed_{};
    std::atomic<bool> sleep_elapsed_{};
    std::atomic<bool> sse_wait_cancelled_{};
    std::atomic<bool> sse_wait_failed_{};
    std::atomic<bool> sse_wait_finished_{};
    std::atomic<int> head_loop_writes_{};
    std::atomic<bool> head_loop_aborted_{};
    std::atomic<bool> head_loop_sleep_rejected_{};
    std::atomic<bool> head_loop_token_stopped_{};
    std::atomic<bool> head_loop_finished_{};
    std::atomic<bool> naive_loop_failed_{};
    std::atomic<bool> naive_loop_finished_{};
    std::atomic<bool> head_trailer_stopped_{};
    std::atomic<bool> head_trailer_sleep_stopped_{};
    std::atomic<bool> head_trailer_aborted_{};
    std::atomic<bool> head_trailer_done_{};
};

const auto sleeping_stream_observed = std::make_shared<sleeping_stream_observation>();
[[maybe_unused]] const bool sleeping_stream_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<sleeping_stream_observation>>([] { return sleeping_stream_observed; });
    return true;
}();

class sleeping_stream_routes final {
    RUVIA_ROUTES_BEGIN(sleeping_stream_routes)
    RUVIA_GET("/http2-sleeping-stream", sleeping);
    RUVIA_POST("/http2-count-upload", count_upload, ruvia::stream_body);
    RUVIA_GET("/http2-quiet-sse", quiet_sse);
    RUVIA_POST("/http2-post-sse", post_sse);
    RUVIA_GET("/http2-paced-sse", paced_sse);
    RUVIA_GET("/http2-head-loop-sse", head_loop_sse);
    RUVIA_GET("/http2-head-naive-sse", head_naive_sse);
    RUVIA_POST("/http2-bounded-buffered", bounded_buffered, ruvia::body_limit<16>);
    RUVIA_POST("/http2-bounded-stream", count_upload, ruvia::stream_body, ruvia::body_limit<16>);
    RUVIA_POST("/http2-bounded-delayed-stream", bounded_delayed_stream, ruvia::stream_body, ruvia::body_limit<16>);
    RUVIA_GET("/http2-head-trailer-write", head_trailer_write);
    RUVIA_GET("/http2-head-trailer-only", head_trailer_only);
    RUVIA_GET("/http2-stream-custom", stream_custom);
    RUVIA_GET("/http2-stream-204", stream_204);
    RUVIA_ROUTES_END

    // HEAD 正常完成，不启动生产循环，也不会取消业务等待 token。
    ruvia::task<void> head_loop_sse(ruvia::context& context_value) {
        if (context_value.req().query("not-modified").has_value()) {
            context_value.draft().status(ruvia::http_status::not_modified);
        }
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        const auto token = context_value.stop_token();
        if (co_await writer.commit() == ruvia::stream_commit_result::body) {
            co_await writer.write({.data_ = "tick"});
            observed.head_loop_writes_ = 1;
        } else {
            observed.head_loop_writes_ = -1;
            try {
                co_await writer.write({.data_ = "late"});
            } catch (const std::logic_error&) {
                observed.head_loop_writes_ = 0;
            }
            try {
                static_cast<void>(co_await writer.sleep(std::chrono::seconds(30)));
            } catch (const std::logic_error&) {
                observed.head_loop_sleep_rejected_ = true;
            }
        }
        observed.head_loop_aborted_ = writer.aborted();
        observed.head_loop_token_stopped_ = token.stop_requested();
        co_await writer.end();
        observed.head_loop_finished_ = true;
    }

    // 提交结果结束无正文响应，不靠轮询取消或等待 peer 断开。
    ruvia::task<void> head_naive_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            observed.naive_loop_finished_ = true;
            co_return;
        }
        try {
            for (int i = 0; i < 1000; ++i) {
                co_await writer.write({.data_ = "tick"});
                static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)));
            }
        } catch (...) {
            observed.naive_loop_failed_ = true;
            observed.naive_loop_finished_ = true;
            throw;
        }
        observed.naive_loop_finished_ = true;
    }

    ruvia::task<void> head_trailer_write(ruvia::context& context_value) {
        auto& out = context_value.stream_text();
        context_value.draft().header("Trailer", "X-Complete");
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("body");
        const ruvia::http_header_view trailers[] = {{"X-Complete", "yes"}};
        co_await out.end(trailers);
    }

    ruvia::task<void> head_trailer_only(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& out = context_value.stream_text();
        const auto token = context_value.stop_token();
        context_value.draft().header("Trailer", "X-Complete");
        const ruvia::http_header_view trailers[] = {{"X-Complete", "yes"}};
        if (co_await out.commit() == ruvia::stream_commit_result::body) {
            co_await out.end(trailers);
        }
        try {
            static_cast<void>(co_await out.sleep(std::chrono::seconds(10)));
        } catch (const std::logic_error&) {
            observed.head_trailer_sleep_stopped_ = true;
        }
        observed.head_trailer_aborted_ = out.aborted();
        observed.head_trailer_stopped_ = token.stop_requested();
        observed.head_trailer_done_ = true;
    }

    ruvia::task<void> stream_custom(ruvia::context& context_value) {
        context_value.draft().header("Content-Encoding", "x-custom");
        auto& out = context_value.stream_text();
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("payload");
        co_await out.end();
    }

    ruvia::task<void> stream_204(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::no_content);
        co_await context_value.stream_text().end();
    }

    ruvia::task<ruvia::http_response> bounded_buffered(ruvia::context& context_value) {
        co_return context_value.text(co_await context_value.req().text());
    }

    ruvia::task<ruvia::http_response> bounded_delayed_stream(ruvia::context& context_value) {
        (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(40));
        if (context_value.req().query("read").has_value()) {
            static_cast<void>(co_await context_value.req().body_reader().read());
        }
        co_return context_value.text("accepted");
    }

    // Streams the request body and answers with the byte count.
    // A buffered POST body is readable before the SSE response starts.
    ruvia::task<void> post_sse(ruvia::context& context_value) {
        const auto received = co_await context_value.req().text();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write({.data_ = received});
    }

    ruvia::task<void> paced_sse(ruvia::context& context_value) {
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (int i = 0; i < 4; ++i) {
            co_await writer.write({.data_ = "tick"});
            (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(35));
        }
    }

    ruvia::task<ruvia::http_response> count_upload(ruvia::context& context_value) {
        std::size_t bytes_value = 0;
        auto& reader_value = context_value.req().body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }
        std::string text;
        text.append(std::to_string(bytes_value));
        co_return context_value.text(std::move(text));
    }

    ruvia::task<void> quiet_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        // The request's own stop: RST_STREAM of this stream requests it.
        const auto stop = context_value.stop_token();
        co_await writer.write({.data_ = "ready"});
        if (context_value.req().query("writer").has_value()) {
            try {
                observed.sleep_elapsed_ =
                    co_await writer.sleep(std::chrono::seconds(30)) == ruvia::timer_sleep_result::elapsed;
            } catch (const std::system_error&) {
                observed.sleep_failed_ = true;
                throw;
            }
            co_return;
        }
        try {
            observed.sse_wait_cancelled_ =
                co_await ruvia::sleep_for(context_value.worker(), std::chrono::seconds(30), stop) ==
                ruvia::timer_sleep_result::stop_requested;
        } catch (...) {
            observed.sse_wait_failed_ = true;
            throw;
        }
        observed.sse_wait_finished_ = true;
    }

    // Writes one chunk, then pauses far longer than the test waits: only a
    // peer disconnect can end the sleep in time.
    ruvia::task<void> sleeping(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("first\n");
        try {
            if (co_await writer.sleep(std::chrono::seconds(30)) == ruvia::timer_sleep_result::elapsed) {
                observed.sleep_elapsed_ = true;
            }
        } catch (const std::system_error&) {
            observed.sleep_failed_ = true;
            throw;
        }
        co_await writer.write("second\n");
    }
};

using ruvia::test::append_frame;

// With expect_continue the HEADERS announce the content but the DATA frame is
// left for the caller to send once the server has answered the expectation.
std::string request_wire(std::string_view method,
    std::string_view path, std::string_view body, bool expect_continue = false) {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    if (!body.empty()) {
        ruvia::hpack_encoder::encode_header(block, "content-length", std::to_string(body.size()));
    }
    if (expect_continue) {
        ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
    }
    append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(body.empty() ? 0x5 : 0x4), 1, block);
    if (!body.empty() && !expect_continue) {
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, body);
    }
    return wire;
}

using ruvia::test::read_frame;

// Reads frames until the first HEADERS frame on stream 1 and returns the
// decoded :status; connection-level frames before it are skipped.
std::string read_first_status(asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
    for (;;) {
        const auto frame = read_frame(client);
        if (frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
            frame.header_.stream_id_ != 1) {
            continue;
        }
        std::string status{};
        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
            if (name == ":status") {
                status = value;
            }
            return true;
        });
        if (decoded.has_value()) {
            throw std::logic_error("invalid HPACK block");
        }
        return status;
    }
}

// Reads frames until the first DATA frame on stream 1 and returns its payload;
// a GOAWAY or RST_STREAM before that ends the read with an empty result.
std::string read_first_data(asio::ip::tcp::socket& client) {
    for (;;) {
        std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
        asio::read(client, asio::buffer(header_bytes));
        const auto header_value = ruvia::parse_http2_frame_header(header_bytes);
        if (!header_value.has_value()) {
            return std::string{};
        }
        std::string payload(header_value->length_, '\0');
        if (!payload.empty()) {
            asio::read(client, asio::buffer(payload));
        }
        if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            header_value->stream_id_ == 1) {
            return payload;
        }
        if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
            header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
            return std::string{};
        }
    }
}

}  // namespace http2_streaming_test

using http2_streaming_test::append_frame;
using http2_streaming_test::read_first_data;
using http2_streaming_test::read_first_status;
using http2_streaming_test::request_wire;
using http2_streaming_test::sleeping_stream_observed;

// A client that disconnects while an HTTP/2 stream producer is inside sleep()
// ends that sleep with the transport error, so the handler, its stream and
// the connection retire instead of staying suspended for the full duration;
// the disconnect is not counted as a connection failure.
TEST(Ruvia, http2_response_stream_sleep_ends_when_the_peer_disconnects) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto failures_before = ruvia::app().stats().connection_failures_;
    for (const bool send_goaway : {false, true}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire("GET", "/http2-sleeping-stream", {});
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_data(client), std::string("first\n"));
        if (send_goaway) {
            std::string goaway{};
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
            asio::write(client, asio::buffer(goaway));
        }
        client.close();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(sleeping_stream_observed->sleep_failed_);
        EXPECT_TRUE(!sleeping_stream_observed->sleep_elapsed_);
        sleeping_stream_observed->sleep_failed_ = false;
    }
    server.finish();
    // The peer going away is a normal end of the connection, not a fault.
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

TEST(Ruvia, http2_post_sse_reads_the_request_body_before_streaming) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire("POST", "/http2-post-sse", "{\"id\":\"1\"}");
    asio::write(client, asio::buffer(wire));
    EXPECT_EQ(read_first_data(client), std::string("data: {\"id\":\"1\"}\n\n"));
}

// A quiet SSE producer waits on context::stop_token(), not on a transport
// write or writer.sleep(). RST_STREAM must wake it without closing its sibling.
TEST(Ruvia, http2_quiet_sse_peer_reset_cancels_wait_and_preserves_connection) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sse_wait_cancelled_ = false;
    sleeping_stream_observed->sse_wait_failed_ = false;
    sleeping_stream_observed->sse_wait_finished_ = false;
    const auto stats_before = ruvia::app().stats();
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire("GET", "/http2-quiet-sse", {});
    asio::write(client, asio::buffer(wire));
    client.non_blocking(true);
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string event{};
    while (event.size() < std::string_view("data: ready\n\n").size()) {
        const auto frame = http2_streaming_test::read_frame(client, ready_deadline);
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
            EXPECT_TRUE(!decoded.has_value());
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            event.append(frame.payload_);
        }
    }
    EXPECT_EQ(event, std::string("data: ready\n\n"));
    EXPECT_TRUE(!sleeping_stream_observed->sse_wait_finished_);

    client.non_blocking(false);
    // Keep a sibling upload open across the reset, rather than merely opening
    // a new stream after cancellation has already finished.
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-count-upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    std::string sibling_request{};
    append_frame(sibling_request, ruvia::http2_frame_type::headers, 0x4, 3, block);
    asio::write(client, asio::buffer(sibling_request));
    std::string reset{};
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sse_wait_finished_ && std::chrono::steady_clock::now() < cancel_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sleeping_stream_observed->sse_wait_finished_);
    EXPECT_TRUE(sleeping_stream_observed->sse_wait_cancelled_);
    EXPECT_TRUE(!sleeping_stream_observed->sse_wait_failed_);

    std::string sibling_body{};
    append_frame(sibling_body, ruvia::http2_frame_type::data, 0x1, 3, "x");
    asio::write(client, asio::buffer(sibling_body));
    client.non_blocking(true);
    const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, response_deadline);
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (frame.header_.stream_id_ == 3 && name == ":status") {
                    status = value;
                }
                return true;
            });
            EXPECT_TRUE(!decoded.has_value());
        }
        if (frame.header_.stream_id_ == 3) {
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(frame.payload_);
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
                frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_EQ(body, std::string("1"));
    client.close();
    server.finish();
    const auto stats_after = ruvia::app().stats();
    EXPECT_EQ(stats_after.connection_failures_, stats_before.connection_failures_);
    EXPECT_EQ(stats_after.worker_failures_, stats_before.worker_failures_);
}

TEST(Ruvia, http2_opt_in_writer_sleep_preserves_reset_error) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto before = ruvia::app().stats().connection_failures_;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("GET", "/http2-quiet-sse?writer=1", {})));
    EXPECT_EQ(read_first_data(client), std::string("data: ready\n\n"));
    std::string reset{};
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sleeping_stream_observed->sleep_failed_);
    EXPECT_TRUE(!sleeping_stream_observed->sleep_elapsed_);
    client.close();
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, before);
}

// A request whose HEADERS and complete DATA arrive in one read is still a
// streaming upload: the handler is dispatched for the head and consumes the
// already-received content rather than the stream being reset.
TEST(Ruvia, http2_streaming_upload_received_in_one_read_is_dispatched) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::size_t body_size : {std::size_t{1}, std::size_t{100}, std::size_t{5000}}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire("POST", "/http2-count-upload", std::string(body_size, 'x'));
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_data(client), std::string(std::to_string(body_size)));
        client.close();
    }
    server.finish();
}

// RFC 9110 §10.1.1 on HTTP/2 follows the HTTP/1 policy: 100 Continue is sent
// when the server starts consuming the content (a streaming handler's first
// body read), and a request whose final status is known from the head alone
// (404) is answered with that status and never asked for its content.
TEST(Ruvia, http2_expect_continue_is_answered_when_the_content_is_consumed) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string body(40, 'x');
        asio::write(client, asio::buffer(request_wire("POST", "/http2-count-upload", body, true)));
        EXPECT_EQ(read_first_status(client, decoder), std::string("100"));
        std::string data{};
        append_frame(data, ruvia::http2_frame_type::data, 0x1, 1, body);
        asio::write(client, asio::buffer(data));
        EXPECT_EQ(read_first_status(client, decoder), std::string("200"));
        EXPECT_EQ(read_first_data(client), std::string("40"));
        client.close();
    }
    {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("POST", "/http2-missing-upload", std::string(40, 'x'), true)));
        EXPECT_EQ(read_first_status(client, decoder), std::string("404"));
        client.close();
    }
    server.finish();
}

// RFC 9110 9.3.2: HEAD on an SSE GET route answers 200 with the GET head and
// ends the stream without content.
TEST(Ruvia, http2_head_falls_back_to_sse_get_route_without_content) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("HEAD", "/http2-paced-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string content_type{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    status = value;
                } else if (name == "content-type") {
                    content_type = value;
                }
                return true;
            });
            ended = (frame.header_.flags_ & 0x1) != 0;
        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            body.append(frame.payload_);
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(content_type.starts_with("text/event-stream"));
    EXPECT_TRUE(body.empty());
    client.close();
    server.finish();
}

// 完成态拒绝非法生产/等待，aborted 与取消 token 只表示真实失败。
TEST(Ruvia, http2_head_and_304_streams_complete_without_abort) {
    for (const bool not_modified : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        ruvia::test::http_server_fixture server(io);
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire(not_modified ? "GET" : "HEAD",
                                not_modified ? "/http2-head-loop-sse?not-modified" : "/http2-head-loop-sse", {})));
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool ended = false;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.stream_id_ == 1) {
                EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }

        auto& observed = *http2_streaming_test::sleeping_stream_observed;
        const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!observed.head_loop_finished_ && std::chrono::steady_clock::now() < finish_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(observed.head_loop_finished_.load());
        EXPECT_EQ(observed.head_loop_writes_.load(), 0);
        EXPECT_TRUE(!observed.head_loop_aborted_.load());
        EXPECT_TRUE(!observed.head_loop_token_stopped_.load());
        EXPECT_TRUE(observed.head_loop_sleep_rejected_.load());
        client.close();
        server.finish();
    }
}

// HEAD 在提交时完成，不要求对端断开才结束生产者。
TEST(Ruvia, http2_head_only_producer_finishes_at_commit) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("HEAD", "/http2-head-naive-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        if (frame.header_.stream_id_ == 1) {
            EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    client.close();

    auto& observed = *http2_streaming_test::sleeping_stream_observed;
    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!observed.naive_loop_finished_ && std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed.naive_loop_finished_.load());
    EXPECT_TRUE(!observed.naive_loop_failed_.load());
    server.finish();
}

// A GET/SSE stream whose request body is already closed (END_STREAM on HEADERS)
// must not be held in connection_scanner::phase::reading_payload and killed by
// request_body_timeout_ or body_completion_timeout_ while streaming responses.
TEST(Ruvia, http2_sse_response_is_not_killed_by_request_body_completion_timeout) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io,
        ruvia::server_config{
            .worker_count_ = 1,
            .idle_timeout_ = std::chrono::milliseconds(2000),
            .connection_scan_interval_ = std::chrono::milliseconds(10),
            .request_header_timeout_ = std::chrono::milliseconds(500),
            .request_body_timeout_ = std::chrono::milliseconds(60),
            .body_completion_timeout_ = std::chrono::milliseconds(80),
        });
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("GET", "/http2-paced-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    status = value;
                }
                return true;
            });
            ended = (frame.header_.flags_ & 0x1) != 0;
        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            body.append(frame.payload_);
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_EQ(body, std::string("data: tick\n\ndata: tick\n\ndata: tick\n\ndata: tick\n\n"));
    client.close();
    server.finish();
}

TEST(Ruvia, http2_oversized_request_body_answers_413_without_100_continue) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const std::string oversized(64, 'x');
    // Read through END_STREAM: a valid head alone cannot prove that the
    // rejection's borrowed diagnostic survived until its response was sent.
    const auto check_body_limit_response = [&](asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::string body{};
        unsigned response_heads = 0;
        bool ended = false;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                (frame.header_.stream_id_ == 1 &&
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream))) {
                throw std::runtime_error("oversized HTTP/2 request reset before its complete 413 response");
            }
            if (frame.header_.stream_id_ != 1) {
                continue;
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                ++response_heads;
                std::string status{};
                const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                    if (name == ":status") {
                        status = value;
                    }
                    return true;
                });
                EXPECT_TRUE(!decoded.has_value());
                EXPECT_EQ(status, std::string("413"));
                ended = (frame.header_.flags_ & 0x1) != 0;
            } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(frame.payload_);
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
        EXPECT_EQ(response_heads, 1U);
        const auto error = ruvia::json_object::parse(body);
        const auto status = error.get<ruvia::uint32>("status").value();
        const auto code = error.get<ruvia::string>("code").value();
        EXPECT_TRUE(status && static_cast<std::uint32_t>(*status) == 413);
        EXPECT_TRUE(code && code->view() == "request_body_too_large");
    };

    // 1. Declared Content-Length exceeding route body_limit<16> with Expect: 100-continue
    //    must answer 413 immediately without sending 100 Continue first.
    for (const std::string_view path : {"/http2-bounded-buffered", "/http2-bounded-stream"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("POST", path, oversized, true)));
        check_body_limit_response(client, decoder);
        client.close();
    }

    // 2. Unannounced (no Content-Length) DATA exceeding route body_limit<16> on both
    //    buffered and stream_body routes must answer 413 rather than RST_STREAM(CANCEL).
    for (const std::string_view path : {"/http2-bounded-buffered", "/http2-bounded-stream"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", path);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, oversized);
        asio::write(client, asio::buffer(wire));
        check_body_limit_response(client, decoder);
        client.close();
    }

    // 3. Post-dispatch oversized DATA while stream_body handler is awaiting another
    //    async operation:
    //    - when the handler returns 200 without reading the body, the server still
    //      answers 413 instead of committing 200;
    //    - when the request carries Expect: 100-continue (without Content-Length) and
    //      the handler performs its first read after oversized DATA arrived, 100 Continue
    //      is skipped and 413 is returned directly.
    for (const bool read_after_wait : {false, true}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(
            block, ":path", read_after_wait ? "/http2-bounded-delayed-stream?read=1" : "/http2-bounded-delayed-stream");
        if (read_after_wait) {
            ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
        }
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, oversized);
        asio::write(client, asio::buffer(wire));
        check_body_limit_response(client, decoder);
        client.close();
    }
    server.finish();
}

TEST(Ruvia, http2_head_stream_with_trailers_strips_trailer_header_and_stops_eof_token) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    auto& observed = *sleeping_stream_observed;
    observed.head_trailer_stopped_ = false;
    observed.head_trailer_sleep_stopped_ = false;
    observed.head_trailer_aborted_ = false;
    observed.head_trailer_done_ = false;

    for (const std::string_view path : {"/http2-head-trailer-write", "/http2-head-trailer-only"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("HEAD", path, {})));
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool ended = false;
        bool saw_trailer_header = false;
        unsigned header_frames = 0;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.stream_id_ == 1) {
                EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
                ++header_frames;
                (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view) {
                    if (name == "trailer" || name == "x-complete") {
                        saw_trailer_header = true;
                    }
                    return true;
                });
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
        EXPECT_EQ(header_frames, 1U);
        EXPECT_TRUE(!saw_trailer_header);
        client.close();
    }

    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!observed.head_trailer_done_ && std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed.head_trailer_done_.load());
    EXPECT_TRUE(!observed.head_trailer_aborted_.load());
    EXPECT_TRUE(!observed.head_trailer_stopped_.load());
    EXPECT_TRUE(observed.head_trailer_sleep_stopped_.load());
    server.finish();
}

TEST(Ruvia, http2_stream_custom_content_encoding_and_bodyless_status_negotiation) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto check_status = [&](std::string_view path, std::string_view accept_encoding,
                                  std::string_view expected_status) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", path);
        ruvia::hpack_encoder::encode_header(block, "accept-encoding", accept_encoding);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_status(client, decoder), std::string(expected_status));
        client.close();
    };
    check_status("/http2-stream-custom", "x-custom, identity;q=0", "200");
    check_status("/http2-stream-custom", "gzip", "406");
    check_status("/http2-stream-204", "*;q=0", "204");
    server.finish();
}

namespace {
ruvia::task<std::string> collect_text(ruvia::http_client_response& response) {
    const auto bytes_value = co_await response.stream().read_all();
    const auto view = bytes_value.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}
struct push_observation final {
    std::atomic<unsigned> promised_{};
    std::atomic<unsigned> refused_{};
    std::atomic<unsigned> streamed_{};
};

const auto push_obs = std::make_shared<push_observation>();
[[maybe_unused]] const bool push_obs_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<push_observation>>([] { return push_obs; });
    return true;
}();

std::shared_ptr<push_observation> prepare_observation() {
    push_obs->promised_ = 0;
    push_obs->refused_ = 0;
    push_obs->streamed_ = 0;
    return push_obs;
}

class push_routes final {
public:
    RUVIA_ROUTES_BEGIN(push_routes)
    RUVIA_GET("/parent", parent);
    RUVIA_GET("/two", parent);
    RUVIA_GET("/large", parent);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/large-asset", large);
    RUVIA_GET("/delayed-parent", parent);
    RUVIA_GET("/delayed-asset", delayed);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> parent(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        const auto count = context_value.req().path() == "/two" ? 2U : 1U;
        const auto path = context_value.req().path() == "/large"            ? "/large-asset"
                          : context_value.req().path() == "/delayed-parent" ? "/delayed-asset"
                                                                            : "/asset";
        const std::array<ruvia::http_header_view, 1> headers{{{"x-promise", "owned-request"}}};
        for (unsigned i = 0; i != count; ++i) {
            if (context_value.push({.scheme_ = "http", .authority_ = context_value.req().authority(), .path_ = path, .headers_ = headers})) {
                ++owner_value.promised_;
            } else {
                ++owner_value.refused_;
            }
        }
        co_return context_value.text("parent");
    }
    ruvia::task<ruvia::http_response> asset(ruvia::context& context_value) {
        if (context_value.req().header("x-promise") != "owned-request") {
            throw std::runtime_error("lost push request metadata");
        }
        context_value.draft().header("x-asset", "metadata");
        co_return context_value.text("pushed-body");
    }
    // Answers well after the client's first, interrupted wait for it.
    ruvia::task<ruvia::http_response> delayed(ruvia::context& context_value) {
        (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(300), context_value.stop_token());
        co_return context_value.text("late-asset");
    }
    ruvia::task<void> large(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        std::string chunk(16384, 'p');
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (unsigned i = 0; i != 80; ++i) {
            co_await context_value.stream().write(chunk);
        }
        ++owner_value.streamed_;
        co_await context_value.stream().end();
    }
};

}  // namespace

TEST(Ruvia, http2_push_routes_and_client_preserve_owners_flow_control_cold_reads_and_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_push> retained;
        std::optional<ruvia::http_client_response> retained_response;
        {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                             .push_ = {.enabled_ = true}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                for (unsigned repeat = 0; repeat != 128; ++repeat) {
                    auto parent_value = co_await client_origin.get("/parent").send();
                    EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                    auto push = client_origin.next_push();
                    EXPECT_TRUE(push.has_value());
                    if (!push) {
                        throw std::runtime_error("missing push");
                    }
                    EXPECT_TRUE(push->request().path_ == "/asset");
                    EXPECT_TRUE(push->request().headers_.front().value() == "owned-request");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        auto overlap = push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    EXPECT_TRUE(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    EXPECT_TRUE(response.status() == ruvia::http_status::ok);
                    EXPECT_TRUE(response.header("x-asset") == "metadata");
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                    EXPECT_TRUE(!client_origin.next_push());
                    if (repeat == 0) {
                        retained.emplace(std::move(moved));
                        retained_response.emplace(std::move(response));
                    }
                    EXPECT_TRUE(retained->request().path_ == "/asset");
                    EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
                }
                auto parent_value = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto pushed = client_origin.next_push();
                EXPECT_TRUE(pushed.has_value());
                if (!pushed) {
                    throw std::runtime_error("missing large push");
                }
                auto response = co_await pushed->response();
                std::size_t bytes_value = 0;
                while (auto chunk = co_await response.stream().text()) {
                    EXPECT_TRUE(chunk->find_first_not_of('p') == std::string_view::npos);
                    bytes_value += chunk->size();
                }
                EXPECT_EQ(bytes_value, std::size_t{80 * 16384});
                EXPECT_EQ(observed->streamed_.load(), 1U);
                auto cancelled_parent = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(cancelled_parent)) == "parent");
                auto cancelled_push = client_origin.next_push();
                EXPECT_TRUE(cancelled_push.has_value());
                cancelled_push.reset();
                auto after_cancel = co_await client_origin.get("/parent").send();
                EXPECT_TRUE((co_await collect_text(after_cancel)) == "parent");
                auto after_push = client_origin.next_push();
                EXPECT_TRUE(after_push.has_value());
                if (after_push) {
                    auto after_response = co_await after_push->response();
                    EXPECT_TRUE((co_await collect_text(after_response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, std::size_t{131});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, std::size_t{0});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        EXPECT_TRUE(retained->request().path_ == "/asset");
        EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
        retained_response.reset();
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, http2_push_queue_overflow_and_disabled_permission_keep_parent_response_usable) {
    for (const bool enabled : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto observed = prepare_observation();
        ruvia::test::http_server_fixture server(io);
        auto run = [&]() -> ruvia::task<void> {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                             .push_ = {.enabled_ = enabled, .max_queued_pushes_ = 1}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                auto parent_value = co_await client_origin.get("/two").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto push = client_origin.next_push();
                EXPECT_TRUE(push.has_value() == enabled);
                if (push) {
                    auto response = co_await push->response();
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(observed->refused_.load(), enabled ? 0U : 2U);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
    }
}

// Waiting for a pushed response is a read: its timeout or an already-stopped
// token ends only that wait, and a later response() still receives it.
TEST(Ruvia, http2_push_response_wait_honours_its_operation) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                         .push_ = {.enabled_ = true}});
        const auto client_origin = client.client();
        std::exception_ptr failure;
        try {
            auto parent_value = co_await client_origin.get("/delayed-parent").send();
            EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
            auto push = client_origin.next_push();
            EXPECT_TRUE(push.has_value());
            if (push) {
                EXPECT_THROW((void)push->response({.timeout_ = std::chrono::milliseconds(0)}), std::invalid_argument);
                const auto failure_of = [](ruvia::task<ruvia::http_client_response> wait) -> ruvia::task<std::optional<std::error_code>> {
                    try {
                        (void)co_await std::move(wait);
                    } catch (const ruvia::http_client_error& error) {
                        co_return error.code();
                    }
                    co_return std::nullopt;
                };
                EXPECT_TRUE((co_await failure_of(push->response({.timeout_ = std::chrono::milliseconds(50)}))) ==
                            std::make_error_code(std::errc::timed_out));
                ruvia::stop_source stop;
                stop.request_stop();
                EXPECT_TRUE((co_await failure_of(push->response({.stop_token_ = stop.token()}))) ==
                            std::make_error_code(std::errc::operation_canceled));
                auto response = co_await push->response();
                EXPECT_TRUE((co_await collect_text(response)) == "late-asset");
            }
            EXPECT_EQ(observed->promised_.load(), 1U);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        server.finish();
        if (failure) {
            std::rethrow_exception(failure);
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

namespace http2_file_flow_control_test {

constexpr std::size_t file_size = 65535;

std::filesystem::path& file_path() {
    static std::filesystem::path path;
    return path;
}

[[nodiscard]] const std::filesystem::path& empty_directory() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-http2-empty-file");
    return directory.path();
}

std::unique_ptr<ruvia::static_root> empty_root;

class file_routes final {
    RUVIA_ROUTES_BEGIN(file_routes)
    RUVIA_GET("/http2-file-flow-control", file);
    RUVIA_GET("/http2-file-mutated", mutated_file);
    RUVIA_GET("/http2-file-ok", ok_text);
    RUVIA_GET("/http2-indexed-empty", indexed_empty);
    RUVIA_GET("/http2-empty-mutated", mutated_empty);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        auto response = co_await context_value.file({.path_ = file_path()});
        // Make the exercised transport path observable, rather than assuming
        // that the file is above the context::file in-memory threshold.
        response.header("x-file-backed", response.file_body().has_value() ? "yes" : "no");
        co_return response;
    }

    ruvia::task<ruvia::http_response> mutated_file(ruvia::context& context_value) {
        {
            std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
            const std::string bytes(file_size, 'f');
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        // The client truncates the file once HEADERS arrive: the file was
        // validated before them, and the multipart boundary DATA is submitted
        // before the file range read fails and resets the stream.
        co_return co_await context_value.file({.path_ = file_path()});
    }

    ruvia::task<ruvia::http_response> mutated_empty(ruvia::context& context_value) {
        const auto path = empty_directory() / "body.bin";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
        }
        auto response = co_await context_value.file({.path_ = path});
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "changed";
        }
        co_return response;
    }

    ruvia::task<ruvia::http_response> indexed_empty(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*empty_root, {.relative_path_ = "body.bin"});
    }

    ruvia::task<ruvia::http_response> ok_text(ruvia::context& context_value) {
        co_return context_value.text("alive");
    }
};

struct test_file final {
    test_file() {
        file_path() = directory_.path() / "body.bin";
        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
        const std::string bytes(file_size, 'f');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write HTTP/2 flow-control test file");
        }
    }

private:
    ruvia::test::scoped_temp_directory directory_{"ruvia-http2-file"};
};

using ruvia::test::append_frame;

std::string request_wire(bool exhaust_connection) {
    std::string wire(ruvia::http2_client_preface);
    // In each case exactly one window reaches zero after the file. Keep the
    // stream window positive when testing the connection window, and vice versa.
    const std::string_view settings = exhaust_connection
                                          ? std::string_view("\0\4\0\1\xff\xfe", 6)   // INITIAL_WINDOW_SIZE = 131070
                                          : std::string_view("\0\4\0\0\xff\xff", 6);  // INITIAL_WINDOW_SIZE = 65535
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
    if (!exhaust_connection) {
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
            std::string_view("\0\0\xff\xff", 4));
    }
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

using ruvia::test::received_frame;

ruvia::task<received_frame> read_frame(asio::ip::tcp::socket& socket) {
    std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
    const auto head = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_read(socket, asio::buffer(header_bytes), std::move(handler));
    });
    if (head.error_code()) {
        throw std::system_error(head.error_code());
    }
    const auto header = ruvia::parse_http2_frame_header(header_bytes);
    if (!header) {
        throw std::logic_error("invalid HTTP/2 flow-control response frame");
    }
    received_frame frame{*header, std::string(header->length_, '\0')};
    if (!frame.payload_.empty()) {
        const auto body = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read(socket, asio::buffer(frame.payload_), std::move(handler));
        });
        if (body.error_code()) {
            throw std::system_error(body.error_code());
        }
    }
    co_return frame;
}

void check_file_end(bool exhaust_connection) {
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool file_backed = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        // Bound missing END_STREAM without a sleeping thread or a race between
        // reading the final file bytes and deciding whether to return credit.
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto wire = request_wire(exhaust_connection);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            bool credited = false;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    const auto acknowledged = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(ack), std::move(handler));
                    });
                    if (acknowledged.error_code()) {
                        throw std::system_error(acknowledged.error_code());
                    }
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 file response aborted");
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        } else if (name == "x-file-backed") {
                            file_backed = value == "yes";
                        }
                        return true;
                    });
                    if (decoded.has_value()) {
                        throw std::runtime_error("invalid HTTP/2 file response headers");
                    }
                    EXPECT_TRUE((frame.header_.flags_ & 0x1) == 0);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    EXPECT_TRUE((frame.header_.flags_ & 0x8) == 0);
                    body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                    if (ended) {
                        EXPECT_TRUE(frame.payload_.empty());
                        EXPECT_TRUE(credited);
                    }
                }
                // The file exhausted one send window. The empty END_STREAM DATA
                // frame waits for that window to open (RFC 9113 section 6.9.1
                // permits sending it at zero window; nghttp2 does not), so one
                // byte of credit releases it.
                if (!ended && !credited && body.size() == file_size) {
                    credited = true;
                    std::string increment{};
                    append_frame(increment, ruvia::http2_frame_type::window_update, 0, exhaust_connection ? 0 : 1,
                        std::string_view("\0\0\0\1", 4));
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(increment), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(file_backed);
    EXPECT_EQ(body, std::string(file_size, 'f'));
    EXPECT_TRUE(ended);
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_file_flow_control_test

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_connection_window) {
    http2_file_flow_control_test::check_file_end(true);
}

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_stream_window) {
    http2_file_flow_control_test::check_file_end(false);
}

TEST(Ruvia, http2_buffered_response_resumes_after_stream_window_update) {
    using namespace http2_file_flow_control_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool credited = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::string wire(ruvia::http2_client_preface);
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view("\0\4\0\0\0\1", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                std::string reply;
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    append_frame(reply, ruvia::http2_frame_type::settings, 0x1, 0, {});
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 buffered response aborted");
                }
                if (frame.header_.stream_id_ == 1) {
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status = value;
                            }
                            return true;
                        });
                        if (decoded.has_value()) {
                            throw std::runtime_error("invalid HTTP/2 buffered response headers");
                        }
                        ended = (frame.header_.flags_ & 0x1) != 0;
                    } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        body.append(frame.payload_);
                        ended = (frame.header_.flags_ & 0x1) != 0;
                        if (!credited && !body.empty()) {
                            EXPECT_EQ(body, std::string_view("a"));
                            EXPECT_FALSE(ended);
                            credited = true;
                            append_frame(reply, ruvia::http2_frame_type::window_update, 0, 1,
                                std::string_view("\0\0\0\4", 4));
                        }
                    }
                }
                if (!reply.empty()) {
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(reply), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_EQ(status, std::string_view("200"));
    EXPECT_EQ(body, std::string_view("alive"));
    EXPECT_TRUE(credited);
    EXPECT_TRUE(ended);
    EXPECT_FALSE(expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// A file body is validated before HEADERS but read only once the peer grants
// send window: bytes the file held before the window opened are never sent.
// Truncating the file between HEADERS and the first WINDOW_UPDATE therefore
// fails the read and resets the stream without a single DATA byte.
TEST(Ruvia, http2_file_body_is_read_only_after_send_window_is_granted) {
    using namespace http2_file_flow_control_test;
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool reset = false;
    std::string status{};
    std::size_t data_bytes = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            std::string wire(ruvia::http2_client_preface);
            // INITIAL_WINDOW_SIZE = 0: the stream has no send window at all.
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            while (!reset) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    co_await send_bytes(ack);
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        }
                        return true;
                    });
                    {
                        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    }
                    std::string update{};
                    append_frame(update, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\0\0\xff\xff", 4));
                    co_await send_bytes(update);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    data_bytes += frame.payload_.size();
                    if ((frame.header_.flags_ & 0x1) != 0) {
                        break;
                    }
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    reset = true;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(reset);
    EXPECT_EQ(data_bytes, std::size_t{0});
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_failed_file_writes_release_output_budget_slots) {
    const http2_file_flow_control_test::test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    std::string final_status{};
    std::string final_body{};
    std::size_t resets_seen = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(15));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::string wire(ruvia::http2_client_preface);
            // A 10-byte initial stream window splits the first multipart
            // boundary chunk (~80 bytes) across window updates, so DATA output
            // is in flight when the file range read fails and resets the stream.
            http2_file_flow_control_test::append_frame(
                wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\x0a", 6));
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            // Five failed file responses exceed the 4 connection-wide DATA
            // output budget slots if any reset leaks its slot.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-mutated");
                ruvia::hpack_encoder::encode_header(block, "range", "bytes=0-3, 10-13");
                std::string req{};
                http2_file_flow_control_test::append_frame(
                    req, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(req);
                bool stream_done = false;
                while (!stream_done) {
                    const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        http2_file_flow_control_test::append_frame(
                            ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        (void)decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
                        std::ofstream output(http2_file_flow_control_test::file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        std::string update{};
                        http2_file_flow_control_test::append_frame(
                            update, ruvia::http2_frame_type::window_update, 0, stream_id,
                            std::string_view("\0\0\x10\0", 4));
                        co_await send_bytes(update);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        ++resets_seen;
                        stream_done = true;
                    }
                }
            }
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            std::string req{};
            http2_file_flow_control_test::append_frame(req, ruvia::http2_frame_type::headers, 0x5, 11, block);
            co_await send_bytes(req);
            bool ended = false;
            while (!ended) {
                const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                if (frame.header_.stream_id_ != 11) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            final_status = value;
                        }
                        return true;
                    });
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    final_body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(resets_seen, std::size_t{5});
    EXPECT_EQ(final_status, std::string("200"));
    EXPECT_EQ(final_body, std::string("alive"));
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_empty_file_validation_precedes_end_stream_headers) {
    using namespace http2_file_flow_control_test;
    const auto& directory = empty_directory();
    const auto path = directory / "body.bin";
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            ruvia::hpack_decoder decoder;
            std::string preface(ruvia::http2_client_preface);
            append_frame(preface, ruvia::http2_frame_type::settings, 0, 0, {});
            co_await send_bytes(preface);
            // Valid empty, changed, removed, changed after framing, then a
            // healthy stream proves failed validation remains stream-local.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                // Changed, removed, and changed after framing: each fails
                // validation before HEADERS and gets an error response.
                const bool invalid = stream_id == 3 || stream_id == 5 || stream_id == 7;
                if (stream_id <= 5) {
                    {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    }
                    empty_root = std::make_unique<ruvia::static_root>(directory,
                        ruvia::static_root_options{
                            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
                        });
                    if (stream_id == 3) {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                        output << "changed";
                    } else if (stream_id == 5) {
                        EXPECT_TRUE(std::filesystem::remove(path));
                    }
                }
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path",
                    stream_id == 9 ? "/http2-file-ok" : stream_id == 7 ? "/http2-empty-mutated"
                                                                       : "/http2-indexed-empty");
                std::string request{};
                append_frame(request, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(request);
                bool ended = false;
                bool reset = false;
                bool saw_head = false;
                std::string status{};
                std::string body{};
                while (!ended && !reset) {
                    const auto frame = co_await read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id) {
                        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                            saw_head = true;
                            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                                if (name == ":status") {
                                    status = value;
                                }
                                return true;
                            });
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                            body.append(frame.payload_);
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                            reset = true;
                        }
                    }
                }
                EXPECT_TRUE(saw_head);
                if (invalid) {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("500"));
                } else {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("200"));
                    EXPECT_EQ(body, stream_id == 9 ? std::string("alive") : std::string{});
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto operation = attachment.loop().start(run());
    io.run();
    std::move(operation).get();
    empty_root.reset();
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace http2_client_upload_test {

enum class response_mode { ordinary,
    continued,
    rejected,
    reset,
    abandoned,
    abandoned_pending,
    buffered,
    streaming,
    disconnected,
    refused,
    refused_always,
    connection_lost,
    connection_lost_upload,
    goaway_upload };

constexpr std::size_t streaming_payload_bytes = 256 * 1024;

using ruvia::test::append_frame;

// A raw peer deliberately keeps the request half open after the final response.
// It advertises one stream and never supplies the optional server-side reset, so
// client retirement cannot be masked by a server request-lease destructor.
class early_final_peer final {
public:
    early_final_peer(asio::io_context& io, const ruvia::worker_ref& worker, response_mode mode)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          done_(worker),
          mode_(mode) {}
    early_final_peer(asio::io_context&, ruvia::worker_ref&&, response_mode) = delete;

    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            done_.notify();
        });
    }

    void stop() {
        stopping_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }

    ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::string received_{};
    bool request_ended_{false};
    bool request_reset_{false};
    unsigned requests_{0};

private:
    asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 client preface");
        }
        if (mode_ == response_mode::connection_lost || mode_ == response_mode::connection_lost_upload ||
            mode_ == response_mode::goaway_upload) {
            std::string wire{};
            const std::array<char, 6> no_streams{0, 3, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {no_streams.data(), no_streams.size()});
            const std::array<char, 8> ping{};
            append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
            co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            for (;;) {
                std::array<char, ruvia::http2_frame_header_bytes> bytes;
                co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
                const auto header = ruvia::parse_http2_frame_header(bytes);
                if (!header) {
                    throw std::runtime_error("invalid client HTTP/2 frame");
                }
                std::string payload(header->length_, '\0');
                if (!payload.empty()) {
                    co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
                }
                const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
                if (type == ruvia::http2_frame_type::headers) {
                    throw std::runtime_error("request submitted despite zero stream limit");
                }
                if (type == ruvia::http2_frame_type::ping && (header->flags_ & 1U) != 0) {
                    break;
                }
            }
            if (mode_ == response_mode::goaway_upload) {
                wire.clear();
                const std::array<char, 8> goaway{};
                append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                std::array<char, 1024> remaining;
                std::error_code error;
                do {
                    co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
                } while (!error);
            }
            socket_.close();
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
            if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
                throw std::runtime_error("missing retried HTTP/2 client preface");
            }
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        if (mode_ == response_mode::disconnected) {
            const std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {zero_window.data(), zero_window.size()});
        }
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        ruvia::hpack_decoder decoder;
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid client HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            wire.clear();
            const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
            if (type == ruvia::http2_frame_type::settings && (header->flags_ & 1U) == 0) {
                append_frame(wire, type, 1, 0, {});
            } else if (type == ruvia::http2_frame_type::headers) {
                std::string method{};
                std::string path{};
                const auto decoded = decoder.decode(payload, [&](std::string_view name, std::string_view value) {
                    if (name == ":method") {
                        method = value;
                    }
                    if (name == ":path") {
                        path = value;
                    }
                    return true;
                });
                if (decoded.has_value() || (header->flags_ & 4U) == 0) {
                    throw std::runtime_error("invalid client request fields");
                }
                ++requests_;
                if ((requests_ == 1 && mode_ == response_mode::refused) || mode_ == response_mode::refused_always) {
                    // RFC 9113 section 8.7: the request was not processed.
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected refused request");
                    }
                    first_stream = header->stream_id_;
                    request_ended_ = (header->flags_ & 1U) != 0;
                    const std::array<char, 4> refused_stream{0, 0, 0, 7};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream,
                        {refused_stream.data(), refused_stream.size()});
                    co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                    continue;
                }
                if (requests_ == 1 && mode_ == response_mode::connection_lost) {
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected retried request");
                    }
                    first_stream = header->stream_id_;
                } else if (requests_ == 1) {
                    if (method != "POST" || path != "/upload") {
                        throw std::runtime_error("unexpected initial request");
                    }
                    first_stream = header->stream_id_;
                    if (mode_ == response_mode::continued) {
                        std::string interim;
                        ruvia::hpack_encoder::encode_header(interim, ":status", "100");
                        append_frame(wire, type, 4, first_stream, interim);
                    }
                } else if (requests_ == 2) {
                    if (method != "GET" || path != "/next" || header->stream_id_ != first_stream + 2 ||
                        (!request_ended_ && !request_reset_ && mode_ != response_mode::reset)) {
                        throw std::runtime_error("previous request half was not retired on the reused connection");
                    }
                } else {
                    throw std::runtime_error("unexpected extra request");
                }
                const bool rejected = requests_ == 1 && mode_ == response_mode::rejected;
                const std::string_view body = requests_ == 2 || mode_ == response_mode::connection_lost ? "next" : rejected ? "no"
                                                                                                                            : "ok";
                std::string fields;
                const bool empty_final = requests_ == 1 &&
                                         (mode_ == response_mode::buffered || mode_ == response_mode::streaming);
                ruvia::hpack_encoder::encode_header(fields, ":status", empty_final ? "204" : rejected ? "413"
                                                                                                      : "200");
                if (!empty_final) {
                    ruvia::hpack_encoder::encode_header(fields, "content-length", std::to_string(body.size()));
                }
                append_frame(wire, type, empty_final ? 5 : 4, header->stream_id_, fields);
                if (!empty_final && (requests_ != 1 || mode_ != response_mode::abandoned_pending)) {
                    append_frame(wire, ruvia::http2_frame_type::data, 1, header->stream_id_, body);
                }
                if (requests_ == 1 && mode_ == response_mode::reset) {
                    const std::array<char, 4> no_error{};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream, {no_error.data(), no_error.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::data) {
                received_.append(payload);
                request_ended_ = (header->flags_ & 1U) != 0;
                if (mode_ == response_mode::buffered && received_.size() == 65535 && !window_returned_) {
                    window_returned_ = true;
                    const std::string_view increment("\0\0\0\1", 4);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream, increment);
                } else if (mode_ == response_mode::streaming && !payload.empty()) {
                    const auto increment_value = static_cast<std::uint32_t>(payload.size());
                    const std::array<char, 4> increment{
                        static_cast<char>((increment_value >> 24U) & 0x7fU),
                        static_cast<char>((increment_value >> 16U) & 0xffU),
                        static_cast<char>((increment_value >> 8U) & 0xffU),
                        static_cast<char>(increment_value & 0xffU)};
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
                        {increment.data(), increment.size()});
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream,
                        {increment.data(), increment.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::rst_stream) {
                request_reset_ = true;
            }
            if (!wire.empty()) {
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            }
            if (requests_ == 2 || (requests_ == 1 && (mode_ == response_mode::disconnected ||
                                                         mode_ == response_mode::connection_lost ||
                                                         ((mode_ == response_mode::connection_lost_upload ||
                                                              mode_ == response_mode::goaway_upload) &&
                                                             request_ended_)))) {
                co_return;
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    ruvia::worker_signal done_;
    response_mode mode_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool finished_{false};
    bool window_returned_{false};
};

class draining_peer final {
public:
    draining_peer(asio::io_context& io, const ruvia::worker_ref& worker)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          start_drain_(io),
          ready_(worker),
          done_(worker) {
        start_drain_.expires_at(asio::steady_timer::time_point::max());
    }
    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            ready_.notify();
            done_.notify();
        });
    }
    void drain() {
        start_drain_.expires_at(asio::steady_timer::time_point::min());
    }
    void stop() {
        stopping_ = true;
        std::error_code ignored;
        start_drain_.cancel();
        socket_.close(ignored);
        acceptor_.close(ignored);
    }
    [[nodiscard]] ruvia::task<void> wait_ready() {
        while (!draining_ && !finished_) {
            co_await ready_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 preface");
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                first_stream = header->stream_id_;
                break;
            }
        }
        wire.clear();
        const std::array<char, 1> ok{static_cast<char>(0x88)};
        append_frame(wire, ruvia::http2_frame_type::headers, 4, first_stream, {ok.data(), ok.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::error_code error;
        co_await start_drain_.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (stopping_) {
            co_return;
        }
        wire.clear();
        const std::array<char, 8> goaway{0, 0, 0, static_cast<char>(first_stream), 0, 0, 0, 0};
        const std::array<char, 8> ping{};
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                throw std::runtime_error("second request was submitted despite stream limit");
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::ping &&
                (header->flags_ & 1U) != 0) {
                draining_ = true;
                ready_.notify();
                break;
            }
        }
        std::array<char, 1024> remaining;
        do {
            co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
        } while (!error);
    }
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer start_drain_;
    ruvia::worker_signal ready_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool draining_{false};
    bool stopping_{false};
    bool finished_{false};
};

void exercise_abandon_during_drain() {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    draining_peer peer(io, worker);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            auto held = co_await client.client().get("/hold").send();
            EXPECT_TRUE(held.status() == ruvia::http_status::ok);
            {
                auto exchange = co_await client.client().post("/abandon").upload({.content_length_ = 7});
                // The peer's single stream and max_open_ leave the upload
                // buffered for admission, its head never submitted.
                for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 1; ++i) {
                    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
                }
                EXPECT_EQ(client.client().stats().buffered_requests_, 1U);
                EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
                peer.drain();
                co_await peer.wait_ready();
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(50));
            }
            for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 0; ++i) {
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            }
            EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
            EXPECT_EQ(client.client().stats().buffered_requests_, 0U);
            EXPECT_TRUE(!held.stream().complete());
        } catch (...) {
            failure = std::current_exception();
        }
        peer.stop();
        try {
            co_await client.shutdown();
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void exercise(response_mode mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    // The peer's signal borrows this handle through completion and join.
    const auto worker = attachment.loop().worker();
    early_final_peer peer(io, worker, mode);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(2),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            if (mode == response_mode::refused_always) {
                bool rejected = false;
                try {
                    (void)co_await client.client().get("/next").send();
                } catch (const ruvia::http_client_error& error) {
                    rejected = true;
                    EXPECT_EQ(error.code(), ruvia::http_client_errc::protocol_error);
                    EXPECT_TRUE(error.request_written());
                    EXPECT_NE(error.connection_info(), nullptr);
                }
                EXPECT_TRUE(rejected);
                EXPECT_EQ(peer.requests_, 2U);
            } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                // These cases start with the request below.
            } else if (mode == response_mode::connection_lost_upload || mode == response_mode::goaway_upload) {
                auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                co_await exchange.body().write("payload");
                co_await exchange.body().end();
                auto response = co_await exchange.response();
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "ok");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, 1U);
                EXPECT_EQ(peer.received_, "payload");
                EXPECT_TRUE(peer.request_ended_);
            } else if (mode == response_mode::abandoned_pending) {
                std::optional<ruvia::http_client_response> retained;
                {
                    auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                    retained.emplace(co_await exchange.response());
                    EXPECT_TRUE(retained->status() == ruvia::http_status::ok);
                    EXPECT_TRUE(!retained->stream().complete());
                }
                bool cancelled = false;
                try {
                    (void)co_await retained->stream().read_all();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(cancelled);
                bool completion_cancelled = false;
                try {
                    (void)retained->stream().complete();
                } catch (const ruvia::http_client_error& error) {
                    completion_cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(completion_cancelled);
            } else if (mode == response_mode::buffered) {
                const std::string payload(65536, 'b');
                auto response = co_await client.client().post("/upload").body(payload).send();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
            } else if (mode == response_mode::streaming) {
                const std::string payload(streaming_payload_bytes, 's');
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = payload.size(), .max_chunk_bytes_ = payload.size()});
                auto response = co_await exchange.response();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
                co_await exchange.body().write(payload);
                co_await exchange.body().end();
                EXPECT_TRUE(exchange.body().complete());
            } else {
                const bool expect = mode == response_mode::continued || mode == response_mode::rejected;
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = 7, .expectation_ = expect ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none});
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), mode == response_mode::rejected ? 413 : 200);
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
                    mode == response_mode::rejected ? "no" : "ok");
                EXPECT_TRUE(response.stream().complete());
                if (mode == response_mode::disconnected) {
                    // Start a write with no stream credit, then close the peer.
                    // The write must report its I/O error without replacing a
                    // response EOF that the application has already observed.
                    ruvia::worker_signal started(worker);
                    auto waiting = started.wait();
                    std::optional<std::error_code> write_error;
                    ruvia::task_scope writers(worker);
                    const auto write = [&]() -> ruvia::task<void> {
                        started.notify();
                        try {
                            co_await exchange.body().write("payload");
                        } catch (const ruvia::http_client_error& error) {
                            write_error = error.code();
                        }
                    };
                    writers.spawn(write());
                    co_await std::move(waiting);
                    peer.stop();
                    co_await writers.join();
                    EXPECT_TRUE(write_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
                    co_await client.shutdown();
                    EXPECT_TRUE(!(co_await response.stream().text()).has_value());
                    EXPECT_TRUE(response.stream().complete());
                } else if (mode == response_mode::ordinary || mode == response_mode::continued) {
                    co_await exchange.body().write("payload");
                    co_await exchange.body().end();
                    EXPECT_TRUE(exchange.body().complete());
                } else if (mode != response_mode::abandoned) {
                    bool cancelled = false;
                    try {
                        co_await exchange.body().write("payload");
                    } catch (const ruvia::http_client_error& error) {
                        cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                    }
                    EXPECT_TRUE(cancelled);
                }
            }
            if (mode != response_mode::disconnected && mode != response_mode::connection_lost_upload &&
                mode != response_mode::goaway_upload && mode != response_mode::refused_always) {
                auto next = co_await client.client().get("/next").send();
                const auto bytes = co_await next.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "next");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, mode == response_mode::connection_lost ? 1U : 2U);
                if (mode == response_mode::ordinary || mode == response_mode::continued ||
                    mode == response_mode::buffered || mode == response_mode::streaming) {
                    const auto expected = mode == response_mode::buffered
                                              ? std::string(65536, 'b')
                                          : mode == response_mode::streaming
                                              ? std::string(streaming_payload_bytes, 's')
                                              : std::string("payload");
                    EXPECT_EQ(peer.received_, expected);
                    EXPECT_TRUE(peer.request_ended_);
                } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(!peer.request_reset_);
                } else {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(mode == response_mode::reset || peer.request_reset_);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.stop();
        try {
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_client_upload_test

TEST(Ruvia, http2_client_early_final_keeps_ordinary_upload_open) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::ordinary);
}
TEST(Ruvia, http2_client_early_final_after_continue_keeps_upload_open) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::continued);
}
TEST(Ruvia, http2_client_early_final_before_continue_retires_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::rejected);
}
TEST(Ruvia, http2_client_early_final_no_error_reset_preserves_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::reset);
}
TEST(Ruvia, http2_client_early_final_abandoned_upload_retires_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned);
}
TEST(Ruvia, http2_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned_pending);
}

TEST(Ruvia, http2_client_early_final_drains_window_blocked_buffered_upload_before_reuse) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::buffered);
}

TEST(Ruvia, http2_client_streaming_upload_drains_one_large_chunk_across_flow_control_windows) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::streaming);
}

TEST(Ruvia, http2_client_upload_transport_failure_preserves_completed_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::disconnected);
}

TEST(Ruvia, http2_client_retries_request_refused_with_refused_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused);
}

TEST(Ruvia, http2_client_retries_unsubmitted_request_after_connection_loss) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_connection_loss) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost_upload);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_goaway) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::goaway_upload);
}

TEST(Ruvia, http2_abandoned_unsubmitted_upload_retires_while_older_stream_drains) {
    http2_client_upload_test::exercise_abandon_during_drain();
}

TEST(Ruvia, http2_client_refused_request_preserves_possible_delivery) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused_always);
}

#endif

namespace client_operation_interruption_test {

void exercise(const std::string& url, ruvia::http_client_versions versions) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client owner(attachment.loop(), {.base_url_ = url, .versions_ = versions});
        try {
            co_await ruvia::test::client_operations::exercise(owner.client(), worker);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await owner.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace client_operation_interruption_test

TEST(Ruvia, http_client_operations_end_one_wait_or_abort_their_stream_over_http1_and_http2) {
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const std::string url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
    std::vector<ruvia::http_client_versions> protocols({ruvia::http_client_versions{.http2_ = false}});
#ifdef RUVIA_ENABLE_HTTP2
    protocols.push_back(ruvia::http_client_versions{.http1_ = false});
#endif
    for (const auto versions : protocols) {
        SCOPED_TRACE(versions.http1_ ? "HTTP/1.1" : "HTTP/2");
        client_operation_interruption_test::exercise(url, versions);
        server.finish();
    }
}

namespace client_handle_and_response_test {

template <typename callback_type>
[[nodiscard]] bool throws_logic_error(callback_type&& callback) {
    try {
        callback();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

// An independent legacy server, with an absolute deadline covering accept,
// TLS and HTTP. It never uses the production TLS policy under test.
class legacy_http_peer final {
public:
    explicit legacy_http_peer(asio::ssl::context& context)
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          deadline_(io_),
          context_(context),
          port_(acceptor_.local_endpoint().port()),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~legacy_http_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            deadline_.cancel();
        });
        finish();
    }

    void finish() {
        if (done_.valid()) {
            try {
                done_.get();
            } catch (const std::system_error&) {
                // Handshake rejection or connection close is expected in
                // negative cases; assertions below inspect all received data.
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::string url() const {
        return "https://127.0.0.1:" + std::to_string(port_);
    }

    std::string request_;
    int tls_version_{};
    bool timed_out_{};

private:
    asio::awaitable<void> serve() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                timed_out_ = true;
                std::error_code ignored;
                acceptor_.close(ignored);
                socket_.close(ignored);
            }
        });
        struct cancel_deadline final {
            asio::steady_timer& timer_;
            ~cancel_deadline() {
                timer_.cancel();
            }
        } guard{deadline_};
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, context_);
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        tls_version_ = SSL_version(stream.native_handle());
        std::error_code error;
        co_await asio::async_read_until(stream, asio::dynamic_buffer(request_),
            "\r\n\r\n", asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nlegacy";
        co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, error));
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer deadline_;
    asio::ssl::context& context_;
    std::uint16_t port_;
    std::future<void> done_;
    std::thread thread_;
};

void exercise_legacy_http_peer(int version, ruvia::http_client_versions protocol,
    bool select_h2, bool matching_san = true, bool elliptic_curve = false) {
    ruvia::test::tls_identity identity("server.example",
        matching_san ? "DNS:server.example" : "DNS:other.example", elliptic_curve);
    auto* context = identity.context_.native_handle();
    ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
    ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
    ASSERT_EQ(SSL_CTX_set_cipher_list(context,
                  "ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA"),
        1);
    // Only the independent fixture allows legacy handshake signatures this
    // way. Trust and SAN verification remain enabled in the real client.
    SSL_CTX_set_security_level(context, 0);
    if (select_h2) {
        SSL_CTX_set_alpn_select_cb(context, [](SSL*, const unsigned char** selected, unsigned char* size, const unsigned char*, unsigned int, void*) -> int {
                static constexpr unsigned char h2[] = {'h', '2'};
                *selected = h2;
                *size = sizeof(h2);
                return SSL_TLSEXT_ERR_OK; }, nullptr);
    }
    legacy_http_peer peer(identity.context_);
    const auto url = peer.url();
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::http_client client(pool.loop(0),
        {.base_url_ = url,
            .connect_timeout_ = std::chrono::seconds(2),
            .request_timeout_ = std::chrono::seconds(2),
            .versions_ = protocol,
            .tls_ = {.ca_file_ = identity.ca_file_, .server_name_ = "server.example"}});
    const bool expect_success = version == TLS1_1_VERSION && !select_h2 && matching_san &&
                                protocol != ruvia::http_client_versions{.http1_ = false};
    std::optional<std::error_code> failure_code;
    std::exception_ptr unexpected;
    bool exchanged = false;
    const auto run = [&]() -> ruvia::task<void> {
        try {
            auto response = co_await client.client().get("/legacy").send();
            EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_TRUE(response.status() == ruvia::http_status::ok);
            const auto body = co_await response.stream().read_all();
            EXPECT_EQ(body.text(), std::string_view("legacy"));
            exchanged = true;
        } catch (const ruvia::http_client_error& error) {
            failure_code = error.code();
        } catch (...) {
            unexpected = std::current_exception();
        }
        co_await client.shutdown();
    };
    pool.start();
    pool.loop(0).start(run()).get();
    pool.join();
    peer.finish();
    if (unexpected) {
        std::rethrow_exception(unexpected);
    }
    EXPECT_FALSE(peer.timed_out_);
    EXPECT_EQ(exchanged, expect_success);
    if (expect_success) {
        EXPECT_FALSE(failure_code.has_value());
        EXPECT_EQ(peer.tls_version_, TLS1_1_VERSION);
        EXPECT_TRUE(peer.request_.starts_with("GET /legacy HTTP/1.1\r\n"));
    } else {
        EXPECT_EQ(failure_code, ruvia::make_error_code(ruvia::http_client_errc::tls_failed));
        EXPECT_TRUE(peer.request_.empty());
        if (version == TLS1_1_VERSION && select_h2 &&
            protocol == ruvia::http_client_versions{} && matching_san) {
            // The intentionally noncompliant peer really selected h2 in a
            // completed TLS 1.1 handshake, rather than failing earlier.
            EXPECT_EQ(peer.tls_version_, TLS1_1_VERSION);
        }
    }
}

}  // namespace client_handle_and_response_test

TEST(Ruvia, http_client_tls11_http1_and_negotiation_exchange_with_verified_san) {
    using namespace client_handle_and_response_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    for (const auto protocol : {ruvia::http_client_versions{.http2_ = false},
             ruvia::http_client_versions{}}) {
        for (const bool elliptic_curve : {false, true}) {
            exercise_legacy_http_peer(TLS1_1_VERSION, protocol, false, true, elliptic_curve);
        }
        exercise_legacy_http_peer(TLS1_1_VERSION, protocol, false, false);
    }
}

TEST(Ruvia, http_client_rejects_tls10_and_h2_below_tls12_without_http2_bytes) {
    using namespace client_handle_and_response_test;
    for (const auto protocol : {ruvia::http_client_versions{.http2_ = false},
             ruvia::http_client_versions{}}) {
        exercise_legacy_http_peer(TLS1_VERSION, protocol, false);
    }
#ifdef RUVIA_ENABLE_HTTP2
    exercise_legacy_http_peer(TLS1_1_VERSION, ruvia::http_client_versions{}, true);
    exercise_legacy_http_peer(TLS1_1_VERSION, ruvia::http_client_versions{.http1_ = false}, true);
#endif
}

TEST(Ruvia, http_client_ref_origin_and_expired_send_reject_after_shutdown) {
    using namespace client_handle_and_response_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = "http://127.0.0.1:8089"});
        auto handle = client.client();
        try {
            EXPECT_TRUE(handle.scheme() == ruvia::http_scheme::http);
            EXPECT_EQ(handle.host(), std::string_view("127.0.0.1"));
            EXPECT_EQ(handle.port(), std::uint16_t{8089});
            const auto origin = handle.origin();
            EXPECT_TRUE(origin.scheme() == ruvia::http_scheme::http);
            EXPECT_EQ(origin.host(), std::string_view("127.0.0.1"));
            EXPECT_EQ(origin.port(), std::uint16_t{8089});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (!failure) {
            try {
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.get("/").send(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.get("/").upload({}); }));
                EXPECT_TRUE(
                    throws_logic_error([&] { (void)handle.connect("127.0.0.1:8089").open(); }));
                EXPECT_TRUE(
                    throws_logic_error([&] { (void)handle.udp("/udp").open(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.with_options({}); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.stats(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.next_push(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.next_advertisement(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.host(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.port(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.scheme(); }));
                EXPECT_TRUE(throws_logic_error([&] { (void)handle.origin(); }));
            } catch (...) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_response_and_body_moved_from_throw_logic_error) {
    using namespace client_handle_and_response_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Test: ok\r\nConnection: close\r\n\r\nhi";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) { peer_failure = error; });

    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = client_url,
                .request_timeout_ = std::chrono::seconds(5)});
        try {
            auto original = co_await client.client().get("/moved").send();
            auto active = std::move(original);

            EXPECT_TRUE(original.stream().complete());
            EXPECT_TRUE(throws_logic_error([&] { (void)original.status(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.received_status(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.protocol_version(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.headers(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.trailers(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.header("x-test"); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.trailer("x-test"); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.informational_responses(); }));
            EXPECT_TRUE(throws_logic_error([&] { original.reprioritize({}); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.stream().read(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.stream().text(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)original.stream().read_all(); }));

            EXPECT_TRUE(active.status() == ruvia::http_status::ok);
            EXPECT_EQ(active.header("x-test"), std::optional<std::string_view>("ok"));
            auto body = co_await active.stream().read_all();
            EXPECT_EQ(body.text(), std::string_view("hi"));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

RUVIA_MODEL(fluent_client_observation,
    RUVIA_REQUIRED_FIELD(method, ruvia::string),
    RUVIA_REQUIRED_FIELD(body, ruvia::string),
    RUVIA_REQUIRED_FIELD(query, ruvia::string),
    RUVIA_REQUIRED_FIELD(single_header, ruvia::string),
    RUVIA_REQUIRED_FIELD(span_header, ruvia::string),
    RUVIA_REQUIRED_FIELD(authorization, ruvia::string),
    RUVIA_REQUIRED_FIELD(proxy_authorization, ruvia::string),
    RUVIA_REQUIRED_FIELD(cookie, ruvia::string),
    RUVIA_REQUIRED_FIELD(content_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(authority, ruvia::string));

namespace fluent_client_tests {

std::atomic<std::size_t> host_cancel_waiters{};
std::atomic<std::size_t> host_cancelled_requests{};

struct redirect_destinations final {
    std::string source_{};
    std::string destination_{};
    std::string secure_{};
    std::string unregistered_{};
};

class fluent_routes final {
public:
    RUVIA_ROUTES_BEGIN(fluent_routes)
    RUVIA_GET("/fluent/inspect", inspect);
    RUVIA_POST("/fluent/inspect", inspect);
    RUVIA_PUT("/fluent/inspect", inspect);
    RUVIA_PATCH("/fluent/inspect", inspect);
    RUVIA_DELETE("/fluent/inspect", inspect);
    RUVIA_HEAD("/fluent/inspect", inspect);
    RUVIA_OPTIONS("/fluent/inspect", inspect);
    RUVIA_GET("/fluent/redirect", redirect);
    RUVIA_POST("/fluent/redirect", redirect);
    RUVIA_PUT("/fluent/redirect", redirect);
    RUVIA_HEAD("/fluent/redirect", redirect);
    RUVIA_GET("/fluent/registered", registered_redirect);
    RUVIA_GET("/fluent/stream-source", stream_source);
    RUVIA_GET("/fluent/host-cancel", host_cancel);
    RUVIA_GET("/fluent/hold", hold);
    RUVIA_GET("/fluent/pipe", pipe);
    RUVIA_GET("/fluent/pipe-bounded", pipe_bounded);
    RUVIA_GET_WS("/fluent/websocket", websocket_echo,
        (ruvia::websocket_route_config{.subprotocols_ = {"echo.v1"}, .require_subprotocol_ = true, .deflate_ = {.enabled_ = true}}));
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> inspect(ruvia::context& c) {
        const auto body = co_await c.req().bytes();
        fluent_client_observation observed;
        observed.set<"method">(c.req().method())
            .set<"body">(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()))
            .set<"query">(c.req().query("escaped name").value_or(""))
            .set<"single_header">(c.req().header("x-owned").value_or(""))
            .set<"span_header">(c.req().header("x-span").value_or(""))
            .set<"authorization">(c.req().header("authorization").value_or(""))
            .set<"proxy_authorization">(c.req().header("proxy-authorization").value_or(""))
            .set<"cookie">(c.req().header("cookie").value_or(""))
            .set<"content_type">(c.req().header("content-type").value_or(""))
            .set<"authority">(c.req().authority().empty() ? c.req().header("host").value_or("") : c.req().authority());
        c.draft().header("x-observed-method", c.req().method());
        c.draft().header("x-observed-user-agent", c.req().header("user-agent").value_or(""));
        if (const auto cookie = c.req().query("set-cookie")) {
            c.draft().header("set-cookie", *cookie);
        }
        co_return c.json(observed);
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        (void)co_await c.req().bytes();
        c.draft().status(ruvia::http_status_code::from_value(c.req().query<std::uint16_t>("status").value_or(302)));
        c.draft().header("location", c.req().query("location").value_or("/fluent/inspect"));
        co_return c.text("redirect");
    }

    ruvia::task<ruvia::http_response> hold(ruvia::context& c) {
        ++host_cancel_waiters;
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(5), c.stop_token());
        co_return c.text("released");
    }

    // Streaming dispatch observes downstream disconnects while the upstream waits.
    ruvia::task<void> host_cancel(ruvia::context& c) {
        ruvia::stop_source independent;
        try {
            auto response = co_await c.client("source")
                                .with_options({.timeout_ = std::chrono::milliseconds(500), .stop_token_ = independent.token()})
                                .get("/fluent/hold")
                                .send();
            (void)co_await response.bytes();
        } catch (const ruvia::http_client_error& error) {
            if (error.code() == std::make_error_code(std::errc::operation_canceled) && !independent.stop_requested()) {
                ++host_cancelled_requests;
            }
        }
        if (c.stop_token().stop_requested()) {
            co_return;
        }
        auto& output = c.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::body) {
            co_await output.write("finished");
            co_await output.end();
        }
    }

    ruvia::task<ruvia::http_response> registered_redirect(ruvia::context& c) {
        const auto& targets = *c.worker_state<std::shared_ptr<redirect_destinations>>();
        const auto mode = c.req().query("mode").value_or("cross");
        const auto& target = mode == "unregistered" ? targets.unregistered_ : targets.destination_;
        auto origin = c.client(mode == "downgrade" ? "secure" : "source");
        auto response = co_await origin.post("/fluent/redirect")
                            .query("location", std::string(target) + "/fluent/inspect")
                            .header("authorization", "Bearer source-secret")
                            .header("proxy-authorization", "Basic proxy-secret")
                            .header("cookie", "explicit=source-secret")
                            .header("content-type", "text/plain")
                            .body("source-body")
                            .send();
        c.draft().status(response.status());
        auto body = co_await response.bytes();
        co_return c.body(body.bytes());
    }

    ruvia::task<void> stream_source(ruvia::context& c) {
        c.draft().status(ruvia::http_status::created);
        c.draft().header("x-stream-source", "retained");
        c.draft().header("Content-Type", "application/octet-stream");
        c.draft().header("Date", "Thu, 01 Jan 1970 00:00:00 GMT");
        c.draft().header("Link", "</first>; rel=preload");
        c.draft().header("Link", "</second>; rel=preload", {.mode_ = ruvia::http_response_header_mode::append});
        auto& output = c.stream();
        if (co_await output.commit() == ruvia::stream_commit_result::body) {
            co_await output.write("first");
            co_await output.write("second");
            const std::array trailers{ruvia::http_header_view{"x-finished", "yes"}};
            co_await output.end(trailers);
        }
    }

    ruvia::task<void> pipe(ruvia::context& c) {
        auto response = co_await c.client("source").get("/fluent/stream-source").send();
        auto forwarding = response.pipe_to(c);
        auto moved = std::move(response);
        co_await std::move(forwarding);
        if (!moved.stream().complete()) {
            throw std::runtime_error("response pipe did not consume the moved source");
        }
    }

    // An already-stopped token forwards nothing, so the bounded forward that
    // follows still commits the whole response.
    ruvia::task<void> pipe_bounded(ruvia::context& c) {
        auto response = co_await c.client("source").get("/fluent/stream-source").send();
        ruvia::stop_source stop;
        stop.request_stop();
        bool cancelled = false;
        try {
            co_await response.pipe_to(c, {.stop_token_ = stop.token()});
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        if (!cancelled || response.stream().complete()) {
            throw std::runtime_error("stopped response pipe did not fail before forwarding");
        }
        bool rejected = false;
        try {
            (void)response.pipe_to(c, {.timeout_ = std::chrono::milliseconds(0)});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("response pipe accepted a non-positive timeout");
        }
        co_await response.pipe_to(c, {.timeout_ = std::chrono::seconds(5)});
    }

    ruvia::task<void> websocket_echo(ruvia::context& c) {
        if (c.req().header("x-owned") != "pooled") {
            throw std::runtime_error("WebSocket builder did not retain its fields");
        }
        if (c.req().query("oversize")) {
            const std::string payload(64, 'm');
            co_await c.websocket().text(payload);
        }
        while (auto message = co_await c.websocket().read()) {
            co_await c.websocket().send(std::move(*message));
        }
    }
};

class server final {
public:
    server()
        : source_port_(io_),
          destination_port_(io_),
          secure_port_(io_),
          unknown_port_(io_),
          identity_("localhost"),
          targets_(std::make_shared<redirect_destinations>()) {
        targets_->source_ = "http://127.0.0.1:" + std::string(std::to_string(source_port_.endpoint().port()));
        targets_->destination_ = "http://127.0.0.1:" + std::string(std::to_string(destination_port_.endpoint().port()));
        targets_->secure_ = "https://localhost:" + std::string(std::to_string(secure_port_.endpoint().port()));
        targets_->unregistered_ = "http://127.0.0.1:" + std::string(std::to_string(unknown_port_.endpoint().port()));
        const auto& key = identity_.private_key_file();
        const std::array listeners{
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = source_port_.endpoint().port(), .https_ = secure_port_.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}}},
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = destination_port_.endpoint().port()}};
        auto& app = ruvia::app();
        app.server({.worker_count_ = 1})
            .listen(std::span<const ruvia::listen_config>(listeners))
            .client({.alias_ = "source", .base_url_ = targets_->source_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}, .cookies_ = {{"configured", "source-secret"}}})
            .client({.alias_ = "destination", .base_url_ = targets_->destination_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}})
            .client({.alias_ = "secure", .base_url_ = targets_->secure_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}});
        app.use_worker_state<std::shared_ptr<redirect_destinations>>([targets = targets_] { return targets; });
        app.on_start([this] {
            ready_.set_value();
        });
        auto ready = ready_.get_future();
        run_.emplace(app);
        ruvia::test::wait_until([&] {
            if (const auto failure = run_->failure()) {
                std::rethrow_exception(failure);
            }
            return ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        },
            std::chrono::seconds(10));
        ready.get();
    }

    ~server() = default;

    const redirect_destinations& targets() const noexcept {
        return *targets_;
    }

    ruvia::http_client_config secure_config() const {
        return {.alias_ = "secure", .base_url_ = targets_->secure_, .versions_ = ruvia::http_client_versions{.http2_ = false}, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}};
    }

    void finish() {
        ruvia::app().stop();
        run_->join();
    }

private:
    asio::io_context io_;
    ruvia::test::tcp_port_reservation source_port_;
    ruvia::test::tcp_port_reservation destination_port_;
    ruvia::test::tcp_port_reservation secure_port_;
    ruvia::test::tcp_port_reservation unknown_port_;
    ruvia::test::tls_identity identity_;
    std::shared_ptr<redirect_destinations> targets_;
    std::promise<void> ready_;
    std::optional<ruvia::test::app_run> run_;
};

// The process-wide application keeps its registrations after shutdown. Run
// these scenarios against the fluent test's one configured, live host.
void verify_standalone_origins(server& server);
void verify_request_host_cancellation(server& server);

}  // namespace fluent_client_tests

TEST(Ruvia, http_client_fluent_ownership_helpers_json_and_registered_redirects) {
    using namespace client_handle_and_response_test;
    fluent_client_tests::server server;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = server.targets().source_,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            const auto origin = client.client();
            auto cold = [&] {
                std::string path("/fluent/inspect");
                std::string name("x-owned");
                std::string value("owned-value");
                std::string query_name("escaped name");
                std::string query_value("a &b=+/%?#\xc3\xa9");
                std::string span_value("span-value");
                std::string payload("owned-payload");
                const std::array fields{ruvia::http_header_view{"x-span", span_value}};
                auto request = origin.post(path);
                request.header(name, value).headers(fields).query(query_name, query_value).body(std::as_bytes(std::span(payload)));
                path.assign("/changed");
                name.assign("x-changed");
                value.assign("changed");
                query_name.assign("changed");
                query_value.assign("changed");
                span_value.assign("changed");
                payload.assign("changed");
                return request.send();
            }();
            auto response = co_await std::move(cold);
            EXPECT_TRUE(response.ok());
            auto observed = co_await response.json<fluent_client_observation>();
            EXPECT_EQ(observed.get<"method">().view(), "POST");
            EXPECT_EQ(observed.get<"body">().view(), "owned-payload");
            EXPECT_EQ(observed.get<"query">().view(), "a &b=+/%?#\xc3\xa9");
            EXPECT_EQ(observed.get<"single_header">().view(), "owned-value");
            EXPECT_EQ(observed.get<"span_header">().view(), "span-value");
            EXPECT_TRUE((co_await response.bytes()).empty());
            EXPECT_TRUE(!(co_await response.read()));
            bool replay_rejected = false;
            try {
                (void)co_await response.json<fluent_client_observation>();
            } catch (const std::exception&) {
                replay_rejected = true;
            }
            EXPECT_TRUE(replay_rejected);
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                auto consumed = origin.get("/fluent/inspect");
                auto first = consumed.send();
                (void)consumed.send();
            }));

            auto temporary = origin.post(std::string("/fluent/inspect"))
                                 .body(std::string("temporary-body"))
                                 .send();
            auto temporary_response = co_await std::move(temporary);
            auto temporary_observed = co_await temporary_response.json<fluent_client_observation>();
            EXPECT_EQ(temporary_observed.get<"body">().view(), "temporary-body");
            {
                auto piped = co_await origin.get("/fluent/pipe").send();
                EXPECT_TRUE(piped.status() == ruvia::http_status::created);
                EXPECT_TRUE(piped.header("x-stream-source") == "retained");
                EXPECT_TRUE(piped.header("content-type") == "application/octet-stream");
                EXPECT_TRUE(piped.header("date") == "Thu, 01 Jan 1970 00:00:00 GMT");
                std::size_t links = 0;
                for (const auto& field : piped.headers()) {
                    if (ruvia::http_ascii_equals_ignore_case(field.name(), "link")) {
                        EXPECT_EQ(field.value(), links == 0 ? "</first>; rel=preload" : "</second>; rel=preload");
                        ++links;
                    }
                }
                EXPECT_EQ(links, std::size_t{2});
                const auto contents = co_await piped.text();
                EXPECT_EQ(contents.text(), "firstsecond");
                EXPECT_TRUE(piped.trailer("x-finished") == "yes");
                auto bounded = co_await origin.get("/fluent/pipe-bounded").send();
                EXPECT_TRUE(bounded.status() == ruvia::http_status::created);
                EXPECT_TRUE(bounded.header("x-stream-source") == "retained");
                const auto bounded_contents = co_await bounded.text();
                EXPECT_EQ(bounded_contents.text(), "firstsecond");
                EXPECT_TRUE(bounded.trailer("x-finished") == "yes");
            }

            auto json_request = [&] {
                const auto encoded = ruvia::to_json(observed);
                auto model = ruvia::from_json<fluent_client_observation>(encoded);
                return origin.post("/fluent/inspect").json(model).send();
            }();
            auto json_response = co_await std::move(json_request);
            auto json_observed = co_await json_response.json<fluent_client_observation>();
            EXPECT_EQ(json_observed.get<"content_type">().view(), "application/json");
            auto echoed_model = ruvia::from_json<fluent_client_observation>(json_observed.get<"body">().view());
            EXPECT_EQ(echoed_model.get<"body">().view(), "owned-payload");

            using method_factory = ruvia::http_client_request (ruvia::http_client_ref::*)(std::string_view) const;
            const std::array<std::pair<std::string_view, method_factory>, 7> methods{{
                {"GET", &ruvia::http_client_ref::get},
                {"POST", &ruvia::http_client_ref::post},
                {"PUT", &ruvia::http_client_ref::put},
                {"PATCH", &ruvia::http_client_ref::patch},
                {"DELETE", &ruvia::http_client_ref::del},
                {"HEAD", &ruvia::http_client_ref::head},
                {"OPTIONS", &ruvia::http_client_ref::options},
            }};
            for (const auto& [method, make] : methods) {
                auto result = co_await (origin.*make)("/fluent/inspect").send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-method") == method);
                auto body = co_await result.text();
                if (method == "HEAD") {
                    EXPECT_TRUE(body.empty());
                } else {
                    auto report = ruvia::from_json<fluent_client_observation>(body.text());
                    EXPECT_EQ(report.get<"method">().view(), method);
                }
            }

            struct redirect_case final {
                std::string_view status_;
                std::string_view method_;
                std::string_view expected_method_;
                bool keep_body_;
            };
            for (const auto test : std::array{
                     redirect_case{"301", "POST", "GET", false},
                     redirect_case{"302", "POST", "GET", false},
                     redirect_case{"303", "PUT", "GET", false},
                     redirect_case{"307", "POST", "POST", true},
                     redirect_case{"308", "PUT", "PUT", true}}) {
                auto result = co_await origin.request(test.method_, "/fluent/redirect")
                                  .query("status", test.status_)
                                  .header("authorization", "Bearer same-origin")
                                  .header("content-type", "text/plain")
                                  .body("redirect-body")
                                  .send();
                EXPECT_TRUE(result.ok());
                auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), test.expected_method_);
                EXPECT_EQ(report.get<"body">().view(), test.keep_body_ ? "redirect-body" : "");
                EXPECT_EQ(report.get<"content_type">().view(), test.keep_body_ ? "text/plain" : "");
                EXPECT_EQ(report.get<"authorization">().view(), "Bearer same-origin");
            }
            {
                auto result = co_await origin.head("/fluent/redirect").query("status", "303").send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-method") == "HEAD");
                EXPECT_TRUE((co_await result.bytes()).empty());
            }
            {
                auto result = co_await origin.get("/fluent/redirect").redirects(0).send();
                EXPECT_TRUE(!result.ok());
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            {
                auto result = co_await origin.get("/fluent/registered").send();
                EXPECT_TRUE(result.ok());
                auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), "GET");
                EXPECT_TRUE(report.get<"body">().view().empty());
                EXPECT_TRUE(report.get<"authorization">().view().empty());
                EXPECT_TRUE(report.get<"proxy_authorization">().view().empty());
                EXPECT_TRUE(report.get<"cookie">().view().empty());
                EXPECT_TRUE(report.get<"content_type">().view().empty());
                EXPECT_EQ(report.get<"authority">().view(),
                    std::string_view(server.targets().destination_).substr(std::string_view("http://").size()));
            }
            for (const auto mode : {"unregistered", "downgrade"}) {
                auto result = co_await origin.get("/fluent/registered").query("mode", mode).redirects(0).send();
                EXPECT_TRUE(!result.ok());
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            for (const auto protocol : {
                     ruvia::http_client_versions{.http2_ = false},
#if defined(RUVIA_ENABLE_HTTP2)
                     ruvia::http_client_versions{.http1_ = false},
#endif
                 }) {
                ruvia::http_client websocket_client(attachment.loop(),
                    {.base_url_ = server.targets().source_,
                        .request_timeout_ = std::chrono::seconds(1),
                        .versions_ = protocol});
                std::exception_ptr websocket_failure;
                try {
                    auto connecting = [&] {
                        const std::string path("/fluent/websocket");
                        const std::string value("pooled");
                        const std::string subprotocol("echo.v1");
                        const std::array fields{ruvia::http_header_view{"x-owned", value}};
                        return websocket_client.client().websocket(path).headers(fields).subprotocol(subprotocol).deflate({.compression_ = {.enabled_ = true}}).connect();
                    }();
                    auto connected = co_await std::move(connecting);
                    auto session = std::move(connected);
                    EXPECT_EQ(session.subprotocol(), "echo.v1");
                    // Accepted WebSockets are not ordinary finite HTTP responses.
                    // The HTTP exchange deadline must not become their lifetime.
                    EXPECT_TRUE(co_await ruvia::sleep_for(session.worker(), std::chrono::milliseconds(1250)) ==
                                ruvia::timer_sleep_result::elapsed);
                    EXPECT_TRUE(session.connected());
                    const std::string payload(32768, 'w');
                    for (const bool compress : {false, true}) {
                        auto writing = [&] {
                            const std::string owned(payload);
                            return compress ? session.binary(std::as_bytes(std::span(owned)), {.compress_ = true})
                                            : session.text(owned, {.compress_ = false});
                        }();
                        co_await std::move(writing);
                        const auto message = co_await session.read();
                        EXPECT_TRUE(message.has_value());
                        if (message) {
                            EXPECT_EQ(message->payload(), payload);
                            EXPECT_TRUE(message->binary() == compress);
                            EXPECT_TRUE(message->text() != compress);
                        }
                    }
                    co_await session.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "pooled"});
                    EXPECT_TRUE(!(co_await session.read()));
                    {
                        auto timed = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                        bool timed_out = false;
                        try {
                            (void)co_await timed.read({.timeout_ = std::chrono::milliseconds(20)});
                        } catch (const std::system_error& error) {
                            timed_out = error.code() == std::errc::timed_out;
                        }
                        EXPECT_TRUE(timed_out);
                        // A read deadline ends only that read: the session keeps
                        // its buffered input and serves the next read.
                        EXPECT_TRUE(timed.connected());
                        co_await timed.text("after-timeout");
                        const auto echoed = co_await timed.read({.timeout_ = std::chrono::seconds(5)});
                        EXPECT_TRUE(echoed.has_value() && echoed->payload() == "after-timeout");
                        EXPECT_TRUE(timed.termination() == ruvia::websocket_termination::none);
                        co_await timed.close();
                        EXPECT_TRUE(timed.termination() == ruvia::websocket_termination::local_close);
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        bool rejected = false;
                        try {
                            co_await limited.text(std::string(33, 'm'));
                        } catch (const std::invalid_argument&) {
                            rejected = true;
                        }
                        EXPECT_TRUE(rejected);
                        EXPECT_TRUE(limited.connected());
                        limited.abort();
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket?oversize=1").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        EXPECT_FALSE((co_await limited.read()).has_value());
                        EXPECT_TRUE(limited.termination() == ruvia::websocket_termination::message_too_large);
                        limited.abort();
                    }
                    auto active = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                    const auto worker = attachment.loop().worker();
                    ruvia::task_scope readers(worker);
                    bool cancelled = false;
                    const auto receive = [&]() -> ruvia::task<void> {
                        try {
                            cancelled = !(co_await active.read()).has_value();
                        } catch (const std::system_error&) {
                            cancelled = true;
                        }
                    };
                    readers.spawn(receive());
                    co_await websocket_client.shutdown();
                    co_await readers.join();
                    EXPECT_TRUE(cancelled);
                    EXPECT_TRUE(!active.connected());
                    EXPECT_TRUE(!session.connected());
                    EXPECT_TRUE(session.peer_close_code() == std::uint16_t{1000});
                    EXPECT_EQ(session.peer_close_reason(), "pooled");
                } catch (...) {
                    websocket_failure = std::current_exception();
                }
                co_await websocket_client.shutdown();
                if (websocket_failure) {
                    std::rethrow_exception(websocket_failure);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (!failure) {
        fluent_client_tests::verify_standalone_origins(server);
        fluent_client_tests::verify_request_host_cancellation(server);
    }
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void fluent_client_tests::verify_standalone_origins(server& server) {
    SCOPED_TRACE("standalone origins, isolated policies and shared budget");
    auto& io = ruvia::test::new_test_io_context();
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        auto secure = server.secure_config();
        auto untrusted = secure;
        untrusted.alias_ = "untrusted";
        untrusted.tls_ = {};
        const std::array configs{
            ruvia::http_client_config{.base_url_ = server.targets().source_,
                .versions_ = ruvia::http_client_versions{.http2_ = false},
                .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
                .user_agent_ = "source-agent",
                .cookies_ = {{"configured", "source"}}},
            ruvia::http_client_config{.alias_ = "destination", .base_url_ = server.targets().destination_, .versions_ = ruvia::http_client_versions{.http2_ = false}, .user_agent_ = "destination-agent", .cookies_ = {{"configured", "destination"}}},
            secure, untrusted};
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>(configs));
        try {
            const auto source = client.client();
            const auto destination = client.client("destination");
            for (const auto status : {"302", "307"}) {
                auto result = co_await source.post("/fluent/redirect")
                                  .query("status", status)
                                  .query("location", std::string(server.targets().destination_) + "/fluent/inspect")
                                  .header("authorization", "Bearer source-secret")
                                  .header("proxy-authorization", "Basic proxy-secret")
                                  .header("cookie", "explicit=source-secret")
                                  .header("content-type", "text/plain")
                                  .body("source-body")
                                  .send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-user-agent") == "destination-agent");
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), std::string_view(status) == "302" ? "GET" : "POST");
                EXPECT_EQ(report.get<"body">().view(), std::string_view(status) == "302" ? "" : "source-body");
                EXPECT_EQ(report.get<"content_type">().view(), std::string_view(status) == "302" ? "" : "text/plain");
                EXPECT_EQ(report.get<"authorization">().view(), "");
                EXPECT_EQ(report.get<"proxy_authorization">().view(), "");
                EXPECT_EQ(report.get<"cookie">().view(), "configured=destination");
                EXPECT_EQ(report.get<"authority">().view(),
                    std::string_view(server.targets().destination_).substr(7));
            }
            for (const auto& origin : {source, destination}) {
                auto set = co_await origin.get("/fluent/inspect").query("set-cookie", "received=retained; Path=/").send();
                (void)co_await set.bytes();
            }
            {
                auto result = co_await source.get("/fluent/inspect").send();
                EXPECT_TRUE(result.header("x-observed-user-agent") == "source-agent");
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"cookie">().view(), "configured=source; received=retained");
            }
            {
                auto result = co_await destination.get("/fluent/inspect").send();
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"cookie">().view(), "configured=destination");
            }
            {
                auto result = co_await source.get("/fluent/redirect")
                                  .query("location", std::string(server.targets().unregistered_) + "/fluent/inspect")
                                  .send();
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                const auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            {
                auto result = co_await client.client("secure").get("/fluent/redirect").query("location", std::string(server.targets().destination_) + "/fluent/inspect").send();
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                const auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            bool tls_rejected = false;
            try {
                (void)co_await client.client("untrusted").get("/fluent/inspect").send();
            } catch (const ruvia::http_client_error& error) {
                tls_rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::tls_failed);
            }
            EXPECT_TRUE(tls_rejected);
            bool unknown_rejected = false;
            try {
                (void)client.client("unknown");
            } catch (const ruvia::http_client_error& error) {
                unknown_rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::not_configured);
            }
            EXPECT_TRUE(unknown_rejected);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (!failure) {
            const std::span<const ruvia::http_client_config> origins(configs.data(), 2);
            ruvia::http_client bounded(attachment.loop(), origins, {.result_budget_ = {.max_retained_bytes_ = 16}});
            try {
                auto first = co_await bounded.client().get("/fluent/stream-source").send();
                const auto retained = co_await first.bytes(11);
                EXPECT_EQ(retained.text(), "firstsecond");
                auto second = co_await bounded.client("destination").get("/fluent/stream-source").send();
                bool exhausted = false;
                try {
                    (void)co_await second.bytes(11);
                } catch (const ruvia::http_client_error& error) {
                    exhausted = error.code() == ruvia::make_error_code(ruvia::http_client_errc::result_budget_exceeded);
                }
                EXPECT_TRUE(exhausted);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await bounded.shutdown();
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void fluent_client_tests::verify_request_host_cancellation(server& server) {
    SCOPED_TRACE("user options preserve request-host cancellation");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client owner(attachment.loop(), {.base_url_ = server.targets().source_,
                                                        .request_timeout_ = std::chrono::seconds(2),
                                                        .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            const auto before_waiters = fluent_client_tests::host_cancel_waiters.load();
            const auto before_cancelled = fluent_client_tests::host_cancelled_requests.load();
            ruvia::stop_source stop;
            bool cancelled = false;
            const auto request = [&]() -> ruvia::task<void> {
                try {
                    (void)co_await owner.client().with_options({.stop_token_ = stop.token()}).get("/fluent/host-cancel").send();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
            };
            ruvia::task_scope tasks(owner.worker());
            tasks.spawn(request());
            const auto started = std::chrono::steady_clock::now();
            while (fluent_client_tests::host_cancel_waiters.load() == before_waiters &&
                   std::chrono::steady_clock::now() - started < std::chrono::seconds(1)) {
                (void)co_await ruvia::sleep_for(owner.worker(), std::chrono::milliseconds(1));
            }
            EXPECT_GT(fluent_client_tests::host_cancel_waiters.load(), before_waiters);
            stop.request_stop();
            co_await tasks.join();
            EXPECT_TRUE(cancelled);
            const auto stopped = std::chrono::steady_clock::now();
            while (fluent_client_tests::host_cancelled_requests.load() == before_cancelled &&
                   std::chrono::steady_clock::now() - stopped < std::chrono::seconds(1)) {
                (void)co_await ruvia::sleep_for(owner.worker(), std::chrono::milliseconds(1));
            }
            EXPECT_GT(fluent_client_tests::host_cancelled_requests.load(), before_cancelled);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await owner.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_unconsumed_builders_outlive_shutdown_and_client_storage) {
    using namespace client_handle_and_response_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_request> request;
        std::optional<ruvia::http_client_connect_request> connect;
        std::optional<ruvia::http_client_websocket_request> websocket;
        {
            ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:1"});
            {
                const std::string path("/" + std::string(2048, 'p'));
                const std::string value(16384, 'v');
                const std::string body(32768, 'b');
                const std::string authority(std::string(63, 'a') + "." + std::string(63, 'b') + ".test:443");
                const auto origin = client.client();
                request.emplace(origin.post(path));
                request->header("x-owned", value).query("owned", path).body(body);
                connect.emplace(origin.connect(authority));
                connect->protocol("owned-protocol").target(path).header("x-owned", value);
                websocket.emplace(origin.websocket(path));
                websocket->header("x-owned", value).subprotocol(std::string(256, 's'));
            }
            co_await client.shutdown();
        }
        try {
            EXPECT_TRUE(throws_logic_error([&] { (void)request->send(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)connect->open(); }));
            EXPECT_TRUE(throws_logic_error([&] { (void)websocket->connect(); }));
            request.reset();
            connect.reset();
            websocket.reset();
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
