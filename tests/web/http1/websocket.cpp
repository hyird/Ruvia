#include "ruvia/web/websocket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
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
};
const auto websocket_eof_observed = std::make_shared<websocket_eof_observation>();
[[maybe_unused]] const bool websocket_eof_observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<websocket_eof_observation>>(
        [] { return websocket_eof_observed; });
    return true;
}();

class http1_websocket_routes final : public ruvia::controller<http1_websocket_routes> {
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_WS("/http1-websocket-eof", read_until_eof);
    RUVIA_ROUTES_END

    ruvia::task<void> read_until_eof(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<websocket_eof_observation>>();
        auto& websocket_value = context_value.get_websocket();
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
}
