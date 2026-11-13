#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/model.h"
#include "ruvia/web/streaming.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_exceptions.h"
#include "test_io_context.h"
#include "test_tls_identity.h"
#include "worker_task_fixture.h"

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

// An independent legacy server, with an absolute deadline covering accept,
// TLS and HTTP. It never uses the production TLS policy under test.
class legacy_http_peer final {
public:
    explicit legacy_http_peer(asio::ssl::context& context)
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          deadline_(io_),
          context_(context),
          port_(acceptor_.local_endpoint().port()),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~legacy_http_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            deadline_.cancel();
        });
        finish();
    }

    void finish() {
        if (done_.valid()) {
            try {
                done_.get();
            } catch (const std::system_error&) {
                // Handshake rejection or connection close is expected in
                // negative cases; assertions below inspect all received data.
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::string url() const {
        return "https://127.0.0.1:" + std::to_string(port_);
    }

    std::string request_;
    int tls_version_{};
    bool timed_out_{};

private:
    asio::awaitable<void> serve() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                timed_out_ = true;
                std::error_code ignored;
                acceptor_.close(ignored);
                socket_.close(ignored);
            }
        });
        struct cancel_deadline final {
            asio::steady_timer& timer_;
            ~cancel_deadline() {
                timer_.cancel();
            }
        } guard{deadline_};
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, context_);
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        tls_version_ = SSL_version(stream.native_handle());
        std::error_code error;
        co_await asio::async_read_until(stream, asio::dynamic_buffer(request_),
            "\r\n\r\n", asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nlegacy";
        co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, error));
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer deadline_;
    asio::ssl::context& context_;
    std::uint16_t port_;
    std::future<void> done_;
    std::thread thread_;
};

void exercise_legacy_http_peer(int version, ruvia::http_client_versions protocol,
    bool select_h2, bool matching_san = true, bool elliptic_curve = false) {
    ruvia::test::tls_identity identity("server.example",
        matching_san ? "DNS:server.example" : "DNS:other.example", elliptic_curve);
    auto* context = identity.context_.native_handle();
    ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
    ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
    ASSERT_EQ(SSL_CTX_set_cipher_list(context,
                  "ECDHE-RSA-AES128-SHA:ECDHE-ECDSA-AES128-SHA"),
        1);
    // Only the independent fixture allows legacy handshake signatures this
    // way. Trust and SAN verification remain enabled in the real client.
    SSL_CTX_set_security_level(context, 0);
    if (select_h2) {
        SSL_CTX_set_alpn_select_cb(context, [](SSL*, const unsigned char** selected, unsigned char* size, const unsigned char*, unsigned int, void*) -> int {
                static constexpr unsigned char h2[] = {'h', '2'};
                *selected = h2;
                *size = sizeof(h2);
                return SSL_TLSEXT_ERR_OK; }, nullptr);
    }
    legacy_http_peer peer(identity.context_);
    const auto url = peer.url();
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::http_client client(pool.loop(0),
        {.base_url_ = url,
            .connect_timeout_ = std::chrono::seconds(2),
            .request_timeout_ = std::chrono::seconds(2),
            .versions_ = protocol,
            .tls_ = {.ca_file_ = identity.ca_file_, .server_name_ = "server.example"}});
    const bool expect_success = version == TLS1_1_VERSION && !select_h2 && matching_san &&
                                protocol != ruvia::http_client_versions{.http1_ = false};
    std::optional<ruvia::http_client_error::code_type> failure_code;
    std::exception_ptr unexpected;
    bool exchanged = false;
    const auto run = [&]() -> ruvia::task<void> {
        try {
            auto response = co_await client.client().get("/legacy").send();
            EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_TRUE(response.status() == ruvia::http_status::ok);
            const auto body = co_await response.stream().read_all();
            EXPECT_EQ(body.text(), std::string_view("legacy"));
            exchanged = true;
        } catch (const ruvia::http_client_error& error) {
            failure_code = error.code();
        } catch (...) {
            unexpected = std::current_exception();
        }
        co_await client.shutdown();
    };
    pool.start();
    pool.loop(0).start(run()).get();
    pool.join();
    peer.finish();
    if (unexpected) {
        std::rethrow_exception(unexpected);
    }
    EXPECT_FALSE(peer.timed_out_);
    EXPECT_EQ(exchanged, expect_success);
    if (expect_success) {
        EXPECT_FALSE(failure_code.has_value());
        EXPECT_EQ(peer.tls_version_, TLS1_1_VERSION);
        EXPECT_TRUE(peer.request_.starts_with("GET /legacy HTTP/1.1\r\n"));
    } else {
        EXPECT_EQ(failure_code, ruvia::http_client_error::code_type::tls_failed);
        EXPECT_TRUE(peer.request_.empty());
        if (version == TLS1_1_VERSION && select_h2 &&
            protocol == ruvia::http_client_versions{} && matching_san) {
            // The intentionally noncompliant peer really selected h2 in a
            // completed TLS 1.1 handshake, rather than failing earlier.
            EXPECT_EQ(peer.tls_version_, TLS1_1_VERSION);
        }
    }
}

}  // namespace

