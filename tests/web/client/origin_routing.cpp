#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/blocking_pool.h"
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
    // HTTP/2 RST_STREAM error codes the client sent.
    std::deque<ruvia::http2_error_code> resets_;
    // HTTP/1 request content bytes read before the response.
    std::size_t content_bytes_{0};
    bool closed_{false};
};

// A real TCP or TLS origin on the client's own io_context. HTTP/1 requests
// receive their target as the body; "/hold" waits for release(), "/drain"
// first reads the complete chunked request content, "/early" answers before
// reading any content, and "/redirect" answers 302 to redirect_location_. A
// TLS connection that negotiated h2 is served by the sans-I/O HTTP/2 core, and
// an HTTP/1 Upgrade request is switched with 101 and echoes its bytes.
class routing_peer final {
public:
    routing_peer(asio::io_context& io, asio::ssl::context* tls, std::string_view address = "127.0.0.1")
        : acceptor_(io, {asio::ip::make_address(address), 0}),
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
    std::string redirect_location_;
    // Sent as Alt-Svc with every HTTP/1 200 response when not empty.
    std::string alt_svc_;
    std::vector<std::string> extra_alt_svc_headers_;

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
                    const auto count = co_await stream.async_read_some(asio::buffer(bytes), asio::use_awaitable);
                    co_await asio::async_write(stream, asio::buffer(bytes.data(), count), asio::use_awaitable);
                }
            }
            if (target == "/hold") {
                ++held_;
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (target == "/drain") {
                // Chunked content up to its last chunk, read before answering.
                const auto end = co_await asio::async_read_until(stream, asio::dynamic_buffer(input), "\r\n0\r\n\r\n", asio::use_awaitable);
                observed.content_bytes_ += end;
                input.erase(0, end);
            }
            if (target == "/redirect" || target == "/drain-redirect") {
                if (target == "/drain-redirect") {
                    const auto end = co_await asio::async_read_until(
                        stream, asio::dynamic_buffer(input), "\r\n0\r\n\r\n", asio::use_awaitable);
                    observed.content_bytes_ += end;
                    input.erase(0, end);
                }
                const std::string response = "HTTP/1.1 302 Found\r\nLocation: " +
                                             redirect_location_ + "\r\nContent-Length: 0\r\n\r\n";
                co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
                continue;
            }
            std::string alt_svc_lines = alt_svc_.empty() ? std::string() : "Alt-Svc: " + alt_svc_ + "\r\n";
            for (const auto& extra : extra_alt_svc_headers_) {
                alt_svc_lines += "Alt-Svc: " + extra + "\r\n";
            }
            const std::string response = "HTTP/1.1 200 OK\r\n" + alt_svc_lines +
                                         "Content-Length: " + std::to_string(target.size()) + "\r\n\r\n" + target;
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    }

    template <typename stream_type>
    asio::awaitable<void> serve_http2(stream_type& stream, observed_connection& observed) {
        auto connection = ruvia::http2_connection::server();
        std::deque<ruvia::http2_request_head_event> unreleased;
        std::string output;
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
                    if (const auto* closed = event->stream_closed();
                        closed != nullptr && closed->source() == ruvia::http2_stream_close_source::peer) {
                        observed.resets_.push_back(closed->error());
                    }
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto built = request->build_request();
                    observed.authorities_.emplace_back(built.authority());
                    ruvia::http_response response;
                    const auto submitted = connection.submit_response_head(request->stream_id(), response,
                        ruvia::plan_buffered_http_response_write(built.known_method(), response));
                    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted)) {
                        throw std::runtime_error("HTTP/2 response rejected");
                    }
                    if (built.target() == "/early") {
                        // Keep receiving the request content after the
                        // complete response, so only the client ends it.
                        unreleased.push_back(std::move(*request));
                    } else if (connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
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
void run_clients(ruvia::event_loop_attachment& attachment, std::initializer_list<routing_peer*> peers,
    std::span<const ruvia::http_client_config> configs, const ruvia::http_client_worker_config& worker_config,
    exercise_type&& exercise) {
    std::exception_ptr failure;
    auto loop = attachment.loop();
    const auto& worker = loop.worker();
    const auto run = [&]() -> ruvia::task<void> {
        {
            ruvia::http_client client(loop, configs, worker_config);
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
    if (failure) {
        std::rethrow_exception(failure);
    }
}

template <typename exercise_type>
void run_client(asio::io_context& io, ruvia::event_loop_attachment& attachment, std::initializer_list<routing_peer*> peers,
    const ruvia::http_client_config& config, exercise_type&& exercise) {
    (void)io;
    run_clients(attachment, peers, std::span<const ruvia::http_client_config>(&config, 1), {},
        std::forward<exercise_type>(exercise));
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
    config.versions_ = ruvia::http_client_versions{.http2_ = false};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
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
    run_client(io, attachment, {&alias_peer, &other_peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        (void)co_await get(client, "/alias", {}, false);
        ruvia::http_client_request_options options = route("visitor.example:8080");
        options.dial_host_.emplace("127.0.0.1");
        options.dial_port_ = other_peer.port();
        (void)co_await get(client, "/other", std::move(options), false);
        // The alias connection is not the overridden route's.
        options = route("visitor.example:8080");
        options.dial_host_.emplace("127.0.0.1");
        options.dial_port_ = alias_peer.port();
        (void)co_await get(client, "/back", std::move(options), true);
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
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
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

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http_client_request_versions_override_the_alias_offer) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        auto multiplexed = co_await client.client().get("/h2").send();
        EXPECT_TRUE(multiplexed.protocol_version() == ruvia::http_protocol_version::http2);
        (void)co_await multiplexed.text();
        ruvia::http_client_request_options http1;
        http1.versions_ = ruvia::http_client_versions{.http2_ = false};
        auto serial = co_await client.client().get("/h1").configure(std::move(http1)).send();
        EXPECT_TRUE(serial.protocol_version() == ruvia::http_protocol_version::http11);
        const auto body = co_await serial.text();
        EXPECT_EQ(body.text(), "/h1");
        ruvia::http_client_request_options none;
        none.versions_ = ruvia::http_client_versions{.http1_ = false, .http2_ = false};
        EXPECT_THROW((void)client.client().get("/").configure(std::move(none)), std::invalid_argument);
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_EQ(peer.connections_[0].selected_alpn_, "h2");
    EXPECT_EQ(peer.connections_[1].offered_alpn_, std::string_view("\x08http/1.1"));
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, http_client_unreachable_alt_svc_falls_back_to_tcp_and_rests) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    // A UDP endpoint that never answers a QUIC Initial.
    asio::ip::udp::socket sink(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::size_t datagrams = 0;
    bool sink_done = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::array<char, 2048> bytes{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            std::error_code error;
            (void)co_await sink.async_receive_from(asio::buffer(bytes), sender, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            ++datagrams;
        }
        sink_done = true; }, asio::detached);
    peer.alt_svc_ = "h3=\":" + std::to_string(sink.local_endpoint().port()) + "\"";
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = true};
    config.connect_timeout_ = 300ms;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        auto learned = co_await client.client().get("/learn").send();
        EXPECT_TRUE(learned.protocol_version() == ruvia::http_protocol_version::http11);
        (void)co_await learned.text();
        // The advertised alternative never answers: the request falls back to TCP.
        auto fallback = co_await client.client().get("/fallback").send();
        EXPECT_TRUE(fallback.protocol_version() == ruvia::http_protocol_version::http11);
        const auto fallback_body = co_await fallback.text();
        EXPECT_EQ(fallback_body.text(), "/fallback");
        (void)co_await ruvia::sleep_for(worker, 100ms);
        EXPECT_NE(datagrams, 0U);
        const auto attempted = datagrams;
        // The failed alternative rests even though every response re-advertises it.
        for (int request = 0; request != 3; ++request) {
            auto rested = co_await client.client().get("/rested").send();
            EXPECT_TRUE(rested.protocol_version() == ruvia::http_protocol_version::http11);
            (void)co_await rested.text();
        }
        (void)co_await ruvia::sleep_for(worker, 100ms);
        EXPECT_EQ(datagrams, attempted);
        std::error_code ignored;
        sink.close(ignored);
        while (!sink_done) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
    });
}

