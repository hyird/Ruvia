#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "blocking_pool.h"
#include "content_decoding_fixture.h"
#include "event_loop.h"
#include "http2_connection.h"
#include "http_client.h"
#include "http_client_types.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response_server.h"
#include "http_server_fixture.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"
#include "worker_task_fixture.h"

namespace {
class test_worker final {
public:
    explicit test_worker(asio::io_context& io) :
        attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8, .blocking_pool_ = &host_lookups_})),
        handle_(attachment_.loop().worker()) {}

    test_worker(const test_worker&) = delete;
    test_worker& operator=(const test_worker&) = delete;

    ~test_worker() {
        host_lookups_.join();
    }

    // Declared before the attachment, which borrows it for "localhost" lookups.
    ruvia::blocking_pool host_lookups_{{.thread_count_ = 1}};
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_ref handle_;
};

[[nodiscard]] ruvia::http_client_config local_http_client_config(std::string_view base_url) {
    return ruvia::http_client_config{.base_url_ = base_url};
}

// Answers one request per connection with the next scripted response head and
// records every request head.
class scripted_head_peer final {
public:
    scripted_head_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, std::initializer_list<std::string_view> heads) :
        acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
        deadline_(io),
        done_(worker_value),
        heads_(heads.begin(), heads.end()),
        io_(io) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::error_code ignored;
                acceptor_.close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            deadline_.cancel();
            done_.notify();
        });
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::vector<std::string> requests_{};

private:
    asio::awaitable<void> serve() {
        for (const auto& head : heads_) {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            auto& request = requests_.emplace_back();
            co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer deadline_;
    ruvia::worker_signal done_;
    std::vector<std::string> heads_{};
    asio::io_context& io_;
    std::exception_ptr failure_;
    bool complete_{false};
};

template <typename operation_type>
void run_operation(test_worker& worker_value, asio::io_context& io, operation_type&& operation) {
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            co_await operation();
        } catch (...) {
            failure = std::current_exception();
        }
        worker_value.attachment_.stop();
    };
    auto root = worker_value.attachment_.loop().start(run());
    worker_value.attachment_.run();
    std::move(root).get();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
void exercise_tls_response_retirement(bool join_before_reuse) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const std::string large(262163, 'b');
    const std::array<std::string_view, 2> expected{"fresh connection body", "healthy reused body"};
    unsigned connections = 0;
    unsigned requests = 0;
    bool cancelled_socket_closed = false;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(large.size()))) + "\r\n\r\n" + large;
            std::error_code error;
            co_await asio::async_write(stream, asio::buffer(response), asio::redirect_error(asio::use_awaitable, error));
            std::array<char, 1024> bytes_value{};
            while (co_await stream.async_read_some(asio::buffer(bytes_value), asio::redirect_error(asio::use_awaitable, error))) {
            }
            cancelled_socket_closed = error == asio::error::eof || error == asio::error::connection_reset || error == asio::ssl::error::stream_truncated;
        }
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
        ++connections;
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const auto body : expected) {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.max_response_bytes_ = 8192;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::stop_source cancellation;
        {
            auto response = co_await client.client().with_options({.stop_token_ = cancellation.token()}).get("/partial").send();
            const auto first = co_await response.stream().text();
            EXPECT_TRUE(first.has_value());
            EXPECT_TRUE(!first->empty());
            EXPECT_TRUE(first->size() < large.size());
            EXPECT_TRUE(std::ranges::all_of(*first, [](char value) {
                return value == 'b';
            }));
            cancellation.request_stop();
            // Cancellation itself must not invalidate a previously returned borrow.
            EXPECT_TRUE(std::ranges::all_of(*first, [](char value) {
                return value == 'b';
            }));
        }
        if (join_before_reuse) {
            while (client.client().stats().in_flight_requests_ != 0) {
                (void)co_await ruvia::async_asio([&io](auto handler) {
                    asio::post(io, [handler = std::move(handler)]() mutable {
                        handler(std::error_code{});
                    });
                });
            }
            EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        }
        for (const auto body : expected) {
            auto response = co_await client.client().get("/next?exact=body").send();
            EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http11);
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), body);
            EXPECT_TRUE(!(co_await response.stream().read()));
        }
        co_await peer_done.wait();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(cancelled_socket_closed);
    EXPECT_EQ(connections, 2U);
    EXPECT_EQ(requests, 3U);
}
}  // namespace

TEST(Ruvia, http1_tls_partial_response_cancellation_retires_transport_and_preserves_healthy_reuse) {
    exercise_tls_response_retirement(true);
}

TEST(Ruvia, http1_tls_queued_reconnect_joins_cancelled_producer_before_replacing_transport) {
    exercise_tls_response_retirement(false);
}

namespace {
// A failed client operation must not leave the peer's accept pending, which
// would keep the I/O context running.
struct acceptor_closer final {
    asio::ip::tcp::acceptor& acceptor_;
    ~acceptor_closer() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }
};
}  // namespace

// RFC 9112 §9.8: only close_notify ends a close-delimited body over TLS; a TCP
// close without it may be an attacker truncating the response.
TEST(Ruvia, http1_tls_close_delimited_response_requires_close_notify) {
    for (const bool close_notify : {true, false}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            std::error_code ignored;
            if (close_notify) {
                co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
            } else {
                // A FIN rather than a close: unread client bytes would turn it into a reset.
                stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
                std::array<char, 256> discarded;
                while (co_await stream.next_layer().async_read_some(asio::buffer(discarded), asio::redirect_error(asio::use_awaitable, ignored))) {
                }
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            auto response = co_await client.client().get("/close-delimited").send();
            std::optional<std::string> body;
            bool truncated = false;
            try {
                const auto bytes_value = co_await response.stream().read_all();
                body.emplace(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size());
            } catch (const ruvia::http_client_error& error) {
                truncated = error.code() == ruvia::make_error_code(ruvia::http_client_errc::io_error);
            }
            if (close_notify) {
                EXPECT_TRUE(body == std::optional<std::string>(std::string("partial")));
            } else {
                EXPECT_TRUE(!body.has_value());
                EXPECT_TRUE(truncated);
            }
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
        };
        run_operation(worker, io, operation);
    }
}

// RFC 9110 §4.3.4 and RFC 9525 §6.3: a DNS name is authenticated only by a
// DNS-ID subjectAltName. A certificate naming the host only in its subject
// Common Name, or through a wildcard that is not a whole left-most label,
// fails the handshake; a whole-label wildcard is accepted.
TEST(Ruvia, http_client_tls_matches_dns_names_only_against_dns_id_subject_alt_names) {
    struct identity_case final {
        const char* common_name_;
        std::string subject_alt_name_{};
        const char* server_name_;
        bool accepted_;
    };
    const std::array cases{
        identity_case{"localhost", std::string(), "localhost", false},
        identity_case{"www.example.com", std::string("DNS:w*.example.com"), "www.example.com", false},
        identity_case{"www.example.com", std::string("DNS:*.example.com"), "www.example.com", true},
    };
    for (const auto& identity_value : cases) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity(identity_value.common_name_, identity_value.subject_alt_name_);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        bool peer_finished = false;
        bool peer_handshake_completed = false;
        ruvia::worker_signal peer_done(worker.handle_);
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            std::error_code error;
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            peer_handshake_completed = true;
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, error));
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr) {
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.tls_.server_name_ = identity_value.server_name_;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            std::optional<std::error_code> failure;
            try {
                auto response = co_await client.client().get("/identity").send();
                EXPECT_EQ(response.status().value(), std::uint16_t{200});
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                failure = error.code();
            }
            EXPECT_EQ(!failure.has_value(), identity_value.accepted_);
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            EXPECT_EQ(peer_handshake_completed, identity_value.accepted_);
        };
        run_operation(worker, io, operation);
    }
}

namespace tls_record_budget_tests {

struct tunnel_peer final {
    tunnel_peer(asio::io_context& io, const ruvia::worker_ref& worker, asio::ssl::context& tls) :
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        done_(worker),
        tls_(tls) {}

    asio::awaitable<void> serve() {
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor_.async_accept(asio::use_awaitable), tls_);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        asio::streambuf request;
        co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        constexpr std::string_view head = "HTTP/1.1 200 Connection Established\r\n\r\n";
        co_await asio::async_write(stream, asio::buffer(head), asio::use_awaitable);
        std::array<char, 128> bytes{};
        for (;;) {
            const auto count = co_await stream.async_read_some(asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
            received_.append(bytes.data(), count);
            if (error) {
                co_return;
            }
        }
    }

