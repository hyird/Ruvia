#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/web/http_client.h"

#include "test_io_context.h"
#include "test_tls_identity.h"
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
    peer(asio::io_context& io, const worker_handle& worker, std::span<const std::string_view> replies)
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

    std::pmr::vector<std::pmr::string> requests_{process_resource()};
    bool client_closed_{};

private:
    asio::awaitable<void> serve() {
        socket_ = co_await acceptor_.async_accept(asio::use_awaitable);
        std::pmr::string input(process_resource());
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
        EXPECT_EQ(error.code(), http_client_error::code_type::closing);
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
                EXPECT_EQ(error.code(), ruvia::http_client_error::code_type::closing);
            }

            auto removed = client.client();
            auto removed_send = removed.get("/removed").send();
            EXPECT_TRUE(client.remove("default"));
            EXPECT_FALSE(client.remove("default"));
            try {
                (void)client.client("default");
                ADD_FAILURE() << "removed alias remained visible";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::http_client_error::code_type::not_configured);
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
    ruvia::test::dynamic_origin_tests::peer peer(io, loop.handle(), replies);
    const auto url = peer.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.alias_ = "stable", .base_url_ = url, .request_timeout_ = std::chrono::seconds(2), .protocol_ = ruvia::http_client_protocol::http1_only, .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send});
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
    ruvia::test::dynamic_origin_tests::peer peer(io, loop.handle(), replies);
    const auto url = peer.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.base_url_ = url, .request_timeout_ = std::chrono::seconds(2)});
        ruvia::task_scope requests(loop.handle());
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
    ruvia::test::dynamic_origin_tests::peer target(io, loop.handle(), target_replies);
    const auto target_url = target.url();
    const auto redirect = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: " + target_url + "/destination\r\n\r\n";
    const std::array source_replies{std::string_view(redirect), std::string_view(redirect), std::string_view(redirect), std::string_view(redirect)};
    ruvia::test::dynamic_origin_tests::peer source(io, loop.handle(), source_replies);
    const auto source_url = source.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.base_url_ = source_url,
                                            .request_timeout_ = std::chrono::seconds(2),
                                            .protocol_ = ruvia::http_client_protocol::http1_only});
        std::exception_ptr failure;
        try {
            auto denied = co_await client.client().get("/without-policy").send();
            EXPECT_EQ(denied.status().value(), 302);
            EXPECT_TRUE(target.requests_.empty());

            auto unconfigured = client.client().get("/policy-without-origin");
            unconfigured.redirect_policy([](ruvia::http_origin_view, ruvia::stop_token) -> ruvia::task<bool> { co_return true; });
            auto still_denied = co_await unconfigured.send();
            EXPECT_EQ(still_denied.status().value(), 302);
            EXPECT_TRUE(target.requests_.empty());

            std::size_t decisions = 0;
            auto request = client.client().get("/with-policy");
            request.header("Authorization", "Bearer private");
            request.header("Proxy-Authorization", "Basic private");
            request.header("Cookie", "source=private");
            request.redirect_policy([&](ruvia::http_origin_view origin, ruvia::stop_token stop) -> ruvia::task<bool> {
                ++decisions;
                EXPECT_EQ(origin.host(), "127.0.0.1");
                co_await client.upsert({.alias_ = "discovered", .base_url_ = target_url, .protocol_ = ruvia::http_client_protocol::http1_only},
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
            rejected.redirect_policy([&](ruvia::http_origin_view, ruvia::stop_token) -> ruvia::task<bool> {
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
        std::optional<ruvia::http_client_handle> retired;
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
                EXPECT_EQ(error.code(), ruvia::http_client_error::code_type::closing);
            }
            auto tunnel = client.client().connect("127.0.0.1:8002").open();
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8003"});
            try {
                (void)co_await std::move(tunnel);
                ADD_FAILURE() << "cold tunnel entered a replaced origin";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::http_client_error::code_type::closing);
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
    const auto worker = attachment.loop().handle();
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
                .address_validator_ = [marker](std::string_view, std::uint16_t) { return marker != nullptr; }});
        }
        std::optional<ruvia::http_client_handle> old(client.client());
        std::optional<ruvia::task<ruvia::http_client_exchange>> cold(old->post("/cold").upload());
        EXPECT_TRUE(client.remove("default"));
        EXPECT_FALSE(observed.expired());
        old.reset();
        EXPECT_FALSE(observed.expired());
        cold.reset();
        ruvia::worker_timer_registration deadline;
        (void)worker.schedule_timer(deadline, ruvia::worker_timer_deadline_after(std::chrono::seconds(2)),
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
    const auto worker = attachment.loop().handle();
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
                const auto expected = mode == cancellation_case::owner_close ? http_client_error::code_type::closing
                                      : mode == cancellation_case::timeout   ? http_client_error::code_type::timeout
                                                                             : http_client_error::code_type::cancelled;
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
    const auto worker = attachment.loop().handle();
    constexpr std::array replies{std::string_view(
        "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: http://127.0.0.1:8123/next\r\n\r\n")};
    peer source(io, worker, replies);
    const auto url = source.url();
    const auto run = [&]() -> task<void> {
        http_client client(attachment.loop(), {.base_url_ = url, .protocol_ = http_client_protocol::http1_only});
        stop_source stop;
        worker_signal changed(worker);
        bool entered = false;
        bool exited = false;
        bool send_finished = false;
        std::exception_ptr failure;
        auto request = client.client().with_options({.timeout_ = mode == cancellation_case::timeout ? timeout_duration{200} : timeout_duration{2000},
                                                        .stop_token_ = stop.token()})
                           .get("/redirect");
        request.redirect_policy([&](http_origin_view origin, stop_token token) -> task<bool> {
            entered = true;
            changed.notify();
            EXPECT_EQ(origin.host(), "127.0.0.1");
            EXPECT_EQ(co_await sleep_for(worker, std::chrono::seconds(10), token), timer_sleep_result::stop_requested);
            EXPECT_EQ(origin.host(), "127.0.0.1");
            EXPECT_EQ(origin.port(), 8123);
            exited = true;
            co_return true;
        });
        const auto send = [&]() -> task<void> {
            try {
                (void)co_await request.send();
                ADD_FAILURE() << "cancelled redirect policy sent a follow-up request";
            } catch (const http_client_error& error) {
                EXPECT_EQ(error.code(), mode == cancellation_case::timeout
                                            ? http_client_error::code_type::timeout
                                            : http_client_error::code_type::cancelled);
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
