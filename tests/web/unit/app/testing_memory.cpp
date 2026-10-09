#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ruvia/core/timer.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/deadline.h"
#include "ruvia/web/testing.h"

#include "test_harness.h"

namespace {

struct memory_counters final {
    std::atomic<std::size_t> allocations_{0};
    std::atomic<std::size_t> deallocations_{0};
    std::atomic<std::size_t> live_{0};
    std::atomic<std::size_t> foreign_deallocations_{0};
};

class counting_resource final : public std::pmr::memory_resource {
public:
    explicit counting_resource(memory_counters* counters) noexcept
        : counters_(counters) {}

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* const value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        counters_->allocations_.fetch_add(1, std::memory_order_relaxed);
        counters_->live_.fetch_add(1, std::memory_order_release);
        return value;
    }

    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        if (std::this_thread::get_id() != owner_) {
            counters_->foreign_deallocations_.fetch_add(1, std::memory_order_relaxed);
        }
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
        counters_->deallocations_.fetch_add(1, std::memory_order_relaxed);
        counters_->live_.fetch_sub(1, std::memory_order_release);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    memory_counters* counters_;
    std::thread::id owner_{std::this_thread::get_id()};
};

struct memory_state final {
    explicit memory_state(memory_counters* counters) noexcept
        : resource_(counters) {}

    counting_resource resource_;
};

class testing_memory_controller final : public ruvia::controller<testing_memory_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/testing-memory")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/owned", owned);
    RUVIA_GET("/throw", throwing);
    RUVIA_GET("/cancel", cancelled, ruvia::deadline<5>);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> owned(ruvia::context& c) {
        auto& state_value = c.worker_state<memory_state>();
        ruvia::http_response response({.resource_ = &state_value.resource_});
        std::pmr::string body(&state_value.resource_);
        body.assign(256 * 1024, 'm');
        response.owned_body(std::move(body));
        co_return response;
    }

    ruvia::task<ruvia::http_response> throwing(ruvia::context& c) {
        auto& state_value = c.worker_state<memory_state>();
        ruvia::http_response response({.resource_ = &state_value.resource_});
        std::pmr::string body(&state_value.resource_);
        body.assign(256 * 1024, 'e');
        response.owned_body(std::move(body));
        throw std::runtime_error("testing memory failure");
        co_return response;
    }

    ruvia::task<ruvia::http_response> cancelled(ruvia::context& c) {
        auto& state_value = c.worker_state<memory_state>();
        ruvia::http_response response({.resource_ = &state_value.resource_});
        std::pmr::string body(256 * 1024, 'c', &state_value.resource_);
        response.owned_body(std::move(body));
        const auto result_value = co_await ruvia::sleep_for(
            c.worker(), std::chrono::hours(1), c.get_stop_token());
        if (result_value != ruvia::timer_sleep_result::stop_requested || !c.deadline_exceeded()) {
            throw std::logic_error("deadline did not cancel the operation");
        }
        co_return response;
    }
};

}  // namespace

RUVIA_TEST(testing_facade_reclaims_worker_owned_response_memory) {
    memory_counters counters;
    std::optional<ruvia::test_response> retained;
    {
        ruvia::test_app app;
        app.use_worker_state<memory_state>([&counters] { return memory_state(&counters); });

        retained.emplace(app.request(ruvia::test_request::get("/testing-memory/owned")));
        RUVIA_CHECK_EQ(retained->body().size(), std::size_t(256 * 1024));
        RUVIA_CHECK(retained->body().front() == 'm');
        RUVIA_CHECK_EQ(counters.live_.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK(counters.allocations_.load() > 0);
        RUVIA_CHECK_EQ(counters.allocations_.load(), counters.deallocations_.load());

        const auto cancelled = app.request(ruvia::test_request::get("/testing-memory/cancel"));
        RUVIA_CHECK_EQ(cancelled.body(), std::string(256 * 1024, 'c'));
        RUVIA_CHECK_EQ(counters.live_.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK_EQ(counters.allocations_.load(), counters.deallocations_.load());

        const auto repeated = app.request(ruvia::test_request::get("/testing-memory/owned"));
        RUVIA_CHECK_EQ(repeated.body().size(), std::size_t(256 * 1024));
        RUVIA_CHECK(retained->body().front() == 'm');
        RUVIA_CHECK_EQ(counters.live_.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK_EQ(counters.allocations_.load(), counters.deallocations_.load());

        const auto failed = app.request(ruvia::test_request::get("/testing-memory/throw"));
        RUVIA_CHECK(failed.status() == ruvia::http_status::internal_server_error);
        RUVIA_CHECK_EQ(counters.live_.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK_EQ(counters.allocations_.load(), counters.deallocations_.load());
    }
    RUVIA_CHECK_EQ(retained->body().size(), std::size_t(256 * 1024));
    RUVIA_CHECK(retained->body().front() == 'm');
    RUVIA_CHECK_EQ(counters.live_.load(), std::size_t(0));
    RUVIA_CHECK_EQ(counters.allocations_.load(), counters.deallocations_.load());
    RUVIA_CHECK_EQ(counters.foreign_deallocations_.load(), std::size_t(0));
}

RUVIA_TEST(testing_facade_unused_worker_allocates_no_response_storage) {
    memory_counters counters;
    {
        ruvia::test_app app;
        app.use_worker_state<memory_state>([&counters] { return memory_state(&counters); });
    }
    RUVIA_CHECK_EQ(counters.allocations_.load(), std::size_t(0));
    RUVIA_CHECK_EQ(counters.deallocations_.load(), std::size_t(0));
}