    ruvia::task<void> exercise(ruvia::http_client& client, std::uint64_t limit) {
        const acceptor_closer closer{acceptor_};
        unsigned completed = 0;
        bool failed = false;
        try {
            auto result = co_await client.client().connect("target.test:443").open();
            auto tunnel = std::move(*result.tunnel());
            for (; completed != 64; ++completed) {
                co_await tunnel.write("x");
            }
        } catch (const ruvia::http_client_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_TRUE(completed < 64);
        EXPECT_EQ(completed != 0, limit != 1);
        co_await client.shutdown();
        while (!finished_) {
            co_await done_.wait();
        }
        EXPECT_TRUE(received_.size() <= limit);
        EXPECT_TRUE(std::ranges::all_of(received_, [](char byte) {
            return byte == 'x';
        }));
    }

    asio::ip::tcp::acceptor acceptor_;
    ruvia::worker_signal done_;
    asio::ssl::context& tls_;
    bool finished_{false};
    std::string received_{};
};

}  // namespace tls_record_budget_tests

TEST(Ruvia, http_client_tls_record_budget_covers_handshake_and_replaced_connect_bio) {
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        for (const std::uint64_t limit : {std::uint64_t{1}, std::uint64_t{32}}) {
            auto& io = ruvia::test::new_test_io_context();
            test_worker worker(io);
            ruvia::test::tls_identity identity("localhost");
            EXPECT_TRUE(SSL_CTX_set_min_proto_version(identity.context_.native_handle(), version) == 1);
            EXPECT_TRUE(SSL_CTX_set_max_proto_version(identity.context_.native_handle(), version) == 1);
            tls_record_budget_tests::tunnel_peer peer(io, worker.handle_, identity.context_);
            asio::co_spawn(io, peer.serve(), [&](std::exception_ptr) {
                peer.finished_ = true;
                peer.done_.notify();
            });
            const std::string config_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(peer.acceptor_.local_endpoint().port())));
            auto config = local_http_client_config(config_url);
            config.tls_.ca_file_ = identity.ca_file_;
            config.tls_.server_name_ = "localhost";
            config.tls_.max_outgoing_records_ = limit;
            config.request_timeout_ = std::chrono::seconds(3);
            ruvia::http_client client(worker.attachment_.loop(), config);
            run_operation(worker, io, [&] {
                return peer.exercise(client, limit);
            });
        }
    }
}

// RFC 9113 §9.2.1: renegotiation is disabled. A TLS 1.2 server's HelloRequest
// is answered with a no_renegotiation alert, which ends the server's attempt,
// while the response already sent is still delivered.
TEST(Ruvia, http_client_tls_refuses_server_initiated_renegotiation) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    EXPECT_EQ(SSL_CTX_set_max_proto_version(identity.context_.native_handle(), TLS1_2_VERSION), 1);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool peer_finished = false;
    bool renegotiation_refused = false;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
        // Sends HelloRequest; the client's reply arrives with later reads.
        if (SSL_renegotiate(stream.native_handle()) != 1) {
            throw std::runtime_error("cannot request TLS renegotiation");
        }
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        std::array<char, 256> bytes_value{};
        std::error_code error;
        while (co_await stream.async_read_some(asio::buffer(bytes_value), asio::redirect_error(asio::use_awaitable, error))) {
        }
        renegotiation_refused = error.category() == asio::error::get_ssl_category() && ERR_GET_REASON(static_cast<unsigned long>(error.value())) == SSL_R_NO_RENEGOTIATION;
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto response = co_await client.client().get("/renegotiate").send();
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
        co_await client.shutdown();
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(renegotiation_refused);
}

// RFC 9112 §9.8 / RFC 9846 §6.1: the client sends close_notify before it
// closes a TLS connection: when a response ends the connection, when shutdown
// retires an idle keep-alive connection, and when shutdown ends an HTTP/2
// session whose reader is still waiting for input.
TEST(Ruvia, tls_client_sends_close_notify_before_closing) {
    enum class peer_mode : std::uint8_t { http1_close,
        http1_keep_alive,
        http2 };
    for (const auto mode : {
             peer_mode::http1_close,
             peer_mode::http1_keep_alive,
#if defined(RUVIA_ENABLE_HTTP2)
             peer_mode::http2,
#endif
         }) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        if (mode == peer_mode::http2) {
            SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen, void*) -> int {
                static constexpr unsigned char h2[] = {2, 'h', '2'};
                return SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, h2, sizeof(h2), in, inlen) == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
            }, nullptr);
        }
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        // eof is the client's close_notify; stream_truncated a bare TCP close.
        std::error_code client_end;
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            std::array<char, 16384> input{};
#if defined(RUVIA_ENABLE_HTTP2)
            if (mode == peer_mode::http2) {
                std::error_code write_error;
                auto connection = ruvia::http2_connection::server();
                std::string output;
                while (!client_end) {
                    if (connection.wants_write()) {
                        ruvia::test::drain_output(connection, output);
                        // A failed write leaves the client's end to the next read.
                        (void)co_await asio::async_write(stream, asio::buffer(output), asio::redirect_error(asio::use_awaitable, write_error));
                    }
                    const auto count = co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, client_end));
                    (void)connection.feed(std::string_view(input.data(), count));
                    while (auto event = connection.next_event()) {
                        if (auto* request = event->request_head()) {
                            const auto stream_id = request->stream_id();
                            const auto built = request->build_request();
                            ruvia::http_response response;
                            response.body("ok");
                            (void)connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                            (void)connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535);
                            (void)connection.release(std::move(*request));
                        }
                    }
                }
                co_return;
            }
#endif
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            const std::string_view response = mode == peer_mode::http1_close ? "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok" : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            while (!client_end) {
                (void)co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, client_end));
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.request_timeout_ = std::chrono::seconds(3);
        if (mode == peer_mode::http2) {
            config.versions_ = ruvia::http_client_versions{.http1_ = false};
        }
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            auto response = co_await client.client().get("/close-notify").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(bytes_value.size(), std::size_t{2});
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            EXPECT_TRUE(client_end == asio::error::eof);
        };
        run_operation(worker, io, operation);
    }
}

// A response on a connection that is not reused completes even when trailing
// bytes arrive with it: they are discarded with the connection, not parsed.
TEST(Ruvia, http1_response_on_a_closing_connection_discards_trailing_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_, {"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhelloXjunk", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\n\r\ndeniedHTTP/1.1 200 OK\r\n\r\n"});
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        {
            auto response = co_await client.client().get("/closing").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "hello");
        }
        {
            auto result = co_await client.client().connect("target.test:443").open();
            auto* response = result.response();
            EXPECT_TRUE(response != nullptr);
            if (response != nullptr) {
                EXPECT_EQ(response->status().value(), std::uint16_t{407});
                const auto bytes_value = co_await response->stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "denied");
            }
        }
        co_await peer.wait();
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_replaces_a_keep_alive_connection_the_peer_closed_while_idle) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    unsigned connections = 0;
    bool idle_closed = false;
    ruvia::worker_signal idle_closed_signal(worker.handle_);
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        for (const std::string_view body : {std::string_view("first"), std::string_view("second")}) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ++connections;
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            if (!idle_closed) {
                // A keep-alive idle timeout: the peer closes without a request.
                std::error_code ignored;
                socket.close(ignored);
                idle_closed = true;
                idle_closed_signal.notify();
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.client().get("/idle").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
            while (!idle_closed) {
                co_await idle_closed_signal.wait();
            }
            // Let the peer's FIN reach the idle pooled socket.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.failed_requests_, std::size_t{0});
        EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

TEST(Ruvia, http_client_retries_only_idempotent_requests_after_a_reused_connection_fails_before_its_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    unsigned connections = 0;
    unsigned retried_requests = 0;
    unsigned non_idempotent_requests = 0;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto read_request = [](asio::ip::tcp::socket& socket) -> asio::awaitable<void> {
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
    };
    const auto answer = [](asio::ip::tcp::socket& socket, std::string_view body, std::string_view connection) -> asio::awaitable<void> {
        const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\nConnection: " + std::string(connection) + "\r\n\r\n" + std::string(body);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        auto first = co_await acceptor.async_accept(asio::use_awaitable);
        ++connections;
        co_await read_request(first);
        co_await answer(first, "prime", "keep-alive");
        co_await read_request(first);
        ++retried_requests;
        std::error_code ignored;
        first.close(ignored);

        std::error_code accept_error;
        auto second = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, accept_error));
        if (accept_error == asio::error::operation_aborted) {
            co_return;
        }
        if (accept_error) {
            throw std::system_error(accept_error);
        }
        ++connections;
        co_await read_request(second);
        ++retried_requests;
        co_await answer(second, "retried", "keep-alive");
        co_await read_request(second);
        ++non_idempotent_requests;
        acceptor.close(ignored);
        second.close(ignored);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto first = co_await client.client().get("/prime").send();
        const auto first_body = co_await first.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first_body.bytes().data()), first_body.size()), "prime");

        std::optional<std::string> retry_body;
        try {
            auto response = co_await client.client().get("/retry").send();
            const auto bytes_value = co_await response.stream().read_all();
            retry_body.emplace(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size());
        } catch (const ruvia::http_client_error&) {
            std::error_code ignored;
            acceptor.close(ignored);
        }
        std::optional<std::error_code> post_failure;
        if (retry_body.has_value()) {
            try {
                (void)co_await client.client().request("POST", "/post").send();
            } catch (const ruvia::http_client_error& error) {
                post_failure = error.code();
            }
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(retry_body == std::optional<std::string>(std::string("retried")));
        EXPECT_TRUE(post_failure == ruvia::make_error_code(ruvia::http_client_errc::io_error));
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.completed_requests_, std::size_t{2});
        EXPECT_EQ(stats.failed_requests_, std::size_t{1});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
    EXPECT_EQ(retried_requests, 2U);
    EXPECT_EQ(non_idempotent_requests, 1U);
}

