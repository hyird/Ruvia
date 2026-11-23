#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

#include "asio_bridge.h"
#include "content_decoding_fixture.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_client.h"
#include "http_client_fixture.h"
#include "http_server_fixture.h"
#include "one_shot.h"
#include "redis_executor.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "tcp_client.h"
#include "worker_signal.h"
#include "worker_task_fixture.h"
#include "worker_wait.h"

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
            auto response = co_await client.ref().with_options({.stop_token_ = cancellation.token()}).get("/partial").send();
            const auto first = co_await response.body().read_text();
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
            while (client.ref().stats().in_flight_requests_ != 0) {
                (void)co_await ruvia::async_asio([&io](auto handler) {
                    asio::post(io, [handler = std::move(handler)]() mutable {
                        handler(std::error_code{});
                    });
                });
            }
            EXPECT_EQ(client.ref().stats().failed_requests_, std::size_t{1});
        }
        for (const auto body : expected) {
            auto response = co_await client.ref().get("/next?exact=body").send();
            EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http11);
            const auto bytes_value = co_await response.collect_body();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), body);
            EXPECT_TRUE(!(co_await response.body().read()));
        }
        co_await peer_done.wait();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_EQ(client.ref().stats().failed_requests_, std::size_t{1});
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
            auto response = co_await client.ref().get("/close-delimited").send();
            std::optional<std::string> body;
            bool truncated = false;
            try {
                const auto bytes_value = co_await response.collect_body();
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
                auto response = co_await client.ref().get("/identity").send();
                EXPECT_EQ(response.status().value(), std::uint16_t{200});
                (void)co_await response.collect_body();
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
            auto result = co_await client.ref().connect("target.test:443").open();
            auto tunnel = std::move(*result.tunnel());
            for (; completed != 64; ++completed) {
                co_await tunnel.stream().write("x");
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

// RFC 6066 §8: a staple belongs to a server identity; a client never sends one.
TEST(Ruvia, http_client_tls_rejects_a_stapled_client_identity) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    auto config = local_http_client_config("https://127.0.0.1:443");
    config.tls_.identity_ = ruvia::tls_identity_config{.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file(), .ocsp_response_ = {std::byte{0x30}}};
    EXPECT_THROW({ ruvia::http_client client(worker.attachment_.loop(), config); }, std::invalid_argument);
}

TEST(Ruvia, http_client_tls_constructor_defers_credential_io_until_first_request) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    auto config = local_http_client_config("https://127.0.0.1:443");
    config.tls_.ca_file_ = identity.ca_file_.parent_path() / "absent-client-ca.pem";
    ruvia::http_client client(worker.attachment_.loop(), config);
    run_operation(worker, io, [&]() -> ruvia::task<void> {
        bool failed = false;
        try {
            (void)co_await client.ref().get("/").send();
        } catch (const std::system_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        co_await client.shutdown();
    });
}

TEST(Ruvia, tcp_client_tls_constructor_defers_credential_io_until_connect) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    ruvia::tcp_client_config config{.host_ = "127.0.0.1", .port_ = 443};
    config.tls_.ca_file_ = identity.ca_file_.parent_path() / "absent-client-ca.pem";
    ruvia::tcp_client client(worker.attachment_.loop(), config);
    run_operation(worker, io, [&]() -> ruvia::task<void> {
        bool failed = false;
        try {
            co_await client.connect();
        } catch (const std::system_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        co_await client.shutdown();
    });
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, redis_client_tls_constructor_defers_credential_io_until_connect) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    ruvia::redis_config config{.host_ = "127.0.0.1", .port_ = 6379};
    config.tls_.ca_file_ = identity.ca_file_.parent_path() / "absent-client-ca.pem";
    ruvia::redis_client client(worker.attachment_.loop(), config);
    run_operation(worker, io, [&]() -> ruvia::task<void> {
        bool failed = false;
        try {
            co_await client.connect();
        } catch (const std::system_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        co_await client.shutdown();
    });
}
#endif

namespace {
void exercise_concurrent_tls_preparation(std::optional<std::size_t> cancelled, bool invalid_ca = false) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::tls_identity identity("localhost");
    ruvia::blocking_pool blocking({.thread_count_ = 1, .queue_capacity_ = 1});
    std::promise<void> entered, release;
    const auto gate = release.get_future().share();
    ASSERT_EQ(blocking.post([gate, &entered] {
        entered.set_value();
        gate.wait();
    }),
        ruvia::post_status::accepted);
    entered.get_future().wait();
    auto attachment = ruvia::event_loop_attachment(io, {.blocking_pool_ = &blocking});
    const auto loop_for_worker = attachment.loop();
    const auto worker = loop_for_worker.worker();
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto base_url = std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port()));
    auto config = local_http_client_config(base_url);
    config.tls_.ca_file_ = invalid_ca ? identity.ca_file_.parent_path() / "absent-preparation-ca.pem" : identity.ca_file_;
    config.connection_limits_.max_open_ = 2;
    config.request_timeout_ = std::chrono::seconds(2);
    ruvia::http_client client(attachment.loop(), config);
    ruvia::worker_signal peer_done(worker);
    bool peer_finished = false;
    std::size_t served = 0;
    std::exception_ptr peer_failure;
    std::promise<void> requests_started;
    const auto expected_connections = invalid_ca ? 0U : (cancelled ? 1U : 2U);
    const auto serve = [&]() -> asio::awaitable<void> {
        for (std::size_t index = 0; index != expected_connections; ++index) {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            ++served;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const auto run = [&]() -> ruvia::task<void> {
        std::array<std::string, 2> bodies;
        std::array<std::exception_ptr, 2> failures;
        ruvia::stop_source stop;
        std::size_t started = 0;
        ruvia::worker_signal progress(worker);
        const auto request = [&](std::size_t index) -> ruvia::task<void> {
            ++started;
            progress.notify();
            try {
                auto response = co_await client.ref().with_options({.stop_token_ = cancelled == index ? stop.token() : ruvia::stop_token{}}).get("/").send();
                const auto bytes = co_await response.collect_body();
                bodies[index].assign(bytes.text());
            } catch (...) {
                failures[index] = std::current_exception();
            }
        };
        ruvia::task_scope requests(worker);
        requests.spawn(request(0));
        requests.spawn(request(1));
        while (started != 2) {
            co_await progress.wait();
        }
        // Both requests are active while an unrelated job holds the sole thread.
        // Only one credential preparation can fit in the remaining queue slot.
        if (cancelled) {
            stop.request_stop();
        }
        requests_started.set_value();
        co_await requests.join();
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        for (std::size_t index = 0; index != bodies.size(); ++index) {
            if (invalid_ca) {
                EXPECT_TRUE(failures[index] != nullptr);
                if (failures[index]) {
                    EXPECT_THROW(std::rethrow_exception(failures[index]), std::system_error);
                }
                EXPECT_TRUE(bodies[index].empty());
            } else if (cancelled == index) {
                EXPECT_TRUE(failures[index] != nullptr);
                if (failures[index]) {
                    try {
                        std::rethrow_exception(failures[index]);
                    } catch (const std::system_error& error) {
                        EXPECT_EQ(error.code(), std::make_error_code(std::errc::operation_canceled));
                    } catch (...) {
                        ADD_FAILURE() << "TLS cancellation returned a non-cancellation failure";
                    }
                }
                EXPECT_TRUE(bodies[index].empty());
            } else {
                if (failures[index]) {
                    EXPECT_NO_THROW(std::rethrow_exception(failures[index]));
                }
                EXPECT_EQ(bodies[index], "ok");
            }
        }
        EXPECT_EQ(served, expected_connections);
        if (served == expected_connections && peer_failure) {
            std::rethrow_exception(peer_failure);
        }
    };
    std::exception_ptr failure;
    std::jthread runner([&] {
        try {
            ruvia::test::run_worker_tasks(attachment, run());
        } catch (...) {
            failure = std::current_exception();
        }
    });
    const bool responsive = requests_started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release.set_value();
    runner.join();
    blocking.join();
    EXPECT_TRUE(responsive);
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

TEST(Ruvia, concurrent_initial_tls_requests_share_one_blocking_admission) {
    exercise_concurrent_tls_preparation(std::nullopt);
}

TEST(Ruvia, initial_tls_cancellation_preserves_the_other_preparing_request) {
    for (const std::size_t cancelled : {0U, 1U}) {
        exercise_concurrent_tls_preparation(cancelled);
    }
}

TEST(Ruvia, concurrent_initial_tls_requests_receive_the_credential_failure) {
    exercise_concurrent_tls_preparation(std::nullopt, true);
}

TEST(Ruvia, tls_admission_rejection_leaves_credentials_available_for_preparation) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::tls_identity identity("localhost");
    ruvia::blocking_pool blocking({.thread_count_ = 1, .queue_capacity_ = 1});
    std::promise<void> entered, release;
    const auto gate = release.get_future().share();
    ASSERT_EQ(blocking.post([gate, &entered] {
        entered.set_value();
        gate.wait();
    }),
        ruvia::post_status::accepted);
    entered.get_future().wait();
    auto attachment = ruvia::event_loop_attachment(io, {.blocking_pool_ = &blocking});
    const auto loop_for_worker = attachment.loop();
    const auto worker = loop_for_worker.worker();
    auto config = local_http_client_config("https://127.0.0.1:443");
    config.tls_.ca_file_ = identity.ca_file_.parent_path() / "absent-after-rejection-ca.pem";
    ruvia::http_client client(attachment.loop(), config);
    const auto run = [&]() -> ruvia::task<void> {
        auto [completion, receiver] = ruvia::make_one_shot<bool>(worker);
        EXPECT_EQ(blocking.post([completion = std::move(completion)] {
            (void)completion.complete(true);
        }),
            ruvia::post_status::accepted);
        std::exception_ptr unexpected;
        try {
            (void)co_await client.ref().get("/").send();
            ADD_FAILURE() << "TLS preparation bypassed the full blocking queue";
        } catch (const ruvia::blocking_operation_rejected& error) {
            EXPECT_EQ(error.status(), ruvia::blocking_status::queue_full);
        } catch (...) {
            unexpected = std::current_exception();
        }
        release.set_value();
        const auto drained = co_await receiver.wait({.timeout_ = std::chrono::seconds(2)});
        EXPECT_TRUE(drained.has_value());
        // A rejected attempt did not read the file. A later admitted attempt
        // must now report the missing CA, not replay the old queue rejection.
        try {
            (void)co_await client.ref().get("/").send();
            ADD_FAILURE() << "TLS preparation ignored a missing CA file";
        } catch (const std::system_error&) {
        } catch (...) {
            if (!unexpected) {
                unexpected = std::current_exception();
            }
        }
        co_await client.shutdown();
        if (unexpected) {
            std::rethrow_exception(unexpected);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    blocking.join();
}

namespace {
enum class tls_startup_cancel { caller,
    timeout,
    shutdown };

void exercise_initial_tls_cancellation(bool raw_tcp, tls_startup_cancel mode) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::tls_identity identity("localhost");
    ruvia::blocking_pool blocking({.thread_count_ = 1, .queue_capacity_ = 4});
    std::promise<void> release;
    const auto gate = release.get_future().share();
    ASSERT_EQ(blocking.post([gate] {
        gate.wait();
    }),
        ruvia::post_status::accepted);
    auto attachment = ruvia::event_loop_attachment(io, {.blocking_pool_ = &blocking});
    const auto loop_for_worker = attachment.loop();
    const auto worker = loop_for_worker.worker();
    auto run = [&]() -> ruvia::task<void> {
        std::unique_ptr<ruvia::tcp_client> tcp;
        std::unique_ptr<ruvia::http_client> http;
        if (raw_tcp) {
            ruvia::tcp_client_config config{.host_ = "127.0.0.1", .port_ = 443};
            config.tls_.ca_file_ = identity.ca_file_;
            tcp = std::make_unique<ruvia::tcp_client>(attachment.loop(), config);
        } else {
            auto config = local_http_client_config("https://127.0.0.1:443");
            config.tls_.ca_file_ = identity.ca_file_;
            http = std::make_unique<ruvia::http_client>(attachment.loop(), config);
        }
        ruvia::stop_source stop;
        bool finished = false;
        bool retired = false;
        std::exception_ptr failure;
        const auto start = [&]() -> ruvia::task<void> {
            try {
                ruvia::operation_options options{.timeout_ = mode == tls_startup_cancel::timeout ? std::chrono::milliseconds(5) : std::chrono::seconds(2), .stop_token_ = stop.token()};
                if (tcp) {
                    co_await tcp->connect(options);
                } else {
                    (void)co_await http->ref().with_options(options).get("/").send();
                }
                ADD_FAILURE() << "cancelled TLS startup succeeded";
            } catch (const ruvia::http_client_error& error) {
                const auto expected = mode == tls_startup_cancel::shutdown ? ruvia::make_error_code(ruvia::http_client_errc::closing) : std::make_error_code(mode == tls_startup_cancel::timeout ? std::errc::timed_out : std::errc::operation_canceled);
                EXPECT_EQ(error.code(), expected);
            } catch (const std::system_error& error) {
                EXPECT_EQ(error.code(), std::make_error_code(mode == tls_startup_cancel::timeout ? std::errc::timed_out : std::errc::operation_canceled));
            } catch (...) {
                failure = std::current_exception();
            }
            finished = true;
        };
        const auto shutdown = [&]() -> ruvia::task<void> {
            if (tcp) {
                co_await tcp->shutdown();
            } else {
                co_await http->shutdown();
            }
            retired = true;
        };
        ruvia::task_scope tasks(worker);
        tasks.spawn(start());
        if (mode == tls_startup_cancel::caller) {
            stop.request_stop();
        } else if (mode == tls_startup_cancel::shutdown) {
            tasks.spawn(shutdown());
        }
        (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20));
        EXPECT_FALSE(finished);
        EXPECT_FALSE(retired);
        release.set_value();
        co_await tasks.join();
        EXPECT_TRUE(finished);
        co_await shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    blocking.join();
}
}  // namespace

TEST(Ruvia, initial_tls_preparation_cancellation_joins_work_before_client_retirement) {
    for (const bool raw_tcp : {false, true}) {
        for (const auto mode : {tls_startup_cancel::caller, tls_startup_cancel::timeout, tls_startup_cancel::shutdown}) {
            exercise_initial_tls_cancellation(raw_tcp, mode);
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
        auto response = co_await client.ref().get("/renegotiate").send();
        const auto bytes_value = co_await response.collect_body();
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
                            (void)connection.submit_data(stream_id, response.body(), ruvia::http2_end_stream::end_stream, 65535);
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
            auto response = co_await client.ref().get("/close-notify").send();
            const auto bytes_value = co_await response.collect_body();
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
                auto response = co_await client.ref().get("/idle").send();
                const auto bytes_value = co_await response.collect_body();
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
            const auto stats = client.ref().stats();
            EXPECT_EQ(stats.failed_requests_, std::size_t{0});
            EXPECT_EQ(stats.idle_connections_with_pending_input_, std::size_t{1});
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        EXPECT_EQ(connections, 2U);
    }
}
