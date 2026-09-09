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

#include "ruvia/core/Timer.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Deadline.h"
#include "ruvia/web/Testing.h"

#include "test_harness.h"

namespace {

struct MemoryCounters final {
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> deallocations{0};
    std::atomic<std::size_t> live{0};
    std::atomic<std::size_t> foreignDeallocations{0};
};

class CountingResource final : public std::pmr::memory_resource {
public:
    explicit CountingResource(MemoryCounters* counters) noexcept
        : counters_(counters) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* const value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        counters_->allocations.fetch_add(1, std::memory_order_relaxed);
        counters_->live.fetch_add(1, std::memory_order_release);
        return value;
    }

    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        if (std::this_thread::get_id() != owner_) {
            counters_->foreignDeallocations.fetch_add(1, std::memory_order_relaxed);
        }
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
        counters_->deallocations.fetch_add(1, std::memory_order_relaxed);
        counters_->live.fetch_sub(1, std::memory_order_release);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    MemoryCounters* counters_;
    std::thread::id owner_{std::this_thread::get_id()};
};

struct MemoryState final {
    explicit MemoryState(MemoryCounters* counters) noexcept
        : resource(counters) {}

    CountingResource resource;
};

class TestingMemoryController final : public ruvia::Controller<TestingMemoryController> {
public:
    RUVIA_CONTROLLER_GROUP("/testing-memory")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/owned", owned);
    RUVIA_GET("/throw", throwing);
    RUVIA_GET("/cancel", cancelled, ruvia::Deadline<5>);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> owned(ruvia::Context& c) {
        auto& state = c.workerState<MemoryState>();
        ruvia::HttpResponse response({.resource = &state.resource});
        std::pmr::string body(&state.resource);
        body.assign(256 * 1024, 'm');
        ruvia::detail::setResponseBodyOwned(response, std::move(body));
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> throwing(ruvia::Context& c) {
        auto& state = c.workerState<MemoryState>();
        ruvia::HttpResponse response({.resource = &state.resource});
        std::pmr::string body(&state.resource);
        body.assign(256 * 1024, 'e');
        ruvia::detail::setResponseBodyOwned(response, std::move(body));
        throw std::runtime_error("testing memory failure");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> cancelled(ruvia::Context& c) {
        auto& state = c.workerState<MemoryState>();
        ruvia::HttpResponse response({.resource = &state.resource});
        std::pmr::string body(256 * 1024, 'c', &state.resource);
        ruvia::detail::setResponseBodyOwned(response, std::move(body));
        const auto result = co_await ruvia::sleepFor(
            c.worker(), std::chrono::hours(1), c.stopToken());
        if (result != ruvia::TimerSleepResult::kStopRequested || !c.deadlineExceeded()) {
            throw std::logic_error("deadline did not cancel the operation");
        }
        co_return response;
    }
};

}  // namespace

RUVIA_TEST(testing_facade_reclaims_worker_owned_response_memory) {
    MemoryCounters counters;
    std::optional<ruvia::TestResponse> retained;
    {
        ruvia::TestApp app;
        app.useWorkerState<MemoryState>([&counters] { return MemoryState(&counters); });

        retained.emplace(app.request(ruvia::TestRequest::get("/testing-memory/owned")));
        RUVIA_CHECK_EQ(retained->body().size(), std::size_t(256 * 1024));
        RUVIA_CHECK(retained->body().front() == 'm');
        RUVIA_CHECK_EQ(counters.live.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK(counters.allocations.load() > 0);
        RUVIA_CHECK_EQ(counters.allocations.load(), counters.deallocations.load());

        const auto cancelled = app.request(ruvia::TestRequest::get("/testing-memory/cancel"));
        RUVIA_CHECK_EQ(cancelled.body(), std::string(256 * 1024, 'c'));
        RUVIA_CHECK_EQ(counters.live.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK_EQ(counters.allocations.load(), counters.deallocations.load());

        const auto repeated = app.request(ruvia::TestRequest::get("/testing-memory/owned"));
        RUVIA_CHECK_EQ(repeated.body().size(), std::size_t(256 * 1024));
        RUVIA_CHECK(retained->body().front() == 'm');
        RUVIA_CHECK_EQ(counters.live.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK_EQ(counters.allocations.load(), counters.deallocations.load());

        const auto failed = app.request(ruvia::TestRequest::get("/testing-memory/throw"));
        RUVIA_CHECK(failed.status() == ruvia::http_status::kInternalServerError);
        RUVIA_CHECK_EQ(counters.live.load(std::memory_order_acquire), std::size_t(0));
        RUVIA_CHECK_EQ(counters.allocations.load(), counters.deallocations.load());
    }
    RUVIA_CHECK_EQ(retained->body().size(), std::size_t(256 * 1024));
    RUVIA_CHECK(retained->body().front() == 'm');
    RUVIA_CHECK_EQ(counters.live.load(), std::size_t(0));
    RUVIA_CHECK_EQ(counters.allocations.load(), counters.deallocations.load());
    RUVIA_CHECK_EQ(counters.foreignDeallocations.load(), std::size_t(0));
}

RUVIA_TEST(testing_facade_unused_worker_allocates_no_response_storage) {
    MemoryCounters counters;
    {
        ruvia::TestApp app;
        app.useWorkerState<MemoryState>([&counters] { return MemoryState(&counters); });
    }
    RUVIA_CHECK_EQ(counters.allocations.load(), std::size_t(0));
    RUVIA_CHECK_EQ(counters.deallocations.load(), std::size_t(0));
}