TEST(Ruvia, http_client_origin_less_alias_follows_alt_svc_per_route) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    asio::ip::udp::socket sink(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::size_t datagrams = 0;
    bool sink_done = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::array<char, 2048> bytes{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            std::error_code error;
            (void)co_await sink.async_receive_from(asio::buffer(bytes), sender, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            ++datagrams;
        }
        sink_done = true; }, asio::detached);
    const auto alternative_port = sink.local_endpoint().port();
    peer.alt_svc_ = "h3=\":" + std::to_string(alternative_port) + "\"";
    std::vector<std::uint16_t> validated_ports;
    ruvia::http_client_config config{.base_url_ = "https://"};
    config.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([&validated_ports](const ruvia::ip_address&, std::uint16_t port) {
        validated_ports.push_back(port);
        return true;
    });
    config.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = true};
    config.connect_timeout_ = 300ms;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    const auto route = [&peer] {
        ruvia::http_client_request_options options;
        options.authority_ = std::string("origin.example");
        options.dial_host_ = std::string("127.0.0.1");
        options.dial_port_ = ruvia::port_number(peer.port());
        return options;
    };
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        auto learned = co_await client.client().get("/learn").configure(route()).send();
        EXPECT_TRUE(learned.protocol_version() == ruvia::http_protocol_version::http11);
        (void)co_await learned.text();
        // The route's advertised alternative is tried, then TCP serves the request.
        auto fallback = co_await client.client().get("/fallback").configure(route()).send();
        EXPECT_TRUE(fallback.protocol_version() == ruvia::http_protocol_version::http11);
        const auto body = co_await fallback.text();
        EXPECT_EQ(body.text(), "/fallback");
        (void)co_await ruvia::sleep_for(worker, 100ms);
        EXPECT_NE(datagrams, 0U);
        std::error_code ignored;
        sink.close(ignored);
        while (!sink_done) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
    });
    // The advertised endpoint passed the alias's address validator before dialing.
    EXPECT_NE(std::ranges::find(validated_ports, alternative_port), validated_ports.end());
}

