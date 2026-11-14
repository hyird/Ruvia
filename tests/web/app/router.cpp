#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "blocking_pool.h"
#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "middleware.h"
#include "stop_token.h"
#include "streaming.h"
#include "task_scope.h"
#ifdef RUVIA_ENABLE_REDIS
#include "redis.h"
#endif

namespace public_router_test {
using namespace std::chrono_literals;

struct scoped_header final {
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        context.policy().header("X-Router", "scoped");
        co_await next();
    }
};

struct unmatched_header final {
    static constexpr bool ruvia_runs_on_unmatched_requests = true;
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        context.policy().header("X-All-Routes", "present");
        co_await next();
    }
};

struct double_next final {
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next) {
        co_await next();
        co_await next();
    }
};

struct lifetime final {
    unsigned live_{};
    unsigned destroyed_{};
    bool fail_{};
};

struct counted_middleware final {
    explicit counted_middleware(std::shared_ptr<lifetime> state) :
        state_(std::move(state)) {
        if (state_->fail_) {
            throw std::runtime_error("constructor failure");
        }
        ++state_->live_;
    }
    ~counted_middleware() {
        --state_->live_;
        ++state_->destroyed_;
    }
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next) {
        co_await next();
    }
    std::shared_ptr<lifetime> state_;
};

ruvia::task<ruvia::http_response> parameter(ruvia::context& context) {
    co_return context.text(context.req().param("id").value_or("missing"));
}

struct upgrade_marker final {
    static inline unsigned calls_{};
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        ++calls_;
        context.draft().header("X-Upgrade-Middleware", "ran");
        co_await next();
    }
};

struct visit_counter final {
    unsigned visits_{};
};

ruvia::task<void> echo_websocket(ruvia::context& context) {
    const auto room = context.req().param("room").value_or("missing");
    while (auto message = co_await context.websocket().read()) {
        std::string echoed(room);
        echoed.append(":");
        echoed.append(message->payload());
        co_await context.websocket().text(std::move(echoed));
    }
}

ruvia::task<void> echo_tunnel(ruvia::context& context) {
    auto& tunnel = context.tunnel();
    while (auto chunk = co_await tunnel.read()) {
        co_await tunnel.write(*chunk);
    }
    co_await tunnel.finish();
}

ruvia::task<void> echo_udp(ruvia::context& context) {
    EXPECT_EQ(context.req().param("host").value_or(""), "echo.test");
    auto channel = context.tunnel().udp();
    while (auto datagram = co_await channel.read()) {
        co_await channel.send(datagram->payload());
    }
    co_await channel.finish();
}

ruvia::task<void> check(ruvia::http_client& client, std::string_view method, std::string_view path, ruvia::http_status_code status, std::string_view body) {
    auto response = co_await client.client().request(method, path).send();
    EXPECT_EQ(response.status(), status);
    const auto content = co_await response.text();
    EXPECT_EQ(content.text(), body);
}

}  // namespace public_router_test