TEST(Ruvia, http_client_tls11_http1_and_negotiation_exchange_with_verified_san) {
    for (const auto protocol : {ruvia::http_client_versions{.http2_ = false},
             ruvia::http_client_versions{}}) {
        for (const bool elliptic_curve : {false, true}) {
            exercise_legacy_http_peer(TLS1_1_VERSION, protocol, false, true, elliptic_curve);
        }
        exercise_legacy_http_peer(TLS1_1_VERSION, protocol, false, false);
    }
}

TEST(Ruvia, http_client_rejects_tls10_and_h2_below_tls12_without_http2_bytes) {
    for (const auto protocol : {ruvia::http_client_versions{.http2_ = false},
             ruvia::http_client_versions{}}) {
        exercise_legacy_http_peer(TLS1_VERSION, protocol, false);
    }
#ifdef RUVIA_ENABLE_HTTP2
    exercise_legacy_http_peer(TLS1_1_VERSION, ruvia::http_client_versions{}, true);
    exercise_legacy_http_peer(TLS1_1_VERSION, ruvia::http_client_versions{.http1_ = false}, true);
#endif
}

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
    std::move(root).get();
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
        std::pmr::string request{ruvia::process_resource()};
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Test: ok\r\nConnection: close\r\n\r\nhi";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) { peer_failure = error; });

    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const std::pmr::string client_url("http://127.0.0.1:" + std::pmr::string(std::to_string(peer.local_endpoint().port()), ruvia::process_resource()), ruvia::process_resource());
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
    std::move(root).get();
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

std::atomic<std::size_t> host_cancel_waiters{};
std::atomic<std::size_t> host_cancelled_requests{};

