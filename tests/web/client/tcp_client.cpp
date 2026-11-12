#include "ruvia/web/tcp_client.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"

#include "test_tls_identity.h"

namespace ruvia::test::raw_tcp {

// A real independent TCP/TLS peer. A stalled peer consumes no bytes, making
// write deadlines exercise kernel backpressure rather than a fake completion.
class byte_peer final {
public:
    explicit byte_peer(std::size_t bytes = 0, asio::ssl::context* tls = nullptr, bool stall_handshake = false)
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          timer_(io_),
          deadline_(io_),
          work_(asio::make_work_guard(io_)),
          bytes_(bytes),
          tls_(tls),
          stall_handshake_(stall_handshake),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~byte_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            timer_.cancel();
            deadline_.cancel();
            work_.reset();
        });
        thread_.join();
        finish();
    }

    [[nodiscard]] tcp_client_config config() const {
        return {.host_ = "127.0.0.1", .port_ = acceptor_.local_endpoint().port(), .tls_ = {.mode_ = client_tls_mode::disabled}, .connect_timeout_ = std::chrono::seconds(2), .read_timeout_ = std::chrono::seconds(2), .write_timeout_ = std::chrono::seconds(2)};
    }

    [[nodiscard]] std::size_t received_bytes() const noexcept {
        return received_bytes_.load();
    }

    [[nodiscard]] int tls_version() const noexcept {
        return tls_version_.load();
    }

    void finish() {
        if (done_.valid()) {
            try {
                done_.get();
            } catch (const std::system_error&) {
                // Peer failure is expected for cancellation and rejected TLS.
            }
        }
    }

private:
    template <typename stream_type>
    asio::awaitable<void> exchange(stream_type& stream) {
        if (bytes_ == 0) {
            timer_.expires_at((asio::steady_timer::time_point::max)());
            co_await timer_.async_wait(asio::use_awaitable);
            co_return;
        }
        std::string bytes(bytes_, '\0');
        std::error_code error;
        received_bytes_ = co_await asio::async_read(stream, asio::buffer(bytes),
            asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            throw std::system_error(error);
        }
        co_await asio::async_write(stream, asio::buffer(bytes), asio::use_awaitable);
    }

    asio::awaitable<void> serve() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::error_code ignored;
                acceptor_.close(ignored);
                socket_.close(ignored);
                timer_.cancel();
            }
        });
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        socket_.set_option(asio::socket_base::receive_buffer_size(4096));
        if (stall_handshake_) {
            co_await exchange(socket_);
        } else if (tls_) {
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, *tls_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            tls_version_ = SSL_version(stream.native_handle());
            co_await exchange(stream);
            co_await stream.async_shutdown(asio::use_awaitable);
        } else {
            co_await exchange(socket_);
            socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer timer_;
    asio::steady_timer deadline_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::size_t bytes_;
    asio::ssl::context* tls_;
    bool stall_handshake_;
    std::atomic<std::size_t> received_bytes_{};
    std::atomic<int> tls_version_{};
    std::future<void> done_;
    std::thread thread_;
};

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        return process_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations_;
        process_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

[[nodiscard]] task<void> read_all(tcp_client& client, std::string& received) {
    while (const auto bytes = co_await client.read()) {
        received.append(as_chars(*bytes));
    }
}

[[nodiscard]] task<void> byte_exchange(event_loop loop, tcp_client_config config) {
    tcp_client client(loop, config);
    co_await client.connect();
    // Read and write are intentionally concurrent; the peer waits for both
    // owned payloads before replying, so a serialized transport would deadlock.
    task_scope tasks(client.worker());
    std::string received;
    tasks.spawn(read_all(client, received));
    auto copied = [&] {
        const std::string temporary(5000, 'a');
        return client.write(std::string_view(temporary));
    }();
    auto moved = [&] {
        counting_resource resource;
        std::pmr::string owned(5000, 'b', &resource);
        auto cold = client.write(std::move(owned));
        EXPECT_EQ(resource.allocations_, 1U);
        EXPECT_EQ(resource.deallocations_, 1U);
        return cold;
    }();
    // Both the custom PMR allocator and the original source have died before
    // either cold write starts; their payloads must now belong to the client.
    co_await std::move(copied);
    co_await std::move(moved);
    co_await tasks.join();
    EXPECT_EQ(received, std::string(5000, 'a') + std::string(5000, 'b'));
    EXPECT_FALSE((co_await client.read()).has_value());
    co_await client.shutdown();
}

