#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"

#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace http3_client_early_data_push_test {

using namespace std::chrono_literals;

class replay_safe_route final : public ruvia::middleware {
public:
    static constexpr bool ruvia_replay_safe = true;
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

class push_routes final : public ruvia::controller<push_routes> {
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/parent", parent, replay_safe_route);
    RUVIA_GET("/asset", asset);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> parent(ruvia::context& context) {
        const bool pushed = co_await context.push(
            {.scheme_ = "https", .authority_ = context.req().authority(), .path_ = "/asset"});
        const bool early = context.early_data_info().received_from_early_data();
        co_return context.text(std::string_view(early ? (pushed ? "early:push" : "early:blocked")
                                                      : (pushed ? "full:push" : "full:blocked")));
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& context) {
        context.header("x-asset", "owned");
        co_return context.text("asset");
    }
};

struct startup_signal final {
    std::atomic<std::promise<void>*> ready_{};
    std::atomic<bool> published_{};
};

// Each run keeps the same listener and origin, but application startup creates
// fresh ticket keys. A client retaining the first run's ticket must reconnect
// and replay any offered early streams before the second run can serve them.
class server_run final {
public:
    explicit server_run(std::shared_ptr<startup_signal> signal)
        : signal_(std::move(signal)) {
        auto ready = ready_.get_future();
        signal_->published_.store(false);
        signal_->ready_.store(&ready_);
        thread_ = std::thread([this] {
            try {
                ruvia::app().run();
            } catch (...) {
                failure_ = std::current_exception();
                if (!signal_->published_.load()) {
                    ready_.set_exception(failure_);
                }
            }
        });
        try {
            if (ready.wait_for(5s) != std::future_status::ready) {
                throw std::runtime_error("HTTP/3 push server startup timed out");
            }
            ready.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    ~server_run() {
        stop();
    }

    void stop() {
        if (thread_.joinable()) {
            ruvia::app().stop();
            thread_.join();
            signal_->ready_.store(nullptr);
        }
    }

    void rethrow_if_failed() const {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    std::shared_ptr<startup_signal> signal_;
    std::promise<void> ready_;
    std::exception_ptr failure_;
    std::thread thread_;
};

class loop_run final {
public:
    explicit loop_run(ruvia::event_loop_attachment& attachment)
        : attachment_(attachment),
          thread_([this] { attachment_.run(); }) {}
    ~loop_run() {
        attachment_.stop();
        thread_.join();
    }

private:
    ruvia::event_loop_attachment& attachment_;
    std::thread thread_;
};

ruvia::task<std::string> fetch_push(ruvia::http_client& client, const ruvia::worker_handle& worker) {
    auto parent = co_await client.send({.target_ = "/parent", .replay_safe_ = true});
    auto body = co_await parent.body().read_all(32);
    const std::string parent_body(reinterpret_cast<const char*>(body.bytes().data()), body.size());
    if (parent_body != "full:push") {
        co_return parent_body;
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto push = client.next_push();
    while (!push && std::chrono::steady_clock::now() < deadline) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
        push = client.next_push();
    }
    if (!push || push->request().path_ != "/asset") {
        throw std::runtime_error("HTTP/3 parent completed without its promised push");
    }
    auto response = co_await push->response();
    auto asset = co_await response.body().read_all(32);
    if (response.header("x-asset") != "owned" ||
        std::string_view(reinterpret_cast<const char*>(asset.bytes().data()), asset.size()) != "asset") {
        throw std::runtime_error("HTTP/3 push lost its response metadata or body");
    }
    // Leave the ready session alive long enough to retain its post-handshake
    // ticket, independently of the response and push result lifetimes.
    (void)co_await ruvia::sleep_for(worker, 100ms);
    co_return parent_body;
}

ruvia::task<void> retire_connection(ruvia::http_client& client) {
    asio::io_context reservation_io;
    asio::ip::udp::socket reservation(reservation_io, {asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    // Cancelling path validation closes that QUIC connection. The client and
    // its ticket store stay alive; the next request must open a fresh session
    // even if the server's final UDP CONNECTION_CLOSE was lost.
    const auto migration = client.start_quic_path_migration(
        {.bytes_ = {std::byte{127}, std::byte{0}, std::byte{0}, std::byte{1}}, .port_ = port});
    if (migration.status_ != ruvia::quic_migration_status::started ||
        client.cancel_quic_path_migration(migration.id_) != ruvia::quic_operation_status::accepted) {
        throw std::runtime_error("HTTP/3 client connection could not retire before server restart");
    }
    co_return;
}

}  // namespace http3_client_early_data_push_test

using namespace http3_client_early_data_push_test;  // NOLINT(google-build-using-namespace)

RUVIA_TEST(http3_client_push_permission_survives_reconnecting_after_server_restart) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(output.get(),
                           SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write HTTP/3 push server private key");
        }
    }
    asio::io_context reservation_io;
    asio::ip::tcp::acceptor tcp(reservation_io, {asio::ip::address_v4::loopback(), 0});
    const auto port = tcp.local_endpoint().port();
    asio::ip::udp::socket udp(reservation_io, {asio::ip::address_v4::loopback(), port});
    tcp.close();
    udp.close();
    const auto signal = std::make_shared<startup_signal>();
    ruvia::app()
        .server({.worker_count_ = 1, .worker_queue_capacity_ = 32, .max_connections_per_worker_ = 8})
        .get_blocking_pool(nullptr)
        .on_start([signal] {
            signal->published_.store(true);
            signal->ready_.load()->set_value();
        })
        .listen({.address_ = "127.0.0.1",
            .https_ = port,
            .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key, .http3_early_data_ = true},
            .http3_ = {.mode_ = ruvia::http3_mode::enabled}});
    server_run first(signal);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().handle();
    ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                     .host_ = "127.0.0.1",
                                                     .port_ = port,
                                                     .connect_timeout_ = 5s,
                                                     .request_timeout_ = 10s,
                                                     .protocol_ = ruvia::http_client_protocol::http3_only,
                                                     .http3_early_data_ = true,
                                                     .push_ = {.enabled_ = true},
                                                     .tls_ = {.ca_file_ = identity.ca_file_.string(), .server_name_ = "localhost"}});
    loop_run loop(attachment);
    std::exception_ptr failure;
    const char* phase = "initial request";
    try {
        auto initial = attachment.loop().start(fetch_push(client, worker));
        RUVIA_CHECK_EQ(initial.get(), "full:push");
        phase = "retire initial client connection";
        auto retired = attachment.loop().start(retire_connection(client));
        retired.get();
        first.stop();
        first.rethrow_if_failed();
        phase = "server restart";
        server_run second(signal);
        phase = "request after server restart";
        auto reconnect = attachment.loop().start(fetch_push(client, worker));
        RUVIA_CHECK_EQ(reconnect.get(), "full:push");
        auto shutdown = attachment.loop().start(client.shutdown());
        shutdown.get();
        second.stop();
        second.rethrow_if_failed();
    } catch (const ruvia::http_client_error& error) {
        failure = std::make_exception_ptr(std::runtime_error(std::string(phase) + ": " + error.what() +
                                                             " (HTTP client error " + std::to_string(static_cast<unsigned>(error.code())) + ")"));
    } catch (...) {
        failure = std::current_exception();
    }
    auto shutdown = attachment.loop().start(client.shutdown());
    shutdown.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
