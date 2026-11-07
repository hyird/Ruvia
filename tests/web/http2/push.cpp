#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_io_context.h"

namespace {
ruvia::task<std::string> collect_text(ruvia::http_client_response& response) {
    const auto bytes_value = co_await response.stream().read_all();
    const auto view = bytes_value.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}
struct push_observation final {
    std::atomic<unsigned> promised_{};
    std::atomic<unsigned> refused_{};
    std::atomic<unsigned> streamed_{};
};

const auto observation = std::make_shared<push_observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<push_observation>>([] { return observation; });
    return true;
}();

std::shared_ptr<push_observation> prepare_observation() {
    observation->promised_ = 0;
    observation->refused_ = 0;
    observation->streamed_ = 0;
    return observation;
}

class push_routes final {
public:
    RUVIA_ROUTES_BEGIN(push_routes)
    RUVIA_GET("/parent", parent);
    RUVIA_GET("/two", parent);
    RUVIA_GET("/large", parent);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/large-asset", large);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> parent(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        const auto count = context_value.req().path() == "/two" ? 2U : 1U;
        const auto path = context_value.req().path() == "/large" ? "/large-asset" : "/asset";
        const std::array<ruvia::http_header_view, 1> headers{{{"x-promise", "owned-request"}}};
        for (unsigned i = 0; i != count; ++i) {
            if (context_value.push({.scheme_ = "http", .authority_ = context_value.req().authority(), .path_ = path, .headers_ = headers})) {
                ++owner_value.promised_;
            } else {
                ++owner_value.refused_;
            }
        }
        co_return context_value.text("parent");
    }
    ruvia::task<ruvia::http_response> asset(ruvia::context& context_value) {
        if (context_value.req().header("x-promise") != "owned-request") {
            throw std::runtime_error("lost push request metadata");
        }
        context_value.draft().header("x-asset", "metadata");
        co_return context_value.text("pushed-body");
    }
    ruvia::task<void> large(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        std::string chunk(16384, 'p');
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (unsigned i = 0; i != 80; ++i) {
            co_await context_value.stream().write(chunk);
        }
        ++owner_value.streamed_;
        co_await context_value.stream().end();
    }
};

}  // namespace

TEST(Ruvia, http2_push_routes_and_client_preserve_owners_flow_control_cold_reads_and_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_push> retained;
        std::optional<ruvia::http_client_response> retained_response;
        {
            const std::string client_url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .protocol_ = ruvia::http_client_protocol::http2_only,
                                                             .push_ = {.enabled_ = true}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                for (unsigned repeat = 0; repeat != 128; ++repeat) {
                    auto parent_value = co_await client_origin.get("/parent").send();
                    EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                    auto push = client_origin.next_push();
                    EXPECT_TRUE(push.has_value());
                    if (!push) {
                        throw std::runtime_error("missing push");
                    }
                    EXPECT_TRUE(push->request().path_ == "/asset");
                    EXPECT_TRUE(push->request().headers_.front().value() == "owned-request");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        auto overlap = push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    EXPECT_TRUE(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    EXPECT_TRUE(response.status() == ruvia::http_status::ok);
                    EXPECT_TRUE(response.header("x-asset") == "metadata");
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                    EXPECT_TRUE(!client_origin.next_push());
                    if (repeat == 0) {
                        retained.emplace(std::move(moved));
                        retained_response.emplace(std::move(response));
                    }
                    EXPECT_TRUE(retained->request().path_ == "/asset");
                    EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
                }
                auto parent_value = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto pushed = client_origin.next_push();
                EXPECT_TRUE(pushed.has_value());
                if (!pushed) {
                    throw std::runtime_error("missing large push");
                }
                auto response = co_await pushed->response();
                std::size_t bytes_value = 0;
                while (auto chunk = co_await response.stream().text()) {
                    EXPECT_TRUE(chunk->find_first_not_of('p') == std::string_view::npos);
                    bytes_value += chunk->size();
                }
                EXPECT_EQ(bytes_value, std::size_t{80 * 16384});
                EXPECT_EQ(observed->streamed_.load(), 1U);
                auto cancelled_parent = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(cancelled_parent)) == "parent");
                auto cancelled_push = client_origin.next_push();
                EXPECT_TRUE(cancelled_push.has_value());
                cancelled_push.reset();
                auto after_cancel = co_await client_origin.get("/parent").send();
                EXPECT_TRUE((co_await collect_text(after_cancel)) == "parent");
                auto after_push = client_origin.next_push();
                EXPECT_TRUE(after_push.has_value());
                if (after_push) {
                    auto after_response = co_await after_push->response();
                    EXPECT_TRUE((co_await collect_text(after_response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, std::size_t{131});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, std::size_t{0});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        EXPECT_TRUE(retained->request().path_ == "/asset");
        EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
        retained_response.reset();
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

TEST(Ruvia, http2_push_queue_overflow_and_disabled_permission_keep_parent_response_usable) {
    for (const bool enabled : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto observed = prepare_observation();
        ruvia::test::http_server_fixture server(io);
        auto run = [&]() -> ruvia::task<void> {
            const std::string client_url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .protocol_ = ruvia::http_client_protocol::http2_only,
                                                             .push_ = {.enabled_ = enabled, .max_queued_pushes_ = 1}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                auto parent_value = co_await client_origin.get("/two").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto push = client_origin.next_push();
                EXPECT_TRUE(push.has_value() == enabled);
                if (push) {
                    auto response = co_await push->response();
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(observed->refused_.load(), enabled ? 0U : 2U);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
    }
}
