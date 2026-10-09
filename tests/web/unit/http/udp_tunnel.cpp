#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_udp_tunnel.h"

#include "router/router_impl.h"
#include "server/native_accepted_socket_ticket.h"
#include "server/web_worker_runtime.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
ruvia::task<void> udp_echo(void*, ruvia::context& context_value) {
    ruvia::http_udp_tunnel udp(context_value.tunnel().capsules());
    while (auto datagram = co_await udp.read()) {
        co_await udp.send(datagram->payload());
    }
    co_await udp.finish();
}
ruvia::task<ruvia::http_response> udp_sibling(void*, ruvia::context& context_value) {
    co_return context_value.text("sibling");
}
ruvia::task<void> forward_connections(asio::ip::tcp::acceptor& acceptor, ruvia::detail::web_worker_runtime& server) {
    for (;;) {
        auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto done) { acceptor.async_accept(std::move(done)); });
        if (accepted.error_code() == asio::error::operation_aborted) {
            co_return;
        }
        if (accepted.error_code()) {
            throw std::system_error(accepted.error_code());
        }
        auto socket = std::move(accepted.result());
        std::error_code error;
        const auto native = socket.release(error);
        if (error) {
            throw std::system_error(error);
        }
        ruvia::detail::native_accepted_socket_ticket ticket(asio::ip::tcp::v4(), 0, native);
        if (!server.network_submission().post([&server, ticket = std::move(ticket)]() mutable {
                                            server.accept_transferred_connection(std::move(ticket));
                                        })
                .accepted()) {
            throw std::runtime_error("UDP tunnel socket dispatch rejected");
        }
    }
}
}  // namespace

RUVIA_TEST(http_udp_tunnel_negotiates_http1_upgrade_and_http2_extended_connect_and_owns_results) {
    for (const auto protocol : {ruvia::http_client_protocol::http1_only, ruvia::http_client_protocol::http2_only}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::detail::router router;
        auto& routes_value = ruvia::detail::router_impl::from(router);
        routes_value.register_tunnel_route("connect-udp", std::pmr::string("/udp/:host/:port"), {nullptr, udp_echo}, {}, {});
        routes_value.register_route(ruvia::http_known_method::get, std::pmr::string("/sibling"), {nullptr, udp_sibling}, ruvia::detail::request_body_mode::buffered, {}, {});
        routes_value.finalize();
        ruvia::detail::web_worker_runtime server(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes_value.route_table());
        server.start();
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::task<void> {
            const auto worker_value = attachment.loop().handle();
            asio::ip::tcp::acceptor source_value(io, {asio::ip::address_v4::loopback(), 0});
            ruvia::task_scope forwarding(worker_value);
            forwarding.spawn(forward_connections(source_value, server));
            std::optional<ruvia::http_udp_datagram> retained;
            {
                ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = source_value.local_endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = protocol});
                try {
                    std::string target = "/udp/target.test/443";
                    auto operation = client.open_udp_tunnel({.target_ = target}, {.max_chunk_bytes_ = 1024});
                    target.assign("mutated");
                    auto result_value = co_await std::move(operation);
                    if (!result_value.tunnel()) {
                        throw std::runtime_error("CONNECT-UDP rejected");
                    }
                    RUVIA_CHECK(result_value.tunnel()->status().value() == (protocol == ruvia::http_client_protocol::http1_only ? 101 : 200));
                    RUVIA_CHECK(result_value.tunnel()->header("capsule-protocol") == "?1");
                    RUVIA_CHECK(!result_value.tunnel()->header("content-length"));
                    if (protocol == ruvia::http_client_protocol::http1_only) {
                        RUVIA_CHECK(result_value.tunnel()->header("upgrade") == "connect-udp");
                    } else {
                        RUVIA_CHECK(!result_value.tunnel()->header("connection"));
                    }
                    auto udp = std::move(*result_value.tunnel()).udp();
                    std::string payload_value(16003, 'u');
                    auto send = udp.send(payload_value);
                    payload_value.assign("mutated");
                    co_await std::move(send);
                    retained = co_await udp.read();
                    RUVIA_CHECK(retained && retained->payload().size() == 16003);
                    RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte value) { return value == std::byte{'u'}; }));
                    co_await udp.send("");
                    auto empty = co_await udp.read();
                    RUVIA_CHECK(empty && empty->payload().empty());
                    co_await udp.finish();
                    RUVIA_CHECK(!(co_await udp.read()));
                    auto rejected = co_await client.open_udp_tunnel({.target_ = "/missing"});
                    RUVIA_CHECK(!rejected.tunnel() && rejected.response());
                    if (!rejected.response()) {
                        throw std::runtime_error("missing UDP tunnel rejection");
                    }
                    RUVIA_CHECK(rejected.response()->status() == ruvia::http_status::not_found);
                    auto denial = co_await rejected.response()->body().read_all();
                    RUVIA_CHECK(!denial.bytes().empty());
                    if (protocol == ruvia::http_client_protocol::http1_only) {
                        const ruvia::http_header_view malformed[]{{"Connection", "Upgrade"}, {"Upgrade", "connect-udp"}};
                        auto bad = co_await client.send({.target_ = "/udp/target.test/443", .headers_ = malformed});
                        RUVIA_CHECK(bad.status() == ruvia::http_status::bad_request);
                        auto error_body = co_await bad.body().read_all();
                        RUVIA_CHECK(!error_body.bytes().empty());
                    }
                    {
                        auto second = co_await client.open_udp_tunnel({.target_ = "/udp/target.test/443"});
                        if (!second.tunnel()) {
                            throw std::runtime_error("second CONNECT-UDP rejected");
                        }
                        auto blocked = std::move(*second.tunnel()).udp();
                        bool failed{};
                        ruvia::task_scope reads(worker_value);
                        auto receive = [&]() -> ruvia::task<void> {
                            try {
                                (void)co_await blocked.read();
                            } catch (const ruvia::http_client_error&) {
                                failed = true;
                            }
                        };
                        reads.spawn(receive());
                        co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1));
                        blocked.abort();
                        co_await reads.join();
                        RUVIA_CHECK(failed);
                    }
                    auto sibling = co_await client.send({.target_ = "/sibling"});
                    std::string body;
                    while (auto bytes = co_await sibling.body().text()) {
                        body.append(*bytes);
                    }
                    RUVIA_CHECK(body == "sibling");
                } catch (...) {
                    failure = std::current_exception();
                }
                co_await client.shutdown();
            }
            RUVIA_CHECK(retained && retained->payload().size() == 16003);
            retained.reset();
            std::error_code ignored;
            source_value.close(ignored);
            try {
                co_await forwarding.join();
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
        server.stop();
        server.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}
