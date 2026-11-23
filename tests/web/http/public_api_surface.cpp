#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "asio_bridge.h"
#include "asio_ip_endpoint.h"
#include "blocking_pool.h"
#include "client_transport_config.h"
#include "controller.h"
#include "crypto.h"
#include "dns_resolver.h"
#include "entity_storage.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "inbound_buffer.h"
#include "model.h"
#include "model_binding.h"
#include "router.h"
#include "static_root.h"
#include "tcp_client.h"
#include "test_app.h"
#include "tls_config.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db_entity.h"
#include "db_entity_codec.h"
#include "db_executor.h"
#include "db_query.h"
#include "db_repository.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "redis_orm.h"
#include "session.h"
#endif

#ifdef RUVIA_ENABLE_JWT
#include "jwt.h"
#endif

namespace public_api_surface_test {

enum class account_tier : std::uint8_t {
    free,
    pro,
    enterprise,
};

RUVIA_ENUM(
    account_tier,
    RUVIA_ENUM_VALUE(account_tier::free, "free"),
    RUVIA_ENUM_VALUE(account_tier::pro, "pro"),
    RUVIA_ENUM_VALUE(account_tier::enterprise, "enterprise"))

RUVIA_MODEL(
    patch_item_input,
    RUVIA_REQUIRED_FIELD(title, ruvia::string, RUVIA_MIN_CHARS(2, "title requires at least 2 codepoints"), RUVIA_MAX_BYTES(32, "title exceeds byte ceiling")),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

using unprocessable_patch_json = ruvia::json_body<patch_item_input, false, 422>;
using unprocessable_strict_patch_json = ruvia::strict_json_body<patch_item_input, 422>;

class public_surface_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/public-api-surface")

    RUVIA_ROUTES_BEGIN(public_surface_controller)
    RUVIA_PATCH("/items", patch_item, unprocessable_patch_json);
    RUVIA_PATCH("/items-strict", patch_item, unprocessable_strict_patch_json);
    RUVIA_GET("/ping", ping);
    RUVIA_GET("/proxy", proxy);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> patch_item(ruvia::context& c) {
        co_return c.json(c.request().validated<patch_item_input>());
    }

    ruvia::task<ruvia::http_response> ping(ruvia::context& c) {
        co_return c.text(ruvia::static_text("standalone-pong"));
    }

    ruvia::task<ruvia::http_response> proxy(ruvia::context& c) {
        auto upstream = co_await c.http_client("upstream").get("/backend").send();
        const auto body = co_await upstream.collect_body();
        co_return c.text(body.text());
    }
};

class scoped_temp_dir final {
public:
    explicit scoped_temp_dir(std::string_view suffix) :
        path_(std::filesystem::path(RUVIA_TEST_TEMP_ROOT) / std::string(suffix)) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }
    scoped_temp_dir(const scoped_temp_dir&) = delete;
    scoped_temp_dir& operator=(const scoped_temp_dir&) = delete;
    ~scoped_temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string read_file_text(const std::filesystem::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to read test file");
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#ifdef RUVIA_ENABLE_JWT
RUVIA_MODEL(
    jwt_custom_claims_model,
    RUVIA_REQUIRED_FIELD(tenant_id, ruvia::uuid),
    RUVIA_REQUIRED_FIELD(tier, account_tier),
    RUVIA_REQUIRED_FIELD(quota, std::int64_t, RUVIA_MIN(1, "quota must be positive")),
    RUVIA_OPTIONAL_FIELD(staff, bool, RUVIA_DEFAULT(false)),
    RUVIA_OPTIONAL_FIELD(scopes, ruvia::array<ruvia::string>));

RUVIA_MODEL(
    jwt_reserved_claim_model,
    RUVIA_REQUIRED_FIELD(sub, ruvia::string));
#endif

#ifdef RUVIA_ENABLE_DATABASE
RUVIA_DB_ENTITY(
    surface_department_entity,
    "departments",
    RUVIA_DB_COLUMN(id, std::int64_t, {.primary_key_ = true}),
    RUVIA_DB_COLUMN(title, std::string))

RUVIA_DB_ENTITY(
    surface_account_entity,
    "accounts",
    RUVIA_DB_COLUMN(id, ruvia::uuid, {.primary_key_ = true}),
    RUVIA_DB_COLUMN(department_id, std::int64_t, {.nullable_ = true}),
    RUVIA_DB_COLUMN(handle, std::string),
    RUVIA_DB_COLUMN(tier, account_tier),
    RUVIA_DB_COLUMN(balance, ruvia::decimal),
    RUVIA_DB_COLUMN(joined_on, ruvia::civil_date),
    RUVIA_DB_COLUMN(updated_at, ruvia::utc_microseconds),
    RUVIA_DB_COLUMN(avatar, ruvia::bytes, {.nullable_ = true}),
    RUVIA_DB_COLUMN(metadata, std::string, {.data_type_ = ruvia::db_data_type::jsonb, .nullable_ = true}),
    RUVIA_DB_COLUMN(tags, std::vector<std::string>),
    RUVIA_DB_COLUMN(audit_seq, std::int64_t, {.read_only_ = true}),
    RUVIA_DB_MANY_TO_ONE(department, surface_department_entity, RUVIA_DB_JOIN_COLUMN(department_id, id)))

RUVIA_DB_PROJECTION(
    surface_account_summary,
    RUVIA_DB_COLUMN(handle, std::string),
    RUVIA_DB_COLUMN(balance, ruvia::decimal))

RUVIA_MODEL(
    surface_department_dto,
    RUVIA_REQUIRED_FIELD(id, std::int64_t),
    RUVIA_REQUIRED_FIELD(title, ruvia::string));

RUVIA_MODEL(
    surface_account_patch_dto,
    RUVIA_OPTIONAL_FIELD(handle, ruvia::string),
    RUVIA_OPTIONAL_FIELD(tier, account_tier),
    RUVIA_OPTIONAL_FIELD(balance, ruvia::decimal),
    RUVIA_OPTIONAL_FIELD(avatar, ruvia::bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(metadata, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(audit_seq, std::int64_t));

RUVIA_MODEL(
    surface_account_dto,
    RUVIA_REQUIRED_FIELD(id, ruvia::uuid),
    RUVIA_REQUIRED_FIELD(handle, ruvia::string),
    RUVIA_REQUIRED_FIELD(tier, account_tier),
    RUVIA_REQUIRED_FIELD(balance, ruvia::decimal),
    RUVIA_REQUIRED_FIELD(joined_on, ruvia::civil_date),
    RUVIA_REQUIRED_FIELD(updated_at, ruvia::utc_microseconds),
    RUVIA_OPTIONAL_FIELD(avatar, ruvia::bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(metadata, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(tags, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(department, surface_department_dto, RUVIA_NULLABLE));

RUVIA_MODEL(
    bad_null_patch,
    RUVIA_OPTIONAL_FIELD(balance, ruvia::decimal),
    RUVIA_OPTIONAL_FIELD(handle, ruvia::string, RUVIA_NULLABLE));
#endif

#ifdef RUVIA_ENABLE_REDIS
RUVIA_REDIS_ENTITY(
    surface_redis_account,
    "acct",
    RUVIA_REDIS_COLUMN(id, ruvia::uuid, {.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(handle, std::string),
    RUVIA_REDIS_COLUMN(tier, account_tier),
    RUVIA_REDIS_COLUMN(credits, std::int64_t),
    RUVIA_REDIS_COLUMN(note, std::string, {.nullable_ = true}))

RUVIA_MODEL(
    surface_redis_patch_dto,
    RUVIA_OPTIONAL_FIELD(credits, std::int64_t),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string, RUVIA_NULLABLE));

RUVIA_MODEL(
    surface_redis_dto,
    RUVIA_REQUIRED_FIELD(id, ruvia::uuid),
    RUVIA_REQUIRED_FIELD(handle, ruvia::string),
    RUVIA_REQUIRED_FIELD(tier, account_tier),
    RUVIA_REQUIRED_FIELD(credits, std::int64_t),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string, RUVIA_NULLABLE));
#endif

}  // namespace public_api_surface_test

TEST(Ruvia, model_binding_accepts_structured_json_suffixes_and_custom_validation_status) {
    using namespace public_api_surface_test;

    EXPECT_TRUE(ruvia::is_json_media_type("application/json"));
    EXPECT_TRUE(ruvia::is_json_media_type("application/json; charset=utf-8"));
    EXPECT_TRUE(ruvia::is_json_media_type("application/merge-patch+json; charset=\"UTF-8\""));
    EXPECT_TRUE(ruvia::is_json_media_type("application/vnd.api+json"));
    EXPECT_FALSE(ruvia::is_json_media_type("application/+json"));
    EXPECT_FALSE(ruvia::is_json_media_type("text/plain+json"));
    EXPECT_FALSE(ruvia::is_json_media_type("application/json; charset=iso-8859-1"));

    EXPECT_TRUE(ruvia::is_form_urlencoded_media_type("application/x-www-form-urlencoded"));
    EXPECT_TRUE(ruvia::is_form_urlencoded_media_type("application/x-www-form-urlencoded; charset=UTF-8"));
    EXPECT_FALSE(ruvia::is_form_urlencoded_media_type("application/x-www-form-urlencoded; charset=latin1"));

    ruvia::test_app app;
    app.controllers<public_surface_controller>();

    // UTF-8 2-codepoint title ("你好" = 6 bytes, 2 chars) over application/merge-patch+json succeeds.
    const auto ok = app.dispatch(ruvia::test_request::patch("/public-api-surface/items").body(R"({"title":"你好","tags":["x"]})", "application/merge-patch+json"));
    EXPECT_EQ(ok.status(), ruvia::http_status::ok);
    EXPECT_EQ(ok.body(), R"({"title":"你好","tags":["x"]})");

    // 1-codepoint title fails RUVIA_MIN_CHARS(2) with configured 422 status.
    const auto too_few_chars = app.dispatch(ruvia::test_request::patch("/public-api-surface/items").body(R"({"title":"你"})", "application/merge-patch+json"));
    EXPECT_EQ(too_few_chars.status(), ruvia::http_status::unprocessable_content);
    EXPECT_NE(too_few_chars.body().find("validation_failed"), std::string_view::npos);

    // Strict mode rejects undeclared fields with configured 422 status, while malformed JSON remains 400.
    const auto unknown_field = app.dispatch(ruvia::test_request::patch("/public-api-surface/items-strict").json(R"({"title":"ok","extra":1})"));
    EXPECT_EQ(unknown_field.status(), ruvia::http_status::unprocessable_content);
    EXPECT_NE(unknown_field.body().find("unknown_field"), std::string_view::npos);

    const auto bad_syntax = app.dispatch(ruvia::test_request::patch("/public-api-surface/items-strict").json("{not-json"));
    EXPECT_EQ(bad_syntax.status(), ruvia::http_status::bad_request);
}

TEST(Ruvia, standalone_router_mounts_macro_controllers_and_test_app_exposes_worker_loop) {
    using namespace public_api_surface_test;

    ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 64});
    pool.start();
    const auto server_loop = pool.loop(0);

    std::unique_ptr<ruvia::router> standalone_router;
    std::unique_ptr<ruvia::http_server> standalone_server;
    asio::io_context reservation_io;
    ruvia::test::tcp_port_reservation port_lease(reservation_io);
    const std::uint16_t bound_port = port_lease.endpoint().port();

    auto boot = [&]() -> ruvia::task<void> {
        standalone_router = std::make_unique<ruvia::router>(server_loop.worker());
        standalone_router->controllers<public_surface_controller>();
        standalone_router->get("/backend", [](ruvia::context& c) -> ruvia::task<ruvia::http_response> {
            co_return c.text("from-standalone-router");
        });
        standalone_router->freeze();

        standalone_server = std::make_unique<ruvia::http_server>(
            server_loop,
            ruvia::http_server_callbacks{
                .request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            co_await standalone_router->dispatch(stream);
        },
            });
        standalone_server->listen({.address_ = "127.0.0.1", .http_ = bound_port, .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await standalone_server->start();
    };
    server_loop.submit(boot()).take_value().take_value();

    // Exercise test_app::loop(), test_app::worker(), and bind_http_client() calling the standalone router!
    {
        ruvia::test_app app;
        EXPECT_TRUE(app.loop().valid());
        EXPECT_TRUE(app.worker().valid());

        const std::string base_url = "http://127.0.0.1:" + std::to_string(bound_port);
        ruvia::http_client_config client_cfg;
        client_cfg.alias_ = "upstream";
        client_cfg.base_url_ = base_url;
        client_cfg.tls_.mode_ = ruvia::client_tls_mode::disabled;
        ruvia::http_client upstream_client(app.loop(), client_cfg);
        app.controllers<public_surface_controller>();
        app.bind_http_client("upstream", upstream_client);

        const auto ping_resp = app.dispatch(ruvia::test_request::get("/public-api-surface/ping"));
        EXPECT_EQ(ping_resp.status(), ruvia::http_status::ok);
        EXPECT_EQ(ping_resp.body(), "standalone-pong");

        const auto proxy_resp = app.dispatch(ruvia::test_request::get("/public-api-surface/proxy"));
        EXPECT_EQ(proxy_resp.status(), ruvia::http_status::ok);
        EXPECT_EQ(proxy_resp.body(), "from-standalone-router");

        // Also verify macro-mounted controller directly on standalone_router via upstream_client.
        auto check_macro_on_router = [&]() -> ruvia::task<std::string> {
            auto resp = co_await upstream_client.ref("upstream").get("/public-api-surface/ping").send();
            const auto body = co_await resp.collect_body();
            auto text = std::string(body.text());
            co_await upstream_client.shutdown();
            co_return text;
        };
        auto task_handle = app.loop().submit(check_macro_on_router()).take_value();
        while (!task_handle.ready()) {
            (void)app.dispatch(ruvia::test_request::get("/public-api-surface/ping"));
        }
        EXPECT_EQ(std::move(task_handle).take_value(), "standalone-pong");
    }

    auto teardown = [&]() -> ruvia::task<void> {
        co_await standalone_server->shutdown();
        standalone_server.reset();
        standalone_router.reset();
    };
    server_loop.submit(teardown()).take_value().take_value();
    pool.join();
}

TEST(Ruvia, static_root_lookup_and_inbound_buffer_budget_accounting) {
    using namespace public_api_surface_test;

    {
        scoped_temp_dir dir("static-root-public-api");
        std::filesystem::create_directories(dir.path() / "docs");
        {
            std::ofstream index_out(dir.path() / "docs" / "index.html", std::ios::binary);
            index_out << "<h1>Hello</h1>";
            std::ofstream js_out(dir.path() / "app.js", std::ios::binary);
            js_out << "console.log(1);";
            std::ofstream secret_out(dir.path() / ".env", std::ios::binary);
            secret_out << "SECRET=1";
        }

        ruvia::static_root root(dir.path(), {.cache_control_ = "public, max-age=60"});
        EXPECT_EQ(root.entry_count(), std::size_t{2});

        const auto js = root.find("app.js");
        ASSERT_TRUE(js.has_value());
        EXPECT_EQ(js->size_, std::uint64_t{15});
        EXPECT_EQ(js->cache_control_, "public, max-age=60");
        EXPECT_FALSE(js->etag_.empty());

        const auto resolved_index = root.resolve("docs/");
        ASSERT_TRUE(resolved_index.has_value());
        EXPECT_EQ(resolved_index->size_, std::uint64_t{14});

        EXPECT_FALSE(root.resolve("/docs/").has_value());
        EXPECT_FALSE(root.resolve("../app.js").has_value());
        EXPECT_FALSE(root.resolve(".env").has_value());
    }

    {
        ruvia::inbound_buffer_budget worker_budget(1024);
        ruvia::inbound_buffer_account conn_a(worker_budget, 600);
        ruvia::inbound_buffer_account conn_b(worker_budget, 600);

        auto res_a = conn_a.reserve(400);
        EXPECT_EQ(conn_a.used(), std::size_t{400});
        EXPECT_EQ(worker_budget.used(), std::size_t{400});
        EXPECT_EQ(conn_a.available(), std::size_t{200});

        auto res_b = conn_b.reserve(500);
        EXPECT_EQ(worker_budget.used(), std::size_t{900});
        // Worker budget has 124 left, tighter than conn_b's 100? conn_b has 100 left; conn_a has 200 own vs 124 worker -> 124!
        EXPECT_EQ(conn_a.available(), std::size_t{124});
        EXPECT_EQ(conn_b.available(), std::size_t{100});

        EXPECT_THROW(res_a.resize_to(550), ruvia::inbound_buffer_limit_error);
        EXPECT_EQ(res_a.bytes(), std::size_t{400});
        EXPECT_EQ(worker_budget.used(), std::size_t{900});

        res_b.shrink_to(200);
        EXPECT_EQ(worker_budget.used(), std::size_t{600});
        res_a.resize_to(550);
        EXPECT_EQ(conn_a.used(), std::size_t{550});

        auto moved = std::move(res_a);
        EXPECT_EQ(res_a.bytes(), std::size_t{0});
        moved.release();
        res_b.clear();
        EXPECT_EQ(worker_budget.used(), std::size_t{0});
    }
}

TEST(Ruvia, asio_endpoint_conversions_dns_primitives_and_in_memory_mtls_tcp_client) {
    // Endpoint round-trips (IPv4 + IPv6 with scope_id).
    const auto v4 = ruvia::ip_endpoint(ruvia::ip_address("192.0.2.10"), 8443);
    const auto asio_tcp_v4 = ruvia::to_asio_tcp_endpoint(v4);
    EXPECT_EQ(ruvia::to_ip_endpoint(asio_tcp_v4), v4);

    const auto v6 = ruvia::ip_endpoint(ruvia::ip_address::ipv6({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 7), 9000);
    const auto asio_udp_v6 = ruvia::to_asio_udp_endpoint(v6);
    EXPECT_EQ(ruvia::to_ip_endpoint(asio_udp_v6), v6);

    // DNS syntax validation & RFC 8305 family interleaving.
    EXPECT_TRUE(ruvia::is_valid_dns_host("api.example.com."));
    EXPECT_FALSE(ruvia::is_valid_dns_host("127.0.0.1"));
    EXPECT_FALSE(ruvia::is_valid_dns_host("-bad.example.com"));

    struct fake_entry final {
        asio::ip::tcp::endpoint ep_;
        [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
            return ep_;
        }
    };
    const std::vector<fake_entry> resolved{
        {asio::ip::tcp::endpoint(asio::ip::make_address("2001:db8::1"), 443)},
        {asio::ip::tcp::endpoint(asio::ip::make_address("2001:db8::2"), 443)},
        {asio::ip::tcp::endpoint(asio::ip::make_address("192.0.2.1"), 443)},
        {asio::ip::tcp::endpoint(asio::ip::make_address("2001:db8::1"), 443)},
    };
    ruvia::address_family_interleaver interleaver(resolved);
    ASSERT_TRUE(interleaver.has_next());
    EXPECT_EQ(interleaver.next()->address().to_string(), "2001:db8::1");
    EXPECT_EQ(interleaver.next()->address().to_string(), "192.0.2.1");
    EXPECT_EQ(interleaver.next()->address().to_string(), "2001:db8::2");
    EXPECT_FALSE(interleaver.has_next());

    // Real mTLS tcp_client with in-memory PEM (ca_pem_, certificate_chain_pem_, private_key_pem_),
    // scatter-gather write(span<const byte_view>), local_endpoint/peer_endpoint, and finish() half-close!
    ruvia::test::tls_identity server_id("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::test::tls_identity client_id("mtls-client", "DNS:mtls-client");

    SSL_CTX_set_verify(server_id.context_.native_handle(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    ASSERT_EQ(SSL_CTX_load_verify_locations(server_id.context_.native_handle(), client_id.ca_file_.string().c_str(), nullptr), 1);

    const std::string server_ca_pem = public_api_surface_test::read_file_text(server_id.ca_file_);
    const std::string client_cert_pem = public_api_surface_test::read_file_text(client_id.ca_file_);
    const std::string client_key_pem = public_api_surface_test::read_file_text(client_id.private_key_file());

    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 64});
    loops.start();
    const auto loop = loops.loop(0);

    auto run_mtls_and_dialer = [&]() -> ruvia::task<void> {
        auto& io = loop.io_context();

        // 1. Verify dns_resolver sync literal path + happy_eyeballs_dialer live connect.
        ruvia::dns_resolver resolver(loop.worker());
        auto literal_op = resolver.resolve("127.0.0.1", 8080, ruvia::operation_deadline{});
        const auto* sync_res = std::get_if<ruvia::dns_resolve_result>(&literal_op);
        EXPECT_TRUE(sync_res != nullptr && sync_res->literal_.has_value() && !sync_res->error_);
        resolver.cancel();

        asio::ip::tcp::acceptor he_acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::resolver native_resolver(io);
        const auto he_results = native_resolver.resolve("127.0.0.1", std::to_string(he_acceptor.local_endpoint().port()));
        asio::ip::tcp::socket accepted_he(io);
        bool he_accepted = false;
        he_acceptor.async_accept(accepted_he, [&](std::error_code ec) {
            he_accepted = !ec;
        });
        ruvia::happy_eyeballs_dialer dialer(loop.worker(), ruvia::asio_executor(loop.worker()), he_results);
        auto dialed = co_await dialer.connect();
        EXPECT_FALSE(dialed.error_);
        EXPECT_TRUE(dialed.socket_.has_value());
        if (dialed.socket_) {
            ruvia::close_socket(*dialed.socket_);
        }
        while (!he_accepted) {
            (void)co_await ruvia::sleep_for(loop.worker(), std::chrono::milliseconds(1));
        }
        ruvia::close_socket(accepted_he);
        he_acceptor.close();

        // 2. Verify tcp_client in-memory mTLS + scatter-gather write + finish() half-close.
        asio::ip::tcp::acceptor tls_acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto server_port = tls_acceptor.local_endpoint().port();
        std::string server_received;
        std::exception_ptr server_error;
        bool server_done = false;

        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await tls_acceptor.async_accept(asio::use_awaitable), server_id.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            std::array<char, 64> buf{};
            for (;;) {
                std::error_code ec;
                const auto n = co_await stream.async_read_some(asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
                if (n > 0) {
                    server_received.append(buf.data(), n);
                }
                if (ec) {
                    break;
                }
            }
            co_await asio::async_write(stream, asio::buffer(std::string_view("ack:")), asio::use_awaitable);
            co_await asio::async_write(stream, asio::buffer(server_received), asio::use_awaitable);
            std::error_code shutdown_ec;
            co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, shutdown_ec));
        },
            [&](std::exception_ptr ep) {
            server_error = ep;
            server_done = true;
        });

        ruvia::tcp_client_config cfg;
        cfg.host_ = "127.0.0.1";
        cfg.port_ = server_port;
        cfg.tls_.ca_pem_ = server_ca_pem;
        cfg.tls_.server_name_ = "localhost";
        cfg.tls_.identity_ = ruvia::tls_identity_config{
            .certificate_chain_pem_ = client_cert_pem,
            .private_key_pem_ = client_key_pem,
        };

        ruvia::tcp_client client(loop, cfg);
        co_await client.connect();
        EXPECT_EQ(client.peer_endpoint().port().value(), server_port);
        EXPECT_NE(client.local_endpoint().port().value(), std::uint16_t{0});

        const std::array<ruvia::byte_view, 2> chunks{
            ruvia::as_bytes(std::string_view("hello-")),
            ruvia::as_bytes(std::string_view("mtls")),
        };
        co_await client.write(chunks);
        co_await client.finish();

        std::string reply;
        while (const auto part = co_await client.read_text()) {
            reply.append(*part);
        }
        EXPECT_EQ(reply, "ack:hello-mtls");
        co_await client.shutdown();

        while (!server_done) {
            (void)co_await ruvia::sleep_for(loop.worker(), std::chrono::milliseconds(1));
        }
        if (server_error) {
            std::rethrow_exception(server_error);
        }
    };

    loop.submit(run_mtls_and_dialer()).take_value().take_value();
    loops.join();
}

TEST(Ruvia, crypto_std_byte_digests_and_signed_cookies) {
    ruvia::sha256 hasher;
    hasher.update(std::string_view("abc"));
    const auto first = hasher.finish();
    EXPECT_EQ(first, ruvia::sha256_of(std::string_view("abc")));

    hasher.reset();
    hasher.update(ruvia::as_bytes(std::string_view("abc")));
    EXPECT_EQ(hasher.finish(), first);

    const std::string signed_cookie = ruvia::sign_cookie_value("super-secret-key", "sid", "payload-123");
    const auto verified = ruvia::verify_signed_cookie_value("super-secret-key", "sid", signed_cookie);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(*verified, "payload-123");
    EXPECT_FALSE(ruvia::verify_signed_cookie_value("super-secret-key", "other_name", signed_cookie).has_value());
    EXPECT_FALSE(ruvia::verify_signed_cookie_value("wrong-key", "sid", signed_cookie).has_value());
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, session_store_cookie_signing_and_redis_entity_interop) {
    using namespace public_api_surface_test;

    const std::string valid_id(48, 'a');
    ruvia::session_store signed_store({
        .cookie_name_ = "app_sid",
        .path_ = "/app",
        .domain_ = "example.com",
        .secret_ = "0123456789abcdef0123456789abcdef",
    });

    const std::string wire_cookie = signed_store.format_cookie_value(valid_id);
    EXPECT_NE(wire_cookie, valid_id);
    const auto extracted = signed_store.verify_cookie_id(wire_cookie);
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(*extracted, valid_id);

    // Unsigned or tampered values are rejected when secret_ is set.
    EXPECT_FALSE(signed_store.verify_cookie_id(valid_id).has_value());
    std::string tampered = wire_cookie;
    tampered.back() = tampered.back() == 'A' ? 'B' : 'A';
    EXPECT_FALSE(signed_store.verify_cookie_id(tampered).has_value());

    const auto attrs = signed_store.cookie_attributes(true);
    EXPECT_EQ(attrs.http_only_, ruvia::cookie_attribute_policy::emit);
    EXPECT_EQ(attrs.secure_, ruvia::cookie_attribute_policy::emit);
    EXPECT_EQ(attrs.path_.view(), "/app");
    EXPECT_EQ(attrs.domain_.view(), "example.com");

    const auto del_attrs = signed_store.deletion_cookie_attributes(false);
    EXPECT_TRUE(del_attrs.max_age_ == std::chrono::seconds{0} || del_attrs.expires_ == ruvia::cookie_deletion_expires);
    EXPECT_EQ(del_attrs.secure_, ruvia::cookie_attribute_policy::omit);

    // Redis entity clone, move assignment, to_json, apply_model_patch, from_entity.
    surface_redis_account acct;
    const ruvia::uuid uid("018f3b5c-7d8e-7000-8000-010203040506");
    acct.set<"id">(uid).set<"handle">("alice").set<"tier">(account_tier::pro).set<"credits">(100).set<"note">("vip");

    auto cloned = acct.clone();
    auto patch = ruvia::parse_json<surface_redis_patch_dto>(R"({"credits":250,"note":null})").take_value();
    ruvia::apply_model_patch(cloned, patch);
    EXPECT_EQ(cloned.get<"credits">(), 250);
    EXPECT_TRUE(cloned.is_null<"note">());
    EXPECT_EQ(acct.get<"note">(), "vip");

    const std::string json_text = ruvia::to_json(cloned);
    EXPECT_EQ(json_text, R"({"id":"018f3b5c-7d8e-7000-8000-010203040506","handle":"alice","tier":"pro","credits":250,"note":null})");

    const auto dto = ruvia::from_entity<surface_redis_dto>(cloned);
    EXPECT_EQ(dto.get<"id">(), uid);
    EXPECT_EQ(dto.get<"tier">(), account_tier::pro);
    EXPECT_EQ(dto.get<"credits">(), 250);
    EXPECT_TRUE(dto.is_null<"note">());
}
#endif

#ifdef RUVIA_ENABLE_JWT
TEST(Ruvia, jwt_typed_model_claims_sign_and_verify) {
    using namespace public_api_surface_test;

    constexpr std::string_view secret = "0123456789abcdef0123456789abcdef";
    const ruvia::uuid tenant("018f3b5c-7d8e-7000-8000-112233445566");

    jwt_custom_claims_model claims;
    claims.set<"tenant_id">(tenant).set<"tier">(account_tier::enterprise).set<"quota">(5000).set<"staff">(true);
    claims.ensure<"scopes">().emplace_back("read:all");
    claims.ensure<"scopes">().emplace_back("write:billing");

    const std::string token = ruvia::sign_jwt_claims(claims, {
                                                                 .secret_ = secret,
                                                                 .issuer_ = "ruvia-auth",
                                                                 .subject_ = "user-42",
                                                                 .audience_ = "api",
                                                             });

    auto verified = ruvia::verify_jwt_claims<jwt_custom_claims_model>({
        .token_ = token,
        .secret_ = secret,
        .issuer_ = "ruvia-auth",
        .audience_ = "api",
    });
    ASSERT_TRUE(verified.has_value());
    const auto taken = std::move(verified).take_value();
    EXPECT_EQ(taken.payload().subject(), "user-42");
    EXPECT_EQ(taken.claims().get<"tenant_id">(), tenant);
    EXPECT_EQ(taken.claims().get<"tier">(), account_tier::enterprise);
    EXPECT_EQ(taken.claims().get<"quota">(), 5000);
    EXPECT_TRUE(taken.claims().get<"staff">());
    ASSERT_TRUE(taken.claims().has_value<"scopes">());
    EXPECT_EQ(taken.claims().get<"scopes">()->size(), std::size_t{2});

    // Reserved JWT claim names in custom_claims must be rejected at sign time.
    jwt_reserved_claim_model bad_claims;
    bad_claims.set<"sub">("conflict");
    EXPECT_THROW((void)ruvia::sign_jwt_claims(bad_claims, {.secret_ = secret}), std::invalid_argument);

    // Token missing required custom claims reports missing_claim; failing RUVIA_MIN(1) reports invalid_payload.
    const std::string plain_token = ruvia::sign_jwt({.secret_ = secret, .subject_ = "user-42"});
    const auto missing = ruvia::verify_jwt_claims<jwt_custom_claims_model>({.token_ = plain_token, .secret_ = secret});
    EXPECT_EQ(missing.error(), ruvia::jwt_error_code::missing_claim);

    static constexpr std::array<ruvia::jwt_claim_view, 1> zero_quota_claim{{{.name_ = "tenant_id", .value_ = "018f3b5c-7d8e-7000-8000-112233445566"}}};
    (void)zero_quota_claim;
    jwt_custom_claims_model invalid_quota;
    invalid_quota.set<"tenant_id">(tenant).set<"tier">(account_tier::free).set<"quota">(0);
    const std::string invalid_quota_token = ruvia::sign_jwt_claims(invalid_quota, {.secret_ = secret});
    const auto rejected_quota = ruvia::verify_jwt_claims<jwt_custom_claims_model>({.token_ = invalid_quota_token, .secret_ = secret});
    EXPECT_EQ(rejected_quota.error(), ruvia::jwt_error_code::invalid_payload);
}
#endif

#ifdef RUVIA_ENABLE_DATABASE
TEST(Ruvia, orm_rich_types_json_serialization_transactional_patch_and_field_comparisons) {
    using namespace public_api_surface_test;

    const ruvia::uuid id("018f3b5c-7d8e-7000-8000-aabbccddeeff");
    const ruvia::decimal bal("12345.67");
    const ruvia::civil_date joined{std::chrono::year{2026}, std::chrono::month{5}, std::chrono::day{19}};
    const ruvia::utc_microseconds updated = std::chrono::sys_days{joined} + std::chrono::hours{12} + std::chrono::microseconds{500};
    const std::array<std::byte, 3> raw_avatar{std::byte{0x01}, std::byte{0x02}, std::byte{0xff}};

    surface_account_entity entity;
    entity.set<"id">(id)
        .set<"department_id">(7)
        .set<"handle">("bob")
        .set<"tier">(account_tier::pro)
        .set<"balance">(bal)
        .set<"joined_on">(joined)
        .set<"updated_at">(updated)
        .set<"avatar">(ruvia::byte_view(raw_avatar))
        .set<"metadata">(R"({"theme":"dark","beta":true})")
        .set<"tags">(std::vector<std::string>{"core", "http"})
        .set<"audit_seq">(99);

    auto& dept = entity.emplace_relation<"department">();
    dept.set<"id">(7).set<"title">("Platform");

    // Deep clone + move assignment preserve columns and loaded relations independently.
    surface_account_entity cloned = entity.clone();
    EXPECT_EQ(cloned.get<"department">().get<"title">(), "Platform");

    // Unified to_json embeds jsonb directly, encodes bytes as Base64 (or Hex), and serializes loaded relations.
    const std::string json_out = ruvia::to_json(cloned);
    EXPECT_NE(json_out.find(R"("metadata":{"theme":"dark","beta":true})"), std::string_view::npos);
    EXPECT_NE(json_out.find(R"("department":{"id":7,"title":"Platform"})"), std::string_view::npos);
    EXPECT_NE(json_out.find(R"("balance":"12345.67")"), std::string_view::npos);

    ruvia::entity_rows<surface_account_summary> summaries;
    surface_account_summary sum;
    sum.set<"handle">("bob").set<"balance">(bal);
    summaries.push_back(std::move(sum));
    EXPECT_EQ(ruvia::to_json(summaries), R"([{"handle":"bob","balance":"12345.67"}])");

    // Two-phase transactional apply_model_patch:
    // 1. Patching non-nullable `handle` with null throws and leaves `cloned` untouched!
    auto failing_patch = ruvia::parse_json<bad_null_patch>(R"({"balance":"999.99","handle":null})").take_value();
    EXPECT_THROW(ruvia::apply_model_patch(cloned, failing_patch), std::invalid_argument);
    EXPECT_EQ(cloned.get<"balance">(), bal);
    EXPECT_EQ(cloned.get<"handle">(), "bob");

    // 2. Valid patch updates present fields, clears nullable `avatar`, updates `jsonb` from json_object, and ignores read_only `audit_seq`!
    auto valid_patch = ruvia::parse_json<surface_account_patch_dto>(R"({"tier":"enterprise","avatar":null,"metadata":{"theme":"light"},"tags":["v2"],"audit_seq":555})").take_value();
    ruvia::apply_model_patch(cloned, valid_patch);
    EXPECT_EQ(cloned.get<"tier">(), account_tier::enterprise);
    EXPECT_TRUE(cloned.is_null<"avatar">());
    EXPECT_EQ(cloned.get<"metadata">(), R"({"theme":"light"})");
    EXPECT_EQ(cloned.get<"tags">(), (std::vector<std::string>{"v2"}));
    EXPECT_EQ(cloned.get<"audit_seq">(), 99);

    // from_entity maps columns, json_object, and loaded relations into RUVIA_MODEL.
    const auto dto = ruvia::from_entity<surface_account_dto>(cloned);
    EXPECT_EQ(dto.get<"id">(), id);
    EXPECT_EQ(dto.get<"tier">(), account_tier::enterprise);
    EXPECT_TRUE(dto.is_null<"avatar">());
    ASSERT_TRUE(dto.has_value<"metadata">());
    const auto theme_lookup = dto.get<"metadata">()->get<ruvia::string>("theme");
    ASSERT_TRUE(theme_lookup.value().has_value());
    EXPECT_EQ(theme_lookup.value()->view(), "light");
    ASSERT_TRUE(dto.has_value<"department">());
    EXPECT_EQ(dto.get<"department">()->get<"title">().view(), "Platform");

    // Direct db_field_reference comparison operators & BYTEA/BLOB compilation.
    constexpr auto handle_col = surface_account_entity::column<"handle">("a");
    constexpr auto dept_fk = surface_account_entity::column<"department_id">("a");
    constexpr auto dept_pk = surface_department_entity::column<"id">("d");
    constexpr auto avatar_col = surface_account_entity::column<"avatar">("a");

    ruvia::db_query q;
    q.select({handle_col.select()})
        .from("accounts", "a")
        .where((handle_col == "bob") && (dept_fk == dept_pk) && (avatar_col != ruvia::byte_view(raw_avatar)));

#ifdef RUVIA_ENABLE_POSTGRESQL
    const auto pg_compiled = q.compile(ruvia::db_driver::postgresql);
    EXPECT_NE(pg_compiled.sql().find(R"("a"."handle" = $1)"), std::string_view::npos);
    EXPECT_NE(pg_compiled.sql().find(R"("a"."department_id" = "d"."id")"), std::string_view::npos);
    ASSERT_EQ(pg_compiled.params().size(), std::size_t{2});
    EXPECT_EQ(pg_compiled.params()[1].type(), ruvia::db_value_type::bytes);

    ruvia::db_query bin_lit;
    bin_lit.select({ruvia::db_expr::value(ruvia::byte_view(raw_avatar)).as("raw")});
    const auto pg_lit_stmt = bin_lit.compile(ruvia::db_driver::postgresql, ruvia::db_parameter_mode::literal);
    EXPECT_NE(pg_lit_stmt.sql().find(R"('\x0102ff'::bytea)"), std::string_view::npos);
#endif
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::db_query maria_lit;
    maria_lit.select({ruvia::db_expr::value(ruvia::byte_view(raw_avatar)).as("raw")});
    const auto maria_lit_stmt = maria_lit.compile(ruvia::db_driver::mariadb, ruvia::db_parameter_mode::literal);
    EXPECT_NE(maria_lit_stmt.sql().find("X'0102ff'"), std::string_view::npos);
#endif
}
#endif
