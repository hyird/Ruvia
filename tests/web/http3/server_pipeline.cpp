#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/streaming.h"

#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

// HTTP/3 requests run the same request admission as HTTP/1.1 and HTTP/2: an
// application rate limit answers 429 with its rate-limit headers, and a
// response stream whose client accepts no content coding answers 406. A
// length-declared upload is read through the public body reader. This executable
// owns its application, so the app-wide rate limit cannot leak into
// other suites.

namespace {

using namespace std::chrono_literals;

constexpr std::size_t admitted_requests = 4;

[[maybe_unused]] const bool rate_limit_configured = [] {
    ruvia::app().get_rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = admitted_requests, .window_ = std::chrono::minutes(10)}});
    return true;
}();

class admission_routes final : public ruvia::controller<admission_routes> {
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/plain", plain);
    RUVIA_GET_STREAM("/events", events);
    RUVIA_POST_STREAM("/upload", upload);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain");
    }

    ruvia::task<void> events(ruvia::context& context_value) {
        co_await context_value.stream().write("event");
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& context_value) {
        std::string body;
        auto& reader = context_value.req().get_body_reader();
        while (const auto chunk = co_await reader.text()) {
            body.append(*chunk);
        }
        co_return context_value.text(std::string_view(body));
    }
};

// A self-signed localhost identity with its private key written beside it.
class identity_files final {
public:
    identity_files()
        : identity_("localhost"),
          private_key_(identity_.ca_file_.parent_path() / "key.pem") {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key_.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(output.get(),
                           SSL_CTX_get0_privatekey(identity_.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write HTTP/3 test private key");
        }
    }

    [[nodiscard]] const std::filesystem::path& certificate() const noexcept {
        return identity_.ca_file_;
    }
    [[nodiscard]] const std::filesystem::path& private_key() const noexcept {
        return private_key_;
    }

private:
    ruvia::test::tls_identity identity_;
    std::filesystem::path private_key_;
};

// Runs the process application with one HTTP/3 listener until destroyed.
class http3_server final {
public:
    explicit http3_server(const identity_files& identity) {
        auto& app = ruvia::app();
        app.server({.worker_count_ = 1, .worker_queue_capacity_ = 32, .max_connections_per_worker_ = 8})
            .on_start([this] {
                started_.store(true);
                ready_.set_value();
            });
        asio::io_context io;
        asio::ip::tcp::acceptor tcp(io, {asio::ip::address_v4::loopback(), 0});
        port_ = tcp.local_endpoint().port();
        asio::ip::udp::socket udp(io, {asio::ip::address_v4::loopback(), port_});
        tcp.close();
        udp.close();
        app.listen({.address_ = "127.0.0.1",
            .https_ = port_,
            .tls_ = {.certificate_chain_file_ = identity.certificate(), .private_key_file_ = identity.private_key()},
            .http3_ = {.mode_ = ruvia::http3_mode::enabled}});
        auto ready = ready_.get_future();
        thread_ = std::thread([this, &app] {
            try {
                app.run();
            } catch (...) {
                // After startup a run failure surfaces as the client's error.
                if (!started_.load()) {
                    ready_.set_exception(std::current_exception());
                }
            }
        });
        try {
            if (ready.wait_for(5s) != std::future_status::ready) {
                throw std::runtime_error("HTTP/3 server startup timed out");
            }
            ready.get();
        } catch (...) {
            app.stop();
            thread_.join();
            throw;
        }
    }

    http3_server(const http3_server&) = delete;
    http3_server& operator=(const http3_server&) = delete;

    ~http3_server() {
        ruvia::app().stop();
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

private:
    std::promise<void> ready_;
    std::atomic<bool> started_{};
    std::thread thread_;
    std::uint16_t port_{};
};

ruvia::task<std::string> collect_text(ruvia::http_client_response& response) {
    const auto bytes_value = co_await response.body().read_all();
    const auto view = bytes_value.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}

}  // namespace

RUVIA_TEST(http3_answers_406_and_rate_limit_through_the_shared_admission) {
    identity_files identity;
    http3_server server(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = server.port(),
                                                         .connect_timeout_ = 10s,
                                                         .request_timeout_ = 12s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_ = {.ca_file_ = identity.certificate().string()}});
        const std::array<ruvia::http_header_view, 1> refuse_every_coding{{{"Accept-Encoding", "identity;q=0, *;q=0"}}};
        std::exception_ptr failure;
        try {
            // A response stream commits its representation before a status can
            // be inspected, so a client that accepts no coding gets 406.
            {
                auto response = co_await client.send({.target_ = "/events", .headers_ = refuse_every_coding});
                RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
                RUVIA_CHECK(response.status() == ruvia::http_status::not_acceptable);
                (void)co_await collect_text(response);
            }
            {
                constexpr std::string_view payload = "length-declared streaming upload";
                auto response = co_await client.send({.method_ = "POST", .target_ = "/upload", .content_ = ruvia::http_client_request_content_view::bytes(payload)});
                RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
                RUVIA_CHECK(response.status() == ruvia::http_status::ok);
                RUVIA_CHECK((co_await collect_text(response)) == payload);
            }
            // The 406 and streamed upload each drew on the client's budget.
            for (std::size_t index = 2; index < admitted_requests; ++index) {
                auto response = co_await client.send({.target_ = "/plain"});
                RUVIA_CHECK(response.status() == ruvia::http_status::ok);
                RUVIA_CHECK((co_await collect_text(response)) == "plain");
            }
            auto response = co_await client.send({.target_ = "/plain"});
            RUVIA_CHECK(response.status() == ruvia::http_status::too_many_requests);
            RUVIA_CHECK(response.header("Retry-After").has_value());
            (void)co_await collect_text(response);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}