struct redirect_destinations final {
    std::pmr::string source_{ruvia::process_resource()};
    std::pmr::string destination_{ruvia::process_resource()};
    std::pmr::string secure_{ruvia::process_resource()};
    std::pmr::string unregistered_{ruvia::process_resource()};
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
    RUVIA_GET("/fluent/host-cancel", host_cancel);
    RUVIA_GET("/fluent/hold", hold);
    RUVIA_GET("/fluent/pipe", pipe);
    RUVIA_GET("/fluent/pipe-bounded", pipe_bounded);
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
        c.draft().header("x-observed-user-agent", c.req().header("user-agent").value_or(""));
        if (const auto cookie = c.req().query("set-cookie")) {
            c.draft().header("set-cookie", *cookie);
        }
        co_return c.json(observed);
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        (void)co_await c.req().bytes();
        c.draft().status(ruvia::http_status_code::from_value(c.req().query<std::uint16_t>("status").value_or(302)));
        c.draft().header("location", c.req().query("location").value_or("/fluent/inspect"));
        co_return c.text("redirect");
    }

    ruvia::task<ruvia::http_response> hold(ruvia::context& c) {
        ++host_cancel_waiters;
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(5), c.stop_token());
        co_return c.text("released");
    }

    // Streaming dispatch observes downstream disconnects while the upstream waits.
    ruvia::task<void> host_cancel(ruvia::context& c) {
        ruvia::stop_source independent;
        try {
            auto response = co_await c.client("source")
                                .with_options({.timeout_ = std::chrono::milliseconds(500), .stop_token_ = independent.token()})
                                .get("/fluent/hold")
                                .send();
            (void)co_await response.bytes();
        } catch (const ruvia::http_client_error& error) {
            if (error.code() == ruvia::http_client_error::code_type::cancelled && !independent.stop_requested()) {
                ++host_cancelled_requests;
            }
        }
        if (c.stop_token().stop_requested()) {
            co_return;
        }
        auto& output = c.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::body) {
            co_await output.write("finished");
            co_await output.end();
        }
    }

    ruvia::task<ruvia::http_response> registered_redirect(ruvia::context& c) {
        const auto& targets = *c.worker_state<std::shared_ptr<redirect_destinations>>();
        const auto mode = c.req().query("mode").value_or("cross");
        const auto& target = mode == "unregistered" ? targets.unregistered_ : targets.destination_;
        auto origin = c.client(mode == "downgrade" ? "secure" : "source");
        auto response = co_await origin.post("/fluent/redirect")
                            .query("location", std::pmr::string(target, ruvia::process_resource()) + "/fluent/inspect")
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

    // An already-stopped token forwards nothing, so the bounded forward that
    // follows still commits the whole response.
    ruvia::task<void> pipe_bounded(ruvia::context& c) {
        auto response = co_await c.client("source").get("/fluent/stream-source").send();
        ruvia::stop_source stop;
        stop.request_stop();
        bool cancelled = false;
        try {
            co_await response.pipe_to(c, {.stop_token_ = stop.token()});
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
        }
        if (!cancelled || response.stream().complete()) {
            throw std::runtime_error("stopped response pipe did not fail before forwarding");
        }
        bool rejected = false;
        try {
            (void)response.pipe_to(c, {.timeout_ = std::chrono::milliseconds(0)});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("response pipe accepted a non-positive timeout");
        }
        co_await response.pipe_to(c, {.timeout_ = std::chrono::seconds(5)});
    }

    ruvia::task<void> websocket_echo(ruvia::context& c) {
        if (c.req().header("x-owned") != "pooled") {
            throw std::runtime_error("WebSocket builder did not retain its fields");
        }
        if (c.req().query("oversize")) {
            const std::pmr::string payload(64, 'm', ruvia::process_resource());
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
        targets_->source_ = "http://127.0.0.1:" + std::pmr::string(std::to_string(source_port_.endpoint().port()), ruvia::process_resource());
        targets_->destination_ = "http://127.0.0.1:" + std::pmr::string(std::to_string(destination_port_.endpoint().port()), ruvia::process_resource());
        targets_->secure_ = "https://localhost:" + std::pmr::string(std::to_string(secure_port_.endpoint().port()), ruvia::process_resource());
        targets_->unregistered_ = "http://127.0.0.1:" + std::pmr::string(std::to_string(unknown_port_.endpoint().port()), ruvia::process_resource());
        const auto& key = identity_.private_key_file();
        const std::array listeners{
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = source_port_.endpoint().port(), .https_ = secure_port_.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity_.ca_file_, .private_key_file_ = key}}},
            ruvia::listen_config{.address_ = "127.0.0.1", .http_ = destination_port_.endpoint().port()}};
        auto& app = ruvia::app();
        app.server({.worker_count_ = 1})
            .listen(std::span<const ruvia::listen_config>(listeners))
            .client({.alias_ = "source", .base_url_ = targets_->source_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}, .cookies_ = {{"configured", "source-secret"}}})
            .client({.alias_ = "destination", .base_url_ = targets_->destination_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}})
            .client({.alias_ = "secure", .base_url_ = targets_->secure_, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}});
        app.use_worker_state<std::shared_ptr<redirect_destinations>>([targets = targets_] { return targets; });
        app.on_start([this] {
            ready_.set_value();
        });
        auto ready = ready_.get_future();
        run_.emplace(app);
        ruvia::test::wait_until([&] {
            if (const auto failure = run_->failure()) {
                std::rethrow_exception(failure);
            }
            return ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        },
            std::chrono::seconds(10));
        ready.get();
    }

    ~server() = default;

    const redirect_destinations& targets() const noexcept {
        return *targets_;
    }

    ruvia::http_client_config secure_config() const {
        return {.alias_ = "secure", .base_url_ = targets_->secure_, .versions_ = ruvia::http_client_versions{.http2_ = false}, .tls_ = {.ca_file_ = identity_.ca_file_, .server_name_ = "localhost"}};
    }

    void finish() {
        ruvia::app().stop();
        run_->join();
    }

private:
    asio::io_context io_;
    ruvia::test::tcp_port_reservation source_port_;
    ruvia::test::tcp_port_reservation destination_port_;
    ruvia::test::tcp_port_reservation secure_port_;
    ruvia::test::tcp_port_reservation unknown_port_;
    ruvia::test::tls_identity identity_;
    std::shared_ptr<redirect_destinations> targets_;
    std::promise<void> ready_;
    std::optional<ruvia::test::app_run> run_;
};

