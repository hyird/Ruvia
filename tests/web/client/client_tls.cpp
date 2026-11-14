#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "content_decoding_fixture.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_client.h"
#include "http_client_fixture.h"
#include "http_server_fixture.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_signal.h"
#include "worker_task_fixture.h"

namespace {
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