TEST(Ruvia, http_client_second_alt_svc_clear_header_evicts_advertised_service) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    asio::ip::udp::socket sink(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::size_t datagrams = 0;
    bool sink_done = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::array<char, 2048> bytes{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            std::error_code error;
            (void)co_await sink.async_receive_from(asio::buffer(bytes), sender, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            ++datagrams;
        }
        sink_done = true; }, asio::detached);

    const auto alternative_port = sink.local_endpoint().port();
    // First advertise h3, then clear it in a second Alt-Svc header field on the same response (RFC 7838 §3).
    peer.alt_svc_ = "h3=\":" + std::to_string(alternative_port) + "\"";
    peer.extra_alt_svc_headers_ = {"clear"};

    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = true};
    config.connect_timeout_ = 300ms;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        auto initial = co_await client.client().get("/cleared-in-same-response").send();
        EXPECT_EQ(initial.protocol_version(), ruvia::http_protocol_version::http11);
        (void)co_await initial.text();

        // Subsequent request must stay on TCP without dialing the cleared UDP alternative.
        peer.alt_svc_.clear();
        peer.extra_alt_svc_headers_.clear();
        auto follow_up = co_await client.client().get("/after-clear").send();
        EXPECT_EQ(follow_up.protocol_version(), ruvia::http_protocol_version::http11);
        const auto follow_up_body = co_await follow_up.text();
        EXPECT_EQ(follow_up_body.text(), "/after-clear");
        (void)co_await ruvia::sleep_for(worker, 50ms);
        EXPECT_EQ(datagrams, 0U);

        // Sanity check: advertising without trailing clear does attempt HTTP/3, and a subsequent
        // response carrying h3 followed by Alt-Svc: clear evicts the previously cached entry too.
        peer.alt_svc_ = "h3=\":" + std::to_string(alternative_port) + "\"; ma=3600";
        auto advertised = co_await client.client().get("/advertise").send();
        (void)co_await advertised.text();
        peer.extra_alt_svc_headers_ = {"clear"};
        // Force a TCP response that carries both h3 and trailing clear before HTTP/3 is attempted:
        auto evicted = co_await client.client().get("/evict").configure({.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = false}}).send();
        (void)co_await evicted.text();
        peer.alt_svc_.clear();
        peer.extra_alt_svc_headers_.clear();

        auto after_evict = co_await client.client().get("/after-evict").send();
        EXPECT_EQ(after_evict.protocol_version(), ruvia::http_protocol_version::http11);
        (void)co_await after_evict.text();
        (void)co_await ruvia::sleep_for(worker, 50ms);
        EXPECT_EQ(datagrams, 0U);

        std::error_code ignored;
        sink.close(ignored);
        while (!sink_done) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
    });
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
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
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
        waiting.configure({.timeouts_ = {.acquire_ = ruvia::http_client_timeout_override::after(50ms)}});
        try {
            (void)co_await waiting.send();
            ADD_FAILURE() << "admission beyond max_open_ succeeded";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::errc::timed_out);
            const auto* client_error = dynamic_cast<const ruvia::http_client_error*>(&error);
            EXPECT_NE(client_error, nullptr);
            if (client_error) {
                EXPECT_FALSE(client_error->request_written());
            }
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

