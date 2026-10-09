#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>

#include "ruvia/core/event_loop_pool.h"

#include "db/db_pool_operations.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

struct pool_backend final {
    struct config final {
        ruvia::db_driver driver_;
        std::chrono::milliseconds acquire_timeout_{100};
    } config_;
    struct slot final {
        std::uint64_t cancellation_id_{};
        ruvia::detail::db_slot_abort_reason abort_reason_{ruvia::detail::db_slot_abort_reason::none};
        unsigned connects_{};
        unsigned closes_{};
    };

    pool_backend(const ruvia::worker_handle& worker_value, ruvia::db_driver driver,
        std::pmr::memory_resource* resource)
        : config_{driver},
          scheduler_(2, worker_value, resource),
          lifecycle_(*this) {}

    ruvia::task<void> connect_unlocked(slot& connection, const ruvia::operation_timeout&) {
        ++connection.connects_;
        if (fail_connect_ && &connection == &slots_.back()) {
            throw std::runtime_error("backend startup failed");
        }
        co_return;
    }

    void close_slot(slot& connection) noexcept {
        ++connection.closes_;
    }

    std::array<slot, 2> slots_;
    ruvia::pool_lease_scheduler scheduler_;
    ruvia::detail::db_pool_lifecycle<pool_backend> lifecycle_;
    bool fail_connect_{};
};

ruvia::task<void> exercise_pool(const ruvia::worker_handle& worker_value, ruvia::db_driver driver,
    std::pmr::memory_resource* resource, ruvia::testing::test_context& ruvia_ctx) {
    pool_backend backend(worker_value, driver, resource);
    auto& lifecycle = backend.lifecycle_;
    co_await lifecycle.connect();
    RUVIA_CHECK(backend.slots_[0].connects_ == 1 && backend.slots_[1].connects_ == 1);
    const auto first = co_await lifecycle.acquire_slot(ruvia::operation_timeout(std::nullopt), {});
    const auto second = co_await lifecycle.acquire_slot(ruvia::operation_timeout(std::nullopt), {});
    RUVIA_CHECK(first != second);

    ruvia::stop_source cancelled;
    cancelled.request_stop();
    bool cancellation_observed{};
    try {
        (void)co_await lifecycle.acquire_slot(ruvia::operation_timeout(std::nullopt), cancelled.token());
    } catch (const ruvia::db_error& error) {
        cancellation_observed = error.code() == ruvia::db_error::code_type::cancelled;
    }
    RUVIA_CHECK(cancellation_observed);

    auto& active = backend.slots_[first];
    active.cancellation_id_ = 42;
    lifecycle.cancel_operation_by_id(43);
    RUVIA_CHECK(active.closes_ == 0);
    lifecycle.cancel_operation_by_id(42);
    RUVIA_CHECK(active.closes_ == 1);
    bool operation_cancelled{};
    try {
        lifecycle.throw_if_cancelled(active);
    } catch (const ruvia::db_error& error) {
        operation_cancelled = error.code() == ruvia::db_error::code_type::cancelled;
    }
    RUVIA_CHECK(operation_cancelled);

    lifecycle.close_now();
    lifecycle.release_slot(first);
    lifecycle.release_slot(second);
    bool closing_observed{};
    try {
        (void)co_await lifecycle.acquire_slot(ruvia::operation_timeout(std::nullopt), {});
    } catch (const ruvia::db_error& error) {
        closing_observed = error.code() == ruvia::db_error::code_type::closing;
    }
    RUVIA_CHECK(closing_observed);
    RUVIA_CHECK(backend.slots_[first].closes_ == 2 && backend.slots_[second].closes_ == 1);

    pool_backend failing(worker_value, driver, resource);
    failing.fail_connect_ = true;
    bool startup_failed{};
    try {
        co_await failing.lifecycle_.connect();
    } catch (const std::runtime_error&) {
        startup_failed = true;
    }
    RUVIA_CHECK(startup_failed);
    failing.lifecycle_.close_now();
    RUVIA_CHECK(failing.slots_[0].closes_ == 1 && failing.slots_[1].closes_ == 1);
}

}  // namespace

RUVIA_TEST(database_pool_lifecycle_schedules_cancels_and_closes_backend_slots) {
    ruvia::test::counting_memory_resource memory;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto worker_value = pool.loop(0).handle();
    pool.start();
    for (auto driver : {ruvia::db_driver::mariadb, ruvia::db_driver::postgresql}) {
        pool.loop(0).start(exercise_pool(worker_value, driver, &memory, ruvia_ctx)).get();
    }
    pool.stop();
    pool.join();
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}
