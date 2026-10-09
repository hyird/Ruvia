#include "client/client_close_state.h"

#include <exception>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/core/event_loop_pool.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

struct pmr_close_failure final {
    explicit pmr_close_failure(std::pmr::memory_resource* resource)
        : payload_(512, 'x', resource) {}

    pmr_close_failure(const pmr_close_failure& other)
        : payload_(other.payload_, other.payload_.get_allocator().resource()) {}
    pmr_close_failure(pmr_close_failure&&) noexcept = default;

    std::pmr::string payload_;
};

struct close_state_probe final {
    explicit close_state_probe(ruvia::event_loop loop, bool* destroyed = nullptr)
        : loop_(std::move(loop)),
          worker_(loop_.handle()),
          state_(std::make_unique<ruvia::detail::client_close_state>(loop_, worker_)),
          destroyed_(destroyed) {}

    ~close_state_probe() {
        state_.reset();
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }

    ruvia::event_loop loop_;
    ruvia::worker_handle worker_;
    std::unique_ptr<ruvia::detail::client_close_state> state_;
    bool* destroyed_;
};

[[nodiscard]] std::exception_ptr make_failure(std::pmr::memory_resource* resource) {
    try {
        throw pmr_close_failure(resource);
    } catch (...) {
        return std::current_exception();
    }
}

ruvia::task<void> cleanup(std::exception_ptr failure = {}) {
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return;
}

ruvia::task<void> finish_failed_close(ruvia::event_loop loop,
    std::pmr::memory_resource* resource, std::exception_ptr& original, bool& state_destroyed,
    bool& caller_observed_same_failure, ruvia::testing::test_context& ruvia_ctx) {
    const auto probe_value = std::make_shared<close_state_probe>(std::move(loop), &state_destroyed);
    auto& state_value = *probe_value->state_;
    original = make_failure(resource);
    state_value.start_cleanup(probe_value, [failure = original] { return cleanup(failure); }, [probe_value](std::exception_ptr failure) { probe_value->state_->finish(std::move(failure)); });
    // Retirement awaits the actual root task without re-reporting its failure.
    co_await state_value.shutdown_owned(probe_value, [] {}, ruvia::detail::client_close_state::observation_mode_type::retirement);
    RUVIA_CHECK(state_value.complete());
    RUVIA_CHECK(state_value.task_started());

    caller_observed_same_failure = true;
    for (unsigned attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool observed_value = false;
        try {
            co_await state_value.shutdown_owned(probe_value, [] {}, ruvia::detail::client_close_state::observation_mode_type::caller);
        } catch (const pmr_close_failure& failure) {
            // rethrow_exception may copy its exception on some platforms.
            observed_value = failure.payload_.get_allocator().resource() == resource &&
                             failure.payload_.size() == 512 && failure.payload_.front() == 'x';
        }
        caller_observed_same_failure = caller_observed_same_failure && observed_value;
    }
    bool restarted = false;
    state_value.start_cleanup(probe_value, [&restarted] {
        restarted = true;
        return cleanup(); }, [](std::exception_ptr) { std::terminate(); });
    RUVIA_CHECK(!restarted);
}

ruvia::task<void> finish_successful_close(ruvia::event_loop loop, bool& cold_stayed_cold,
    bool& close_remained_one_shot) {
    {
        const auto probe_value = std::make_shared<close_state_probe>(loop);
        auto& state_value = *probe_value->state_;
        state_value.complete_before_publication();
        cold_stayed_cold = true;
        state_value.start_cleanup(probe_value, [&cold_stayed_cold] {
            cold_stayed_cold = false;
            return cleanup(); }, [](std::exception_ptr) { std::terminate(); });
        state_value.observe_failure(ruvia::detail::client_close_state::observation_mode_type::caller);
        state_value.observe_failure(ruvia::detail::client_close_state::observation_mode_type::retirement);
    }
    {
        const auto probe_value = std::make_shared<close_state_probe>(loop);
        auto& state_value = *probe_value->state_;
        state_value.start_cleanup(probe_value, [] { return cleanup(); }, [probe_value](std::exception_ptr failure) { probe_value->state_->finish(std::move(failure)); });
        co_await state_value.shutdown_owned(probe_value, [] {}, ruvia::detail::client_close_state::observation_mode_type::caller);
        close_remained_one_shot = state_value.task_started() && state_value.complete();
        state_value.start_cleanup(probe_value, [&close_remained_one_shot] {
            close_remained_one_shot = false;
            return cleanup(); }, [](std::exception_ptr) { std::terminate(); });
    }
}

}  // namespace

RUVIA_TEST(client_close_state_reports_unobserved_failure_and_rethrows_to_callers) {
    ruvia::test::counting_memory_resource memory;
    std::exception_ptr original;
    bool state_destroyed = false;
    bool caller_observed_same_failure = false;
    {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        auto loop = pool.loop(0);
        loop.start(finish_failed_close(
                       loop, &memory, original, state_destroyed, caller_observed_same_failure, ruvia_ctx))
            .get();
        RUVIA_CHECK(caller_observed_same_failure);
        RUVIA_CHECK(memory.live_allocations() > 0);

        bool pool_observed_failure = false;
        try {
            pool.join();
        } catch (const pmr_close_failure&) {
            pool_observed_failure = true;
        }
        RUVIA_CHECK(pool_observed_failure);
        RUVIA_CHECK(state_destroyed);
    }
    original = {};
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK(memory.deallocation_count() > 0);
}

RUVIA_TEST(client_close_state_successful_and_cold_completion_is_one_shot) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    bool cold_stayed_cold = false;
    bool close_remained_one_shot = false;
    pool.loop(0).start(finish_successful_close(
                           pool.loop(0), cold_stayed_cold, close_remained_one_shot))
        .get();
    RUVIA_CHECK(cold_stayed_cold);
    RUVIA_CHECK(close_remained_one_shot);
    pool.stop();
    pool.join();
}
