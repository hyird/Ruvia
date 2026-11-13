#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/http_client.h"

#include "output_drain.h"
#include "test_io_context.h"
#include "test_tls_identity.h"

namespace origin_routing_tests {

using namespace std::chrono_literals;

// What one accepted connection saw: TLS server name and ALPN offer, and the
// Host / :authority of each request in order.
struct observed_connection final {
    std::string server_name_;
    std::string offered_alpn_;
    std::string selected_alpn_;
    std::deque<std::string> authorities_;
    bool closed_{false};
};

// A real TCP or TLS origin on the client's own io_context. HTTP/1 requests
// receive their target as the body; "/hold" waits for release(). A TLS
// connection that negotiated h2 is served by the sans-I/O HTTP/2 core, and an
// HTTP/1 Upgrade request is switched with 101.
class routing_peer final {
public:
    routing_peer(asio::io_context& io, asio::ssl::context* tls)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          gate_(io),
          tls_(tls) {
        gate_.expires_at(asio::steady_timer::time_point::max());
        if (tls_ != nullptr) {
            SSL_CTX_set_alpn_select_cb(tls_->native_handle(), select_alpn, nullptr);
        }
        ++active_;
        asio::co_spawn(io, accept(), asio::detached);
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void release() {
        gate_.expires_at(asio::steady_timer::time_point::min());
    }
    void stop() {
        std::error_code ignored;
        acceptor_.close(ignored);
        release();
    }
    [[nodiscard]] bool finished() const noexcept {
        return active_ == 0;
    }

    std::deque<observed_connection> connections_;
    std::size_t held_{0};

private:
    // asio owns the SSL app data; the observation uses its own slot.
    [[nodiscard]] static int observation_index() {
        static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        return index;
    }
    static int select_alpn(SSL* ssl, const unsigned char** out, unsigned char* out_size, const unsigned char* in,
        unsigned int in_size, void*) {
        auto& observed = *static_cast<observed_connection*>(SSL_get_ex_data(ssl, observation_index()));
        observed.offered_alpn_.assign(reinterpret_cast<const char*>(in), in_size);
        static constexpr unsigned char preference[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        unsigned char* selected = nullptr;
        if (SSL_select_next_proto(&selected, out_size, preference, sizeof(preference), in, in_size) != OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        *out = selected;
        observed.selected_alpn_.assign(reinterpret_cast<const char*>(selected), *out_size);
        return SSL_TLSEXT_ERR_OK;
    }

    asio::awaitable<void> accept() {
        for (;;) {
            std::error_code error;
            auto socket = co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            auto& observed = connections_.emplace_back();
            ++active_;
            asio::co_spawn(acceptor_.get_executor(), serve(std::move(socket), observed), asio::detached);
        }
        --active_;
    }

    asio::awaitable<void> serve(asio::ip::tcp::socket socket, observed_connection& observed) {
        try {
            if (tls_ == nullptr) {
                co_await serve_http1(socket, observed);
            } else {
                asio::ssl::stream<asio::ip::tcp::socket&> stream(socket, *tls_);
                SSL_set_ex_data(stream.native_handle(), observation_index(), &observed);
                co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
                if (const char* name = SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name)) {
                    observed.server_name_ = name;
                }
                if (observed.selected_alpn_ == "h2") {
                    co_await serve_http2(stream, observed);
                } else {
                    co_await serve_http1(stream, observed);
                }
            }
        } catch (const std::exception&) {
            // A client close or abort ends the connection; callers inspect closed_.
        }
        observed.closed_ = true;
        --active_;
    }

    template <typename stream_type>
    asio::awaitable<void> serve_http1(stream_type& stream, observed_connection& observed) {
        std::string input;
        for (;;) {
            const auto head_bytes = co_await asio::async_read_until(stream, asio::dynamic_buffer(input), "\r\n\r\n", asio::use_awaitable);
            const std::string head = input.substr(0, head_bytes);
            input.erase(0, head_bytes);
            const auto host = head.find("\r\nHost: ");
            const auto host_end = head.find("\r\n", host + 2);
            observed.authorities_.push_back(host == std::string::npos ? std::string() : head.substr(host + 8, host_end - host - 8));
            const auto target = head.substr(head.find(' ') + 1, head.find(' ', head.find(' ') + 1) - head.find(' ') - 1);
            if (head.find("\r\nUpgrade: ") != std::string::npos) {
                constexpr std::string_view switched = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: chat\r\n\r\n";
                co_await asio::async_write(stream, asio::buffer(switched), asio::use_awaitable);
                std::array<char, 256> bytes{};
                for (;;) {
                    (void)co_await stream.async_read_some(asio::buffer(bytes), asio::use_awaitable);
                }
            }
            if (target == "/hold") {
                ++held_;
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(target.size()) + "\r\n\r\n" + target;
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    }

    template <typename stream_type>
    asio::awaitable<void> serve_http2(stream_type& stream, observed_connection& observed) {
        auto connection = ruvia::http2_connection::server();
        std::pmr::string output(ruvia::process_resource());
        std::array<char, 16384> input{};
        for (;;) {
            ruvia::test::drain_output(connection, output);
            if (!output.empty()) {
                co_await asio::async_write(stream, asio::buffer(output), asio::use_awaitable);
                output.clear();
            }
            const auto count = co_await stream.async_read_some(asio::buffer(input), asio::use_awaitable);
            const std::string_view bytes(input.data(), count);
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 input");
                }
                while (auto event = connection.next_event()) {
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto built = request->build_request();
                    observed.authorities_.emplace_back(built.authority());
                    ruvia::http_response response;
                    const auto submitted = connection.submit_response_head(request->stream_id(), response,
                        ruvia::plan_buffered_http_response_write(built.known_method(), response));
                    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted) ||
                        connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                        throw std::runtime_error("HTTP/2 response rejected");
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer gate_;
    asio::ssl::context* tls_;
    std::size_t active_{0};
};

// Runs `exercise` against a client of `config`, then retires the client and
// every peer connection before the loop stops.
template <typename exercise_type>
void run_client(asio::io_context& io, ruvia::event_loop_attachment& attachment, std::initializer_list<routing_peer*> peers,
    const ruvia::http_client_config& config, exercise_type&& exercise) {
    std::exception_ptr failure;
    auto loop = attachment.loop();
    const auto& worker = loop.handle();
    const auto run = [&]() -> ruvia::task<void> {
        {
            ruvia::http_client client(loop, config);
            try {
                co_await exercise(client, worker);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        for (auto* peer : peers) {
            peer->stop();
        }
        for (auto* peer : peers) {
            while (!peer->finished()) {
                (void)co_await ruvia::sleep_for(worker, 1ms);
            }
        }
        attachment.stop();
    };
    auto root = loop.start(run());
    attachment.run();
    std::move(root).get();
    (void)io;
    if (failure) {
        std::rethrow_exception(failure);
    }
}

[[nodiscard]] ruvia::http_client_request_options route(std::optional<std::string_view> authority,
    std::optional<std::string_view> server_name = std::nullopt) {
    ruvia::http_client_request_options options;
    if (authority) {
        options.authority_.emplace(*authority);
    }
    if (server_name) {
        options.server_name_.emplace(*server_name);
    }
    return options;
}

ruvia::task<std::uint64_t> get(ruvia::http_client& client, std::string_view target, ruvia::http_client_request_options options,
    bool expect_reused) {
    auto response = co_await client.client().get(target).configure(std::move(options)).send();
    EXPECT_TRUE(response.ok());
    EXPECT_EQ(response.connection_info().reused_, expect_reused);
    const auto body = co_await response.text();
    EXPECT_EQ(body.text(), target);
    co_return response.connection_info().id_;
}

}  // namespace origin_routing_tests

TEST(Ruvia, http_client_request_routes_send_their_host_and_sni_and_share_connections_by_server_name) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("a.example", "DNS:a.example,DNS:b.example,DNS:c.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://alias.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.protocol_ = ruvia::http_client_protocol::http1_only;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_handle&) -> ruvia::task<void> {
        EXPECT_EQ(client.client().stats().open_connections_, 0U);
        const auto a = co_await get(client, "/a1", route("a.example"), false);
        // HTTP/1 keep-alive reuse follows (dial, server name); Host is not part of the key.
        const auto a_again = co_await get(client, "/a2", route("a.example"), true);
        EXPECT_EQ(a_again, a);
        const auto b = co_await get(client, "/b1", route("b.example:8443"), false);
        EXPECT_NE(b, a);
        const auto c = co_await get(client, "/c1", route("c.example", "a.example"), true);
        EXPECT_EQ(c, a);
        EXPECT_EQ(client.client().stats().open_connections_, 2U);
        EXPECT_THROW((void)client.client().get("/").configure(route("user@a.example")), std::invalid_argument);
        EXPECT_THROW((void)client.client().get("/").configure(route("a.example:0")), std::invalid_argument);
        EXPECT_THROW((void)client.client().get("/").configure(route(std::nullopt, "bad host")), std::invalid_argument);
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    const auto& first = peer.connections_[0];
    EXPECT_EQ(first.server_name_, "a.example");
    EXPECT_EQ(first.authorities_, (std::deque<std::string>{"a.example", "a.example", "c.example"}));
    const auto& second = peer.connections_[1];
    EXPECT_EQ(second.server_name_, "b.example");
    EXPECT_EQ(second.authorities_, (std::deque<std::string>{"b.example:8443"}));
}

TEST(Ruvia, http_client_request_dial_overrides_reach_another_listener) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer alias_peer(io, nullptr);
    routing_peer other_peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(alias_peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&alias_peer, &other_peer}, config, [&](ruvia::http_client& client, const ruvia::worker_handle&) -> ruvia::task<void> {
        (void)co_await get(client, "/alias", {}, false);
        ruvia::http_client_request_options options = route("visitor.example:8080");
        options.dial_host_.emplace("127.0.0.1");
        options.dial_port_ = other_peer.port();
        (void)co_await get(client, "/other", options, false);
        // The alias connection is not the overridden route's.
        options.dial_port_ = alias_peer.port();
        (void)co_await get(client, "/back", options, true);
    });
    ASSERT_EQ(other_peer.connections_.size(), 1U);
    EXPECT_EQ(other_peer.connections_[0].authorities_, (std::deque<std::string>{"visitor.example:8080"}));
    ASSERT_EQ(alias_peer.connections_.size(), 1U);
    EXPECT_EQ(alias_peer.connections_[0].authorities_.size(), 2U);
    EXPECT_EQ(alias_peer.connections_[0].authorities_.back(), "visitor.example:8080");
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http_client_upgrade_on_a_negotiated_h2_origin_uses_an_http11_connection) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_handle&) -> ruvia::task<void> {
        auto ordinary = co_await client.client().get("/ordinary").send();
        EXPECT_TRUE(ordinary.protocol_version() == ruvia::http_protocol_version::http2);
        (void)co_await ordinary.text();
        auto upgraded = co_await client.client().get("/chat").header("Connection", "Upgrade").header("Upgrade", "chat").upgrade();
        auto* tunnel = upgraded.tunnel();
        if (tunnel == nullptr) {
            ADD_FAILURE() << "Upgrade was not switched";
            co_return;
        }
        EXPECT_EQ(tunnel->status().value(), 101);
        EXPECT_TRUE(tunnel->protocol_version() == ruvia::http_protocol_version::http11);
        tunnel->abort();
        // Ordinary requests keep multiplexing on the h2 connection.
        auto again = co_await client.client().get("/again").send();
        EXPECT_TRUE(again.protocol_version() == ruvia::http_protocol_version::http2);
        EXPECT_TRUE(again.connection_info().reused_);
        (void)co_await again.text();
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_EQ(peer.connections_[0].selected_alpn_, "h2");
    EXPECT_EQ(peer.connections_[0].authorities_, (std::deque<std::string>{"origin.example", "origin.example"}));
    EXPECT_EQ(peer.connections_[1].offered_alpn_, std::string_view("\x08http/1.1"));
    EXPECT_EQ(peer.connections_[1].selected_alpn_, "http/1.1");
}
#endif

TEST(Ruvia, http_client_connection_limits_bound_open_and_idle_connections_without_preallocation) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    config.connection_limits_ = {.max_open_ = 2, .max_idle_http1_ = 1, .max_idle_multiplexed_ = 0};
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_handle& worker) -> ruvia::task<void> {
        const auto handle = client.client();
        EXPECT_EQ(handle.stats().open_connections_, 0U);
        std::optional<std::uint64_t> first_id;
        std::optional<std::uint64_t> second_id;
        ruvia::task_scope held(worker);
        const auto hold = [&](std::optional<std::uint64_t>& id) -> ruvia::task<void> {
            auto response = co_await handle.get("/hold").send();
            id = response.connection_info().id_;
            (void)co_await response.text();
        };
        held.spawn(hold(first_id));
        held.spawn(hold(second_id));
        while (peer.held_ != 2) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
        EXPECT_EQ(handle.stats().open_connections_, 2U);
        // max_open_ is reached by two active connections: a third request
        // waits for admission until its acquisition bound.
        auto waiting = handle.get("/late");
        waiting.timeouts({.acquire_ = ruvia::http_client_timeout_override::after(50ms)});
        try {
            (void)co_await waiting.send();
            ADD_FAILURE() << "admission beyond max_open_ succeeded";
        } catch (const ruvia::http_client_error& error) {
            EXPECT_EQ(error.code(), ruvia::http_client_error::code_type::timeout);
        }
        peer.release();
        co_await held.join();
        // Beyond max_idle_http1_ the least recently used idle connection closed.
        for (int attempt = 0; attempt != 1000 && handle.stats().open_connections_ != 1; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
        EXPECT_EQ(handle.stats().open_connections_, 1U);
        EXPECT_EQ(handle.stats().idle_connections_, 1U);
        auto reused = co_await handle.get("/reuse").send();
        EXPECT_TRUE(reused.connection_info().reused_);
        const auto kept = reused.connection_info().id_;
        EXPECT_TRUE(kept == *first_id || kept == *second_id);
        (void)co_await reused.text();
        EXPECT_TRUE(handle.close_connection(kept));
        for (int attempt = 0; attempt != 1000 && handle.stats().open_connections_ != 0; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
        EXPECT_EQ(handle.stats().open_connections_, 0U);
        EXPECT_EQ(handle.stats().idle_connections_, 0U);
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_TRUE(peer.connections_[0].closed_);
    EXPECT_TRUE(peer.connections_[1].closed_);
}