// The idle probe never decrypts: any record a TLS peer sends while the pooled
// connection idles, a TLS 1.3 session ticket as much as a close_notify,
// retires the connection, and the next request uses a fresh one.
TEST(Ruvia, http_client_tls_idle_probe_retires_a_connection_with_unread_peer_records) {
    for (const bool peer_closes : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        unsigned connections = 0;
        bool idle_sent = false;
        ruvia::worker_signal idle_sent_signal(worker.handle_);
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto answer = [](asio::ssl::stream<asio::ip::tcp::socket>& stream, std::string_view body) -> asio::awaitable<void> {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        };
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> first(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await first.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await answer(first, "first");
            if (peer_closes) {
                // Starting the shutdown writes close_notify at once; it then
                // waits for the client's reply, which ends when the client
                // drops the retired connection.
                idle_sent = true;
                idle_sent_signal.notify();
                std::error_code ignored;
                co_await first.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
            } else {
                if (SSL_new_session_ticket(first.native_handle()) != 1) {
                    throw std::runtime_error("TLS 1.3 was not negotiated");
                }
                // A post-handshake handshake step sends the ticket immediately.
                co_await first.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
                idle_sent = true;
                idle_sent_signal.notify();
            }
            // No FIN precedes the client's probe: only the idle record retires
            // the first connection.
            asio::ssl::stream<asio::ip::tcp::socket> second(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await second.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await answer(second, "second");
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.connection_limits_.max_open_ = 1;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
                auto response = co_await client.client().get("/idle").send();
                const auto bytes_value = co_await response.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
                while (!idle_sent) {
                    co_await idle_sent_signal.wait();
                }
                // Let the idle record reach the pooled socket.
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            }
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.failed_requests_, std::size_t{0});
            EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{1});
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        EXPECT_EQ(connections, 2U);
    }
}

TEST(Ruvia, http_client_abandoned_queued_request_is_never_written) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::vector<std::string> request_lines{};
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::string buffered{};
        for (unsigned index = 0; index != 2; ++index) {
            const auto head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
            request_lines.emplace_back(std::string_view(buffered).substr(0, buffered.find("\r\n")));
            buffered.erase(0, head_bytes);
            if (!request_lines.back().starts_with("GET ")) {
                co_return;
            }
            if (index == 0) {
                // Keep the only slot leased while the queued request is abandoned.
                asio::steady_timer delay(socket.get_executor(), std::chrono::milliseconds(100));
                co_await delay.async_wait(asio::use_awaitable);
            }
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope first(worker.handle_);
        const auto send_first = [&]() -> ruvia::task<void> {
            auto response = co_await client.client().get("/first").send();
            (void)co_await response.stream().read_all();
        };
        first.spawn(send_first());
        {
            auto abandoned = co_await client.client().request("POST", "/abandoned").upload({});
            // Let the request driver queue behind the leased slot.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable {
                        done(std::error_code{});
                    });
                });
            }
        }
        co_await first.join();
        auto response = co_await client.client().get("/third").send();
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(request_lines.size(), std::size_t{2});
    if (request_lines.size() == 2) {
        EXPECT_EQ(request_lines[0], "GET /first HTTP/1.1");
        EXPECT_EQ(request_lines[1], "GET /third HTTP/1.1");
    }
}

// Dropping an exchange whose connection is still being established cancels
// that connect: the slot frees at once instead of after connect_timeout_.
TEST(Ruvia, http_client_abandoned_request_cancels_its_connect_in_progress) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    // Accepts TCP but never answers the TLS ClientHello, so the client's
    // connect stays in progress until its connect_timeout_ or a cancellation.
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string client_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    ruvia::http_client_config config{.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .connect_timeout_ = std::chrono::seconds(5), .request_timeout_ = std::chrono::seconds(10)};
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto abandoned = co_await client.client().request("POST", "/abandoned").upload({});
            // Let the driver reach the TLS handshake.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.in_flight_requests_ + stats.buffered_requests_, std::size_t{1});
        }
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            const auto stats = client.client().stats();
            if (stats.in_flight_requests_ + stats.buffered_requests_ == 0) {
                break;
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
                throw std::runtime_error("abandoned connect kept its pool slot leased");
            }
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(10));
        }
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 §7.5 permits the final response to finish before its request. A
// persistent final response does not cancel content that was already released.
TEST(Ruvia, http_client_finishes_upload_after_an_early_persistent_response_before_reusing_connection) {
    enum class release_mode { immediate,
        continued,
        timed_out,
        submillisecond_timeout };
    for (const auto mode : {release_mode::immediate, release_mode::continued, release_mode::timed_out, release_mode::submillisecond_timeout}) {
        const bool expect_continue = mode != release_mode::immediate;
        const bool timeout_release = mode == release_mode::timed_out || mode == release_mode::submillisecond_timeout;
        for (const bool known_length : {false, true}) {
            auto& io = ruvia::test::new_test_io_context();
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
            std::exception_ptr peer_failure;
            bool peer_finished = false;
            ruvia::worker_signal peer_done(worker.handle_);
            std::string request_body{};
            ruvia::http_request_trailers request_trailers;
            std::string next_request{};
            const auto serve = [&]() -> asio::awaitable<void> {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                std::string buffered{};
                const auto head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                buffered.erase(0, head_bytes);
                if (timeout_release) {
                    if (buffered.empty()) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered), asio::transfer_at_least(1), asio::use_awaitable);
                    }
                }
                std::string response{};
                if (expect_continue && !timeout_release) {
                    response += "HTTP/1.1 100 Continue\r\n\r\n";
                }
                response += "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
                if (known_length) {
                    if (buffered.size() < 3) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered), asio::transfer_exactly(3 - buffered.size()), asio::use_awaitable);
                    }
                    request_body.assign(buffered.data(), 3);
                    buffered.erase(0, 3);
                } else {
                    ruvia::http1_chunked_body_decoder decoder;
                    for (;;) {
                        const auto decoded = decoder.decode(buffered);
                        if (decoded.failure()) {
                            throw std::runtime_error("invalid upload chunk framing");
                        }
                        if (const auto* chunk = decoded.body_chunk()) {
                            request_body.append(chunk->bytes());
                        }
                        const bool complete = decoded.complete() != nullptr;
                        if (complete && request_trailers.append_received(decoded.complete()->trailers())) {
                            throw std::runtime_error("invalid upload trailers");
                        }
                        buffered.erase(0, decoded.consumed_bytes());
                        if (complete) {
                            break;
                        }
                        if (decoded.need_more()) {
                            co_await asio::async_read(socket, asio::dynamic_buffer(buffered), asio::transfer_at_least(1), asio::use_awaitable);
                        }
                    }
                }
                const auto next_head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                next_request.assign(buffered.data(), next_head_bytes);
                constexpr std::string_view final_response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
                co_await asio::async_write(socket, asio::buffer(final_response), asio::use_awaitable);
            };
            asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
                peer_failure = failure;
                peer_finished = true;
                peer_done.notify();
            });
            const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
            auto config = local_http_client_config(config_url);
            config.connection_limits_.max_open_ = 1;
            config.request_timeout_ = std::chrono::seconds(3);
            ruvia::http_client client(worker.attachment_.loop(), config);
            auto operation = [&]() -> ruvia::task<void> {
                const acceptor_closer closer{acceptor};
                ruvia::http_client_upload_config upload{.content_length_ = known_length ? std::optional<ruvia::http_content_length>{3} : std::nullopt, .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none, .continue_timeout_ = std::chrono::duration<std::uint64_t, std::milli>{std::numeric_limits<std::uint64_t>::max()}};
                if (timeout_release) {
                    upload.continue_timeout_ = mode == release_mode::submillisecond_timeout ? std::chrono::microseconds(1) : std::chrono::microseconds(20000);
                }
                auto exchange = co_await client.client().request("POST", "/early").upload(upload);
                if (timeout_release) {
                    co_await exchange.body().write("a");
                }
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), std::uint16_t{200});
                const auto early_body = co_await response.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(early_body.bytes().data()), early_body.size()), "ok");
                EXPECT_TRUE(response.stream().complete());
                EXPECT_TRUE(!exchange.body().complete());
                EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
                co_await exchange.body().write(timeout_release ? "bc" : "abc");
                const ruvia::http_header_view trailers[] = {{"X-End", "retained"}};
                co_await exchange.body().end(known_length ? std::span<const ruvia::http_header_view>{} : std::span<const ruvia::http_header_view>(trailers));
                auto next = co_await client.client().get("/next").send();
                const auto next_body = co_await next.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
                while (!peer_finished) {
                    co_await peer_done.wait();
                }
                if (peer_failure) {
                    std::rethrow_exception(peer_failure);
                }
                co_await client.shutdown();
            };
            run_operation(worker, io, operation);
            EXPECT_EQ(request_body, "abc");
            EXPECT_TRUE(request_trailers.field("x-end") == (known_length ? std::nullopt : std::optional<std::string_view>("retained")));
            ruvia::http1_server_request_parse_state parsed;
            parsed.parse_head(next_request);
            EXPECT_TRUE(parsed.head_ready());
            EXPECT_EQ(parsed.request().method(), "GET");
            EXPECT_EQ(parsed.request().target(), "/next");
        }
    }
}

