#include <array>
#include <chrono>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

#include <asio.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace final_response_commit_test {

class invalid_final_response_routes final {
    RUVIA_ROUTES_BEGIN(invalid_final_response_routes)
    RUVIA_GET("/server-upgrade-required-without-upgrade", upgrade_required);
    RUVIA_ROUTES_END

    // 426 must name the required protocol in Upgrade (RFC 9110 15.5.22).
    ruvia::task<ruvia::http_response> upgrade_required(ruvia::context& context_value) {
        context_value.status(ruvia::http_status::upgrade_required);
        co_return context_value.text("upgrade");
    }
};

}  // namespace final_response_commit_test

RUVIA_TEST(server_answers_an_uncommittable_route_response_with_a_closing_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::string response;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            // Keep-alive request: only the server's close verdict ends the read.
            const std::string request =
                "GET /server-upgrade-required-without-upgrade HTTP/1.1\r\nHost: localhost\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(request), std::move(handler));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::array<char, 1024> buffer;
            for (;;) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(buffer), std::move(handler));
                });
                if (received.error_code() == asio::error::eof) {
                    break;
                }
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                response.append(buffer.data(), received.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        static_cast<void>(watchdog.cancel());
        ruvia::close_socket(client);
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    done.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(response.starts_with("HTTP/1.1 500 "));
    RUVIA_CHECK(response.find("\r\nConnection: close\r\n") != std::string::npos);
}