TEST(Ruvia, http_client_upload_progress_keeps_the_response_head_wait_alive) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.read_timeout_ = 100ms;
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        // The origin reads the complete request before it answers; the upload
        // outlasts read_timeout_ while every chunk arrives within it.
        auto exchange = co_await client.client().post("/drain").upload();
        for (int chunk = 0; chunk != 6; ++chunk) {
            co_await exchange.body().write(std::string_view("chunk"));
            (void)co_await ruvia::sleep_for(worker, 50ms);
        }
        co_await exchange.body().end();
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.ok());
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "/drain");
    });
    ASSERT_EQ(peer.connections_.size(), 1U);
    EXPECT_GT(peer.connections_[0].content_bytes_, 6U * 5U);
}

TEST(Ruvia, http_client_upgrade_tunnels_follow_their_own_idle_timeout) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.read_timeout_ = 50ms;
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        const auto upgrade = [&](ruvia::http_client_tunnel_config tunnel) {
            return client.client().get("/chat").header("Connection", "Upgrade").header("Upgrade", "chat").upgrade(tunnel);
        };
        {
            // Without an idle timeout, read_timeout_ no longer bounds the tunnel.
            auto opened = co_await upgrade({});
            auto* tunnel = opened.tunnel();
            if (tunnel == nullptr) {
                ADD_FAILURE() << "Upgrade was not switched";
                co_return;
            }
            (void)co_await ruvia::sleep_for(worker, 200ms);
            co_await tunnel->write(std::string_view("ping"));
            const auto echoed = co_await tunnel->text();
            EXPECT_TRUE(echoed == std::optional<std::string_view>("ping"));
        }
        {
            auto opened = co_await upgrade({.idle_timeout_ = std::chrono::milliseconds(150)});
            auto* tunnel = opened.tunnel();
            if (tunnel == nullptr) {
                ADD_FAILURE() << "Upgrade was not switched";
                co_return;
            }
            // Traffic in either direction restarts the idle bound.
            for (int round = 0; round != 3; ++round) {
                (void)co_await ruvia::sleep_for(worker, 75ms);
                co_await tunnel->write(std::string_view("tick"));
                EXPECT_TRUE((co_await tunnel->text()) == std::optional<std::string_view>("tick"));
            }
            std::optional<std::error_code> failure;
            try {
                (void)co_await tunnel->read();
            } catch (const ruvia::http_client_error& error) {
                failure = error.code();
            }
            EXPECT_EQ(failure, std::make_error_code(std::errc::timed_out));
        }
    });
}