TEST(Ruvia, http_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view head = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        std::array<char, 1> input;
        std::error_code error;
        const auto bytes = co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        if (bytes != 0 || (error != asio::error::eof && error != asio::error::connection_reset)) {
            throw std::runtime_error("abandoned HTTP/1 upload did not close");
        }
        auto next_socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf next_request;
        co_await asio::async_read_until(next_socket, next_request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view next_response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    // A broken abandonment path fails as timeout rather than hanging the suite.
    config.request_timeout_ = std::chrono::seconds(1);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        std::optional<ruvia::http_client_response> response;
        {
            auto exchange = co_await client.client().post("/abandoned").upload({.content_length_ = 6});
            response.emplace(co_await exchange.response());
            EXPECT_TRUE(!response->stream().complete());
        }
        bool cancelled = false;
        try {
            (void)co_await response->stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(cancelled);
        auto next = co_await client.client().get("/next").send();
        const auto next_body = co_await next.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_bounds_unfinished_upload_after_response_body_completion) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    bool first_closed = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 256> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
        if (error != asio::error::eof && error != asio::error::connection_reset) {
            throw std::system_error(error);
        }
        first_closed = true;
        peer_done.notify();
        auto next_socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf next_request;
        co_await asio::async_read_until(next_socket, next_request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view next_response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        first_closed = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::milliseconds(100);
    config.connection_limits_.max_open_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.client().request("POST", "/unfinished").upload({});
        auto response = co_await exchange.response();
        (void)co_await response.stream().read_all();
        while (!first_closed) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        bool stopped = false;
        try {
            co_await exchange.body().write("late");
        } catch (const ruvia::http_client_error& error) {
            stopped = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(stopped);
        auto next = co_await client.client().get("/after-timeout").send();
        const auto next_body = co_await next.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
        EXPECT_TRUE(!(co_await response.stream().text()).has_value());
        EXPECT_TRUE(response.stream().complete());
    };
    run_operation(worker, io, operation);
}

// A transport failure while the response body is still arriving is the real
// cause of the exchange failure, even though the writer sees it first and its
// cleanup cancels the receiver's read; it is reported as such, not as a
// cancellation. The response body is left unread so the receiver sits in the
// backpressure wait without a read in flight and only the upload observes the reset.
TEST(Ruvia, http_client_reports_upload_transport_failure_during_response_body) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n0123456789012345678901234567890123456789";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 4096> input{};
        std::size_t received = 0;
        while (received < 64 * 1024) {
            received += co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
        }
        // SO_LINGER 0 turns close() into a reset the client's next write observes.
        socket.set_option(asio::socket_base::linger(true, 0));
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 16;
    config.connection_limits_.max_open_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.client().request("POST", "/reset").upload({});
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        const std::string chunk(16 * 1024, 'u');
        std::optional<std::error_code> upload_error;
        for (int i = 0; i < 1024 && !upload_error.has_value(); ++i) {
            try {
                co_await exchange.body().write(chunk);
            } catch (const ruvia::http_client_error& error) {
                upload_error = error.code();
            }
        }
        EXPECT_TRUE(upload_error.has_value());
        if (upload_error.has_value()) {
            EXPECT_TRUE(*upload_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_capsule_client_retains_payloads_across_reads_and_rejects_truncated_frames) {
    for (const bool truncated : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), 0});
        ruvia::worker_signal peer_done(worker.handle_);
        std::exception_ptr peer_failure;
        std::string first_payload(128 * 1024 + 17, 'a');
        first_payload[4095] = '\0';
        first_payload[8192] = '\xff';
        const std::string second_payload(4096, 'b');
        std::string wire{"HTTP/1.1 200 Connection Established\r\n\r\n"};
        const auto append_capsule = [&](std::uint64_t type, std::string_view payload) {
            std::array<char, 16> header;
            const auto encoded = ruvia::encode_http_capsule_header(header, type, payload.size());
            wire.append(header.data(), std::get<std::size_t>(encoded));
            wire.append(payload);
        };
        append_capsule(17, first_payload);
        append_capsule(18, second_payload);
        if (truncated) {
            wire.pop_back();
        } else {
            append_capsule(19, {});
        }
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf head;
            co_await asio::async_read_until(socket, head, "\r\n\r\n", asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_done.notify();
        });
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.client().connect("target.test:443").open();
            if (result.tunnel() == nullptr) {
                throw std::runtime_error("expected an accepted tunnel");
            }
            std::optional<ruvia::http_capsule> retained;
            {
                auto stream = std::move(*result.tunnel()).capsules();
                retained = co_await stream.read();
                EXPECT_TRUE(retained && retained->type() == 17 && retained->payload() == first_payload);
                bool rejected = false;
                try {
                    auto second = co_await stream.read();
                    EXPECT_TRUE(second && second->type() == 18 && second->payload() == second_payload);
                    auto empty = co_await stream.read();
                    EXPECT_TRUE(empty && empty->type() == 19 && empty->payload().empty());
                    EXPECT_TRUE(!(co_await stream.read()));
                    co_await stream.finish();
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                EXPECT_EQ(rejected, truncated);
            }
            EXPECT_TRUE(retained && retained->payload() == first_payload);
            retained.reset();
            co_await client.shutdown();
            co_await peer_done.wait();
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
        };
        run_operation(worker, io, operation);
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_client_uses_available_stream_capacity_across_connections) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::array<std::optional<asio::ip::tcp::socket>, 2> sockets;
    std::array<ruvia::http2_connection, 2> connections{ruvia::http2_connection::server(), ruvia::http2_connection::server()};
    ruvia::worker_signal peer_done(worker.handle_);
    asio::steady_timer deadline(io);
    std::exception_ptr peer_failure;
    bool peer_finished = false;

    const auto flush = [&](std::size_t index, std::optional<std::uint32_t> stream_limit = std::nullopt) -> asio::awaitable<void> {
        if (stream_limit && connections[index].submit_max_concurrent_streams(*stream_limit) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 capacity settings rejected");
        }
        std::string output;
        ruvia::test::drain_output(connections[index], output);
        if (!output.empty()) {
            co_await asio::async_write(*sockets[index], asio::buffer(output), asio::use_awaitable);
        }
    };
    const auto read_request = [&](std::size_t index) -> asio::awaitable<ruvia::http2_request_head_event> {
        std::array<char, 16384> input{};
        for (;;) {
            while (auto event = connections[index].next_event()) {
                if (auto* request = event->request_head()) {
                    co_return std::move(*request);
                }
            }
            const auto count = co_await sockets[index]->async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connections[index].feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 capacity test input");
                }
                while (auto event = connections[index].next_event()) {
                    if (auto* request = event->request_head()) {
                        co_return std::move(*request);
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    const auto respond = [&](std::size_t index, ruvia::http2_request_head_event&& request) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        const auto submitted = connections[index].submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted) || connections[index].release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 capacity test response rejected");
        }
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        // Concurrent requests of one route share the first connection's
        // negotiation; its single stream slot then sends the next request to
        // a second connection, which has two.
        sockets[0].emplace(co_await acceptor.async_accept(asio::use_awaitable));
        co_await flush(0, 1);
        sockets[1].emplace(co_await acceptor.async_accept(asio::use_awaitable));
        co_await flush(1, 2);

        auto first_warm = co_await read_request(0);
        auto second_warm = co_await read_request(1);
        respond(1, std::move(second_warm));
        co_await flush(1);
        respond(0, std::move(first_warm));
        co_await flush(0);

        auto saturated = co_await read_request(0);
        auto available = co_await read_request(1);
        auto rerouted = co_await read_request(1);
        if (rerouted.build_request().target() != "/probe") {
            throw std::runtime_error("HTTP/2 request was not routed to available stream capacity");
        }
        respond(0, std::move(saturated));
        respond(1, std::move(available));
        respond(1, std::move(rerouted));
        co_await flush(0);
        co_await flush(1);
        for (auto& socket : sockets) {
            std::error_code ignored;
            socket->close(ignored);
        }
    };
    deadline.expires_after(std::chrono::seconds(3));
    deadline.async_wait([&](const std::error_code& error) {
        if (error) {
            return;
        }
        std::error_code ignored;
        acceptor.close(ignored);
        for (auto& socket : sockets) {
            if (socket) {
                socket->close(ignored);
            }
        }
    });
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        deadline.cancel();
        peer_done.notify();
    });

    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = 2;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(2);
    ruvia::http_client client(worker.attachment_.loop(), config);
    const auto send = [&](ruvia::http_client& client_value, std::string_view target) -> ruvia::task<void> {
        auto response = co_await client_value.client().get(target).send();
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        EXPECT_TRUE((co_await response.stream().read_all()).empty());
    };
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            ruvia::task_scope warm(worker.handle_);
            warm.spawn(send(client, "/warm-first"));
            warm.spawn(send(client, "/warm-second"));
            co_await warm.join();
        }
        {
            ruvia::task_scope requests(worker.handle_);
            requests.spawn(send(client, "/hold-saturated"));
            requests.spawn(send(client, "/hold-available"));
            requests.spawn(send(client, "/probe"));
            co_await requests.join();
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http2_cold_start_cancelled_waiter_does_not_close_active_session) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_done(worker.handle_);
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::stop_source cancel_second;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        // Cancel the second queued request while the first is completing the
        // HTTP/2 connection preface, so the second wakes from http1_scheduler_
        // after protocol_ is already http2.
        cancel_second.request_stop();
        auto connection = ruvia::http2_connection::server();
        std::array<char, 16384> input{};
        std::string output;
        bool answered = false;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            if (answered && !connection.wants_write()) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
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
                    const auto stream_id = request->stream_id();
                    const auto built = request->build_request();
                    ruvia::http_response response;
                    response.body("first-ok");
                    const auto head = connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete || connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                        throw std::runtime_error("HTTP/2 response submission failed");
                    }
                    answered = true;
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    ruvia::http_client client(worker.attachment_.loop(), config);
    const auto send_first = [](ruvia::http_client& c, bool& ok_out) -> ruvia::task<void> {
        auto response = co_await c.client().get("/first").send();
        const auto body = co_await response.stream().read_all();
        ok_out = response.status() == ruvia::http_status::ok && std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()) == "first-ok";
    };
    const auto send_second = [](ruvia::http_client& c, ruvia::stop_token token, bool& cancelled_out) -> ruvia::task<void> {
        try {
            (void)co_await c.client().with_options({.stop_token_ = std::move(token)}).get("/second").send();
        } catch (const ruvia::http_client_error& error) {
            cancelled_out = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
    };
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope tasks(worker.handle_);
        bool first_ok = false;
        bool second_cancelled = false;
        tasks.spawn(send_first(client, first_ok));
        tasks.spawn(send_second(client, cancel_second.token(), second_cancelled));
        co_await tasks.join();
        EXPECT_TRUE(first_ok);
        EXPECT_TRUE(second_cancelled);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_alpn_renegotiates_after_http2_disconnect_and_closes_on_mismatch) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    std::atomic<bool> offer_h2{true};
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen, void* arg) -> int {
        const bool want_h2 = static_cast<std::atomic<bool>*>(arg)->load();
        const std::string_view target = want_h2 ? "h2" : "http/1.1";
        unsigned int pos = 0;
        while (pos < inlen) {
            const unsigned int len = in[pos++];
            if (pos + len <= inlen && std::string_view(reinterpret_cast<const char*>(in + pos), len) == target) {
                *out = in + pos;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            pos += len;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }, &offer_h2);

    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_done(worker.handle_);
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    bool mismatch_socket_closed = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        // 1st connection: negotiate h2, serve one request, then close.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            auto connection = ruvia::http2_connection::server();
            std::array<char, 16384> input{};
            std::string output;
            bool answered = false;
            for (;;) {
                if (connection.wants_write()) {
                    ruvia::test::drain_output(connection, output);
                    co_await asio::async_write(stream, asio::buffer(output), asio::use_awaitable);
                }
                // The drain ends with the final GOAWAY sent after the client's
                // PING ACK, or earlier when the retiring client closes first.
                if (answered && connection.drain_final() && !connection.wants_write()) {
                    break;
                }
                std::error_code read_error;
                const auto count = co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, read_error));
                if (read_error && answered) {
                    break;
                }
                if (read_error) {
                    throw std::system_error(read_error);
                }
                (void)connection.feed(std::string_view(input.data(), count));
                while (auto event = connection.next_event()) {
                    if (auto* request = event->request_head()) {
                        const auto stream_id = request->stream_id();
                        const auto built = request->build_request();
                        ruvia::http_response response;
                        response.body("h2-ok");
                        (void)connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                        (void)connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535);
                        (void)connection.release(std::move(*request));
                        // GOAWAY precedes the close so the client retires this
                        // session deterministically: seen before the next send it
                        // opens a new connection; the PING round trip then
                        // advertises last_stream_id = 1 before the close.
                        connection.begin_drain();
                        answered = true;
                    }
                }
            }
            offer_h2.store(false);
            std::error_code ignored;
            stream.lowest_layer().close(ignored);
        }
        // 2nd connection: negotiate http/1.1 for the negotiate client.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nh1-ok";
            co_await asio::async_write(stream, asio::buffer(reply), asio::use_awaitable);
        }
        // 3rd connection: http2_only client rejects http/1.1 ALPN and closes socket.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            std::array<char, 16> buf{};
            std::error_code read_ec;
            (void)co_await stream.next_layer().async_read_some(asio::buffer(buf), asio::redirect_error(asio::use_awaitable, read_ec));
            mismatch_socket_closed = read_ec == asio::error::eof || read_ec == asio::error::connection_reset;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{};
    ruvia::http_client negotiate_client(worker.attachment_.loop(), config);

    auto h2_only_config = config;
    h2_only_config.versions_ = ruvia::http_client_versions{.http1_ = false};
    ruvia::http_client h2_only_client(worker.attachment_.loop(), h2_only_config);

    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto first = co_await negotiate_client.client().get("/first").send();
            EXPECT_TRUE(first.protocol_version() == ruvia::http_protocol_version::http2);
            const auto first_body = co_await first.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first_body.bytes().data()), first_body.size()), "h2-ok");
        }
        {
            auto second = co_await negotiate_client.client().get("/second").send();
            EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http11);
            const auto second_body = co_await second.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(second_body.bytes().data()), second_body.size()), "h1-ok");
        }
        bool protocol_unavailable = false;
        try {
            (void)co_await h2_only_client.client().get("/mismatch").send();
        } catch (const ruvia::http_client_error& error) {
            protocol_unavailable = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_unavailable);
        }
        EXPECT_TRUE(protocol_unavailable);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(mismatch_socket_closed);
        co_await negotiate_client.shutdown();
        co_await h2_only_client.shutdown();
    };
    run_operation(worker, io, operation);
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), nullptr, nullptr);
}

