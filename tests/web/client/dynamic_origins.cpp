#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"

namespace ruvia::test::dynamic_origin_tests {

struct observed_configuration final {
    worker_signal& retired_;
    bool& destroyed_;
    ~observed_configuration() {
        destroyed_ = true;
        retired_.notify();
    }
};
// One real persistent connection. An empty reply deliberately leaves a request
// pending until its client closes the transport.
class peer final {
public:
    peer(asio::io_context& io, const worker_ref& worker, std::span<const std::string_view> replies)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          replies_(replies),
          received_(worker),
          finished_(worker) {
        asio::co_spawn(io, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            received_.notify();
            finished_.notify();
        });
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
    }

    task<void> received() {
        while (requests_.empty() && !complete_) {
            co_await received_.wait();
        }
        if (requests_.empty()) {
            throw std::runtime_error("HTTP peer ended before receiving a request");
        }
    }

    task<void> join() {
        while (!complete_) {
            co_await finished_.wait();
        }
        if (failure_ && !aborted_) {
            std::rethrow_exception(failure_);
        }
    }

    void abort() noexcept {
        aborted_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }

    std::vector<std::string> requests_{};
    bool client_closed_{};

private:
    asio::awaitable<void> serve() {
        socket_ = co_await acceptor_.async_accept(asio::use_awaitable);
        std::string input;
        for (const auto reply : replies_) {
            const auto count = co_await asio::async_read_until(socket_, asio::dynamic_buffer(input),
                "\r\n\r\n", asio::use_awaitable);
            requests_.emplace_back(input.substr(0, count));
            input.erase(0, count);
            received_.notify();
            if (!reply.empty()) {
                co_await asio::async_write(socket_, asio::buffer(reply), asio::use_awaitable);
            }
        }
        std::array<char, 1> trailing;
        std::error_code error;
        (void)co_await socket_.async_read_some(asio::buffer(trailing),
            asio::redirect_error(asio::use_awaitable, error));
        client_closed_ = error == asio::error::eof || error == asio::error::connection_reset;
        if (!client_closed_) {
            throw std::runtime_error("HTTP peer expected client transport retirement");
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::span<const std::string_view> replies_;
    worker_signal received_;
    worker_signal finished_;
    std::exception_ptr failure_;
    bool complete_{};
    bool aborted_{};
};

[[nodiscard]] task<void> expect_closed(task<http_client_response> operation) {
    try {
        (void)co_await std::move(operation);
        ADD_FAILURE() << "retired origin admitted an HTTP request";
    } catch (const http_client_error& error) {
        EXPECT_EQ(error.code(), make_error_code(http_client_errc::closing));
    }
}

}  // namespace ruvia::test::dynamic_origin_tests

TEST(Ruvia, http_client_dynamic_origins_own_inputs_and_atomically_replace_one_alias) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>{});
        std::exception_ptr failure;
        try {
            std::string alias = "default";
            std::string url = "http://127.0.0.1:8001";
            auto update = client.upsert({.alias_ = alias, .base_url_ = url});
            alias.assign("changed");
            url.assign("http://127.0.0.1:8999");
            co_await std::move(update);
            auto original = client.client();
            EXPECT_EQ(original.port(), 8001);
            auto cold_send = original.get("/cold").send();
            auto cold_upload = original.post("/cold-upload").upload();

            EXPECT_THROW((void)client.upsert({.base_url_ = "http://127.0.0.1:8002/path"}), std::invalid_argument);
            EXPECT_EQ(client.client().port(), 8001);
            EXPECT_THROW((void)client.upsert({.alias_ = "", .base_url_ = "http://127.0.0.1:8002"}), std::invalid_argument);
            EXPECT_THROW((void)client.remove(""), std::invalid_argument);
            EXPECT_FALSE(client.remove("missing"));

            co_await client.upsert({.alias_ = "unchanged", .base_url_ = "http://127.0.0.1:8003"});
            auto unchanged = client.client("unchanged");
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
            EXPECT_EQ(client.client().port(), 8002);
            EXPECT_EQ(original.port(), 8001);
            EXPECT_EQ(unchanged.port(), 8003);
            co_await ruvia::test::dynamic_origin_tests::expect_closed(std::move(cold_send));
            try {
                (void)co_await std::move(cold_upload);
                ADD_FAILURE() << "retired origin admitted a cold upload";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::closing));
            }

            auto removed = client.client();
            auto removed_send = removed.get("/removed").send();
            EXPECT_TRUE(client.remove("default"));
            EXPECT_FALSE(client.remove("default"));
            try {
                (void)client.client("default");
                ADD_FAILURE() << "removed alias remained visible";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::not_configured));
            }
            EXPECT_EQ(removed.port(), 8002);
            EXPECT_EQ(client.client().port(), 8003);
            co_await ruvia::test::dynamic_origin_tests::expect_closed(std::move(removed_send));
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8004"});
            EXPECT_EQ(client.client().port(), 8004);
            EXPECT_EQ(client.client("unchanged").port(), 8003);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        EXPECT_THROW((void)client.upsert({.base_url_ = "http://127.0.0.1:8005"}), ruvia::http_client_error);
        EXPECT_THROW((void)client.remove("default"), ruvia::http_client_error);
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_dynamic_origins_preserve_other_pool_connections_and_cookies) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto loop = attachment.loop();
    constexpr std::string_view reply =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nSet-Cookie: stable=1; Path=/\r\n\r\n";
    const std::array replies{reply, reply};
    ruvia::test::dynamic_origin_tests::peer peer(io, loop.worker(), replies);
    const auto url = peer.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.alias_ = "stable", .base_url_ = url, .request_timeout_ = std::chrono::seconds(2), .versions_ = ruvia::http_client_versions{.http2_ = false}, .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send});
        std::exception_ptr failure;
        try {
            auto stable = client.client("stable");
            auto first = co_await stable.get("/first").send();
            EXPECT_TRUE(first.stream().complete());
            const auto before = stable.stats().completed_requests_;
            co_await client.upsert({.alias_ = "other", .base_url_ = "http://127.0.0.1:8001"});
            co_await client.upsert({.alias_ = "other", .base_url_ = "http://127.0.0.1:8002"});
            EXPECT_TRUE(client.remove("other"));
            auto second = co_await client.client("stable").get("/second").send();
            EXPECT_TRUE(second.stream().complete());
            EXPECT_EQ(stable.stats().completed_requests_, before + 1);
            EXPECT_EQ(peer.requests_.size(), 2U);
            if (peer.requests_.size() == 2) {
                EXPECT_NE(peer.requests_[1].find("stable=1"), std::string_view::npos);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            peer.abort();
        }
        co_await peer.join();
        EXPECT_TRUE(peer.client_closed_);
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_dynamic_origin_replacement_cancels_and_joins_live_requests) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto loop = attachment.loop();
    constexpr std::array replies{std::string_view{}};
    ruvia::test::dynamic_origin_tests::peer peer(io, loop.worker(), replies);
    const auto url = peer.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.base_url_ = url, .request_timeout_ = std::chrono::seconds(2)});
        ruvia::task_scope requests(loop.worker());
        requests.spawn(ruvia::test::dynamic_origin_tests::expect_closed(client.client().get("/pending").send()));
        co_await peer.received();
        co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
        co_await requests.join();
        co_await peer.join();
        EXPECT_TRUE(peer.client_closed_);
        EXPECT_EQ(client.client().port(), 8002);
        co_await client.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_redirect_policy_can_register_an_origin_and_remains_fail_closed) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto loop = attachment.loop();
    constexpr std::array target_replies{std::string_view("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
    ruvia::test::dynamic_origin_tests::peer target(io, loop.worker(), target_replies);
    const auto target_url = target.url();
    const auto redirect = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: " + target_url + "/destination\r\n\r\n";
    const std::array source_replies{std::string_view(redirect), std::string_view(redirect), std::string_view(redirect), std::string_view(redirect)};
    ruvia::test::dynamic_origin_tests::peer source(io, loop.worker(), source_replies);
    const auto source_url = source.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.base_url_ = source_url,
                                            .request_timeout_ = std::chrono::seconds(2),
                                            .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            auto denied = co_await client.client().get("/without-policy").send();
            EXPECT_EQ(denied.status().value(), 302);
            EXPECT_TRUE(target.requests_.empty());

            auto unconfigured = client.client().get("/policy-without-origin");
            unconfigured.on_redirect([](const ruvia::http_client_redirect_hop&, ruvia::stop_token) -> ruvia::task<bool> { co_return true; });
            auto still_denied = co_await unconfigured.send();
            EXPECT_EQ(still_denied.status().value(), 302);
            EXPECT_TRUE(target.requests_.empty());

            std::size_t decisions = 0;
            auto request = client.client().get("/with-policy");
            request.header("Authorization", "Bearer private");
            request.header("Proxy-Authorization", "Basic private");
            request.header("Cookie", "source=private");
            request.on_redirect([&](const ruvia::http_client_redirect_hop& hop, ruvia::stop_token stop) -> ruvia::task<bool> {
                ++decisions;
                EXPECT_EQ(hop.target_origin().host(), "127.0.0.1");
                co_await client.upsert({.alias_ = "discovered", .base_url_ = target_url, .versions_ = ruvia::http_client_versions{.http2_ = false}},
                    {.stop_token_ = std::move(stop)});
                co_return true;
            });
            auto accepted = co_await request.send();
            EXPECT_EQ(accepted.status().value(), 200);
            EXPECT_EQ(decisions, 1U);
            EXPECT_EQ(target.requests_.size(), 1U);
            if (!target.requests_.empty()) {
                const auto& wire = target.requests_.front();
                EXPECT_TRUE(wire.starts_with("GET /destination HTTP/1.1\r\n"));
                EXPECT_EQ(wire.find("private"), std::string_view::npos);
            }

            auto rejected = client.client().get("/policy-denies-registered");
            rejected.on_redirect([&](const ruvia::http_client_redirect_hop&, ruvia::stop_token) -> ruvia::task<bool> {
                ++decisions;
                co_return false;
            });
            auto result = co_await rejected.send();
            EXPECT_EQ(result.status().value(), 302);
            EXPECT_EQ(decisions, 2U);
            EXPECT_EQ(target.requests_.size(), 1U);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            source.abort();
            target.abort();
        }
        co_await source.join();
        co_await target.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_retired_origin_handles_and_cold_operations_expire_with_owner) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_ref> retired;
        std::optional<ruvia::task<ruvia::http_client_response>> cold;
        std::optional<ruvia::task<ruvia::http_client_exchange>> upload;
        std::optional<ruvia::task<ruvia::http_client_tunnel_result>> tunnel;
        {
            ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
            retired.emplace(client.client());
            cold.emplace(retired->get("/cold").send());
            upload.emplace(client.client().post("/cold-upload").upload());
            tunnel.emplace(client.client().connect("127.0.0.1:8001").open());
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
            co_await client.shutdown();
        }
        EXPECT_THROW((void)retired->port(), std::logic_error);
        try {
            (void)co_await std::move(*cold);
            ADD_FAILURE() << "cold request survived its origin owner";
        } catch (const std::logic_error&) {
        }
        try {
            (void)co_await std::move(*upload);
            ADD_FAILURE() << "cold upload survived its origin owner";
        } catch (const std::logic_error&) {
        }
        try {
            (void)co_await std::move(*tunnel);
            ADD_FAILURE() << "cold tunnel survived its origin owner";
        } catch (const std::logic_error&) {
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_dynamic_origins_reject_non_worker_mutation) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
    EXPECT_THROW((void)client.upsert({.base_url_ = "http://127.0.0.1:8002"}), std::logic_error);
    EXPECT_THROW((void)client.remove("default"), std::logic_error);
    const auto run = [&]() -> ruvia::task<void> {
        EXPECT_EQ(client.client().port(), 8001);
        co_await client.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_cold_upload_and_tunnel_pin_replaced_origins_without_handles) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        std::exception_ptr failure;
        try {
            auto upload = client.client().post("/cold-upload").upload();
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
            try {
                (void)co_await std::move(upload);
                ADD_FAILURE() << "cold upload entered a replaced origin";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::closing));
            }
            auto tunnel = client.client().connect("127.0.0.1:8002").open();
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8003"});
            try {
                (void)co_await std::move(tunnel);
                ADD_FAILURE() << "cold tunnel entered a replaced origin";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::closing));
            }
            auto send = client.client().get("/cold-send").send();
            EXPECT_TRUE(client.remove("default"));
            co_await ruvia::test::dynamic_origin_tests::expect_closed(std::move(send));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_retired_origin_resources_release_after_last_cold_borrow) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>{});
        ruvia::worker_signal retired(worker);
        bool destroyed = false;
        bool expired = false;
        std::weak_ptr<ruvia::test::dynamic_origin_tests::observed_configuration> observed;
        {
            auto marker = std::make_shared<ruvia::test::dynamic_origin_tests::observed_configuration>(retired, destroyed);
            observed = marker;
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8001",
                .address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([marker](const ruvia::ip_address&, std::uint16_t) { return marker != nullptr; })});
        }
        std::optional<ruvia::http_client_ref> old(client.client());
        std::optional<ruvia::task<ruvia::http_client_exchange>> cold(old->post("/cold").upload());
        EXPECT_TRUE(client.remove("default"));
        EXPECT_FALSE(observed.expired());
        old.reset();
        EXPECT_FALSE(observed.expired());
        cold.reset();
        ruvia::worker_timer_registration deadline;
        (void)worker.schedule_timer(deadline, ruvia::deadline_after(std::chrono::seconds(2)),
            [&](ruvia::worker_timer_outcome outcome) noexcept {
                if (outcome == ruvia::worker_timer_outcome::expired) {
                    expired = true;
                    retired.notify();
                }
            });
        while (!destroyed && !expired) {
            co_await retired.wait();
        }
        deadline.cancel();
        EXPECT_TRUE(destroyed);
        EXPECT_TRUE(observed.expired());
        co_await client.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_tls_upsert_prepares_owned_paths_and_password_off_worker) {
    ruvia::test::tls_identity identity("localhost");
    const auto encrypted_key = identity.private_key_file().parent_path() / "encrypted.pem";
    const std::string password("secret\0part", 11);
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(encrypted_key.string().c_str(), "w"), BIO_free);
        ASSERT_NE(output, nullptr);
        ASSERT_EQ(PEM_write_bio_PrivateKey(output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle()),
                      EVP_aes_256_cbc(), reinterpret_cast<const unsigned char*>(password.data()),
                      static_cast<int>(password.size()), nullptr, nullptr),
            1);
    }
    auto& io = ruvia::test::new_test_io_context();
    ruvia::blocking_pool blocking({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &blocking});
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        std::exception_ptr failure;
        try {
            std::string alias = "secure";
            std::string url = "https://localhost:8443";
            ruvia::http_client_config config{.alias_ = alias, .base_url_ = url};
            config.tls_.ca_file_ = identity.ca_file_;
            config.tls_.identity_.emplace();
            config.tls_.identity_->certificate_chain_file_ = identity.ca_file_;
            config.tls_.identity_->private_key_file_ = encrypted_key;
            config.tls_.identity_->private_key_password_.assign(password);
            auto update = client.upsert(config);
            alias.assign("overwritten");
            url.assign("http://127.0.0.1:8999");
            config.tls_.ca_file_ = encrypted_key.parent_path() / "missing-ca.pem";
            config.tls_.identity_->private_key_file_ = encrypted_key.parent_path() / "missing-key.pem";
            config.tls_.identity_->private_key_password_.assign("incorrect");
            co_await std::move(update);
            auto secure = client.client("secure");
            EXPECT_EQ(secure.scheme(), ruvia::http_scheme::https);
            EXPECT_EQ(secure.port(), 8443);
            EXPECT_EQ(client.client().port(), 8001);

            config.alias_ = "secure";
            config.base_url_ = "https://localhost:9443";
            auto failed_update = client.upsert(config);
            bool failed = false;
            try {
                co_await std::move(failed_update);
            } catch (const std::exception&) {
                failed = true;
            }
            EXPECT_TRUE(failed);
            EXPECT_EQ(client.client("secure").port(), 8443);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    blocking.join();
}