TEST(Ruvia, public_router_dispatches_parameters_bodies_streams_methods_and_middleware) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::router routes(attachment.loop().worker());
        unsigned errors = 0;
        unsigned single_shot_calls = 0;
        unsigned connection_failures = 0;
        routes.use_at<scoped_header>({.prefix_ = "/api"});
        routes.use<unmatched_header>();
        routes.on_error([&](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            ++errors;
            context.draft().status(error.status());
            co_return context.text("router error");
        });
        routes.add("GET", "/api/:id", &parameter);
        routes.add("GET", "/api/literal", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("literal");
        });
        routes.add("GET", "/api/files/*", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(context.req().param("*").value());
        });
        routes.add("GET", "/api/url/:id", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(context.url_for("/api/:id", {context.req().param("id").value()}));
        });
        routes.add<ruvia::body_limit<4>>("POST", "/api/body", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(co_await context.req().text());
        });
        routes.stream("GET", "/api/stream/:id", [](ruvia::context& context) -> ruvia::task<void> {
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write(context.req().param("id").value());
            }
        });
        routes.stream<ruvia::stream_body, ruvia::body_limit<4>>("POST", "/api/upload", [](ruvia::context& context) -> ruvia::task<void> {
            std::string content;
            while (auto chunk = co_await context.req().body_reader().text()) {
                content.append(*chunk);
            }
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write(std::move(content));
            }
        });
        routes.add("GET", "/api/fail", [](ruvia::context&) -> ruvia::task<ruvia::http_response> {
            throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
            co_return ruvia::http_response{};
        });
        routes.stream("GET", "/api/stream-fail", [](ruvia::context&) -> ruvia::task<void> {
            throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
            co_return;
        });
        routes.stream("GET", "/api/committed-fail", [](ruvia::context& context) -> ruvia::task<void> {
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write("partial");
                throw std::runtime_error("failure after commitment");
            }
        });
        routes.add<double_next>("GET", "/api/twice", [&](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            ++single_shot_calls;
            co_return context.text("once");
        });
        routes.add<ruvia::rate_limit<1, 60'000>>("GET", "/api/limited", &parameter);
        routes.add("PROPFIND", "/extension", &parameter);
        routes.freeze();
        EXPECT_THROW(routes.add("GET", "/late", &parameter), std::logic_error);

        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) {
            return routes.dispatch(stream);
        }, .connection_failure_ = [&](const ruvia::connection_failure_record&) noexcept {
            ++connection_failures;
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            co_await check(client, "GET", "/api/hello%20world", ruvia::http_status::ok, "hello world");
            co_await check(client, "GET", "/api/%6citeral", ruvia::http_status::ok, "literal");
            co_await check(client, "GET", "/api/files/a/b", ruvia::http_status::ok, "a/b");
            co_await check(client, "GET", "/api/url/a%2Fb", ruvia::http_status::ok, "/api/a%2Fb");
            co_await check(client, "GET", "/api/stream/value", ruvia::http_status::ok, "value");
            co_await check(client, "HEAD", "/api/value", ruvia::http_status::ok, "");
            co_await check(client, "HEAD", "/api/stream/value", ruvia::http_status::ok, "");
            for (const auto method : {"OPTIONS", "PUT"}) {
                auto response = co_await client.client().request(method, "/api/stream/value").send();
                EXPECT_EQ(response.status(), std::string_view(method) == "OPTIONS" ? ruvia::http_status::no_content : ruvia::http_status::method_not_allowed);
                const auto allow = response.header("Allow").value_or("");
                EXPECT_NE(allow.find("GET"), std::string_view::npos);
                EXPECT_NE(allow.find("HEAD"), std::string_view::npos);
                EXPECT_EQ(response.header("X-All-Routes").value_or(""), "present");
                EXPECT_FALSE(response.header("X-Router").has_value());
                (void)co_await response.text();
            }
            for (const auto path : {"/api/body", "/api/upload"}) {
                auto response = co_await client.client().post(path).body("four").send();
                EXPECT_EQ(response.status(), ruvia::http_status::ok);
                EXPECT_EQ(response.header("X-Router").value_or(""), "scoped");
                const auto content = co_await response.text();
                EXPECT_EQ(content.text(), "four");
                auto oversized = co_await client.client().post(path).body("large").send();
                EXPECT_EQ(oversized.status(), ruvia::http_status::content_too_large);
                const auto rejected = co_await oversized.text();
                EXPECT_EQ(rejected.text(), "router error");
            }
            co_await check(client, "GET", "/api/fail", ruvia::http_status::conflict, "router error");
            co_await check(client, "GET", "/api/stream-fail", ruvia::http_status::conflict, "router error");
            auto misuse = co_await client.client().get("/api/twice").send();
            EXPECT_EQ(misuse.status(), ruvia::http_status::internal_server_error);
            const auto misuse_body = co_await misuse.text();
            EXPECT_NE(misuse_body.text().find("next_called_multiple_times"), std::string_view::npos);
            EXPECT_EQ(single_shot_calls, 1);
            co_await check(client, "GET", "/api/limited", ruvia::http_status::ok, "missing");
            co_await check(client, "GET", "/api/limited", ruvia::http_status::too_many_requests, "router error");
            co_await check(client, "PROPFIND", "/extension", ruvia::http_status::ok, "missing");
            co_await check(client, "UNREGISTERED", "/extension", ruvia::http_status::not_implemented, "router error");
            auto absent = co_await client.client().get("/apix/missing").send();
            EXPECT_EQ(absent.status(), ruvia::http_status::not_found);
            EXPECT_FALSE(absent.header("X-Router").has_value());
            (void)co_await absent.text();
            EXPECT_GE(errors, 7);
            bool truncated = false;
            try {
                auto response = co_await client.client().get("/api/committed-fail").send();
                (void)co_await response.text();
            } catch (const ruvia::http_client_error&) {
                truncated = true;
            }
            EXPECT_TRUE(truncated);
            EXPECT_EQ(connection_failures, 1);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_freeze_rejects_ambiguity_and_rolls_back_middleware_lifetimes) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        for (const auto other : {"/items/:second", "/%69tems/:first"}) {
            ruvia::router conflict(worker);
            conflict.add("GET", "/items/:first", &parameter);
            conflict.add("GET", other, &parameter);
            EXPECT_THROW(conflict.freeze(), std::invalid_argument);
        }
        ruvia::router extension(worker);
        extension.add("PROPFIND", "/items/:id", &parameter);
        EXPECT_THROW(extension.freeze(), std::invalid_argument);
        auto first = std::make_shared<lifetime>();
        auto second = std::make_shared<lifetime>();
        second->fail_ = true;
        {
            ruvia::router routes(worker);
            routes.use<counted_middleware>(first);
            routes.use<counted_middleware>(second);
            routes.add("GET", "/items/:id", &parameter);
            EXPECT_THROW(routes.freeze(), std::runtime_error);
            EXPECT_EQ(first->live_, 0);
            EXPECT_EQ(first->destroyed_, 1);
            EXPECT_EQ(second->live_, 0);
            second->fail_ = false;
            routes.freeze();
            EXPECT_EQ(first->live_, 1);
            EXPECT_EQ(second->live_, 1);
            EXPECT_THROW(routes.freeze(), std::logic_error);
        }
        EXPECT_EQ(first->live_, 0);
        EXPECT_EQ(first->destroyed_, 2);
        EXPECT_EQ(second->live_, 0);
        EXPECT_EQ(second->destroyed_, 1);
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_retains_cold_dispatch_after_facade_destruction_and_joins_cancellation) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket peer(io);
    peer.connect(acceptor.local_endpoint());
    auto accepted = acceptor.accept();
    const std::string request = "GET /wait HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(peer, asio::buffer(request));
    auto state = std::make_shared<lifetime>();
    bool cancelled = false;
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        ruvia::worker_signal entered(worker);
        auto routes = std::make_unique<ruvia::router>(worker);
        routes->use<counted_middleware>(state);
        routes->add("GET", "/wait", [&](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            entered.notify();
            cancelled = co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token()) == ruvia::timer_sleep_result::stop_requested;
            co_return context.text("cancelled");
        });
        routes->freeze();
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            auto pending = routes->dispatch(stream);
            routes.reset();
            EXPECT_EQ(state->live_, 1);
            co_await std::move(pending);
        }});
        co_await server.add_connection_service({.address_ = "127.0.0.1", .http_ = acceptor.local_endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await server.start();
        server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.release());
        co_await entered.wait();
        EXPECT_EQ(routes, nullptr);
        EXPECT_EQ(state->live_, 1);
        co_await server.shutdown();
        EXPECT_TRUE(cancelled);
        EXPECT_EQ(state->live_, 0);
        EXPECT_EQ(state->destroyed_, 1);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_applies_host_representation_policies_and_its_rate_limit) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        {
            ruvia::router invalid(worker);
            EXPECT_THROW(invalid.rate_limit({.rule_ = {.max_requests_ = 0, .window_ = 1s}}), std::invalid_argument);
            EXPECT_THROW(invalid.rate_limit({.rule_ = {.max_requests_ = 1, .window_ = 1s}, .capacity_per_worker_ = 3}), std::invalid_argument);
        }
        static const std::string text_body(4096, 'r');
        ruvia::router routes(worker);
        routes.rate_limit({.rule_ = {.max_requests_ = 4, .window_ = 60s}, .capacity_per_worker_ = 64});
        routes.on_error([](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            context.draft().status(error.status());
            co_return context.text("router error");
        });
        routes.add("GET", "/text", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text(text_body);
        });
        routes.stream("GET", "/stream", [](ruvia::context& context) -> ruvia::task<void> {
            context.draft().header("Content-Type", "text/plain");
            if (co_await context.stream().commit() == ruvia::stream_commit_result::body) {
                co_await context.stream().write(std::string_view(text_body));
            }
        });
        routes.freeze();
        EXPECT_THROW(routes.rate_limit({.rule_ = {.max_requests_ = 1, .window_ = 1s}}), std::logic_error);

        ruvia::http_server server(attachment.loop(),
            {.request_stream_ =
                    [&](ruvia::request_stream& stream) {
            return routes.dispatch(stream);
        }},
            {.compression_ = ruvia::compression_config{.min_bytes_ = 16}, .cors_ = ruvia::http_cors_policy_config{}});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            for (const auto path : {"/text", "/stream"}) {
                auto response = co_await client.client().get(path).header("Origin", "https://site.test").header("Accept-Encoding", "gzip").configure({.decode_content_ = false}).send();
                EXPECT_EQ(response.status(), ruvia::http_status::ok) << path;
                EXPECT_EQ(response.header("Content-Encoding").value_or(""), "gzip") << path;
                EXPECT_EQ(response.header("Access-Control-Allow-Origin").value_or(""), "*") << path;
                const auto encoded = co_await response.text();
                EXPECT_LT(encoded.text().size(), text_body.size()) << path;
            }
            auto preflight = co_await client.client().request("OPTIONS", "/text").header("Origin", "https://site.test").header("Access-Control-Request-Method", "GET").send();
            EXPECT_EQ(preflight.status(), ruvia::http_status::no_content);
            EXPECT_EQ(preflight.header("Access-Control-Allow-Origin").value_or(""), "*");
            (void)co_await preflight.text();
            co_await check(client, "GET", "/text", ruvia::http_status::ok, text_body);
            auto limited = co_await client.client().get("/missing").send();
            EXPECT_EQ(limited.status(), ruvia::http_status::too_many_requests);
            EXPECT_TRUE(limited.header("Retry-After").has_value());
            const auto limited_body = co_await limited.text();
            EXPECT_EQ(limited_body.text(), "router error");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_routes_websocket_and_connect_upgrades_over_http1_and_http2) {
    using namespace public_router_test;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::router routes(attachment.loop().worker());
        unsigned errors = 0;
        routes.on_error([&](ruvia::context& context, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            ++errors;
            context.draft().status(error.status());
            co_return context.text("router error");
        });
        routes.websocket<upgrade_marker>("/ws/:room", &echo_websocket);
        routes.connect<upgrade_marker>("echo.test:443", &echo_tunnel);
        routes.connect_protocol<upgrade_marker>("connect-udp", "/.well-known/masque/udp/:host/:port/", &echo_udp);
        routes.freeze();

        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) {
            return routes.dispatch(stream);
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        std::exception_ptr failure;
        try {
            co_await server.start();
            for (const auto protocol : {ruvia::http_client_versions{.http2_ = false}, ruvia::http_client_versions{.http1_ = false}}) {
                const std::string label = protocol == ruvia::http_client_versions{.http2_ = false} ? "HTTP/1.1" : "h2c";
                ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = protocol});
                std::exception_ptr exchange_failure;
                try {
                    upgrade_marker::calls_ = 0;
                    {
                        auto session = co_await client.client().websocket("/ws/lobby").connect();
                        co_await session.text("hello");
                        const auto message = co_await session.read();
                        EXPECT_TRUE(message.has_value()) << label;
                        if (message) {
                            EXPECT_EQ(message->payload(), "lobby:hello") << label;
                        }
                        co_await session.close();
                        EXPECT_FALSE(co_await session.read()) << label;
                    }
                    {
                        auto opened = co_await client.client().connect("echo.test:443").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << label;
                        if (tunnel != nullptr) {
                            EXPECT_EQ(tunnel->status().value(), 200) << label;
                            co_await tunnel->write(std::string_view("tunnel bytes"));
                            co_await tunnel->finish();
                            std::string echoed;
                            while (auto chunk = co_await tunnel->text()) {
                                echoed.append(*chunk);
                            }
                            EXPECT_EQ(echoed, "tunnel bytes") << label;
                        }
                    }
                    {
                        auto opened = co_await client.client().udp("/.well-known/masque/udp/echo.test/53/").open();
                        auto* tunnel = opened.tunnel();
                        EXPECT_TRUE(tunnel != nullptr) << label;
                        if (tunnel != nullptr) {
                            auto channel = std::move(*tunnel).udp();
                            co_await channel.send(std::string_view("datagram"));
                            const auto packet = co_await channel.read();
                            EXPECT_TRUE(packet.has_value()) << label;
                            if (packet) {
                                EXPECT_EQ(packet->text(), "datagram") << label;
                            }
                            co_await channel.finish();
                            EXPECT_FALSE((co_await channel.read()).has_value()) << label;
                        }
                    }
                    EXPECT_EQ(upgrade_marker::calls_, 3U) << label;
                    {
                        auto refused = co_await client.client().connect("other.test:443").open();
                        auto* response = refused.response();
                        EXPECT_TRUE(response != nullptr) << label;
                        if (response != nullptr) {
                            EXPECT_FALSE(response->status().is_successful()) << label;
                            (void)co_await response->text();
                        }
                    }
                    const auto before = errors;
                    auto plain = co_await client.client().get("/ws/lobby").send();
                    EXPECT_EQ(plain.status(), ruvia::http_status::bad_request) << label;
                    const auto plain_body = co_await plain.text();
                    EXPECT_EQ(plain_body.text(), "router error") << label;
                    EXPECT_EQ(errors, before + 1) << label;
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) {
                    std::rethrow_exception(exchange_failure);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, public_router_lends_borrowed_clients_and_state_to_session_middleware) {
    using namespace public_router_test;
    ruvia::test::redis_session_store_peer peer;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto& worker = attachment.loop().worker();
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::redis_client redis(attachment.loop(), {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = 2s, .command_timeout_ = 2s});
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        visit_counter visits;
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::router routes(worker);
            routes.bind_redis("default", redis).bind_http_client("default", client).bind_worker_state(visits);
            EXPECT_THROW(routes.bind_redis("default", redis), std::invalid_argument);
            EXPECT_THROW(routes.bind_http_client("default", client), std::invalid_argument);
            EXPECT_THROW(routes.bind_worker_state(visits), std::invalid_argument);
            routes.use<ruvia::session_middleware>(ruvia::session_config{.cookie_name_ = "sid"});
            routes.add("GET", "/set", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                ++context.worker_state<visit_counter>().visits_;
                context.session().set("private");
                co_return context.text("set");
            });
            routes.add("GET", "/read", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                ++context.worker_state<visit_counter>().visits_;
                auto value = context.session();
                co_return context.text(value.payload().empty() ? "empty" : value.payload());
            });
            routes.add("GET", "/rotate", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                context.session().regenerate();
                co_return context.text("rotate");
            });
            routes.add("GET", "/plain", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                co_return context.text("plain");
            });
            routes.add("GET", "/relay", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
                auto upstream = co_await context.client().get("/plain").send();
                const auto content = co_await upstream.text();
                const auto stored = co_await context.redis().get("absent");
                co_return context.text(std::string(content.text()) + (stored ? ":stored" : ":absent"));
            });
            routes.freeze();
            EXPECT_THROW(routes.bind_redis("other", redis), std::logic_error);

            ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) {
                return routes.dispatch(stream);
            }});
            server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
            std::exception_ptr serving_failure;
            try {
                co_await server.start();
                const auto request = [&](std::string_view path, std::string_view cookie) -> ruvia::task<std::pair<std::string, std::string>> {
                    auto builder = client.client().get(path);
                    if (!cookie.empty()) {
                        builder.header("Cookie", cookie);
                    }
                    auto response = co_await builder.send();
                    EXPECT_EQ(response.status(), ruvia::http_status::ok) << path;
                    std::string set_cookie(response.header("Set-Cookie").value_or(""));
                    set_cookie = set_cookie.substr(0, set_cookie.find(';'));
                    const auto content = co_await response.text();
                    co_return std::pair<std::string, std::string>(std::string(content.text()), std::move(set_cookie));
                };
                const auto [created_body, created] = co_await request("/set", {});
                EXPECT_EQ(created_body, "set");
                EXPECT_TRUE(created.starts_with("sid="));
                EXPECT_EQ(created.size(), std::size_t{4 + 48});
                EXPECT_EQ((co_await request("/read", created)).first, "private");
                const auto [rotated_body, rotated] = co_await request("/rotate", created);
                EXPECT_EQ(rotated_body, "rotate");
                EXPECT_TRUE(rotated.starts_with("sid="));
                EXPECT_NE(rotated, created);
                EXPECT_EQ((co_await request("/read", created)).first, "empty");
                EXPECT_EQ((co_await request("/read", rotated)).first, "private");
                EXPECT_EQ(visits.visits_, 4U);
                EXPECT_EQ((co_await request("/relay", {})).first, "plain:absent");
            } catch (...) {
                serving_failure = std::current_exception();
            }
            co_await server.shutdown();
            if (serving_failure) {
                std::rethrow_exception(serving_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await redis.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}
#endif

// A buffered body read through the router is bounded per call on every
// protocol: a timeout or stop ends only that call, the bytes that arrived stay
// buffered, and the next call returns the whole body.
TEST(Ruvia, public_router_buffered_body_reads_honour_their_operation) {
    using namespace std::chrono_literals;
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto run = [&]() -> ruvia::task<void> {
        unsigned phase = 0;
        unsigned rejected = 0;
        unsigned prestopped = 0;
        unsigned interruptions = 0;
        const auto code_of = [](auto operation) -> ruvia::task<std::error_code> {
            try {
                (void)co_await std::move(operation);
            } catch (const std::system_error& error) {
                co_return error.code();
            }
            co_return std::error_code{};
        };
        ruvia::router routes(attachment.loop().worker());
        routes.add("POST", "/buffered", [&](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            const auto request = context.req();
            for (const auto attempt : {0, 1, 2}) {
                try {
                    if (attempt == 0) {
                        (void)request.text({.timeout_ = 0ms});
                    } else if (attempt == 1) {
                        (void)request.bytes({.timeout_ = -1ms});
                    } else {
                        (void)request.discard_body({.timeout_ = 0ms});
                    }
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
            }
            ruvia::stop_source stopped;
            stopped.request_stop();
            prestopped += (co_await code_of(request.text({.stop_token_ = stopped.token()}))) == std::errc::operation_canceled;
            interruptions += (co_await code_of(request.text({.timeout_ = 50ms}))) == std::errc::timed_out;
            phase = 1;
            ruvia::stop_source stop;
            ruvia::task_scope stopper(context.worker());
            const auto stop_later = [&]() -> ruvia::task<void> {
                (void)co_await ruvia::sleep_for(context.worker(), 50ms);
                std::thread([&stop] {
                    stop.request_stop();
                }).join();
            };
            stopper.spawn(stop_later());
            interruptions += (co_await code_of(request.bytes({.stop_token_ = stop.token()}))) == std::errc::operation_canceled;
            co_await stopper.join();
            phase = 2;
            co_return context.text(co_await request.text());
        });
        routes.freeze();
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) {
            return routes.dispatch(stream);
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        struct target final {
            std::string base_url_;
            ruvia::http_client_versions versions_;
            std::string label_;
        };
        std::vector<target> targets{{"http://127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false}, "HTTP/1"}};
#if defined(RUVIA_ENABLE_HTTP2)
        targets.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false}, "HTTP/2"});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
        targets.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true}, "HTTP/3"});