#endif

// RFC 6265 section 5.4: stored and configured cookies belong to the fixed
// origin. A CONNECT-UDP tunnel to another authority carries none of them, and
// that authority's Set-Cookie does not enter the origin's jar.
TEST(Ruvia, http_client_scopes_cookies_to_the_origin_authority_for_tunnels) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 407 Proxy Authentication Required\r\nSet-Cookie: tunnel=other\r\nContent-Length: 0\r\n"
         "Connection: close\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.cookies_ = {{"sid", "secret"}};
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        const std::array fields{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
        auto result = co_await client.client().connect("proxy.test:8443").protocol("connect-udp").target("/.well-known/masque/udp/target.test/53/").headers(fields).open();
        EXPECT_TRUE(result.tunnel() == nullptr);
        if (auto* rejected = result.response()) {
            EXPECT_EQ(rejected->status().value(), std::uint16_t{407});
            EXPECT_TRUE((co_await rejected->stream().read_all()).empty());
        }
        auto origin = co_await client.client().get("/").send();
        EXPECT_TRUE((co_await origin.stream().read_all()).empty());
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            EXPECT_TRUE(peer.requests_[0].find("secret") == std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("sid=secret") != std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("tunnel=other") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// read_timeout_ bounds each response read rather than the exchange: a peer
// that stops sending the head, or the body after its head, fails the response
// with timeout long before request_timeout_ and loses the connection.
TEST(Ruvia, http1_client_read_timeout_bounds_a_stalled_response_head_and_body) {
    for (const bool head_sent : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        bool client_closed = false;
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            if (head_sent) {
                const std::string partial("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab");
                co_await asio::async_write(socket, asio::buffer(partial), asio::use_awaitable);
            }
            // Stall until the client abandons the read and closes.
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            client_closed = true;
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.read_timeout_ = std::chrono::milliseconds(100);
        config.request_timeout_ = std::chrono::seconds(20);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                auto response = co_await client.client().get("/stall").send();
                EXPECT_TRUE(head_sent);
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            EXPECT_TRUE(client_closed);
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

// idle_timeout_ closes a pooled keep-alive connection that no exchange reused,
// without a request; the next request opens a new connection.
TEST(Ruvia, http1_client_idle_timeout_closes_an_unused_pooled_connection) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::optional<asio::ip::tcp::socket> idle_socket;
    asio::steady_timer guard(io);
    unsigned connections = 0;
    bool idle_closed = false;
    std::chrono::steady_clock::duration idle_for{};
    ruvia::worker_signal idle_closed_signal(worker.handle_);
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        for (const std::string_view body : {std::string_view("first"), std::string_view("second")}) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ++connections;
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            if (idle_closed) {
                continue;
            }
            // Keep the connection open; only the client's idle bound ends it.
            const auto responded = std::chrono::steady_clock::now();
            idle_socket.emplace(std::move(socket));
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await idle_socket->async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            idle_for = std::chrono::steady_clock::now() - responded;
            idle_closed = true;
            idle_closed_signal.notify();
        }
    };
    guard.expires_after(std::chrono::seconds(10));
    guard.async_wait([&](const std::error_code& error) {
        if (!error && idle_socket) {
            std::error_code ignored;
            idle_socket->close(ignored);
        }
    });
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        guard.cancel();
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.idle_timeout_ = std::chrono::milliseconds(100);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.client().get("/idle").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
            while (!idle_closed) {
                co_await idle_closed_signal.wait();
            }
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(idle_for < std::chrono::seconds(5));
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.failed_requests_, std::size_t{0});
        EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

namespace {
#if defined(RUVIA_ENABLE_HTTP2)
// One prior-knowledge HTTP/2 connection served by the sans-I/O core, which
// answers PINGs as it reads. The mode decides each request's answer.
class http2_liveness_peer final {
public:
    enum class mode_type : std::uint8_t {
        // The first request never gets a response; later ones get "ok".
        stall_first_request,
        answer,
        // The request is answered after the client sent two liveness PINGs.
        answer_after_pings,
        // After the request the peer reads but neither processes nor writes.
        silent_after_request,
    };

    http2_liveness_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, mode_type mode) :
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        guard_(io),
        done_(worker_value),
        io_(io),
        mode_(mode) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        guard_.expires_after(std::chrono::seconds(10));
        guard_.async_wait([this](const std::error_code& error) {
            if (error) {
                return;
            }
            std::error_code ignored;
            acceptor_.close(ignored);
            if (socket_) {
                socket_->close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            finished_ = true;
            guard_.cancel();
            done_.notify();
        });
    }

    // Completes once the client closed the connection.
    [[nodiscard]] ruvia::task<void> wait() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    // From the last response to the client's close.
    std::chrono::steady_clock::duration idle_for_{};
    // The client sent GOAWAY before it closed (RFC 9113 section 6.8).
    bool goaway_received_{false};

private:
    // A liveness PING the client sent: length 8, type PING, no ACK flag.
    [[nodiscard]] static std::size_t count_pings(std::string_view bytes) noexcept {
        static constexpr std::string_view ping_header("\0\0\x08\x06\0", 5);
        std::size_t count = 0;
        for (auto at = bytes.find(ping_header); at != std::string_view::npos; at = bytes.find(ping_header, at + 1)) {
            ++count;
        }
        return count;
    }

    void respond(ruvia::http2_connection& connection, ruvia::http2_request_head_event&& request) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        response.body("ok");
        const auto head = connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete || connection.release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 liveness peer response rejected");
        }
        responded_ = std::chrono::steady_clock::now();
    }

    asio::awaitable<void> serve() {
        socket_.emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        auto connection = ruvia::http2_connection::server();
        std::array<char, 16384> input{};
        std::string output;
        std::vector<ruvia::http2_request_head_event> stalled{};
        std::optional<ruvia::http2_request_head_event> pending;
        std::size_t requests = 0;
        std::size_t pings = 0;
        bool silent = false;
        for (;;) {
            std::error_code error;
            if (!silent && connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                // The core answers a client GOAWAY with its own, which may
                // reach a client that already closed.
                co_await asio::async_write(*socket_, asio::buffer(output), asio::redirect_error(asio::use_awaitable, error));
            }
            std::size_t count = 0;
            if (!error) {
                count = co_await socket_->async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            if (error) {
                idle_for_ = std::chrono::steady_clock::now() - responded_;
                co_return;
            }
            if (silent) {
                continue;
            }
            const auto bytes = std::string_view(input.data(), count);
            if (pending) {
                pings += count_pings(bytes);
            }
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 liveness peer input");
                }
                while (auto event = connection.next_event()) {
                    if (event->goaway() != nullptr) {
                        goaway_received_ = true;
                    }
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    ++requests;
                    switch (mode_) {
                        case mode_type::stall_first_request:
                            if (requests == 1) {
                                stalled.push_back(std::move(*request));
                            } else {
                                respond(connection, std::move(*request));
                            }
                            break;
                        case mode_type::answer:
                            respond(connection, std::move(*request));
                            break;
                        case mode_type::answer_after_pings:
                            pending.emplace(std::move(*request));
                            break;
                        case mode_type::silent_after_request:
                            stalled.push_back(std::move(*request));
                            silent = true;
                            break;
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
            if (pending && pings >= 2) {
                respond(connection, std::move(*pending));
                pending.reset();
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer guard_;
    ruvia::worker_signal done_;
    asio::io_context& io_;
    mode_type mode_;
    std::optional<asio::ip::tcp::socket> socket_;
    std::chrono::steady_clock::time_point responded_{std::chrono::steady_clock::now()};
    std::exception_ptr failure_;
    bool finished_{false};
};

[[nodiscard]] ruvia::http_client_config local_http2_client_config(std::string_view base_url) {
    auto config = local_http_client_config(base_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(20);
    return config;
}

#endif

[[nodiscard]] std::string_view response_text(const ruvia::http_client_response_bytes& bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()};
}
}  // namespace

#if defined(RUVIA_ENABLE_HTTP2)
// The HTTP/2 read inactivity bound resets only the stalled stream: the next
// request reuses the same session.
TEST(Ruvia, http2_client_read_timeout_resets_the_stalled_stream_and_keeps_the_session) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::stall_first_request);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::error_code> code;
        try {
            (void)co_await client.client().get("/stall").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
        }
        EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        // The peer accepts one connection only.
        auto response = co_await client.client().get("/ok").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        co_await client.shutdown();
        co_await peer.wait();
    };
    run_operation(worker, io, operation);
}

