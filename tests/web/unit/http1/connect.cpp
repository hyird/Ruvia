#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Socket.h"

#include "router/RouterImpl.h"
#include "server/NativeAcceptedSocketTicket.h"
#include "server/WebWorkerRuntime.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct Observation {
    std::string bytes;
    bool finishFirst{};
    bool ended{};
    bool retainedStable{true};
};
ruvia::Task<void> tunnelHandler(void* raw, ruvia::Context& context) {
    auto& observed = *static_cast<Observation*>(raw);
    auto& tunnel = context.tunnel();
    if (observed.finishFirst) {
        co_await tunnel.finish();
    }
    std::optional<std::pmr::string> retained;
    while (auto bytes = co_await tunnel.read()) {
        observed.bytes.append(*bytes);
        if (!retained) {
            retained.emplace(*bytes, context.pool());
        }
        if (!observed.finishFirst) {
            auto output = tunnel.write(std::string_view(*bytes));
            bytes->assign("input changed before awaiting output");
            co_await std::move(output);
        }
        observed.retainedStable = observed.retainedStable && retained->find_first_not_of('t') == std::string_view::npos;
    }
    observed.ended = true;
    co_await tunnel.finish();
}
void exerciseConnect(ruvia::testing::TestContext& ruvia_ctx, bool finishFirst) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    Observation observed;
    observed.finishFirst = finishFirst;
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    routes.registerTunnelRoute({}, std::pmr::string("target.test:443"), {&observed, &tunnelHandler}, {}, {});
    routes.finalize();
    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes.routeTable(), {});
    server.start();
    asio::ip::tcp::acceptor source(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket client(io);
    client.connect(source.local_endpoint());
    asio::ip::tcp::socket accepted(io);
    source.accept(accepted);
    std::error_code error;
    auto native = accepted.release(error);
    RUVIA_CHECK(!error);
    auto ticket = ruvia::detail::NativeAcceptedSocketTicket(asio::ip::tcp::v4(), 0, native);
    const auto posted = server.networkSubmission().post([&server, ticket = std::move(ticket)]() mutable {
        server.acceptTransferredConnection(std::move(ticket));
    });
    RUVIA_CHECK(posted.accepted());
    const std::string initial(5007, 't');
    const std::string additional(100003, 't');
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::closeSocket(client); } });
        try {
            const auto send = [&](std::string_view bytes) -> ruvia::Task<void> {
                const auto result = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes), std::move(handler)); });
                if (result.errorCode()) {
                    throw std::system_error(result.errorCode());
                }
            };
            const std::string request = "CONNECT TARGET.TEST:0443 HTTP/1.1\r\nHost: TARGET.TEST:0443\r\n\r\n" + initial;
            co_await send(request);
            std::string head;
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.errorCode()) {
                    throw std::system_error(read.errorCode());
                }
                head.push_back(byte);
            }
            RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
            RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
            RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string::npos);
            std::array<char, 4096> input{};
            std::string echoed;
            if (!finishFirst) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
            for (;;) {
                const auto read = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.errorCode() == asio::error::eof) {
                    break;
                }
                if (read.errorCode()) {
                    throw std::system_error(read.errorCode());
                }
                echoed.append(input.data(), read.result());
            }
            RUVIA_CHECK(echoed == (finishFirst ? std::string{} : initial + additional));
            if (finishFirst) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::closeSocket(client);
        }
        (void)watchdog.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    // A serving worker can still be consuming the bytes sent after its FIN.
    // Closing admission does not destroy its pending connection coroutines.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.stats().activeConnections != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.stop();
    server.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(observed.ended && observed.retainedStable);
    RUVIA_CHECK(observed.bytes == initial + additional);
}
}  // namespace

RUVIA_TEST(http1ConnectRoutesTransferBufferedBytesAndKeepReceiveDirectionAfterSendFin) {
    exerciseConnect(ruvia_ctx, false);
    exerciseConnect(ruvia_ctx, true);
}
