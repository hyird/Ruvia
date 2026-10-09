#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <asio.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"

#include "http2_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct tunnel_observation {
    std::mutex mutex_;
    std::string bytes_;
    std::atomic<bool> finish_first_{};
    std::atomic<bool> eof_{};
    [[nodiscard]] std::string received() {
        std::lock_guard lock(mutex_);
        return bytes_;
    }
};
const auto observation = std::make_shared<tunnel_observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<tunnel_observation>>([] { return observation; });
    return true;
}();

class client_tunnel_routes final : public ruvia::controller<client_tunnel_routes> {
    RUVIA_ROUTES_BEGIN
    RUVIA_CONNECT("client-target.test:443", echo);
    RUVIA_CONNECT_PROTOCOL("test-protocol", "/tunnel", echo);
    RUVIA_CONNECT_PROTOCOL("test-capsules", "/capsules", capsules);
    RUVIA_GET("/sibling", sibling);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& c);
    ruvia::task<void> capsules(ruvia::context& c);
    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
};
ruvia::task<void> client_tunnel_routes::echo(ruvia::context& c) {
    auto& observation_value = *c.worker_state<std::shared_ptr<tunnel_observation>>();
    {
        std::lock_guard lock(observation_value.mutex_);
        observation_value.bytes_.clear();
    }
    observation_value.eof_ = false;
    auto& tunnel = c.tunnel();
    if (observation_value.finish_first_) {
        co_await tunnel.write("greeting");
        co_await tunnel.finish();
    }
    while (auto bytes = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.bytes_.append(*bytes);
        }
        if (!observation_value.finish_first_) {
            co_await tunnel.write(std::string_view(*bytes));
        }
    }
    observation_value.eof_ = true;
    co_await tunnel.finish();
}
}  // namespace
RUVIA_TEST(http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http_client_tunnel> retained_tunnel;
    ruvia::test::http2_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        auto& observation_value = *observation;
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = server.endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation_value.finish_first_ = round >= 2;
                std::string authority = round == 1 ? "proxy.test" : "client-target.test:443";
                auto cold = client.open_tunnel({.authority_ = authority, .protocol_ = round == 1 ? "test-protocol" : "", .target_ = round == 1 ? "/tunnel" : ""});
                authority.assign("mutated");
                auto result_value = co_await std::move(cold);
                RUVIA_CHECK(result_value.response() == nullptr && result_value.tunnel() != nullptr);
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                RUVIA_CHECK(tunnel.status() == ruvia::http_status::ok);
                RUVIA_CHECK(!tunnel.header("content-length"));
                std::string echoed;
                const auto receive = [&]() -> ruvia::task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation_value.finish_first_) {
                    co_await receive();
                    RUVIA_CHECK(echoed == "greeting");
                }
                ruvia::task_scope readers(worker_value);
                if (round == 3) {
                    tunnel.abort();
                    bool half_closed_write_rejected = false;
                    try {
                        auto late_write = tunnel.write("late");
                        static_cast<void>(late_write);
                    } catch (const ruvia::http_client_error& error) {
                        half_closed_write_rejected = error.code() == ruvia::http_client_error::code_type::cancelled;
                    }
                    RUVIA_CHECK(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client.send({.target_ = "/sibling"});
                    std::string sibling_body;
                    while (auto bytes = co_await sibling.body().text()) {
                        sibling_body.append(*bytes);
                    }
                    RUVIA_CHECK(sibling_body == "sibling");
                    retained_tunnel.emplace(std::move(tunnel));
                    continue;
                }
                if (!observation_value.finish_first_) {
                    readers.spawn(receive());
                }
                std::string payload_value(100003, 't');
                for (std::size_t offset = 0; offset != payload_value.size();) {
                    const auto count = std::min<std::size_t>(16384, payload_value.size() - offset);
                    auto output = tunnel.write(std::string_view(payload_value).substr(offset, count));
                    bool busy = false;
                    try {
                        (void)tunnel.finish();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await readers.join();
                if (!observation_value.finish_first_) {
                    RUVIA_CHECK(echoed == payload_value);
                }
                auto sibling = co_await client.send({.target_ = "/sibling"});
                std::string sibling_body;
                while (auto bytes = co_await sibling.body().text()) {
                    sibling_body.append(*bytes);
                }
                RUVIA_CHECK(sibling_body == "sibling");
                RUVIA_CHECK(observation_value.received() == payload_value && observation_value.eof_);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (retained_tunnel) {
            retained_tunnel->abort();
            retained_tunnel->abort();
            bool late_write_rejected = false;
            try {
                auto late_write = retained_tunnel->write("late");
                static_cast<void>(late_write);
            } catch (const ruvia::http_client_error&) {
                late_write_rejected = true;
            }
            RUVIA_CHECK(late_write_rejected);
            retained_tunnel.reset();
        }
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace {
ruvia::task<void> client_tunnel_routes::capsules(ruvia::context& c) {
    auto stream = c.tunnel().capsules();
    while (auto capsule = co_await stream.read()) {
        co_await stream.write(capsule->type(), capsule->payload());
    }
    co_await stream.finish();
}
}  // namespace
RUVIA_TEST(http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http2_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_capsule> retained;
        std::unique_ptr<ruvia::scoped_operation<void>> cold;
        {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = server.endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
            try {
                auto result_value = co_await client.open_tunnel({.authority_ = "proxy.test", .protocol_ = "test-capsules", .target_ = "/capsules"}, {.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*result_value.tunnel()).capsules();
                    std::string bytes_value(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes_value);
                    bytes_value.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    RUVIA_CHECK(retained && retained->type() == 0x123456789ULL && retained->payload() == std::string(4099, 'c'));
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    RUVIA_CHECK(empty && empty->type() == 0 && empty->payload().empty());
                    co_await stream.finish();
                    RUVIA_CHECK(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                auto second = co_await client.open_tunnel({.authority_ = "proxy.test", .protocol_ = "test-capsules", .target_ = "/capsules"});
                if (!second.tunnel()) {
                    throw std::runtime_error("second capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*second.tunnel()).capsules();
                    cold.reset(new ruvia::scoped_operation<void>(stream.write(12, std::string(4099, 'x'))));
                }
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload() == std::string(4099, 'c'));
        cold.reset();
        retained.reset();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