TEST(Ruvia, http1_client_abandoned_upload_keeps_the_early_response) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        auto exchange = co_await client.client().post("/early").upload();
        EXPECT_TRUE(exchange.body().writable());
        co_await exchange.body().write(std::string_view("partial"));
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.ok());
        exchange.body().abandon();
        EXPECT_FALSE(exchange.body().complete());
        EXPECT_FALSE(exchange.body().writable());
        EXPECT_THROW((void)exchange.body().write(std::string_view("late")), ruvia::http_client_error);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "/early");
        // The unfinished request leaves the connection unusable.
        auto next = co_await client.client().get("/next").send();
        EXPECT_FALSE(next.connection_info().reused_);
        (void)co_await next.text();
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_TRUE(peer.connections_[0].closed_);
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_client_abandoned_upload_ends_only_its_sending_half) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        // The origin answers the head at once and completes its response
        // while the request content is still open.
        auto exchange = co_await client.client().post("/early").upload();
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.ok());
        EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http2);
        exchange.body().abandon();
        EXPECT_FALSE(exchange.body().complete());
        const auto body = co_await response.text();
        EXPECT_TRUE(body.empty());
        EXPECT_TRUE(response.stream().complete());
        for (int attempt = 0; attempt != 200 && (peer.connections_.empty() || peer.connections_[0].resets_.empty()); ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 5ms);
        }
    });
    ASSERT_EQ(peer.connections_.size(), 1U);
    EXPECT_EQ(peer.connections_[0].resets_, (std::deque<ruvia::http2_error_code>{ruvia::http2_error_code::no_error}));
}
#endif

TEST(Ruvia, http_client_worker_idle_bound_spans_aliases) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer first_peer(io, nullptr);
    routing_peer second_peer(io, nullptr);
    const std::string first_url = "http://127.0.0.1:" + std::to_string(first_peer.port());
    const std::string second_url = "http://127.0.0.1:" + std::to_string(second_peer.port());
    const std::array configs{ruvia::http_client_config{.alias_ = "first", .base_url_ = first_url},
        ruvia::http_client_config{.alias_ = "second", .base_url_ = second_url}};
    run_clients(attachment, {&first_peer, &second_peer}, configs, {.max_idle_http1_ = 1},
        [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            const auto first = client.client("first");
            const auto second = client.client("second");
            auto a = co_await first.get("/a").send();
            (void)co_await a.text();
            EXPECT_EQ(first.stats().idle_connections_, 1U);
            auto b = co_await second.get("/b").send();
            (void)co_await b.text();
            // Each alias allows 64 idle connections; the worker keeps one, so
            // the least recently used, of the other alias, closes.
            for (int attempt = 0; attempt != 200 && first.stats().open_connections_ != 0; ++attempt) {
                (void)co_await ruvia::sleep_for(worker, 5ms);
            }
            EXPECT_EQ(first.stats().open_connections_, 0U);
            EXPECT_EQ(second.stats().idle_connections_, 1U);
        });
    ASSERT_EQ(first_peer.connections_.size(), 1U);
    EXPECT_TRUE(first_peer.connections_[0].closed_);
}