// The process-wide application keeps its registrations after shutdown. Run
// these scenarios against the fluent test's one configured, live host.
void verify_standalone_origins(server& server);
void verify_request_host_cancellation(server& server);

}  // namespace fluent_client_tests

TEST(Ruvia, http_client_fluent_ownership_helpers_json_and_registered_redirects) {
    fluent_client_tests::server server;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = server.targets().source_,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            const auto origin = client.client();
            auto cold = [&] {
                std::pmr::string path("/fluent/inspect", ruvia::process_resource());
                std::pmr::string name("x-owned", ruvia::process_resource());
                std::pmr::string value("owned-value", ruvia::process_resource());
                std::pmr::string query_name("escaped name", ruvia::process_resource());
                std::pmr::string query_value("a &b=+/%?#\xc3\xa9", ruvia::process_resource());
                std::pmr::string span_value("span-value", ruvia::process_resource());
                std::pmr::string payload("owned-payload", ruvia::process_resource());
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
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                auto consumed = origin.get("/fluent/inspect");
                auto first = consumed.send();
                (void)consumed.send();
            }));

            auto temporary = origin.post(std::pmr::string("/fluent/inspect", ruvia::process_resource()))
                                 .body(std::pmr::string("temporary-body", ruvia::process_resource()))
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
                auto bounded = co_await origin.get("/fluent/pipe-bounded").send();
                EXPECT_TRUE(bounded.status() == ruvia::http_status::created);
                EXPECT_TRUE(bounded.header("x-stream-source") == "retained");
                const auto bounded_contents = co_await bounded.text();
                EXPECT_EQ(bounded_contents.text(), "firstsecond");
                EXPECT_TRUE(bounded.trailer("x-finished") == "yes");
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
                     ruvia::http_client_versions{.http2_ = false},
#if defined(RUVIA_ENABLE_HTTP2)
                     ruvia::http_client_versions{.http1_ = false},
