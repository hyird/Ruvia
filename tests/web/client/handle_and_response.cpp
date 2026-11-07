#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/model.h"
#include "ruvia/web/streaming.h"

#include "tcp_port_reservation.h"
#include "test_exceptions.h"
#include "test_io_context.h"
#include "test_tls_identity.h"

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

TEST(Ruvia, http_client_handle_origin_and_expired_send_reject_after_shutdown) {
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
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_response_and_body_moved_from_throw_logic_error) {
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
        const std::string client_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
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
    root.get();
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

struct redirect_destinations final {
    std::string source_;
    std::string destination_;
    std::string secure_;
    std::string unregistered_;
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
    RUVIA_GET("/fluent/pipe", pipe);
    RUVIA_GET_WS("/fluent/websocket", websocket_echo,
        (ruvia::websocket_route_config{.subprotocols_ = {"echo.v1"}, .require_subprotocol_ = true, .deflate_ = {.enabled_ = true}}));
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> inspect(ruvia::context& c) {
        const auto body = co_await c.req().bytes();
        fluent_client_observation observed({.resource_ = c.arena()});
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
        co_return c.json(observed);
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        (void)co_await c.req().bytes();
        c.draft().status(ruvia::http_status_code::from_value(c.req().query<std::uint16_t>("status").value_or(302)));
        c.draft().header("location", c.req().query("location").value_or("/fluent/inspect"));
        co_return c.text("redirect");
    }

    ruvia::task<ruvia::http_response> registered_redirect(ruvia::context& c) {
        const auto& targets = *c.worker_state<std::shared_ptr<redirect_destinations>>();
        const auto mode = c.req().query("mode").value_or("cross");
        const auto& target = mode == "unregistered" ? targets.unregistered_ : targets.destination_;
        auto origin = c.client(mode == "downgrade" ? "secure" : "source");
        auto response = co_await origin.post("/fluent/redirect")
                            .query("location", target + "/fluent/inspect")
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
        targets_->source_ = "http://127.0.0.1:" + std::to_string(source_port_.endpoint().port());
        targets_->destination_ = "http://127.0.0.1:" + std::to_string(destination_port_.endpoint().port());
        targets_->secure_ = "https://localhost:" + std::to_string(secure_port_.endpoint().port());
        targets_->unregistered_ = "http://127.0.0.1:" + std::to_string(unknown_port_.endpoint().port());
        const auto key = identity_.ca_file_.parent_path() / "key.pem";
        {
            std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(key.string().c_str(), "w"), BIO_free);
            if (!output || ruvia::test::write_tls_private_key(output.get(), SSL_CTX_get0_privatekey(identity_.context_.native_handle())) != 1) {
                throw std::runtime_error("cannot write fluent client test TLS key");
            }
        }
        const std::array listeners{
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = source_port_.endpoint().port(), .https_ = secure_port_.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}}},
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = destination_port_.endpoint().port()}};
        auto& app = ruvia::app();
        app.server({.worker_count_ = 1})
            .listen(std::span<const ruvia::listen_config>(listeners))
            .client({.alias_ = "source", .base_url_ = targets_->source_, .request_timeout_ = std::chrono::seconds(3), .protocol_ = ruvia::http_client_protocol::http1_only, .cookies_ = {{"configured", "source-secret"}}})
            .client({.alias_ = "destination", .base_url_ = targets_->destination_, .request_timeout_ = std::chrono::seconds(3), .protocol_ = ruvia::http_client_protocol::http1_only})
            .client({.alias_ = "secure", .base_url_ = targets_->secure_, .request_timeout_ = std::chrono::seconds(3), .protocol_ = ruvia::http_client_protocol::http1_only, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}});
        app.use_worker_state<std::shared_ptr<redirect_destinations>>([targets = targets_] { return targets; });
        app.on_start([this] {
            announced_.store(true);
            ready_.set_value();
        });
        auto ready = ready_.get_future();
        thread_ = std::thread([this] {
            try {
                ruvia::app().run();
            } catch (...) {
                failure_ = std::current_exception();
                if (!announced_.load()) {
                    ready_.set_exception(failure_);
                }
            }
        });
        try {
            ready.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    ~server() {
        stop();
    }

    const redirect_destinations& targets() const noexcept {
        return *targets_;
    }

    void finish() {
        stop();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void stop() {
        if (thread_.joinable()) {
            ruvia::app().stop();
            thread_.join();
        }
    }

    asio::io_context io_;
    ruvia::test::tcp_port_reservation source_port_;
    ruvia::test::tcp_port_reservation destination_port_;
    ruvia::test::tcp_port_reservation secure_port_;
    ruvia::test::tcp_port_reservation unknown_port_;
    ruvia::test::tls_identity identity_;
    std::shared_ptr<redirect_destinations> targets_;
    std::promise<void> ready_;
    std::atomic_bool announced_{false};
    std::exception_ptr failure_;
    std::thread thread_;
};

}  // namespace fluent_client_tests