// idle_timeout_ closes an HTTP/2 connection once its last stream is gone,
// with GOAWAY before the close (RFC 9113 section 6.8).
TEST(Ruvia, http2_client_idle_timeout_closes_a_connection_without_streams) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::answer);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.idle_timeout_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/ok").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        // Only the idle bound closes the connection before the guard does.
        co_await peer.wait();
        EXPECT_TRUE(peer.idle_for_ < std::chrono::seconds(5));
        EXPECT_TRUE(peer.goaway_received_);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// http2_ping_interval_: a connection that stays silent for an interval gets a
// PING, and one that stays silent for another fails with timeout.
TEST(Ruvia, http2_client_ping_fails_a_silent_connection) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::silent_after_request);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::nullopt;
    config.http2_ping_interval_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::error_code> code;
        try {
            (void)co_await client.client().get("/silent").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
        }
        EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        // The failed session closed its transport.
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// A peer that acknowledges the liveness PINGs keeps a quiet connection alive
// beyond two intervals.
TEST(Ruvia, http2_client_ping_ack_keeps_a_quiet_connection_alive) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::answer_after_pings);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::nullopt;
    config.http2_ping_interval_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/slow").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        co_await client.shutdown();
        co_await peer.wait();
    };
    run_operation(worker, io, operation);
}

#endif

namespace http_client_pool_test {

// Completion, progress and gating of one scripted origin coroutine, observed
// from the client's worker. Its guard closes the acceptor and every connection
// the script registered, so a client that stops early cannot leave it pending.
class scripted_origin final {
public:
    scripted_origin(asio::io_context& io, const ruvia::worker_ref& worker_value) :
        io_(io),
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        guard_(io),
        gate_(io),
        progress_(worker_value) {
        gate_.expires_at(asio::steady_timer::time_point::max());
    }
    scripted_origin(asio::io_context&, ruvia::worker_ref&&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    [[nodiscard]] asio::ip::tcp::acceptor& acceptor() noexcept {
        return acceptor_;
    }

    void start(asio::awaitable<void> script) {
        guard_.expires_after(std::chrono::seconds(10));
        guard_.async_wait([this](const std::error_code& error) {
            if (error || finished_) {
                return;
            }
            std::error_code ignored;
            acceptor_.close(ignored);
            for (auto* socket : sockets_) {
                socket->close(ignored);
            }
        });
        asio::co_spawn(io_, std::move(script), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            finished_ = true;
            guard_.cancel();
            progress_.notify();
        });
    }

