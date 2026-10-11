#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/http_client.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {

ruvia::task<std::string> read_to_end(asio::ip::tcp::socket& stream) {
    std::array<char, 4096> bytes_value{};
    std::string received;
    for (;;) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            stream.async_read_some(asio::buffer(bytes_value), std::move(handler));
        });
        if (read.error_code() == asio::error::eof) {
            co_return received;
        }
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        received.append(bytes_value.data(), read.result());
    }
}

ruvia::task<void> serve_client_connect(asio::ip::tcp::socket& stream, unsigned mode, std::string& received_value) {
    std::string head;
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        head.push_back(byte);
    }
    if (!head.starts_with("CONNECT target.test:443 HTTP/1.1\r\n")) {
        throw std::runtime_error("invalid CONNECT authority-form");
    }
    if (head.find("Content-Length:") != std::string::npos || head.find("Transfer-Encoding:") != std::string::npos) {
        throw std::runtime_error("CONNECT head has HTTP content framing");
    }
    const auto send = [&](std::string_view bytes_value, bool end = false) -> ruvia::task<void> {
        const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(stream, asio::buffer(bytes_value), std::move(handler));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code());
        }
        if (end) {
            std::error_code error;
            stream.shutdown(asio::ip::tcp::socket::shutdown_send, error);
            if (error) {
                throw std::system_error(error);
            }
        }
    };
    if (mode == 2) {
        co_await send("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\nConnection: close\r\nProxy-Authenticate: Basic realm=proxy\r\n\r\ndenied", true);
        co_return;
    }
    co_await send("HTTP/1.1 200 Connection Established\r\nX-Tunnel: owned-metadata\r\n\r\n");
    if (mode == 1) {
        co_await send(std::string(100003, 's'), true);
    }
    received_value = co_await read_to_end(stream);
    if (mode == 0) {
        co_await send(std::string(100003, 's'), true);
    }
}

void exercise_client_connect(ruvia::testing::test_context& ruvia_ctx, unsigned mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::string received;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::task_scope tasks(worker_value);
        const auto serve = [&]() -> ruvia::task<void> {
            auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
            if (accepted.error_code()) {
                throw std::system_error(accepted.error_code());
            }
            auto socket = std::move(accepted.result());
            co_await serve_client_connect(socket, mode, received);
        };
        tasks.spawn(serve());
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port(), .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http1_only});
        try {
            auto result_value = co_await client.open_tunnel({.authority_ = "target.test:443"});
            if (mode == 2) {
                RUVIA_CHECK(!result_value.tunnel() && result_value.response());
                if (!result_value.response()) {
                    throw std::runtime_error("missing CONNECT rejection");
                }
                RUVIA_CHECK(result_value.response()->status().value() == 407);
                std::string text;
                while (auto bytes = co_await result_value.response()->body().text()) {
                    text.append(*bytes);
                }
                RUVIA_CHECK(text == "denied");
            } else {
                RUVIA_CHECK(result_value.tunnel() && !result_value.response());
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                const auto read_greeting = [&]() -> ruvia::task<void> {
                    std::string greeting;
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's'));
                };
                if (mode == 1) {
                    co_await read_greeting();
                }
                for (unsigned part = 0; part != 8; ++part) {
                    std::string payload_value(16384, 't');
                    auto write = tunnel.write(std::string_view(payload_value));
                    payload_value.assign("mutated");
                    co_await std::move(write);
                }
                co_await tunnel.finish();
                co_await tunnel.finish();
                if (mode == 0) {
                    co_await read_greeting();
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (mode != 2) {
        RUVIA_CHECK(received == std::string(8 * 16384, 't'));
    }
}

}  // namespace

RUVIA_TEST(http1_client_tunnel_and_rejections_preserve_ownership_and_tcp_half_close) {
    for (unsigned mode = 0; mode != 3; ++mode) {
        exercise_client_connect(ruvia_ctx, mode);
    }
}
