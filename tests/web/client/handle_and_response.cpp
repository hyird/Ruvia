#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/http_client.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {

template <typename callback_type>
[[nodiscard]] bool throws_logic_error(callback_type&& callback) {
    try {
        callback();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

}  // namespace

RUVIA_TEST(http_client_handle_origin_and_expired_send_reject_after_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(),
            {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = 8089});
        auto handle = client.handle();
        try {
            RUVIA_CHECK(handle.scheme() == ruvia::http_scheme::http);
            RUVIA_CHECK_EQ(handle.host(), std::string_view("127.0.0.1"));
            RUVIA_CHECK_EQ(handle.port(), std::uint16_t{8089});
            const auto origin = handle.origin();
            RUVIA_CHECK(origin.scheme() == ruvia::http_scheme::http);
            RUVIA_CHECK_EQ(origin.host(), std::string_view("127.0.0.1"));
            RUVIA_CHECK_EQ(origin.port(), std::uint16_t{8089});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (!failure) {
            try {
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.send({.target_ = "/"}); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.open_request({.target_ = "/"}); }));
                RUVIA_CHECK(
                    throws_logic_error([&] { (void)handle.open_tunnel({.authority_ = "127.0.0.1:8089"}); }));
                RUVIA_CHECK(
                    throws_logic_error([&] { (void)handle.open_udp_tunnel({.target_ = "/udp"}); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.with_options({}); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.stats(); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.next_push(); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.next_advertisement(); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.host(); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.port(); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.scheme(); }));
                RUVIA_CHECK(throws_logic_error([&] { (void)handle.origin(); }));
            } catch (...) {
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
}

RUVIA_TEST(http_client_response_and_body_moved_from_throw_logic_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Test: ok\r\nConnection: close\r\n\r\nhi";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) { peer_failure = error; });

    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(),
            {.scheme_ = ruvia::http_scheme::http,
                .host_ = "127.0.0.1",
                .port_ = peer.local_endpoint().port(),
                .request_timeout_ = std::chrono::seconds(5)});
        try {
            auto original = co_await client.handle().send({.target_ = "/moved"});
            auto active = std::move(original);

            RUVIA_CHECK(original.body().complete());
            RUVIA_CHECK(throws_logic_error([&] { (void)original.status(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.received_status(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.protocol_version(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.headers(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.trailers(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.header("x-test"); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.trailer("x-test"); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.informational_responses(); }));
            RUVIA_CHECK(throws_logic_error([&] { original.reprioritize({}); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.body().read(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.body().text(); }));
            RUVIA_CHECK(throws_logic_error([&] { (void)original.body().read_all(); }));

            RUVIA_CHECK(active.status() == ruvia::http_status::ok);
            RUVIA_CHECK_EQ(active.header("x-test"), std::optional<std::string_view>("ok"));
            auto body = co_await active.body().read_all();
            RUVIA_CHECK_EQ(body.text(), std::string_view("hi"));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}