TEST(Ruvia, http_client_alias_without_a_fixed_origin_routes_each_request_to_its_authority) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    routing_peer other_peer(io, nullptr);
    const auto authority = "127.0.0.1:" + std::to_string(peer.port());
    const auto other_authority = "127.0.0.1:" + std::to_string(other_peer.port());
    peer.redirect_location_ = "http://" + other_authority + "/landing";
    ruvia::http_client_config config{.base_url_ = "http://"};
    config.request_timeout_ = 5s;
    config.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([](const ruvia::ip_address& ip, std::uint16_t) { return ip == *ruvia::ip_address::parse("127.0.0.1"); });
    run_client(io, attachment, {&peer, &other_peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        const auto handle = client.client();
        const auto to = [](std::string_view value) {
            ruvia::http_client_request_options options;
            options.authority_.emplace(value);
            return options;
        };
        const auto fails_with = [&](ruvia::http_client_request request) -> ruvia::task<std::optional<std::error_code>> {
            try {
                (void)co_await request.send();
            } catch (const ruvia::http_client_error& error) {
                co_return error.code();
            }
            co_return std::nullopt;
        };
        bool rejected = false;
        try {
            (void)co_await handle.get("/").send();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        auto response = co_await handle.get("/direct").configure(to(authority)).send();
        const auto direct = co_await response.text();
        EXPECT_EQ(direct.text(), "/direct");
        // Every dial address passes the validator.
        const auto rejected_dial = co_await fails_with(std::move(handle.get("/").configure(to("127.0.0.2:" + std::to_string(peer.port())))));
        EXPECT_EQ(rejected_dial, ruvia::make_error_code(ruvia::http_client_errc::resolve_failed));
        // Without a policy a cross-origin redirect is not followed.
        auto unfollowed = co_await handle.get("/redirect").configure(to(authority)).send();
        EXPECT_EQ(unfollowed.status().value(), 302);
        (void)co_await unfollowed.text();
        // An accepted origin is reached through this alias with its authority.
        auto followed = co_await handle.get("/redirect").configure(to(authority)).on_redirect([](const ruvia::http_client_redirect_hop&, ruvia::stop_token) -> ruvia::task<bool> {
                                                                                     co_return true;
                                                                                 })
                            .send();
        const auto landing = co_await followed.text();
        EXPECT_EQ(landing.text(), "/landing");
    });
    ASSERT_EQ(peer.connections_.size(), 1U);
    EXPECT_EQ(peer.connections_[0].authorities_.front(), authority);
    ASSERT_EQ(other_peer.connections_.size(), 1U);
    EXPECT_EQ(other_peer.connections_[0].authorities_, (std::deque<std::string>{other_authority}));
}

TEST(Ruvia, http_client_alias_without_a_fixed_origin_requires_an_address_validator) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    EXPECT_THROW(ruvia::http_client(attachment.loop(), {.base_url_ = "http://"}), std::invalid_argument);
    ruvia::http_client_config cookies{.base_url_ = "https://"};
    cookies.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([](const ruvia::ip_address&, std::uint16_t) { return true; });
    cookies.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    EXPECT_THROW(ruvia::http_client(attachment.loop(), cookies), std::invalid_argument);
}

TEST(Ruvia, http_client_dns_refresh_queries_only_live_routes_and_retires_moved_connections) {
    using namespace origin_routing_tests;
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    routing_peer peer(io, nullptr, "0.0.0.0");
    const std::string base_url = "http://localhost:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    config.dns_refresh_interval_ = 30ms;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        const auto handle = client.client();
        (void)co_await ruvia::sleep_for(worker, 150ms);
        // No connection, no query.
        EXPECT_EQ(handle.stats().dns_refreshes_, 0U);
        const std::array moved{ruvia::http_client_endpoint_address("127.0.0.2", peer.port())};
        handle.update_dns(moved);
        auto first = co_await handle.get("/first").send();
        EXPECT_EQ(first.connection_info().peer_ip_.to_string(), "127.0.0.2");
        (void)co_await first.text();
        // The refresh finds localhost elsewhere and retires the moved connection.
        for (int attempt = 0; attempt != 400 && !peer.connections_[0].closed_; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 5ms);
        }
        EXPECT_TRUE(peer.connections_[0].closed_);
        EXPECT_GE(handle.stats().dns_refreshes_, 1U);
        auto second = co_await handle.get("/second").send();
        EXPECT_NE(second.connection_info().peer_ip_.to_string(), "127.0.0.2");
        (void)co_await second.text();
        lookups.join();
    });
}

