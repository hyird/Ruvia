#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"

#include "router/router_impl.h"
#include "server/native_accepted_socket_ticket.h"
#include "server/web_worker_runtime.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct observation {
    std::string bytes_;
    bool finish_first_{};
    bool ended_{};
    bool retained_stable_{true};
};
ruvia::task<void> tunnel_handler(void* raw, ruvia::context& context_value) {
    auto& observed_value = *static_cast<observation*>(raw);
    auto& tunnel = context_value.tunnel();
    if (observed_value.finish_first_) {
        co_await tunnel.finish();
    }
    std::optional<std::pmr::string> retained;
    while (auto bytes = co_await tunnel.read()) {
        observed_value.bytes_.append(*bytes);
        if (!retained) {
            retained.emplace(*bytes, context_value.pool());
        }
        if (!observed_value.finish_first_) {
            auto output = tunnel.write(std::string_view(*bytes));
            bytes->assign("input changed before awaiting output");
            co_await std::move(output);
        }
        observed_value.retained_stable_ = observed_value.retained_stable_ && retained->find_first_not_of('t') == std::string_view::npos;
    }
    observed_value.ended_ = true;
    co_await tunnel.finish();
}
void exercise_connect(ruvia::testing::test_context& ruvia_ctx, bool finish_first) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    observation observed;
    observed.finish_first_ = finish_first;
    ruvia::detail::router router;
    auto& routes_value = ruvia::detail::router_impl::from(router);
    routes_value.register_tunnel_route({}, std::pmr::string("target.test:443"), {&observed, &tunnel_handler}, {}, {});
    routes_value.finalize();
    ruvia::detail::web_worker_runtime server(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes_value.route_table(), {});
    server.start();
    asio::ip::tcp::acceptor source_value(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket client(io);
    client.connect(source_value.local_endpoint());
    asio::ip::tcp::socket accepted(io);
    source_value.accept(accepted);
    std::error_code error;
    auto native = accepted.release(error);
    RUVIA_CHECK(!error);
    auto ticket = ruvia::detail::native_accepted_socket_ticket(asio::ip::tcp::v4(), 0, native);
    const auto posted = server.network_submission().post([&server, ticket = std::move(ticket)]() mutable {
        server.accept_transferred_connection(std::move(ticket));
    });
    RUVIA_CHECK(posted.accepted());
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
            const std::string request = "CONNECT TARGET.TEST:0443 HTTP/1.1\r\nHost: TARGET.TEST:0443\r\n\r\n" + initial_value;
            co_await send(request);
            std::string head;
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
            RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
            RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string::npos);
            std::array<char, 4096> input{};
            std::string echoed;
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
            RUVIA_CHECK(echoed == (finish_first ? std::string{} : initial_value + additional));
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
    root.get();
    // A serving worker can still be consuming the bytes sent after its FIN.
    // Closing admission does not destroy its pending connection coroutines.
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.stats().active_connections_ != 0 && std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.stop();
    server.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(observed.ended_ && observed.retained_stable_);
    RUVIA_CHECK(observed.bytes_ == initial_value + additional);
}
}  // namespace

RUVIA_TEST(http1_connect_routes_transfer_buffered_bytes_and_keep_receive_direction_after_send_fin) {
    exercise_connect(ruvia_ctx, false);
    exercise_connect(ruvia_ctx, true);
}