    // Script side.
    void watch(asio::ip::tcp::socket::lowest_layer_type& socket) {
        sockets_.push_back(&socket);
    }
    void advance() noexcept {
        ++step_;
        progress_.notify();
    }
    [[nodiscard]] asio::awaitable<void> wait_for_release() {
        std::error_code ignored;
        co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        gate_.expires_at(asio::steady_timer::time_point::max());
    }

    // Client side.
    void release() {
        gate_.expires_at(asio::steady_timer::time_point::min());
    }
    [[nodiscard]] ruvia::task<void> reach(std::size_t step) {
        while (step_ < step && !finished_) {
            co_await progress_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (step_ < step) {
            throw std::runtime_error("scripted origin ended early");
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await progress_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    void stop_accepting() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }

private:
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer guard_;
    asio::steady_timer gate_;
    ruvia::worker_signal progress_;
    std::vector<asio::ip::tcp::socket::lowest_layer_type*> sockets_{};
    std::exception_ptr failure_;
    std::size_t step_{0};
    bool finished_{false};
};

#if defined(RUVIA_ENABLE_HTTP2)
// One server connection of a scripted HTTP/2 origin over TCP or TLS, driven by
// the sans-I/O core, which answers SETTINGS and PING itself. Request heads
// queue in arrival order.
template <typename stream_type>
class http2_origin_connection final {
public:
    http2_origin_connection(scripted_origin& origin, stream_type stream) :
        stream_(std::move(stream)) {
        origin.watch(stream_.lowest_layer());
    }

    [[nodiscard]] ruvia::http2_connection& connection() noexcept {
        return connection_;
    }
    [[nodiscard]] bool goaway_received() const noexcept {
        return goaway_received_;
    }

    // Submit capacity settings through the protocol engine so its ACK accounting
    // stays synchronized with the peer, then write every pending frame.
    [[nodiscard]] asio::awaitable<bool> flush(std::optional<std::uint8_t> stream_limit = std::nullopt) {
        if (stream_limit && connection_.submit_max_concurrent_streams(*stream_limit) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 scripted origin settings rejected");
        }
        ruvia::test::drain_output(connection_, output_);
        if (output_.empty()) {
            co_return true;
        }
        std::error_code error;
        co_await asio::async_write(stream_, asio::buffer(output_), asio::redirect_error(asio::use_awaitable, error));
        co_return !error;
    }
    [[nodiscard]] asio::awaitable<ruvia::http2_request_head_event> next_request() {
        while (requests_.empty()) {
            if (!co_await read()) {
                throw std::runtime_error("client ended the scripted HTTP/2 connection before a request");
            }
        }
        auto request = std::move(requests_.front());
        requests_.pop_front();
        co_return request;
    }
    // Reads until the drain this side started is final and written.
    [[nodiscard]] asio::awaitable<void> finish_drain() {
        while (!connection_.drain_final() || connection_.wants_write()) {
            if (!co_await read()) {
                throw std::runtime_error("client ended the scripted HTTP/2 connection during its drain");
            }
        }
    }
    [[nodiscard]] asio::awaitable<void> read_until_end() {
        while (co_await read()) {
        }
    }
    void respond(ruvia::http2_request_head_event&& request, std::string_view body) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        response.body(body);
        const auto head = connection_.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection_.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete || connection_.release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("scripted HTTP/2 response rejected");
        }
    }
    void refuse(ruvia::http2_request_head_event&& request, ruvia::http2_error_code error) {
        if (connection_.submit_reset(request.stream_id(), error) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("scripted HTTP/2 reset rejected");
        }
        (void)connection_.release(std::move(request));
    }
    // An abrupt end of the connection, without GOAWAY.
    void close() {
        std::error_code ignored;
        stream_.lowest_layer().close(ignored);
    }
    // Sends FIN; reading continues until the client closes.
    void end_output() {
        std::error_code ignored;
        stream_.lowest_layer().shutdown(asio::socket_base::shutdown_send, ignored);
    }

private:
    // One read: feeds the core, queues request heads and writes its answers.
    [[nodiscard]] asio::awaitable<bool> read() {
        if (!co_await flush()) {
            co_return false;
        }
        std::error_code error;
        const auto count = co_await stream_.async_read_some(asio::buffer(input_), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return false;
        }
        const auto bytes = std::string_view(input_.data(), count);
        for (;;) {
            const auto status = connection_.feed(bytes);
            if (status == ruvia::http2_feed_result::protocol_failure) {
                throw std::runtime_error("invalid client HTTP/2 input");
            }
            while (auto event = connection_.next_event()) {
                if (event->goaway() != nullptr) {
                    goaway_received_ = true;
                }
                if (auto* request = event->request_head()) {
                    requests_.push_back(std::move(*request));
                }
            }
            if (status != ruvia::http2_feed_result::events_pending) {
                break;
            }
        }
        co_return co_await flush();
    }

    stream_type stream_;
    ruvia::http2_connection connection_{ruvia::http2_connection::server()};
    std::array<char, 16384> input_{};
    std::string output_;
    std::deque<ruvia::http2_request_head_event> requests_{};
    bool goaway_received_{false};
};

using tcp_origin_connection = http2_origin_connection<asio::ip::tcp::socket>;

[[nodiscard]] ruvia::http_client_config http2_client_config(std::string_view base_url) {
    auto config = local_http_client_config(base_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(5);
    return config;
}

#endif

[[nodiscard]] ruvia::task<void> send_and_discard(ruvia::http_client& client, std::string_view target) {
    try {
        auto response = co_await client.client().get(target).send();
        (void)co_await response.stream().read_all();
    } catch (const ruvia::http_client_error&) {
        // Ended by the client's shutdown.
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
[[nodiscard]] ruvia::task<void> send_and_keep(ruvia::http_client& client, std::string_view target, std::optional<std::string>& body, bool& done, ruvia::worker_signal& done_signal) {
    try {
        auto response = co_await client.client().get(target).send();
        const auto bytes_value = co_await response.stream().read_all();
        body.emplace(response_text(bytes_value));
    } catch (const ruvia::http_client_error&) {
        // `body` stays unset; the caller's check reports it.
    }
    done = true;
    done_signal.notify();
}

#endif

// Runs `operation`, then always shuts the client down and joins the requests
// it spawned into `requests` and `origin`, so a failed check cannot leave the
// origin, the client or a request pending.
template <typename operation_type>
[[nodiscard]] ruvia::task<void> run_against(ruvia::http_client& client, scripted_origin& origin, ruvia::task_scope& requests, operation_type&& operation) {
    std::exception_ptr failure;
    try {
        co_await operation();
    } catch (...) {
        failure = std::current_exception();
    }
    origin.stop_accepting();
    co_await client.shutdown();
    try {
        co_await requests.join();
        co_await origin.join();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http_client_pool_test

#if defined(RUVIA_ENABLE_HTTP2)
// A draining HTTP/2 session never holds a new request beyond acquire_timeout_,
// even without a request timeout: the wait for the slot's reconnect is a
// connection acquisition (Pingora never reuses a shutting-down connection).
TEST(Ruvia, http2_client_draining_session_bounds_a_new_request_by_acquire_timeout) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection first(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await first.flush();
        // The held request stays open across the drain.
        [[maybe_unused]] const auto held = co_await first.next_request();
        first.connection().begin_drain();
        co_await first.finish_drain();
        origin.advance();
        co_await first.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.request_timeout_ = std::nullopt;
    config.acquire_timeout_ = std::chrono::milliseconds(200);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_discard(client, "/hold"));
            co_await origin.reach(1);
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().with_options({.timeout_ = std::chrono::seconds(5)}).get("/next").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
        });
    };
    run_operation(worker, io, operation);
}

// A request routed to a draining HTTP/2 session uses a free, unconnected slot
// instead of waiting for the drain. With two connections of two streams each,
// leases alternate between the slots: /b and /c complete in that order, so
// /next takes the lease of the draining slot.
TEST(Ruvia, http2_client_draining_session_yields_to_a_free_slot) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection draining(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await draining.flush();
        [[maybe_unused]] const auto held = co_await draining.next_request();
        origin.advance();
        // Requests share the session while its two streams last.
        auto c = co_await draining.next_request();
        origin.advance();
        tcp_origin_connection closing(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await closing.flush();
        auto b = co_await closing.next_request();
        origin.advance();
        closing.respond(std::move(b), "b");
        (void)co_await closing.flush();
        // The peer ends this connection: its slot becomes free and unconnected.
        closing.end_output();
        co_await origin.wait_for_release();
        draining.respond(std::move(c), "c");
        (void)co_await draining.flush();
        draining.connection().begin_drain();
        co_await draining.finish_drain();
        origin.advance();
        tcp_origin_connection fresh(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await fresh.flush();
        fresh.respond(co_await fresh.next_request(), "next");
        (void)co_await fresh.flush();
        co_await closing.read_until_end();
        co_await fresh.read_until_end();
        co_await draining.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = 2;
    config.acquire_timeout_ = std::chrono::milliseconds(300);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::string> b_body;
    std::optional<std::string> c_body;
    bool b_done = false;
    bool c_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_discard(client, "/hold"));
            co_await origin.reach(1);
            requests.spawn(send_and_keep(client, "/c", c_body, c_done, done_signal));
            co_await origin.reach(2);
            requests.spawn(send_and_keep(client, "/b", b_body, b_done, done_signal));
            co_await origin.reach(3);
            while (!b_done) {
                co_await done_signal.wait();
            }
            // Its driver releases the lease after the body completed.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(50));
            origin.release();
            while (!c_done) {
                co_await done_signal.wait();
            }
            co_await origin.reach(4);
            // The ended connection's session retires meanwhile.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            EXPECT_TRUE(b_body == std::optional<std::string>(std::string("b")));
            EXPECT_TRUE(c_body == std::optional<std::string>(std::string("c")));
            auto next = co_await client.client().get("/next").send();
            EXPECT_EQ(response_text(co_await next.stream().read_all()), "next");
        });
    };
    run_operation(worker, io, operation);
}

