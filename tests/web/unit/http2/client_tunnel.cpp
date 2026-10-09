#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"

#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "memory_resource_fixture.h"
#include "router/router_impl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct tunnel_observation {
    std::string bytes_;
    bool finish_first_{};
    bool eof_{};
};
ruvia::task<void> client_tunnel_echo(void* raw, ruvia::context& c) {
    auto& observation_value = *static_cast<tunnel_observation*>(raw);
    observation_value.bytes_.clear();
    observation_value.eof_ = false;
    auto& tunnel = c.tunnel();
    if (observation_value.finish_first_) {
        co_await tunnel.write("greeting");
        co_await tunnel.finish();
    }
    while (auto bytes = co_await tunnel.read()) {
        observation_value.bytes_.append(*bytes);
        if (!observation_value.finish_first_) {
            co_await tunnel.write(std::string_view(*bytes));
        }
    }
    observation_value.eof_ = true;
    co_await tunnel.finish();
}
ruvia::task<ruvia::http_response> client_tunnel_sibling(void*, ruvia::context& c) {
    co_return c.text("sibling");
}
ruvia::task<void> serve_client_tunnel(asio::ip::tcp::acceptor& acceptor, const ruvia::worker_handle& worker_value, const ruvia::detail::route_table& routes_value, ruvia::worker_memory& memory) {
    auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
    if (accepted.error_code()) {
        throw std::system_error(accepted.error_code());
    }
    auto socket = std::move(accepted.result());
    ruvia::test::http2_sans_io_session_fixture fixture;
    co_await ruvia::detail::run_http2_sans_io_session(socket, routes_value, memory, fixture.context(fixture.services(worker_value)));
}
}  // namespace
RUVIA_TEST(http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource allocation;
    std::exception_ptr failure;
    std::optional<ruvia::http_client_tunnel> retained_tunnel;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::worker_memory memory(allocation);
        tunnel_observation observation;
        ruvia::detail::router router;
        auto& routes_value = ruvia::detail::router_impl::from(router);
        routes_value.register_tunnel_route({}, std::pmr::string("target.test:443"), {&observation, client_tunnel_echo}, {}, {});
        routes_value.register_tunnel_route("test-protocol", std::pmr::string("/tunnel"), {&observation, client_tunnel_echo}, {}, {});
        routes_value.register_route(ruvia::http_known_method::get, std::pmr::string("/sibling"), {nullptr, client_tunnel_sibling}, ruvia::detail::request_body_mode::buffered, {}, {});
        routes_value.finalize();
        ruvia::task_scope tasks(worker_value);
        tasks.spawn(serve_client_tunnel(acceptor, worker_value, routes_value.route_table(), memory));
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation.finish_first_ = round >= 2;
                std::string authority = round == 1 ? "proxy.test" : "target.test:443";
                auto cold = client.open_tunnel({.authority_ = authority, .protocol_ = round == 1 ? "test-protocol" : "", .target_ = round == 1 ? "/tunnel" : ""});
                authority.assign("mutated");
                auto result_value = co_await std::move(cold);
                RUVIA_CHECK(result_value.response() == nullptr && result_value.tunnel() != nullptr);
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                RUVIA_CHECK(tunnel.status() == ruvia::http_status::ok);
                RUVIA_CHECK(!tunnel.header("content-length"));
                std::string echoed;
                const auto receive = [&]() -> ruvia::task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation.finish_first_) {
                    co_await receive();
                    RUVIA_CHECK(echoed == "greeting");
                }
                ruvia::task_scope readers(worker_value);
                if (round == 3) {
                    tunnel.abort();
                    bool half_closed_write_rejected = false;
                    try {
                        auto late_write = tunnel.write("late");
                        static_cast<void>(late_write);
                    } catch (const ruvia::http_client_error& error) {
                        half_closed_write_rejected = error.code() == ruvia::http_client_error::code_type::cancelled;
                    }
                    RUVIA_CHECK(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client.send({.target_ = "/sibling"});
                    std::string sibling_body;
                    while (auto bytes = co_await sibling.body().text()) {
                        sibling_body.append(*bytes);
                    }
                    RUVIA_CHECK(sibling_body == "sibling");
                    retained_tunnel.emplace(std::move(tunnel));
                    continue;
                }
                if (!observation.finish_first_) {
                    readers.spawn(receive());
                }
                std::string payload_value(100003, 't');
                for (std::size_t offset = 0; offset != payload_value.size();) {
                    const auto count = std::min<std::size_t>(16384, payload_value.size() - offset);
                    auto output = tunnel.write(std::string_view(payload_value).substr(offset, count));
                    bool busy = false;
                    try {
                        (void)tunnel.finish();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await readers.join();
                if (!observation.finish_first_) {
                    RUVIA_CHECK(echoed == payload_value);
                }
                auto sibling = co_await client.send({.target_ = "/sibling"});
                std::string sibling_body;
                while (auto bytes = co_await sibling.body().text()) {
                    sibling_body.append(*bytes);
                }
                RUVIA_CHECK(sibling_body == "sibling");
                RUVIA_CHECK(observation.bytes_ == payload_value && observation.eof_);
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
            RUVIA_CHECK(late_write_rejected);
            retained_tunnel.reset();
        }
        std::error_code ignored;
        acceptor.close(ignored);
        co_await tasks.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocation.live_allocations(), std::size_t{0});
}

namespace {
ruvia::task<void> capsule_echo(void*, ruvia::context& c) {
    auto stream = c.tunnel().capsules();
    while (auto capsule = co_await stream.read()) {
        co_await stream.write(capsule->type(), capsule->payload());
    }
    co_await stream.finish();
}
}  // namespace
RUVIA_TEST(http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource allocation;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::worker_memory memory(allocation);
        ruvia::detail::router router;
        auto& routes_value = ruvia::detail::router_impl::from(router);
        routes_value.register_tunnel_route("test-capsules", std::pmr::string("/capsules"), {nullptr, capsule_echo}, {}, {});
        routes_value.finalize();
        ruvia::task_scope tasks(worker_value);
        tasks.spawn(serve_client_tunnel(acceptor, worker_value, routes_value.route_table(), memory));
        std::optional<ruvia::http_capsule> retained;
        std::unique_ptr<ruvia::scoped_operation<void>> cold;
        {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
            try {
                auto result_value = co_await client.open_tunnel({.authority_ = "proxy.test", .protocol_ = "test-capsules", .target_ = "/capsules"}, {.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*result_value.tunnel()).capsules();
                    std::string bytes_value(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes_value);
                    bytes_value.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    RUVIA_CHECK(retained && retained->type() == 0x123456789ULL && retained->payload() == std::string(4099, 'c'));
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    RUVIA_CHECK(empty && empty->type() == 0 && empty->payload().empty());
                    co_await stream.finish();
                    RUVIA_CHECK(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                auto second = co_await client.open_tunnel({.authority_ = "proxy.test", .protocol_ = "test-capsules", .target_ = "/capsules"});
                if (!second.tunnel()) {
                    throw std::runtime_error("second capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*second.tunnel()).capsules();
                    cold.reset(new ruvia::scoped_operation<void>(stream.write(12, std::string(4099, 'x'))));
                }
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload() == std::string(4099, 'c'));
        cold.reset();
        retained.reset();
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
    RUVIA_CHECK_EQ(allocation.live_allocations(), std::size_t{0});
}