[[nodiscard]] task<void> alpn_exchange(event_loop loop, tcp_client_config config, std::string expected) {
    tcp_client client(loop, config);
    config.alpn_.clear();
    config.alpn_.emplace_back("not-offered");
    co_await client.connect();
    EXPECT_EQ(client.alpn(), expected);
    co_await client.write("ALPN");
    std::string received;
    co_await read_all(client, received);
    EXPECT_EQ(received, "ALPN");
    co_await client.shutdown();
}

[[nodiscard]] task<void> expect_io_failure(tcp_client& client, bool writing, operation_options options,
    std::errc expected, std::promise<void>* started = nullptr) {
    if (started) {
        // Nested tasks enter the transport without yielding. Publish readiness
        // on the next worker turn, after cancellation is armed and socket I/O
        // has started, rather than announcing a still-cold operation.
        if (!client.worker().post([started] { started->set_value(); }).accepted()) {
            throw std::runtime_error("TCP operation readiness was rejected");
        }
    }
    bool failed = false;
    try {
        if (writing) {
            co_await client.write(std::pmr::string(16 * 1024 * 1024, 'x', process_resource()), options);
        } else {
            (void)co_await client.read(options);
        }
    } catch (const std::system_error& error) {
        EXPECT_EQ(error.code(), std::make_error_code(expected));
        failed = true;
    }
    EXPECT_TRUE(failed);
}

[[nodiscard]] task<void> deadline_case(event_loop loop, tcp_client_config config, bool writing) {
    if (writing) {
        config.write_timeout_ = std::chrono::milliseconds(30);
    } else {
        config.read_timeout_ = std::chrono::milliseconds(30);
    }
    tcp_client client(loop, config);
    co_await client.connect();
    co_await expect_io_failure(client, writing, {.timeout_ = std::chrono::seconds(5)}, std::errc::timed_out);
    co_await client.shutdown();
}

[[nodiscard]] task<void> cancellation_case(tcp_client& client, stop_token token, bool writing, bool connecting) {
    if (connecting) {
        bool cancelled = false;
        try {
            co_await client.connect({.stop_token_ = token});
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(std::errc::operation_canceled));
            cancelled = true;
        }
        EXPECT_TRUE(cancelled);
    } else {
        co_await client.connect();
        co_await expect_io_failure(client, writing, {.stop_token_ = token}, std::errc::operation_canceled);
    }
    co_await client.shutdown();
}

[[nodiscard]] task<void> overlap_case(event_loop loop, tcp_client_config config, bool writing) {
    tcp_client client(loop, config);
    co_await client.connect();
    task_scope scope(client.worker());
    scope.spawn(expect_io_failure(client, writing, {}, std::errc::operation_canceled));
    // Yield after spawn so the first operation owns its direction.
    (void)co_await sleep_for(client.worker(), std::chrono::milliseconds(10));
    bool rejected = false;
    try {
        if (writing) {
            co_await client.write("overlap");
        } else {
            (void)co_await client.read();
        }
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    client.close();
    co_await scope.join();
    co_await client.shutdown();
}

[[nodiscard]] task<void> retired_cold_case(event_loop loop, tcp_client_config config) {
    auto client = std::make_unique<tcp_client>(loop, config);
    co_await client->connect();
    counting_resource resource;
    {
        auto cold_write = client->write(std::pmr::string(5000, 'x', &resource));
        auto cold_read = client->read();
        auto shutdown = client->shutdown();
        client.reset();
        co_await std::move(shutdown);
        EXPECT_EQ(resource.allocations_, resource.deallocations_);
        // Expired tasks are destroyed cold, with no borrowed backend remaining.
    }
}

}  // namespace ruvia::test::raw_tcp

using namespace ruvia::test::raw_tcp;

TEST(Ruvia, tcp_client_exchanges_owned_bytes_with_concurrent_read_and_eof) {
    byte_peer peer(10000);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(byte_exchange(pool.loop(0), peer.config())).get();
    pool.join();
}