#endif
        std::exception_ptr failure;
        try {
            co_await server.start();
            for (const auto& entry : targets) {
                ruvia::http_client_config config{.base_url_ = entry.base_url_, .versions_ = entry.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                config.request_timeout_ = 10s;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    const auto await_phase = [&](unsigned expected) -> ruvia::task<void> {
                        for (unsigned count = 0; phase != expected && count != 5000; ++count) {
                            (void)co_await ruvia::sleep_for(client.worker(), 1ms);
                        }
                        EXPECT_EQ(phase, expected) << entry.label_;
                    };
                    phase = 0;
                    auto exchange = co_await client.client().post("/buffered").upload({});
                    co_await exchange.body().write("abc");
                    co_await await_phase(1);
                    co_await exchange.body().write("def");
                    co_await await_phase(2);
                    co_await exchange.body().write("ghi");
                    co_await exchange.body().end();
                    auto response = co_await exchange.response();
                    EXPECT_EQ(response.status(), ruvia::http_status::ok) << entry.label_;
                    const auto content = co_await response.text();
                    EXPECT_EQ(content.text(), "abcdefghi") << entry.label_;
                } catch (...) {
                    exchange_failure = std::current_exception();
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_EQ(rejected, 3 * targets.size());
        EXPECT_EQ(prestopped, targets.size());
        EXPECT_EQ(interruptions, 2 * targets.size());
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, public_router_binds_http_clients_by_its_own_unique_aliases) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client first(attachment.loop(), {.base_url_ = origin, .versions_ = {.http2_ = false}});
        ruvia::http_client second(attachment.loop(), {.base_url_ = origin, .versions_ = {.http2_ = false}});
        ruvia::router routes(attachment.loop().worker());
        routes.bind_http_client("one", first).bind_http_client("two", second).bind_http_client("default", first);
        EXPECT_THROW(routes.bind_http_client("one", second), std::invalid_argument);
        EXPECT_THROW(routes.bind_http_client("", first), std::invalid_argument);
        routes.add("GET", "/one", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("one");
        });
        routes.add("GET", "/two", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("two");
        });
        routes.add("GET", "/relay", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            auto one = co_await context.client("one").get("/one").send();
            const auto first = co_await one.text();
            auto two = co_await context.client("two").get("/two").send();
            const auto second = co_await two.text();
            co_return context.text(std::string(first.text()) + ":" + std::string(second.text()));
        });
        routes.freeze();
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) {
            return routes.dispatch(stream);
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await server.start();
            auto response = co_await first.client().get("/relay").send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            const auto content = co_await response.text();
            EXPECT_EQ(content.text(), "one:two");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await first.shutdown();
        co_await second.shutdown();
        co_await server.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}