TEST(Ruvia, http_client_tls_upsert_without_blocking_pool_fails_without_publication) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        try {
            co_await client.upsert({.base_url_ = "https://localhost:8443"});
            ADD_FAILURE() << "TLS update used a worker without a blocking pool";
        } catch (const ruvia::blocking_operation_rejected& error) {
            EXPECT_EQ(error.status(), ruvia::blocking_status::unavailable);
        }
        EXPECT_EQ(client.client().port(), 8001);
        auto cold = client.upsert({.base_url_ = "https://localhost:9443"});
        co_await client.shutdown();
        try {
            co_await std::move(cold);
            ADD_FAILURE() << "cold origin update survived shutdown";
        } catch (const std::logic_error&) {
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

namespace ruvia::test::dynamic_origin_tests {

enum class cancellation_case { timeout,
    caller_stop,
    owner_close };

void exercise_origin_preparation_cancellation(cancellation_case mode) {
    tls_identity identity("localhost");
    auto& io = new_test_io_context();
    blocking_pool blocking({.thread_count_ = 1, .queue_capacity_ = 4});
    std::promise<void> release;
    auto gate = release.get_future().share();
    ASSERT_EQ(blocking.submit([gate] { gate.wait(); }), blocking_submit_status::accepted);
    auto attachment = attach_event_loop(io, {.blocking_pool_ = &blocking});
    const auto worker = attachment.loop().worker();
    const auto run = [&]() -> task<void> {
        http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        http_client_config config{.base_url_ = "https://localhost:8443"};
        config.tls_.ca_file_ = identity.ca_file_;
        stop_source stop;
        bool update_finished = false;
        bool shutdown_finished = false;
        std::exception_ptr failure;
        const auto update = [&]() -> task<void> {
            try {
                co_await client.upsert(config, {.timeout_ = mode == cancellation_case::timeout
                                                                ? timeout_duration{5}
                                                                : timeout_duration{2000},
                                                   .stop_token_ = stop.token()});
                ADD_FAILURE() << "cancelled TLS preparation published an origin";
            } catch (const http_client_error& error) {
                const auto expected = mode == cancellation_case::owner_close ? make_error_code(http_client_errc::closing)
                                      : mode == cancellation_case::timeout   ? std::make_error_code(std::errc::timed_out)
                                                                             : std::make_error_code(std::errc::operation_canceled);
                EXPECT_EQ(error.code(), expected);
            } catch (...) {
                failure = std::current_exception();
            }
            update_finished = true;
        };
        const auto shutdown = [&]() -> task<void> {
            co_await client.shutdown();
            shutdown_finished = true;
        };
        task_scope tasks(worker);
        tasks.spawn(update());
        if (mode == cancellation_case::caller_stop) {
            stop.request_stop();
        } else if (mode == cancellation_case::owner_close) {
            tasks.spawn(shutdown());
        }
        (void)co_await sleep_for(worker, std::chrono::milliseconds(20));
        EXPECT_FALSE(update_finished);
        EXPECT_FALSE(shutdown_finished);
        release.set_value();
        co_await tasks.join();
        EXPECT_TRUE(update_finished);
        if (mode != cancellation_case::owner_close) {
            EXPECT_EQ(client.client().port(), 8001);
        } else {
            EXPECT_TRUE(shutdown_finished);
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_worker_tasks(attachment, run());
    blocking.join();
}

void exercise_redirect_policy_cancellation(cancellation_case mode) {
    auto& io = new_test_io_context();
    auto attachment = attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    constexpr std::array replies{std::string_view(
        "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: http://127.0.0.1:8123/next\r\n\r\n")};
    peer source(io, worker, replies);
    const auto url = source.url();
    const auto run = [&]() -> task<void> {
        http_client client(attachment.loop(), {.base_url_ = url, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        stop_source stop;
        worker_signal changed(worker);
        bool entered = false;
        bool exited = false;
        bool send_finished = false;
        std::exception_ptr failure;
        auto request = client.client().with_options({.timeout_ = mode == cancellation_case::timeout ? timeout_duration{200} : timeout_duration{2000},
                                                        .stop_token_ = stop.token()})
                           .get("/redirect");
        request.on_redirect([&](const http_client_redirect_hop& hop, stop_token token) -> task<bool> {
            entered = true;
            changed.notify();
            EXPECT_EQ(hop.target_origin().host(), "127.0.0.1");
            EXPECT_EQ(co_await sleep_for(worker, std::chrono::seconds(10), token), timer_sleep_result::stop_requested);
            EXPECT_EQ(hop.target_origin().host(), "127.0.0.1");
            EXPECT_EQ(hop.target_origin().port(), 8123);
            exited = true;
            co_return true;
        });
        const auto send = [&]() -> task<void> {
            try {
                (void)co_await request.send();
                ADD_FAILURE() << "cancelled redirect policy sent a follow-up request";
            } catch (const http_client_error& error) {
                EXPECT_EQ(error.code(), mode == cancellation_case::timeout
                                            ? std::make_error_code(std::errc::timed_out)
                                            : std::make_error_code(std::errc::operation_canceled));
                EXPECT_NE(error.connection_info(), nullptr);
                EXPECT_TRUE(exited);
            } catch (...) {
                failure = std::current_exception();
            }
            send_finished = true;
            changed.notify();
        };
        task_scope requests(worker);
        requests.spawn(send());
        while (!entered && !send_finished) {
            co_await changed.wait();
        }
        if (entered && mode == cancellation_case::caller_stop) {
            stop.request_stop();
        } else if (entered && mode == cancellation_case::owner_close) {
            client.close();
        }
        co_await requests.join();
        co_await client.shutdown();
        EXPECT_TRUE(entered);
        EXPECT_TRUE(exited);
        if (failure || !entered) {
            source.abort();
        }
        co_await source.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_worker_tasks(attachment, run());
}

}  // namespace ruvia::test::dynamic_origin_tests

TEST(Ruvia, http_client_tls_preparation_cancellation_and_shutdown_join_without_publication) {
    using namespace ruvia::test::dynamic_origin_tests;
    for (const auto mode : {cancellation_case::timeout, cancellation_case::caller_stop, cancellation_case::owner_close}) {
        exercise_origin_preparation_cancellation(mode);
    }
}

TEST(Ruvia, http_client_async_redirect_policy_joins_on_deadline_stop_and_owner_shutdown) {
    using namespace ruvia::test::dynamic_origin_tests;
    for (const auto mode : {cancellation_case::timeout, cancellation_case::caller_stop, cancellation_case::owner_close}) {
        exercise_redirect_policy_cancellation(mode);
    }
}

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
        std::string owned(5000, 'b');
        return client.write(std::move(owned));
    }();
    // Both temporary sources have died before
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
            co_await client.write(std::string(16 * 1024 * 1024, 'x'), options);
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
    {
        auto cold_write = client->write(std::string(5000, 'x'));
        auto cold_read = client->read();
        auto shutdown = client->shutdown();
        client.reset();
        co_await std::move(shutdown);
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

TEST(Ruvia, tcp_client_minimum_tls_version_rejects_downgrade_before_application_bytes) {
    for (const auto minimum : {ruvia::tls_version::tls_1_2, ruvia::tls_version::tls_1_3}) {
        for (const int version : {TLS1_1_VERSION, TLS1_2_VERSION, TLS1_3_VERSION}) {
            SCOPED_TRACE(static_cast<int>(minimum));
            SCOPED_TRACE(version);
            ruvia::test::tls_identity server("server.example");
            auto* context = server.context_.native_handle();
            ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
            ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
            if (version == TLS1_1_VERSION) {
                ASSERT_EQ(SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES128-SHA"), 1);
                SSL_CTX_set_security_level(context, 0);
            }
            byte_peer peer(10000, &server.context_);
            auto config = peer.config();
            config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
            config.tls_.minimum_version_ = minimum;
            config.tls_.ca_file_ = server.ca_file_;
            config.tls_.server_name_.emplace("server.example");
            ruvia::event_loop_pool pool({.loop_count_ = 1});
            pool.start();
            if (version >= static_cast<int>(minimum)) {
                pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
                EXPECT_EQ(peer.tls_version(), version);
                EXPECT_EQ(peer.received_bytes(), 10000U);
            } else {
                ruvia::tcp_client client(pool.loop(0), config);
                EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
                pool.loop(0).start(client.shutdown()).get();
                EXPECT_EQ(peer.received_bytes(), 0U);
            }
            pool.join();
        }
    }
}

TEST(Ruvia, tcp_client_rejects_invalid_tls_minimum_configuration) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::tcp_client_config config{.host_ = "127.0.0.1", .port_ = 443};
    config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
    config.tls_.minimum_version_ = static_cast<ruvia::tls_version>(0x0301);
    EXPECT_THROW(ruvia::tcp_client(pool.loop(0), config), std::invalid_argument);
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
        std::move(operation).get();
        std::move(other).get();
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
        std::move(operation).get();
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
            std::move(operation).get();
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