TEST(Ruvia, http_client_dns_refresh_concurrency_is_bounded_per_worker) {
    using namespace origin_routing_tests;
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://localhost:" + std::to_string(peer.port());
    std::array configs{ruvia::http_client_config{.alias_ = "first", .base_url_ = base_url},
        ruvia::http_client_config{.alias_ = "second", .base_url_ = base_url}};
    for (auto& config : configs) {
        config.request_timeout_ = 5s;
        config.dns_refresh_interval_ = 20ms;
    }
    run_clients(attachment, {&peer}, configs, {.max_concurrent_dns_refreshes_ = 1},
        [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            const auto first = client.client("first");
            const auto second = client.client("second");
            auto a = co_await first.get("/a").send();
            (void)co_await a.text();
            auto b = co_await second.get("/b").send();
            (void)co_await b.text();
            // Hold the only lookup thread: the first refresh started stays
            // running, and every other period of either alias defers.
            std::promise<void> release;
            auto released = release.get_future();
            ruvia::task_scope blocker(worker);
            blocker.spawn([](ruvia::worker_ref owner, std::future<void>& gate) -> ruvia::task<void> {
                (void)co_await ruvia::run_blocking(std::move(owner), ruvia::operation_options{}, [&gate] { gate.wait(); });
            }(worker, released));
            const auto started = [&] { return first.stats().dns_refreshes_ + second.stats().dns_refreshes_; };
            for (int attempt = 0; attempt != 200 && started() == 0; ++attempt) {
                (void)co_await ruvia::sleep_for(worker, 5ms);
            }
            (void)co_await ruvia::sleep_for(worker, 150ms);
            EXPECT_EQ(started(), 1U);
            release.set_value();
            co_await blocker.join();
            for (int attempt = 0; attempt != 200 && started() < 3; ++attempt) {
                (void)co_await ruvia::sleep_for(worker, 5ms);
            }
            EXPECT_GE(started(), 3U);
        });
    lookups.join();
}

