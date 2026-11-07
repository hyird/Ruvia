#include "ruvia/web/session.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

#include "http_server_fixture.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/web/redis/redis_handle.h"

TEST(Ruvia, session_middleware_rejects_invalid_config_before_use) {
    const auto rejected = [](const ruvia::session_config& config) {
        try {
            const ruvia::session_middleware middleware(config);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    auto config = ruvia::session_config{};
    config.redis_alias_ = "";
    EXPECT_TRUE(rejected(config));

    config = ruvia::session_config{};
    config.cookie_name_ = "bad cookie";
    EXPECT_TRUE(rejected(config));

    config = ruvia::session_config{};
    config.key_prefix_ = "";
    EXPECT_TRUE(rejected(config));

    config = ruvia::session_config{};
    config.ttl_ = std::chrono::seconds::zero();
    EXPECT_TRUE(rejected(config));

    // A new session is written with a millisecond expiry.
    constexpr auto max_ttl = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max());
    config.ttl_ = max_ttl + std::chrono::seconds(1);
    EXPECT_TRUE(rejected(config));
    config.ttl_ = std::chrono::seconds::max();
    EXPECT_TRUE(rejected(config));
}

namespace session_test {

// Writes a cookie under the session's name before clearing the session, so the
// commit stage finds a same-name Set-Cookie already in the response. The
// session middleware only runs on matched requests, so this needs a route.
class same_name_cookie_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/session-test")
    RUVIA_ROUTES_BEGIN(same_name_cookie_controller)
    RUVIA_GET("/same-name-cookie", same_name_cookie);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> same_name_cookie(ruvia::context& c) {
        c.draft().set_cookie({.name_ = "ruvia_session", .value_ = "handler"});
        c.session().clear();
        co_return c.text("ok");
    }
};

class stored_session_controller final {
public:
    RUVIA_ROUTES_BEGIN(stored_session_controller)
    RUVIA_GET("/session-ids/:space/read", read);
    RUVIA_GET("/session-ids/:space/set", set);
    RUVIA_GET("/session-ids/:space/clear", clear);
    RUVIA_GET("/session-ids/:space/rotate", rotate);
    RUVIA_GET("/session-ids/b/stale", stale);
    RUVIA_GET("/session-store", stored);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> read(ruvia::context& c) {
        auto value = c.session();
        co_return c.text(value.data().empty() ? "empty" : value.data());
    }

    ruvia::task<ruvia::http_response> set(ruvia::context& c) {
        c.session().set("private");
        co_return c.text("set");
    }

    ruvia::task<ruvia::http_response> clear(ruvia::context& c) {
        c.session().clear();
        co_return c.text("clear");
    }

    ruvia::task<ruvia::http_response> rotate(ruvia::context& c) {
        c.session().regenerate();
        co_return c.text("rotate");
    }

    ruvia::task<ruvia::http_response> stale(ruvia::context& c) {
        // Revoke after middleware loaded the session but before response commit.
        const auto id = c.req().cookie("sid_b");
        if (id) {
            const std::string key = "sess:a" + std::string(*id);
            (void)co_await c.redis().del(key);
        }
        c.session().regenerate();
        co_return c.text("stale");
    }

    ruvia::task<ruvia::http_response> stored(ruvia::context& c) {
        const auto id = c.req().query("id");
        const std::string key = "sess:" + std::string(id.value_or(""));
        const auto value = co_await c.redis().get(key);
        co_return c.text(value ? std::string_view(*value) : "empty");
    }
};

// Stateful, isolated RESP store. All map access and connection teardown run on
// its own event loop. Expiry is not simulated: these cases do not test time.
class session_store_peer final {
public:
    session_store_peer()
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          work_(asio::make_work_guard(io_)) {
        for (const auto size : {47, 48, 49, 128}) {
            store_.emplace("sess:" + std::string(size, 'b'), "existing");
        }
        store_.emplace("sess:" + std::string(48, 'B'), "existing");
        store_.emplace("sess:" + std::string(48, 'g'), "existing");
        asio::co_spawn(io_, accept(), [this](std::exception_ptr failure) { fail(failure); });
        thread_ = std::thread([this] { io_.run(); });
    }

    ~session_store_peer() {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
            work_.reset();
        });
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    using socket_type = asio::ip::tcp::socket;

    void fail(std::exception_ptr failure) {
        if (failure) {
            failure_ = failure;
            std::error_code ignored;
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
        }
    }

    [[nodiscard]] bool closed(const std::error_code& error) const noexcept {
        return error == asio::error::eof || error == asio::error::connection_reset ||
               error == asio::error::connection_aborted ||
               (closing_ && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor));
    }

    asio::awaitable<void> accept() {
        try {
            for (;;) {
                auto socket = std::make_shared<socket_type>(io_);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);
                sockets_.push_back(socket);
                asio::co_spawn(io_, serve(socket), [this](std::exception_ptr failure) { fail(failure); });
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    static asio::awaitable<std::string> line(socket_type& socket, std::string& buffer) {
        const auto size = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffer), "\r\n", asio::use_awaitable);
        auto result = buffer.substr(0, size - 2);
        buffer.erase(0, size);
        co_return result;
    }

    static asio::awaitable<std::vector<std::string>> command(socket_type& socket, std::string& buffer) {
        const auto header = co_await line(socket, buffer);
        if (header.empty() || header.front() != '*') {
            throw std::runtime_error("expected RESP array");
        }
        const auto count = std::stoull(header.substr(1));
        std::vector<std::string> args;
        for (std::size_t index = 0; index < count; ++index) {
            const auto bulk = co_await line(socket, buffer);
            if (bulk.empty() || bulk.front() != '$') {
                throw std::runtime_error("expected RESP bulk string");
            }
            const auto size = std::stoull(bulk.substr(1));
            if (buffer.size() < size + 2) {
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer),
                    asio::transfer_exactly(size + 2 - buffer.size()), asio::use_awaitable);
            }
            args.push_back(buffer.substr(0, size));
            buffer.erase(0, size + 2);
        }
        co_return args;
    }

    std::string execute(const std::vector<std::string>& args) {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (args.size() == 2 && args[0] == "GET") {
            const auto found = store_.find(args[1]);
            return found == store_.end() ? "$-1\r\n" : "$" + std::to_string(found->second.size()) + "\r\n" + found->second + "\r\n";
        }
        if (args.size() >= 3 && args[0] == "SET") {
            // Session creation uses NX; an existing key must remain unchanged.
            return store_.emplace(args[1], args[2]).second ? "+OK\r\n" : "$-1\r\n";
        }
        if (args.size() == 2 && args[0] == "DEL") {
            return ":" + std::to_string(store_.erase(args[1])) + "\r\n";
        }
        if (args.size() == 7 && args[0] == "EVAL" && args[2] == "2") {
            // Atomic compare-and-replace of an existing identity, not a canned
            // script response: missing sources and occupied targets cannot rotate.
            if (!store_.contains(args[3]) || !store_.emplace(args[4], args[5]).second) {
                return ":0\r\n";
            }
            store_.erase(args[3]);
            return ":1\r\n";
        }
        throw std::runtime_error("unsupported session store command");
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::string buffer;
        try {
            for (;;) {
                const auto args = co_await command(*socket, buffer);
                const auto reply = execute(args);
                co_await asio::async_write(*socket, asio::buffer(reply), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::map<std::string, std::string> store_;
    std::vector<std::shared_ptr<socket_type>> sockets_;
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

struct session_response final {
    unsigned status_;
    std::string body_;
    std::string cookie_;
};

[[nodiscard]] session_response request(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view target, std::string_view cookie = {}) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    const std::string wire = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nCookie: " + std::string(cookie) + "\r\n\r\n";
    asio::write(socket, asio::buffer(wire));
    socket.non_blocking(true);
    std::string response;
    std::array<char, 4096> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        std::error_code error;
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (error == asio::error::eof) {
            break;
        }
        if (error && error != asio::error::would_block && error != asio::error::try_again) {
            throw std::system_error(error);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("session HTTP response timed out");
        }
        if (error) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto head_end = response.find("\r\n\r\n");
    if (!response.starts_with("HTTP/1.1 ") || head_end == std::string::npos) {
        throw std::runtime_error("invalid session HTTP response");
    }
    session_response result{static_cast<unsigned>(std::stoul(response.substr(9, 3))), response.substr(head_end + 4), {}};
    const auto cookie_start = response.find("\r\nSet-Cookie: ");
    if (cookie_start != std::string::npos && cookie_start < head_end) {
        const auto start = cookie_start + std::string_view("\r\nSet-Cookie: ").size();
        result.cookie_ = response.substr(start, response.find(';', start) - start);
    }
    return result;
}

}  // namespace session_test

TEST(Ruvia, session_cookie_replaces_a_same_name_cookie_set_by_the_handler) {
    ruvia::test_app app;
    {
        ruvia::session_config config;
        config.cookie_name_ = "ruvia_session";
        app.use<ruvia::session_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/session-test/same-name-cookie"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    std::size_t session_cookies = 0;
    bool expired = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie") || !header.value().starts_with("ruvia_session=")) {
            continue;
        }
        ++session_cookies;
        expired = expired || (header.value().starts_with("ruvia_session=;") &&
                                 header.value().find("; Expires=Thu, 01 Jan 1970 00:00:00 GMT") != std::string_view::npos &&
                                 header.value().find("Max-Age") == std::string_view::npos);
    }
    EXPECT_EQ(session_cookies, std::size_t{1});
    EXPECT_TRUE(expired);
}

TEST(Ruvia, session_ids_isolate_prefixes_and_preserve_fixed_length_lifecycle) {
    // Construct before the shared HTTP runtime so the server retires its Redis
    // client before this peer is destroyed at process exit.
    static session_test::session_store_peer peer;
    ruvia::app()
        .redis({.config_ = {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = std::chrono::seconds(2), .command_timeout_ = std::chrono::seconds(2)}})
        .use_at<ruvia::session_middleware>({.prefix_ = "/session-ids/a"},
            ruvia::session_config{.cookie_name_ = "sid_a", .key_prefix_ = "sess:"})
        .use_at<ruvia::session_middleware>({.prefix_ = "/session-ids/b"},
            ruvia::session_config{.cookie_name_ = "sid_b", .key_prefix_ = "sess:a"});
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto request = [&](std::string_view path, std::string_view cookie = {}) {
        return session_test::request(io, server.endpoint(), path, cookie);
    };
    const auto expect = [&](std::string_view path, std::string_view cookie, std::string_view body) {
        const auto response = request(path, cookie);
        EXPECT_EQ(response.status_, 200U);
        EXPECT_EQ(response.body_, body);
        return response;
    };

    // These IDs have actual stored values. Rejection must hide the data and
    // leave it unchanged across both clear and regenerate, not merely avoid a
    // backend error. Uppercase and non-hex IDs still remain invalid at length 48.
    for (const auto& id : {std::string(47, 'b'), std::string(49, 'b'), std::string(128, 'b'),
             std::string(48, 'B'), std::string(48, 'g')}) {
        const auto cookie = "sid_a=" + id;
        expect("/session-ids/a/read", cookie, "empty");
        expect("/session-ids/a/clear", cookie, "clear");
        const auto rotated = expect("/session-ids/a/rotate", cookie, "rotate");
        EXPECT_TRUE(rotated.cookie_.empty());
        expect("/session-store?id=" + id, {}, "existing");
    }

    // A freshly minted B session must not be loadable or mutable through A's
    // shorter prefix, even though raw prefix + presented ID names the same key.
    for (const auto action : {"read", "clear", "rotate"}) {
        const auto created = expect("/session-ids/b/set", {}, "set");
        EXPECT_TRUE(created.cookie_.starts_with("sid_b="));
        EXPECT_EQ(created.cookie_.size(), std::size_t{6 + 48});
        const auto alias = "sid_a=a" + created.cookie_.substr(6);
        const auto response = expect(std::string("/session-ids/a/") + action, alias,
            std::string_view(action) == "read" ? "empty" : action);
        if (std::string_view(action) != "clear") {
            EXPECT_TRUE(response.cookie_.empty());
        }
        expect("/session-ids/b/read", created.cookie_, "private");
        expect("/session-ids/b/clear", created.cookie_, "clear");
        expect("/session-ids/b/read", created.cookie_, "empty");
    }

    // An already-stored 48-character ID retains the same raw key format.
    const auto existing = "sid_a=" + std::string(48, 'b');
    const auto loaded = expect("/session-ids/a/read", existing, "existing");
    EXPECT_TRUE(loaded.cookie_.empty());
    const auto rotated = expect("/session-ids/a/rotate", existing, "rotate");
    EXPECT_EQ(rotated.cookie_.size(), std::size_t{6 + 48});
    EXPECT_TRUE(rotated.cookie_ != existing);
    expect("/session-ids/a/read", existing, "empty");
    expect("/session-ids/a/read", rotated.cookie_, "existing");
    expect("/session-ids/a/clear", rotated.cookie_, "clear");
    expect("/session-ids/a/read", rotated.cookie_, "empty");

    const auto created = expect("/session-ids/b/set", {}, "set");
    const auto stale = request("/session-ids/b/stale", created.cookie_);
    EXPECT_EQ(stale.status_, 409U);
    EXPECT_TRUE(stale.cookie_.empty());
    expect("/session-ids/b/read", created.cookie_, "empty");
    server.finish();
}

#endif