TEST(Ruvia, http_client_fluent_ownership_helpers_json_and_registered_redirects) {
    fluent_client_tests::server server;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = server.targets().source_,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .protocol_ = ruvia::http_client_protocol::http1_only});
        try {
            const auto origin = client.client();
            auto cold = [&] {
                std::string path = "/fluent/inspect";
                std::string name = "x-owned";
                std::string value = "owned-value";
                std::string query_name = "escaped name";
                std::string query_value = "a &b=+/%?#\xc3\xa9";
                std::string span_value = "span-value";
                std::string payload = "owned-payload";
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
            EXPECT_TRUE(ruvia::testing::throws_on([&] {
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

            using method_factory = ruvia::http_client_request (ruvia::http_client_handle::*)(std::string_view) const;
            const std::array<std::pair<std::string_view, method_factory>, 7> methods{{
                {"GET", &ruvia::http_client_handle::get},
                {"POST", &ruvia::http_client_handle::post},
                {"PUT", &ruvia::http_client_handle::put},
                {"PATCH", &ruvia::http_client_handle::patch},
                {"DELETE", &ruvia::http_client_handle::del},
                {"HEAD", &ruvia::http_client_handle::head},
                {"OPTIONS", &ruvia::http_client_handle::options},
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
                     ruvia::http_client_protocol::http1_only,
#if defined(RUVIA_ENABLE_HTTP2)
                     ruvia::http_client_protocol::http2_only,
#endif
                 }) {
                ruvia::http_client websocket_client(attachment.loop(),
                    {.base_url_ = server.targets().source_,
                        .request_timeout_ = std::chrono::seconds(1),
                        .protocol_ = protocol});
                std::exception_ptr websocket_failure;
                try {
                    auto connecting = [&] {
                        const std::string path = "/fluent/websocket";
                        const std::string value = "pooled";
                        const std::string subprotocol = "echo.v1";
                        const std::array fields{ruvia::http_header_view{"x-owned", value}};
                        return websocket_client.client().websocket(path).headers(fields).subprotocol(subprotocol).deflate({.compression_ = {.enabled_ = true}}).connect();
                    }();
                    auto connected = co_await std::move(connecting);
                    auto session = std::move(connected);
                    EXPECT_EQ(session.subprotocol(), "echo.v1");
                    EXPECT_TRUE(session.protocol_version() ==
                                (protocol == ruvia::http_client_protocol::http1_only
                                        ? ruvia::http_protocol_version::http11
                                        : ruvia::http_protocol_version::http2));
                    // Accepted WebSockets are not ordinary finite HTTP responses.
                    // The HTTP exchange deadline must not become their lifetime.
                    EXPECT_TRUE(co_await ruvia::sleep_for(session.worker(), std::chrono::milliseconds(1250)) ==
                                ruvia::timer_sleep_result::elapsed);
                    EXPECT_TRUE(session.connected());
                    const std::string payload(32768, 'w');
                    for (const bool compress : {false, true}) {
                        auto writing = [&] {
                            const std::string owned = payload;
                            return compress ? session.send(std::as_bytes(std::span(owned)), {.compress_ = true})
                                            : session.send(owned, {.compress_ = false});
                        }();
                        co_await std::move(writing);
                        const auto message = co_await session.receive();
                        EXPECT_TRUE(message.has_value());
                        if (message) {
                            EXPECT_EQ(message->payload(), payload);
                            EXPECT_TRUE(message->binary() == compress);
                            EXPECT_TRUE(message->text() != compress);
                        }
                    }
                    co_await session.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "pooled"});
                    EXPECT_TRUE(!(co_await session.receive()));
                    {
                        auto timed = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                        bool timed_out = false;
                        try {
                            (void)co_await timed.receive({.timeout_ = std::chrono::milliseconds(20)});
                        } catch (const ruvia::http_client_error& error) {
                            timed_out = error.code() == ruvia::http_client_error::code_type::timeout;
                        }
                        EXPECT_TRUE(timed_out);
                        EXPECT_TRUE(!timed.connected());
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        bool rejected = false;
                        try {
                            co_await limited.send(std::string(33, 'm'));
                        } catch (const ruvia::http_client_error& error) {
                            rejected = error.code() == ruvia::http_client_error::code_type::message_too_large;
                        }
                        EXPECT_TRUE(rejected);
                        EXPECT_TRUE(!limited.connected());
                        limited.abort();
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket?oversize=1").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        bool rejected = false;
                        try {
                            (void)co_await limited.receive();
                        } catch (const ruvia::http_client_error& error) {
                            rejected = error.code() == ruvia::http_client_error::code_type::protocol_error;
                        }
                        EXPECT_TRUE(rejected);
                        limited.abort();
                    }
                    auto active = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                    const auto worker = attachment.loop().handle();
                    ruvia::task_scope readers(worker);
                    bool cancelled = false;
                    const auto receive = [&]() -> ruvia::task<void> {
                        try {
                            (void)co_await active.receive();
                        } catch (const ruvia::http_client_error& error) {
                            cancelled = error.code() == ruvia::http_client_error::code_type::closing ||
                                        error.code() == ruvia::http_client_error::code_type::cancelled;
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
    root.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_unconsumed_builders_outlive_shutdown_and_client_storage) {
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
                const std::string path = "/" + std::string(2048, 'p');
                const std::string value(16384, 'v');
                const std::string body(32768, 'b');
                const std::string authority = std::string(63, 'a') + "." + std::string(63, 'b') + ".test:443";
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
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