TEST(Ruvia, http_client_redirect_hop_observation_and_streaming_upload_redirect) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    routing_peer second_peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    const std::string second_url = "http://127.0.0.1:" + std::to_string(second_peer.port());
    peer.redirect_location_ = second_url + "/final-destination";
    std::array configs{
        ruvia::http_client_config{.alias_ = "default", .base_url_ = base_url},
        ruvia::http_client_config{.alias_ = "second", .base_url_ = second_url},
    };
    run_clients(attachment, {&peer, &second_peer}, configs, {},
        [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            const auto handle = client.client();
            std::vector<std::string> observed_targets;
            std::vector<std::uint16_t> observed_statuses;

            // 1) Streaming upload after write + end follows 302 -> GET and reports hop to on_redirect.
            auto upload_exchange = co_await handle.post("/drain-redirect")
                                       .on_redirect([&](const ruvia::http_client_redirect_hop& hop) {
                                           observed_targets.emplace_back(hop.target_url());
                                           observed_statuses.push_back(hop.status().value());
                                           EXPECT_EQ(hop.hop_index(), 0U);
                                           EXPECT_EQ(hop.source_target(), "/drain-redirect");
                                           EXPECT_TRUE(hop.cross_origin());
                                           EXPECT_EQ(hop.connection_info().peer_port_, peer.port());
                                           return true;
                                       })
                                       .upload();
            co_await upload_exchange.body().write("upload-payload");
            co_await upload_exchange.body().end();
            auto redirected_resp = co_await upload_exchange.response();
            EXPECT_EQ(redirected_resp.status(), ruvia::http_status::ok);
            const auto landed_body = co_await redirected_resp.text();
            EXPECT_EQ(landed_body.text(), "/final-destination");
            EXPECT_EQ(observed_targets.size(), 1U);
            if (!observed_targets.empty()) {
                EXPECT_EQ(observed_targets[0], second_url + "/final-destination");
                EXPECT_EQ(observed_statuses[0], 302U);
            }

            // 2) Early 302 during streaming upload stops the writer and follows redirect.
            auto early_upload = co_await handle.post("/redirect").upload();
            auto early_resp = co_await early_upload.response();
            EXPECT_FALSE(early_upload.body().writable());
            const auto early_body = co_await early_resp.text();
            EXPECT_EQ(early_body.text(), "/final-destination");

            // 3) Returning false from on_redirect stops following and returns the 302 response.
            auto stopped_resp = co_await handle.get("/redirect")
                                    .on_redirect([](const ruvia::http_client_redirect_hop&) {
                                        return false;
                                    })
                                    .send();
            EXPECT_EQ(stopped_resp.status(), ruvia::http_status::found);
            (void)co_await stopped_resp.text();

            // 4) Subsequent hop connection failure reports redirect_hops() == 1 after hop 0 succeeded.
            second_peer.stop();
            bool first_hop_recorded = false;
            std::size_t failed_hop_count = 0;
            try {
                (void)co_await handle.get("/redirect")
                    .configure({.fresh_connection_ = true})
                    .on_redirect([&](const ruvia::http_client_redirect_hop& hop) {
                        first_hop_recorded = (hop.hop_index() == 0);
                        return true;
                    })
                    .send();
            } catch (const ruvia::http_client_error& error) {
                failed_hop_count = error.redirect_hops();
                EXPECT_TRUE(error.followed_redirect());
            }
            EXPECT_TRUE(first_hop_recorded);
            EXPECT_EQ(failed_hop_count, 1U);
        });
}

TEST(Ruvia, http_client_non_destructive_dns_resolution_preserves_active_connections) {
    using namespace origin_routing_tests;
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    routing_peer peer(io, nullptr, "0.0.0.0");
    const std::string base_url = "http://localhost:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config,
        [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            const auto handle = client.client();
            const std::array pinned{ruvia::http_client_endpoint_address("127.0.0.2", peer.port())};
            handle.update_dns(pinned);

            auto first = co_await handle.get("/first").send();
            EXPECT_EQ(first.connection_info().peer_ip_.to_string(), "127.0.0.2");
            (void)co_await first.text();

            // Non-destructive DNS resolution returns sorted, deduplicated addresses
            // without retiring the active 127.0.0.2 connection or changing dns_snapshot().
            const auto resolved_handle = co_await handle.resolve_dns();
            EXPECT_FALSE(resolved_handle.empty());
            EXPECT_TRUE(std::ranges::is_sorted(resolved_handle));

            const auto resolved_standalone = co_await ruvia::resolve_dns(worker, "localhost");
            EXPECT_EQ(resolved_handle, resolved_standalone);

            const auto resolved_loop = co_await ruvia::resolve_dns(attachment.loop(), "127.0.0.1");
            EXPECT_EQ(resolved_loop.size(), 1U);
            if (!resolved_loop.empty()) {
                EXPECT_EQ(resolved_loop[0].to_string(), "127.0.0.1");
            }

            EXPECT_FALSE(peer.connections_[0].closed_);
            auto second = co_await handle.get("/second").send();
            EXPECT_TRUE(second.connection_info().reused_);
            EXPECT_EQ(second.connection_info().peer_ip_.to_string(), "127.0.0.2");
            (void)co_await second.text();
            lookups.join();
        });
}