// An idempotent request whose reused HTTP/2 connection fails at the transport
// after its head was sent is retried once on a new connection, like an HTTP/1
// exchange (Pingora RetryType::ReusedOnly); a non-idempotent one is not.
TEST(Ruvia, http2_client_retries_only_idempotent_requests_after_a_reused_connection_fails) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    unsigned connections = 0;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tcp_origin_connection first(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
            ++connections;
            (void)co_await first.flush();
            first.respond(co_await first.next_request(), "prime");
            (void)co_await first.flush();
            (void)co_await first.next_request();
            // No GOAWAY: an L4 middlebox reset, for example.
            first.close();
        }
        tcp_origin_connection second(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        ++connections;
        (void)co_await second.flush();
        second.respond(co_await second.next_request(), "retried");
        (void)co_await second.flush();
        (void)co_await second.next_request();
        second.close();
        std::error_code accept_error;
        [[maybe_unused]] auto extra = co_await origin.acceptor().async_accept(asio::redirect_error(asio::use_awaitable, accept_error));
        if (!accept_error) {
            ++connections;
        }
    };
    origin.start(script());
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    ruvia::http_client client(worker.attachment_.loop(), http2_client_config(client_url));
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto prime = co_await client.client().get("/prime").send();
            EXPECT_EQ(response_text(co_await prime.stream().read_all()), "prime");
            auto retried = co_await client.client().get("/retry").send();
            EXPECT_EQ(response_text(co_await retried.stream().read_all()), "retried");
            std::optional<std::error_code> post_failure;
            try {
                (void)co_await client.client().request("POST", "/post").send();
            } catch (const ruvia::http_client_error& error) {
                post_failure = error.code();
            }
            EXPECT_TRUE(post_failure == ruvia::make_error_code(ruvia::http_client_errc::io_error));
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.completed_requests_, std::size_t{2});
            EXPECT_EQ(stats.failed_requests_, std::size_t{1});
        });
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

// A peer SETTINGS_MAX_CONCURRENT_STREAMS below the local limit parks a request
// for stream capacity at most acquire_timeout_, even without a request
// timeout; a parked request proceeds once a stream closes.
TEST(Ruvia, http2_client_stream_capacity_wait_is_an_acquisition) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection only(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await only.flush(std::uint8_t{1});
        auto held = co_await only.next_request();
        origin.advance();
        co_await origin.wait_for_release();
        only.respond(std::move(held), "held");
        (void)co_await only.flush();
        only.respond(co_await only.next_request(), "after");
        (void)co_await only.flush();
        co_await only.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.request_timeout_ = std::nullopt;
    config.acquire_timeout_ = std::chrono::milliseconds(500);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::string> held_body;
    std::optional<std::string> after_body;
    bool held_done = false;
    bool after_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_keep(client, "/held", held_body, held_done, done_signal));
            co_await origin.reach(1);
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().with_options({.timeout_ = std::chrono::seconds(5)}).get("/blocked").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
            requests.spawn(send_and_keep(client, "/after", after_body, after_done, done_signal));
            // Parked for capacity before the held stream closes.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(50));
            origin.release();
            while (!held_done || !after_done) {
                co_await done_signal.wait();
            }
            EXPECT_TRUE(held_body == std::optional<std::string>(std::string("held")));
            EXPECT_TRUE(after_body == std::optional<std::string>(std::string("after")));
        });
    };
    run_operation(worker, io, operation);
}

#endif

// Pingora's idle_poll: a pooled HTTP/1 connection closes as soon as its peer
// ends it or sends unsolicited bytes, even without idle_timeout_.
TEST(Ruvia, http1_client_closes_an_idle_connection_its_peer_ended_or_wrote_to) {
    for (const bool peer_writes : {false, true}) {
        using namespace http_client_pool_test;
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        scripted_origin origin(io, worker.handle_);
        std::optional<std::chrono::steady_clock::duration> closed_after;
        const auto script = [&]() -> asio::awaitable<void> {
            auto socket = co_await origin.acceptor().async_accept(asio::use_awaitable);
            origin.watch(socket.lowest_layer());
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            origin.advance();
            // Lets the client pool the connection first.
            asio::steady_timer pause(socket.get_executor(), std::chrono::milliseconds(50));
            co_await pause.async_wait(asio::use_awaitable);
            const auto acted = std::chrono::steady_clock::now();
            if (peer_writes) {
                co_await asio::async_write(socket, asio::buffer(std::string_view("X")), asio::use_awaitable);
            } else {
                socket.shutdown(asio::socket_base::shutdown_send);
            }
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                closed_after = std::chrono::steady_clock::now() - acted;
            }
        };
        origin.start(script());
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
        auto config = local_http_client_config(config_url);
        config.connection_limits_.max_open_ = 1;
        config.idle_timeout_ = std::nullopt;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::task_scope requests(worker.handle_);
        auto operation = [&]() -> ruvia::task<void> {
            co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
                auto response = co_await client.client().get("/first").send();
                EXPECT_EQ(response_text(co_await response.stream().read_all()), "first");
                // The origin ends only once the client closed the connection;
                // its guard would close it after 10 seconds.
                co_await origin.join();
                EXPECT_TRUE(closed_after.has_value() && *closed_after < std::chrono::seconds(2));
                EXPECT_EQ(client.client().stats().idle_connections_with_pending_data_, peer_writes ? std::size_t{1} : std::size_t{0});
            });
        };
        run_operation(worker, io, operation);
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
// RFC 9113 section 7: RST_STREAM(HTTP_1_1_REQUIRED) before any response head
// makes a negotiate client retry the request over HTTP/1.1, and every later
// connection offers only http/1.1 (Pingora's prefer_h1).
TEST(Ruvia, http_client_retries_over_http11_after_http11_required) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    // Whether each handshake offered h2; the origin selects it when offered.
    std::vector<bool> offered_h2;
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen, void* arg) -> int {
        bool h2 = false;
        const unsigned char* http11 = nullptr;
        for (unsigned int pos = 0; pos < inlen;) {
            const unsigned int len = in[pos++];
            if (pos + len > inlen) {
                break;
            }
            const std::string_view protocol(reinterpret_cast<const char*>(in + pos), len);
            if (protocol == "h2") {
                h2 = true;
                *out = in + pos;
                *outlen = static_cast<unsigned char>(len);
            } else if (protocol == "http/1.1") {
                http11 = in + pos;
            }
            pos += len;
        }
        static_cast<std::vector<bool>*>(arg)->push_back(h2);
        if (h2) {
            return SSL_TLSEXT_ERR_OK;
        }
        if (http11 != nullptr) {
            *out = http11;
            *outlen = 8;
            return SSL_TLSEXT_ERR_OK;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }, &offered_h2);
    scripted_origin origin(io, worker.handle_);
    using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tls_stream stream(co_await origin.acceptor().async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            http2_origin_connection<tls_stream> refusing(origin, std::move(stream));
            (void)co_await refusing.flush();
            refusing.refuse(co_await refusing.next_request(), ruvia::http2_error_code::http11_required);
            (void)co_await refusing.flush();
            co_await refusing.read_until_end();
        }
        tls_stream stream(co_await origin.acceptor().async_accept(asio::use_awaitable), identity.context_);
        origin.watch(stream.lowest_layer());
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const std::string_view body : {std::string_view("h1-ok"), std::string_view("h1-again")}) {
            std::string request{};
            co_await asio::async_read_until(stream, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
        std::array<char, 64> input{};
        std::error_code error;
        while (!error) {
            (void)co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        }
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(origin.port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(5);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto first = co_await client.client().get("/first").send();
            EXPECT_TRUE(first.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_EQ(response_text(co_await first.stream().read_all()), "h1-ok");
            auto second = co_await client.client().get("/second").send();
            EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_EQ(response_text(co_await second.stream().read_all()), "h1-again");
        });
    };
    run_operation(worker, io, operation);
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), nullptr, nullptr);
    EXPECT_TRUE(offered_h2 == std::vector<bool>({true, false}));
}
#endif