#endif
                 }) {
                ruvia::http_client websocket_client(attachment.loop(),
                    {.base_url_ = server.targets().source_,
                        .request_timeout_ = std::chrono::seconds(1),
                        .versions_ = protocol});
                std::exception_ptr websocket_failure;
                try {
                    auto connecting = [&] {
                        const std::pmr::string path("/fluent/websocket", ruvia::process_resource());
                        const std::pmr::string value("pooled", ruvia::process_resource());
                        const std::pmr::string subprotocol("echo.v1", ruvia::process_resource());
                        const std::array fields{ruvia::http_header_view{"x-owned", value}};
                        return websocket_client.client().websocket(path).headers(fields).subprotocol(subprotocol).deflate({.compression_ = {.enabled_ = true}}).connect();
                    }();
                    auto connected = co_await std::move(connecting);
                    auto session = std::move(connected);
                    EXPECT_EQ(session.subprotocol(), "echo.v1");
                    EXPECT_TRUE(session.protocol_version() ==
                                (protocol == ruvia::http_client_versions{.http2_ = false}
                                        ? ruvia::http_protocol_version::http11
                                        : ruvia::http_protocol_version::http2));
                    // Accepted WebSockets are not ordinary finite HTTP responses.
                    // The HTTP exchange deadline must not become their lifetime.
                    EXPECT_TRUE(co_await ruvia::sleep_for(session.worker(), std::chrono::milliseconds(1250)) ==
                                ruvia::timer_sleep_result::elapsed);
                    EXPECT_TRUE(session.connected());
                    const std::pmr::string payload(32768, 'w', ruvia::process_resource());
                    for (const bool compress : {false, true}) {
                        auto writing = [&] {
                            const std::pmr::string owned(payload, ruvia::process_resource());
                            return compress ? session.binary(std::as_bytes(std::span(owned)), {.compress_ = true})
                                            : session.text(owned, {.compress_ = false});
                        }();
                        co_await std::move(writing);
                        const auto message = co_await session.read();
                        EXPECT_TRUE(message.has_value());
                        if (message) {
                            EXPECT_EQ(message->payload(), payload);
                            EXPECT_TRUE(message->binary() == compress);
                            EXPECT_TRUE(message->text() != compress);
                        }
                    }
                    co_await session.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "pooled"});
                    EXPECT_TRUE(!(co_await session.read()));
                    {
                        auto timed = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").connect();
                        bool timed_out = false;
                        try {
                            (void)co_await timed.read({.timeout_ = std::chrono::milliseconds(20)});
                        } catch (const ruvia::http_client_error& error) {
                            timed_out = error.code() == ruvia::http_client_error::code_type::timeout;
                        }
                        EXPECT_TRUE(timed_out);
                        // A read deadline ends only that read: the session keeps
                        // its buffered input and serves the next read.
                        EXPECT_TRUE(timed.connected());
                        co_await timed.text("after-timeout");
                        const auto echoed = co_await timed.read({.timeout_ = std::chrono::seconds(5)});
                        EXPECT_TRUE(echoed.has_value() && echoed->payload() == "after-timeout");
                        EXPECT_TRUE(timed.termination() == ruvia::websocket_termination::none);
                        co_await timed.close();
                        EXPECT_TRUE(timed.termination() == ruvia::websocket_termination::local_close);
                    }
                    {
                        auto limited = co_await websocket_client.client().websocket("/fluent/websocket").header("x-owned", "pooled").subprotocol("echo.v1").max_message_bytes(32).connect();
                        bool rejected = false;
                        try {
                            co_await limited.text(std::pmr::string(33, 'm', ruvia::process_resource()));
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
                            (void)co_await limited.read();
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
                            (void)co_await active.read();
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
    std::move(root).get();
    if (!failure) {
        fluent_client_tests::verify_standalone_origins(server);
        fluent_client_tests::verify_request_host_cancellation(server);
    }
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void fluent_client_tests::verify_standalone_origins(server& server) {
    SCOPED_TRACE("standalone origins, isolated policies and shared budget");
    auto& io = ruvia::test::new_test_io_context();
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        auto secure = server.secure_config();
        auto untrusted = secure;
        untrusted.alias_ = "untrusted";
        untrusted.tls_ = {};
        const std::array configs{
            ruvia::http_client_config{.base_url_ = server.targets().source_,
                .versions_ = ruvia::http_client_versions{.http2_ = false},
                .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
                .user_agent_ = "source-agent",
                .cookies_ = {{"configured", "source"}}},
            ruvia::http_client_config{.alias_ = "destination", .base_url_ = server.targets().destination_, .versions_ = ruvia::http_client_versions{.http2_ = false}, .user_agent_ = "destination-agent", .cookies_ = {{"configured", "destination"}}},
            secure, untrusted};
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>(configs));
        try {
            const auto source = client.client();
            const auto destination = client.client("destination");
            for (const auto status : {"302", "307"}) {
                auto result = co_await source.post("/fluent/redirect")
                                  .query("status", status)
                                  .query("location", std::pmr::string(server.targets().destination_, ruvia::process_resource()) + "/fluent/inspect")
                                  .header("authorization", "Bearer source-secret")
                                  .header("proxy-authorization", "Basic proxy-secret")
                                  .header("cookie", "explicit=source-secret")
                                  .header("content-type", "text/plain")
                                  .body("source-body")
                                  .send();
                EXPECT_TRUE(result.ok());
                EXPECT_TRUE(result.header("x-observed-user-agent") == "destination-agent");
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"method">().view(), std::string_view(status) == "302" ? "GET" : "POST");
                EXPECT_EQ(report.get<"body">().view(), std::string_view(status) == "302" ? "" : "source-body");
                EXPECT_EQ(report.get<"content_type">().view(), std::string_view(status) == "302" ? "" : "text/plain");
                EXPECT_EQ(report.get<"authorization">().view(), "");
                EXPECT_EQ(report.get<"proxy_authorization">().view(), "");
                EXPECT_EQ(report.get<"cookie">().view(), "configured=destination");
                EXPECT_EQ(report.get<"authority">().view(),
                    std::string_view(server.targets().destination_).substr(7));
            }
            for (const auto& origin : {source, destination}) {
                auto set = co_await origin.get("/fluent/inspect").query("set-cookie", "received=retained; Path=/").send();
                (void)co_await set.bytes();
            }
            {
                auto result = co_await source.get("/fluent/inspect").send();
                EXPECT_TRUE(result.header("x-observed-user-agent") == "source-agent");
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"cookie">().view(), "configured=source; received=retained");
            }
            {
                auto result = co_await destination.get("/fluent/inspect").send();
                const auto report = co_await result.json<fluent_client_observation>();
                EXPECT_EQ(report.get<"cookie">().view(), "configured=destination");
            }
            {
                auto result = co_await source.get("/fluent/redirect")
                                  .query("location", std::pmr::string(server.targets().unregistered_, ruvia::process_resource()) + "/fluent/inspect")
                                  .send();
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                const auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            {
                auto result = co_await client.client("secure").get("/fluent/redirect").query("location", std::pmr::string(server.targets().destination_, ruvia::process_resource()) + "/fluent/inspect").send();
                EXPECT_TRUE(result.status() == ruvia::http_status::found);
                const auto text = co_await result.text();
                EXPECT_EQ(text.text(), "redirect");
            }
            bool tls_rejected = false;
            try {
                (void)co_await client.client("untrusted").get("/fluent/inspect").send();
            } catch (const ruvia::http_client_error& error) {
                tls_rejected = error.code() == ruvia::http_client_error::code_type::tls_failed;
            }
            EXPECT_TRUE(tls_rejected);
            bool unknown_rejected = false;
            try {
                (void)client.client("unknown");
            } catch (const ruvia::http_client_error& error) {
                unknown_rejected = error.code() == ruvia::http_client_error::code_type::not_configured;
            }
            EXPECT_TRUE(unknown_rejected);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (!failure) {
            const std::span<const ruvia::http_client_config> origins(configs.data(), 2);
            ruvia::http_client bounded(attachment.loop(), origins, {.result_budget_ = {.max_retained_bytes_ = 16}});
            try {
                auto first = co_await bounded.client().get("/fluent/stream-source").send();
                const auto retained = co_await first.bytes(11);
                EXPECT_EQ(retained.text(), "firstsecond");
                auto second = co_await bounded.client("destination").get("/fluent/stream-source").send();
                bool exhausted = false;
                try {
                    (void)co_await second.bytes(11);
                } catch (const ruvia::http_client_error& error) {
                    exhausted = error.code() == ruvia::http_client_error::code_type::result_budget_exceeded;
                }
                EXPECT_TRUE(exhausted);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await bounded.shutdown();
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void fluent_client_tests::verify_request_host_cancellation(server& server) {
    SCOPED_TRACE("user options preserve request-host cancellation");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client owner(attachment.loop(), {.base_url_ = server.targets().source_,
                                                        .request_timeout_ = std::chrono::seconds(2),
                                                        .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            const auto before_waiters = fluent_client_tests::host_cancel_waiters.load();
            const auto before_cancelled = fluent_client_tests::host_cancelled_requests.load();
            ruvia::stop_source stop;
            bool cancelled = false;
            const auto request = [&]() -> ruvia::task<void> {
                try {
                    (void)co_await owner.client().with_options({.stop_token_ = stop.token()}).get("/fluent/host-cancel").send();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
                }
            };
            ruvia::task_scope tasks(owner.worker());
            tasks.spawn(request());
            const auto started = std::chrono::steady_clock::now();
            while (fluent_client_tests::host_cancel_waiters.load() == before_waiters &&
                   std::chrono::steady_clock::now() - started < std::chrono::seconds(1)) {
                (void)co_await ruvia::sleep_for(owner.worker(), std::chrono::milliseconds(1));
            }
            EXPECT_GT(fluent_client_tests::host_cancel_waiters.load(), before_waiters);
            stop.request_stop();
            co_await tasks.join();
            EXPECT_TRUE(cancelled);
            const auto stopped = std::chrono::steady_clock::now();
            while (fluent_client_tests::host_cancelled_requests.load() == before_cancelled &&
                   std::chrono::steady_clock::now() - stopped < std::chrono::seconds(1)) {
                (void)co_await ruvia::sleep_for(owner.worker(), std::chrono::milliseconds(1));
            }
            EXPECT_GT(fluent_client_tests::host_cancelled_requests.load(), before_cancelled);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await owner.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
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
                const std::pmr::string path("/" + std::pmr::string(2048, 'p', ruvia::process_resource()), ruvia::process_resource());
                const std::pmr::string value(16384, 'v', ruvia::process_resource());
                const std::pmr::string body(32768, 'b', ruvia::process_resource());
                const std::pmr::string authority(std::pmr::string(63, 'a', ruvia::process_resource()) + "." + std::pmr::string(63, 'b', ruvia::process_resource()) + ".test:443", ruvia::process_resource());
                const auto origin = client.client();
                request.emplace(origin.post(path));
                request->header("x-owned", value).query("owned", path).body(body);
                connect.emplace(origin.connect(authority));
                connect->protocol("owned-protocol").target(path).header("x-owned", value);
                websocket.emplace(origin.websocket(path));
                websocket->header("x-owned", value).subprotocol(std::pmr::string(256, 's', ruvia::process_resource()));
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
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