TEST(Ruvia, tcp_client_tls_and_mutual_tls_authenticate_san_and_exchange_bytes) {
    for (const bool mutual : {false, true}) {
        ruvia::test::tls_identity server("server.example");
        ruvia::test::tls_identity identity("client.example");
        if (mutual) {
            server.context_.load_verify_file(identity.ca_file_.string());
            server.context_.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
        }
        byte_peer peer(10000, &server.context_);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        if (mutual) {
            config.tls_.identity_.emplace();
            config.tls_.identity_->certificate_chain_file_ = identity.ca_file_;
            config.tls_.identity_->private_key_file_ = identity.private_key_file();
        }
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_tls11_exchanges_bytes_and_still_authenticates_san) {
    for (const bool matching_san : {true, false}) {
        ruvia::test::tls_identity server("server.example",
            matching_san ? "DNS:server.example" : "DNS:other.example");
        auto* context = server.context_.native_handle();
        ASSERT_EQ(SSL_CTX_set_min_proto_version(context, TLS1_1_VERSION), 1);
        ASSERT_EQ(SSL_CTX_set_max_proto_version(context, TLS1_1_VERSION), 1);
        ASSERT_EQ(SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES128-SHA"), 1);
        // Independent legacy peer only: production clients retain their shared
        // security callback and certificate/key-strength checks.
        SSL_CTX_set_security_level(context, 0);
        byte_peer peer(10000, &server.context_);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        if (matching_san) {
            pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
            EXPECT_EQ(peer.tls_version(), TLS1_1_VERSION);
            EXPECT_EQ(peer.received_bytes(), 10000U);
        } else {
            ruvia::tcp_client client(pool.loop(0), config);
            EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
            pool.loop(0).start(client.shutdown()).get();
        }
        pool.join();
    }
}

TEST(Ruvia, tcp_client_rejects_tls10_and_h2_below_tls12_before_application_bytes) {
    for (const int version : {TLS1_VERSION, TLS1_1_VERSION}) {
        for (const bool h2_only : {false, true}) {
            ruvia::test::tls_identity server("server.example");
            auto* context = server.context_.native_handle();
            ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
            ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
            ASSERT_EQ(SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES128-SHA"), 1);
            SSL_CTX_set_security_level(context, 0);
            SSL_CTX_set_alpn_select_cb(context, [](SSL*, const unsigned char** selected, unsigned char* size, const unsigned char*, unsigned int, void*) -> int {
                    static constexpr unsigned char h2[] = {'h', '2'};
                    *selected = h2;
                    *size = sizeof(h2);
                    return SSL_TLSEXT_ERR_OK; }, nullptr);
            byte_peer peer(4, &server.context_);
            auto config = peer.config();
            config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
            config.tls_.ca_file_ = server.ca_file_;
            config.tls_.server_name_.emplace("server.example");
            config.alpn_.emplace_back("h2");
            if (!h2_only) {
                config.alpn_.emplace_back("http/1.1");
            }
            ruvia::event_loop_pool pool({.loop_count_ = 1});
            ruvia::tcp_client client(pool.loop(0), config);
            pool.start();
            EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
            pool.loop(0).start(client.shutdown()).get();
            pool.join();
            peer.finish();
            if (version == TLS1_1_VERSION && !h2_only) {
                EXPECT_EQ(peer.tls_version(), TLS1_1_VERSION);
            }
            EXPECT_EQ(peer.received_bytes(), 0U);
        }
    }
}

TEST(Ruvia, tcp_client_tls_rejects_wrong_san_even_when_common_name_matches) {
    ruvia::test::tls_identity server("wanted.example", "DNS:other.example");
    byte_peer peer(0, &server.context_);
    auto config = peer.config();
    config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
    config.tls_.ca_file_ = server.ca_file_;
    config.tls_.server_name_.emplace("wanted.example");
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::tcp_client client(pool.loop(0), config);
    pool.start();
    EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
}

TEST(Ruvia, tcp_client_owns_opaque_alpn_offers_and_reports_server_selection) {
    for (const auto& protocol : {std::string("embed.v1"), std::string(255, 'x'), std::string("bi\0n", 4)}) {
        std::vector<unsigned char> server_offer{static_cast<unsigned char>(protocol.size())};
        server_offer.insert(server_offer.end(), protocol.begin(), protocol.end());
        ruvia::test::tls_identity server("server.example");
        SSL_CTX_set_alpn_select_cb(server.context_.native_handle(), [](SSL*, const unsigned char** output, unsigned char* output_size, const unsigned char* offered, unsigned int offered_size, void* argument) -> int {
                const auto& supported = *static_cast<const std::vector<unsigned char>*>(argument);
                unsigned char* selected = nullptr;
                if (SSL_select_next_proto(&selected, output_size, supported.data(),
                        static_cast<unsigned int>(supported.size()), offered, offered_size) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                *output = selected;
                return SSL_TLSEXT_ERR_OK; }, &server_offer);
        byte_peer peer(4, &server.context_);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        config.alpn_.emplace_back("h2");
        config.alpn_.emplace_back(protocol);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(alpn_exchange(pool.loop(0), config, protocol)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_rejects_invalid_alpn_names_size_and_plaintext_offers) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::tcp_client_config config{.host_ = "127.0.0.1", .port_ = 443};
    config.alpn_.emplace_back();
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    config.alpn_.front().assign(256, 'x');
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    config.alpn_.clear();
    for (int index = 0; index < 256; ++index) {
        config.alpn_.emplace_back(255, 'x');
    }
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    config.alpn_.clear();
    config.alpn_.emplace_back("embed.v1");
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
}

TEST(Ruvia, tcp_client_bounds_read_and_backpressured_write_independently) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(deadline_case(pool.loop(0), peer.config(), writing)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_rejects_overlapping_reads_and_writes_without_aborting_first_operation) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(overlap_case(pool.loop(0), peer.config(), writing)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_honors_precancellation_for_connect_read_and_write) {
    for (const int direction : {0, 1, 2}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), peer.config());
        ruvia::stop_source stop;
        stop.request_stop();
        pool.start();
        pool.loop(0).start(cancellation_case(client, stop.token(), direction == 2, direction == 0)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_cancels_inflight_reads_and_writes_from_another_thread) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), peer.config());
        ruvia::stop_source stop;
        pool.start();
        pool.loop(0).start(client.connect()).get();
        std::promise<void> started;
        std::promise<void> other_started;
        auto operation = pool.loop(0).start(expect_io_failure(client, writing, {.stop_token_ = stop.token()}, std::errc::operation_canceled, &started));
        auto other = pool.loop(0).start(expect_io_failure(client, !writing, {}, std::errc::operation_canceled, &other_started));
        started.get_future().get();
        other_started.get_future().get();
        // Only one direction owns this token. Its cancellation must also end
        // the opposite in-flight operation when the connection is retired.
        stop.request_stop();
        operation.get();
        other.get();
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_shutdown_retires_cold_buffers_after_owner_destruction) {
    byte_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(retired_cold_case(pool.loop(0), peer.config())).get();
    pool.join();
}

TEST(Ruvia, tcp_client_loop_stop_joins_pending_reads_and_writes) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), peer.config());
        pool.start();
        pool.loop(0).start(client.connect()).get();
        std::promise<void> started;
        auto operation = pool.loop(0).start(expect_io_failure(client, writing, {}, std::errc::operation_canceled, &started));
        started.get_future().get();
        pool.stop();
        operation.get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_tls_handshake_has_deadline_and_inflight_cancellation) {
    for (const bool cancelling : {false, true}) {
        ruvia::test::tls_identity server("server.example");
        byte_peer peer(0, nullptr, true);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        config.connect_timeout_ = std::chrono::milliseconds(cancelling ? 2000 : 30);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), config);
        ruvia::stop_source stop;
        pool.start();
        auto operation = pool.loop(0).start(client.connect({.stop_token_ = stop.token()}));
        if (cancelling) {
            const auto barrier = [&]() -> ruvia::task<void> {
                (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(10));
            };
            pool.loop(0).start(barrier()).get();
            stop.request_stop();
        }
        bool failed = false;
        try {
            operation.get();
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(cancelling ? std::errc::operation_canceled : std::errc::timed_out));
            failed = true;
        }
        EXPECT_TRUE(failed);
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_resolves_dns_through_its_event_loop_blocking_pool) {
    byte_peer peer(10000);
    auto config = peer.config();
    config.host_ = "localhost";
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    ruvia::event_loop_pool pool({.loop_count_ = 1, .blocking_pool_ = &lookups});
    pool.start();
    pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
    pool.join();
}

TEST(Ruvia, tcp_client_checks_ports_before_narrowing_and_rejects_zero) {
    EXPECT_THROW(static_cast<void>(ruvia::tcp_client_config{.host_ = "127.0.0.1", .port_ = 70000}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ruvia::tcp_client_config{.host_ = "127.0.0.1", .port_ = -1}), std::invalid_argument);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const ruvia::tcp_client_config config{.host_ = "127.0.0.1", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    pool.join();
}

TEST(Ruvia, tcp_client_cold_connect_remains_safe_after_public_owner_destruction) {
    byte_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto client = std::make_unique<ruvia::tcp_client>(pool.loop(0), peer.config());
    auto cold = client->connect();
    client.reset();
    pool.start();
    EXPECT_THROW(pool.loop(0).start(std::move(cold)).get(), std::runtime_error);
    pool.join();
}
