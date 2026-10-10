#include "ruvia/core/scoped_operation.h"

#include <atomic>
#include <future>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t in_use_allocations_{};
    std::size_t in_use_bytes_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++in_use_allocations_;
        in_use_bytes_ += bytes_value;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        --in_use_allocations_;
        in_use_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class test_scoped_capability final {
public:
    test_scoped_capability(ruvia::operation_scope& scope, int& expired_count,
        counting_resource* resource = nullptr, std::size_t* bytes_at_expire = nullptr,
        const bool* lease_active = nullptr, bool* lease_at_expire = nullptr) noexcept
        : expired_count_(&expired_count),
          resource_(resource),
          bytes_at_expire_(bytes_at_expire),
          lease_active_(lease_active),
          lease_at_expire_(lease_at_expire),
          registration_(scope, this, &test_scoped_capability::expire) {}

    test_scoped_capability(const test_scoped_capability& other) noexcept
        : expired_count_(other.expired_count_),
          resource_(other.resource_),
          bytes_at_expire_(other.bytes_at_expire_),
          lease_active_(other.lease_active_),
          lease_at_expire_(other.lease_at_expire_),
          registration_(other.registration_, this) {}

    test_scoped_capability(test_scoped_capability&& other) noexcept
        : expired_count_(std::exchange(other.expired_count_, nullptr)),
          resource_(other.resource_),
          bytes_at_expire_(other.bytes_at_expire_),
          lease_active_(other.lease_active_),
          lease_at_expire_(other.lease_at_expire_),
          registration_(std::move(other.registration_), this) {}

    void use() const {
        registration_.require_active();
    }

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<test_scoped_capability*>(target);
        ++*capability.expired_count_;
        if (capability.resource_ != nullptr && capability.bytes_at_expire_ != nullptr) {
            *capability.bytes_at_expire_ = capability.resource_->in_use_bytes_;
        }
        if (capability.lease_active_ != nullptr && capability.lease_at_expire_ != nullptr) {
            *capability.lease_at_expire_ = *capability.lease_active_;
        }
    }

    int* expired_count_;
    counting_resource* resource_;
    std::size_t* bytes_at_expire_;
    const bool* lease_active_;
    bool* lease_at_expire_;
    ruvia::scoped_capability_registration registration_;
};

ruvia::task<void> await_scoped_operation(ruvia::scoped_operation<void>& operation) {
    co_await std::move(operation);
}

ruvia::task<void> cold_with_payload(std::pmr::string payload_value) {
    if (payload_value.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return;
}

class frame_lease final {
public:
    explicit frame_lease(bool& active) noexcept
        : active_(&active) {
        *active_ = true;
    }
    frame_lease(const frame_lease&) = delete;
    frame_lease& operator=(const frame_lease&) = delete;
    frame_lease(frame_lease&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    frame_lease& operator=(frame_lease&&) = delete;
    ~frame_lease() {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

private:
    bool* active_;
};

ruvia::task<std::size_t> join_and_observe_bytes(
    ruvia::operation_scope& scope, const counting_resource& resource) {
    co_await scope.close_and_join();
    co_return resource.in_use_bytes_;
}

ruvia::task<void> wait_with_payload(std::pmr::string payload_value,
    ruvia::worker_signal& signal, std::promise<void>& started, frame_lease lease_value) {
    static_cast<void>(lease_value);
    started.set_value();
    co_await signal.wait();
    if (payload_value.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
}

ruvia::task<void> wait_then_throw(std::pmr::string payload_value,
    ruvia::worker_signal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    if (!payload_value.empty()) {
        throw std::runtime_error("operation failure");
    }
}

ruvia::task<bool> wait_then_observe_cancellation(std::pmr::string payload_value,
    ruvia::worker_signal& signal, std::promise<void>& started,
    std::atomic_bool& stop_requested) {
    started.set_value();
    co_await signal.wait();
    co_return !payload_value.empty() && stop_requested.load();
}

// A moved-from result can still borrow its owner (as MSVC's PMR string debug
// proxy does). Track those borrows without relying on a library's string layout.
class delivery_borrow final {
public:
    explicit delivery_borrow(std::atomic_uint& borrows) noexcept
        : borrows_(&borrows) {
        borrows_->fetch_add(1);
    }
    delivery_borrow(const delivery_borrow&) = delete;
    delivery_borrow& operator=(const delivery_borrow&) = delete;
    delivery_borrow(delivery_borrow&& other) noexcept
        : delivery_borrow(*other.borrows_) {}
    delivery_borrow& operator=(delivery_borrow&&) = delete;
    ~delivery_borrow() {
        borrows_->fetch_sub(1);
    }

private:
    std::atomic_uint* borrows_;
};

ruvia::task<delivery_borrow> return_delivery_borrow(std::atomic_uint& borrows, bool fail) {
    if (fail) {
        throw std::runtime_error("delivery failed");
    }
    co_return delivery_borrow(borrows);
}

ruvia::task<delivery_borrow> return_cold_delivery_borrow(delivery_borrow borrow) {
    co_return std::move(borrow);
}

struct owned_result final {
    explicit owned_result(std::pmr::memory_resource* resource)
        : bytes_(resource) {}
    std::pmr::string bytes_;
};

ruvia::task<owned_result> wait_and_return(std::pmr::string payload_value, counting_resource& result_resource,
    ruvia::worker_signal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    owned_result result_value(&result_resource);
    result_value.bytes_.assign(2048, 'r');
    if (payload_value.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return result_value;
}

ruvia::task<owned_result> return_with_payload(std::pmr::string payload_value, counting_resource& result_resource) {
    owned_result result_value(&result_resource);
    result_value.bytes_.assign(2048, 's');
    if (payload_value.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return result_value;
}

struct throw_on_second_move final {
    throw_on_second_move(counting_resource& resource, int& moves)
        : bytes_(2048, 'm', &resource),
          moves_(&moves) {}
    throw_on_second_move(const throw_on_second_move&) = delete;
    throw_on_second_move& operator=(const throw_on_second_move&) = delete;
    throw_on_second_move(throw_on_second_move&& other)
        : bytes_(std::move(other.bytes_)),
          moves_(other.moves_) {
        if (++*moves_ == 2) {
            throw std::runtime_error("result extraction failed");
        }
    }
    throw_on_second_move& operator=(throw_on_second_move&&) = delete;

    std::pmr::string bytes_;
    int* moves_;
};

ruvia::task<throw_on_second_move> wait_and_return_throwing_result(std::pmr::string payload_value,
    counting_resource& result_resource, int& moves,
    ruvia::worker_signal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    if (payload_value.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    throw_on_second_move result(result_resource, moves);
    co_return result;
}

ruvia::task<void> await_throwing_result(ruvia::scoped_operation<throw_on_second_move>& operation) {
    static_cast<void>(co_await std::move(operation));
}

ruvia::task<owned_result> await_scoped_result(ruvia::scoped_operation<owned_result>& operation) {
    co_return co_await std::move(operation);
}

ruvia::task<bool> await_scoped_bool(ruvia::scoped_operation<bool>& operation) {
    co_return co_await std::move(operation);
}

struct checker_target final {
    bool alive_{true};
    int calls_{};
    int invalid_calls_{};
    std::size_t allocated_bytes_at_check_{};
    counting_resource* resource_{};
};

void check_target(void* target) noexcept {
    auto& checker = *static_cast<checker_target*>(target);
    ++checker.calls_;
    if (!checker.alive_) {
        ++checker.invalid_calls_;
    }
    if (checker.resource_ != nullptr) {
        checker.allocated_bytes_at_check_ = checker.resource_->in_use_bytes_;
    }
}

ruvia::task<void> observe_check_at_task_start(checker_target& checker, int& checks_at_start) {
    checks_at_start = checker.calls_;
    co_return;
}

ruvia::task<void> complete_immediately() {
    co_return;
}

struct reentrant_retirement_stats final {
    bool child_spawned_{};
    bool pending_operations_at_spawn_{};
    int expirations_at_spawn_{};
    std::size_t bytes_at_spawn_{};
    std::size_t allocations_after_join_{};
    std::size_t bytes_after_join_{};
    int expirations_after_join_{};
};

ruvia::task<void> join_and_observe_task(ruvia::operation_scope& scope,
    counting_resource& resource, int& expired_count, reentrant_retirement_stats& stats) {
    co_await scope.close_and_join();
    stats.allocations_after_join_ = resource.in_use_allocations_;
    stats.bytes_after_join_ = resource.in_use_bytes_;
    stats.expirations_after_join_ = expired_count;
}

struct reentrant_frame_input final {
    reentrant_frame_input(std::pmr::memory_resource* resource, ruvia::task_scope& children,
        ruvia::operation_scope& parent_scope, counting_resource& counting_resource_value,
        int& expired_count, reentrant_retirement_stats& stats)
        : payload_(1024, 'r', resource),
          children_(&children),
          parent_scope_(&parent_scope),
          counting_resource_(&counting_resource_value),
          expired_count_(&expired_count),
          stats_(&stats) {}
    reentrant_frame_input(const reentrant_frame_input&) = delete;
    reentrant_frame_input& operator=(const reentrant_frame_input&) = delete;
    reentrant_frame_input(reentrant_frame_input&& other) noexcept
        : payload_(std::move(other.payload_)),
          children_(std::exchange(other.children_, nullptr)),
          parent_scope_(other.parent_scope_),
          counting_resource_(other.counting_resource_),
          expired_count_(other.expired_count_),
          stats_(other.stats_) {}
    reentrant_frame_input& operator=(reentrant_frame_input&&) = delete;
    ~reentrant_frame_input() noexcept {
        if (children_ == nullptr) {
            return;
        }
        children_->spawn(join_and_observe_task(
            *parent_scope_, *counting_resource_, *expired_count_, *stats_));
        stats_->child_spawned_ = true;
        stats_->pending_operations_at_spawn_ = parent_scope_->has_pending_operations();
        stats_->expirations_at_spawn_ = *expired_count_;
        stats_->bytes_at_spawn_ = counting_resource_->in_use_bytes_;
    }

    std::pmr::string payload_;
    ruvia::task_scope* children_;
    ruvia::operation_scope* parent_scope_;
    counting_resource* counting_resource_;
    int* expired_count_;
    reentrant_retirement_stats* stats_;
};

ruvia::task<void> cold_with_reentrant_input(reentrant_frame_input input) {
    if (input.payload_.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return;
}

struct sibling_drop_input final {
    sibling_drop_input(counting_resource& resource,
        std::unique_ptr<ruvia::scoped_operation<void>>& sibling)
        : payload_(1024, 's', &resource),
          sibling_(&sibling) {}
    sibling_drop_input(const sibling_drop_input&) = delete;
    sibling_drop_input& operator=(const sibling_drop_input&) = delete;
    sibling_drop_input(sibling_drop_input&& other) noexcept
        : payload_(std::move(other.payload_)),
          sibling_(std::exchange(other.sibling_, nullptr)) {}
    sibling_drop_input& operator=(sibling_drop_input&&) = delete;
    ~sibling_drop_input() {
        if (sibling_ != nullptr) {
            sibling_->reset();
        }
    }

    std::pmr::string payload_;
    std::unique_ptr<ruvia::scoped_operation<void>>* sibling_;
};

ruvia::task<void> cold_with_sibling_drop(sibling_drop_input input) {
    if (input.payload_.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return;
}

ruvia::task<void> run_reentrant_cold_drop(const ruvia::worker_handle& worker_value,
    ruvia::operation_scope& parent_scope, counting_resource& resource,
    int& expired_count, reentrant_retirement_stats& stats, bool close_scope = false) {
    ruvia::task_scope children(worker_value);
    {
        auto operation = ruvia::make_scoped_operation(
            parent_scope, cold_with_reentrant_input(reentrant_frame_input(&resource, children, parent_scope, resource, expired_count, stats)));
        if (close_scope) {
            parent_scope.close();
        }
        static_cast<void>(operation);
    }
    co_await children.join();
}

}  // namespace

RUVIA_TEST(scoped_operation_start_check_runs_before_task_body) {
    ruvia::operation_scope scope;
    checker_target checker;
    int checks_at_start = 0;
    auto operation = ruvia::make_scoped_operation(
        scope, observe_check_at_task_start(checker, checks_at_start), &check_target, &checker);
    auto root = ruvia::event_loop_pool({.loop_count_ = 1});
    const auto loop = root.loop(0);
    auto awaited = loop.start(await_scoped_operation(operation));
    root.start();
    awaited.get();
    root.stop();
    root.join();
    RUVIA_CHECK_EQ(checks_at_start, 1);
    RUVIA_CHECK_EQ(checker.calls_, 2);
}

RUVIA_TEST(scoped_operation_start_check_runs_before_cold_frame_destruction) {
    ruvia::operation_scope scope;
    counting_resource resource;
    checker_target checker{.resource_ = &resource};
    {
        std::pmr::string payload_value(1024, 'p', &resource);
        auto operation = ruvia::make_scoped_operation(
            scope, cold_with_payload(std::move(payload_value)), &check_target, &checker);
        RUVIA_CHECK_EQ(checker.calls_, 0);
    }
    RUVIA_CHECK_EQ(checker.calls_, 1);
    RUVIA_CHECK(checker.allocated_bytes_at_check_ > 0U);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
    scope.close();
}

RUVIA_TEST(scoped_operation_close_and_join_releases_frame_before_expiring_capabilities) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::worker_signal signal(worker_value);
    ruvia::operation_scope scope;
    int expired_count = 0;
    counting_resource parameter_resource;
    std::size_t bytes_at_expire = static_cast<std::size_t>(-1);
    bool lease_active = false;
    bool lease_at_expire = true;
    test_scoped_capability capability(scope, expired_count, &parameter_resource, &bytes_at_expire,
        &lease_active, &lease_at_expire);
    static_cast<void>(capability);
    auto started_promise = std::promise<void>();
    auto started = started_promise.get_future();
    // Only the coroutine frame owns payload storage during the observation;
    // a moved-from local string can retain a debug iterator proxy.
    auto operation = ruvia::make_scoped_operation(
        scope, wait_with_payload(std::pmr::string(1024, 'p', &parameter_resource), signal, started_promise, frame_lease(lease_active)));
    auto operation_root = loop.start(await_scoped_operation(operation));
    loops.start();
    started.get();

    auto join_root = loop.start(join_and_observe_bytes(scope, parameter_resource));
    const auto notified = loop.post([&] { signal.notify(); });
    const auto bytes_at_join = join_root.get();
    operation_root.get();

    RUVIA_CHECK(notified == ruvia::post_status::accepted);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(parameter_resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(bytes_at_join, 0U);
    RUVIA_CHECK(!lease_at_expire);
    RUVIA_CHECK(!lease_active);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_exception_releases_frame_before_expiring_capabilities) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::worker_signal signal(worker_value);
    ruvia::operation_scope scope;
    int expired_count = 0;
    counting_resource resource;
    std::size_t bytes_at_expire = static_cast<std::size_t>(-1);
    test_scoped_capability capability(scope, expired_count, &resource, &bytes_at_expire);
    static_cast<void>(capability);
    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    auto operation = ruvia::make_scoped_operation(
        scope, wait_then_throw(std::pmr::string(1024, 'x', &resource), signal, started_promise));
    auto operation_root = loop.start(await_scoped_operation(operation));
    loops.start();
    started.get();

    auto join_root = loop.start(join_and_observe_bytes(scope, resource));
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(join_root.get(), 0U);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { operation_root.get(); }));
    RUVIA_CHECK(notified == ruvia::post_status::accepted);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_cooperative_cancellation_releases_frame_before_expiring_capabilities) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::worker_signal signal(worker_value);
    ruvia::operation_scope scope;
    int expired_count = 0;
    counting_resource resource;
    std::size_t bytes_at_expire = static_cast<std::size_t>(-1);
    test_scoped_capability capability(scope, expired_count, &resource, &bytes_at_expire);
    static_cast<void>(capability);
    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    std::atomic_bool stop_requested{false};
    auto operation = ruvia::make_scoped_operation(scope,
        wait_then_observe_cancellation(std::pmr::string(1024, 'x', &resource), signal, started_promise, stop_requested));
    auto operation_root = loop.start(await_scoped_bool(operation));
    loops.start();
    started.get();

    auto join_root = loop.start(join_and_observe_bytes(scope, resource));
    stop_requested.store(true);
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(join_root.get(), 0U);
    RUVIA_CHECK(operation_root.get());
    RUVIA_CHECK(notified == ruvia::post_status::accepted);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_result_survives_join_after_frame_release) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::worker_signal signal(worker_value);
    ruvia::operation_scope scope;
    int expired_count = 0;
    counting_resource parameter_resource;
    counting_resource result_resource;
    const auto allocations_per_result = [&] {
        const std::pmr::string sample(2048, 'r', &result_resource);
        return result_resource.in_use_allocations_;
    }();
    std::size_t bytes_at_expire = static_cast<std::size_t>(-1);
    test_scoped_capability capability(scope, expired_count, &parameter_resource, &bytes_at_expire);
    static_cast<void>(capability);
    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    auto operation = ruvia::make_scoped_operation(scope,
        wait_and_return(std::pmr::string(1024, 'p', &parameter_resource), result_resource, signal, started_promise));
    auto operation_root = loop.start(await_scoped_result(operation));
    loops.start();
    started.get();

    auto join_root = loop.start(join_and_observe_bytes(scope, parameter_resource));
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(join_root.get(), 0U);
    {
        owned_result result_value = operation_root.get();
        RUVIA_CHECK(notified == ruvia::post_status::accepted);
        RUVIA_CHECK_EQ(expired_count, 1);
        RUVIA_CHECK_EQ(bytes_at_expire, 0U);
        RUVIA_CHECK_EQ(parameter_resource.in_use_allocations_, 0U);
        RUVIA_CHECK_EQ(result_value.bytes_.size(), 2048U);
        RUVIA_CHECK_EQ(result_resource.in_use_allocations_, allocations_per_result);
        {
            ruvia::operation_scope repeated_scope;
            auto repeated = ruvia::make_scoped_operation(repeated_scope,
                return_with_payload(std::pmr::string(1024, 'q', &parameter_resource), result_resource));
            auto repeated_root = loop.start(await_scoped_result(repeated));
            auto repeated_result = repeated_root.get();
            RUVIA_CHECK_EQ(parameter_resource.in_use_allocations_, 0U);
            RUVIA_CHECK_EQ(result_resource.in_use_allocations_, 2 * allocations_per_result);
            RUVIA_CHECK_EQ(result_value.bytes_.find_first_not_of('r'), std::pmr::string::npos);
            RUVIA_CHECK_EQ(repeated_result.bytes_.size(), 2048U);
            RUVIA_CHECK_EQ(repeated_result.bytes_.find_first_not_of('s'), std::pmr::string::npos);
        }
        RUVIA_CHECK_EQ(result_resource.in_use_allocations_, allocations_per_result);
    }
    RUVIA_CHECK_EQ(result_resource.in_use_allocations_, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(root_result_publication_retires_delivery_borrows_before_get_returns) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    loops.start();
    std::atomic_uint borrows{0};
    for (unsigned repeat = 0; repeat != 64; ++repeat) {
        auto root = loop.start(return_delivery_borrow(borrows, false));
        {
            auto result_value = root.get();
            RUVIA_CHECK_EQ(borrows.load(), 1U);
        }
        RUVIA_CHECK_EQ(borrows.load(), 0U);
        auto failed = loop.start(return_delivery_borrow(borrows, true));
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)failed.get(); }));
        RUVIA_CHECK_EQ(borrows.load(), 0U);
    }
    loops.stop();
    loops.join();
}

RUVIA_TEST(root_rejected_launch_retires_cold_input_before_publishing_failure) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_uint borrows{0};
    RUVIA_CHECK(loop.post([] { throw std::runtime_error("stop launch queue"); }) == ruvia::post_status::accepted);
    auto root = loop.start(return_cold_delivery_borrow(delivery_borrow(borrows)));
    RUVIA_CHECK_EQ(borrows.load(), 1U);
    loops.start();
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)root.get(); }));
    RUVIA_CHECK_EQ(borrows.load(), 0U);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { loops.join(); }));
}

RUVIA_TEST(scoped_operation_result_move_failure_releases_frame_before_join) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::worker_signal signal(worker_value);
    ruvia::operation_scope scope;
    counting_resource parameter_resource;
    counting_resource result_resource;
    int moves = 0;
    int expired_count = 0;
    std::size_t bytes_at_expire = std::numeric_limits<std::size_t>::max();
    test_scoped_capability capability(scope, expired_count, &parameter_resource, &bytes_at_expire);
    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    auto operation = ruvia::make_scoped_operation(scope,
        wait_and_return_throwing_result(std::pmr::string(1024, 'p', &parameter_resource),
            result_resource, moves, signal, started_promise));
    auto operation_root = loop.start(await_throwing_result(operation));
    loops.start();
    started.get();

    auto join_root = loop.start(join_and_observe_bytes(scope, parameter_resource));
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(join_root.get(), 0U);
    bool failed_at_extraction = false;
    try {
        operation_root.get();
    } catch (const std::runtime_error& error) {
        failed_at_extraction = std::string_view(error.what()) == "result extraction failed";
    }
    RUVIA_CHECK(failed_at_extraction);
    RUVIA_CHECK(notified == ruvia::post_status::accepted);
    RUVIA_CHECK_EQ(moves, 2);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(parameter_resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(result_resource.in_use_allocations_, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_scope_close_reclaims_cold_frame_before_capability_expiry) {
    ruvia::operation_scope scope;
    counting_resource resource;
    int expired_count = 0;
    std::size_t bytes_at_expire = std::numeric_limits<std::size_t>::max();
    test_scoped_capability capability(scope, expired_count, &resource, &bytes_at_expire);
    auto operation = ruvia::make_scoped_operation(
        scope, cold_with_payload(std::pmr::string(1024, 'p', &resource)));
    RUVIA_CHECK(resource.in_use_allocations_ != 0);
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
    RUVIA_CHECK(!scope.has_pending_operations());
    // The expired public operation may remain alive after its frame is gone.
    static_cast<void>(operation);
}

RUVIA_TEST(scoped_operation_expiration_clears_borrowed_start_check) {
    ruvia::operation_scope scope;
    counting_resource resource;
    checker_target checker{.resource_ = &resource};
    {
        auto operation = ruvia::make_scoped_operation(scope,
            cold_with_payload(std::pmr::string(1024, 'p', &resource)), &check_target, &checker);
        RUVIA_CHECK(resource.in_use_bytes_ > 0U);
        scope.close();
        RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
        checker.alive_ = false;

        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        auto expired_root = loop.start(await_scoped_operation(operation));
        loops.start();
        bool rejected_as_expired = false;
        try {
            expired_root.get();
        } catch (const std::logic_error&) {
            rejected_as_expired = true;
        }
        RUVIA_CHECK(rejected_as_expired);
        loops.stop();
        loops.join();
    }
    RUVIA_CHECK_EQ(checker.calls_, 1);
    RUVIA_CHECK_EQ(checker.invalid_calls_, 0);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
}

RUVIA_TEST(scoped_operation_created_after_scope_close_discards_cold_frame) {
    ruvia::operation_scope scope;
    counting_resource resource;
    checker_target checker{.resource_ = &resource};
    scope.close();
    checker.alive_ = false;

    auto operation = ruvia::make_scoped_operation(scope,
        cold_with_payload(std::pmr::string(1024, 'p', &resource)), &check_target, &checker);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);

    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    auto expired_root = loop.start(await_scoped_operation(operation));
    loops.start();
    bool rejected_as_expired = false;
    try {
        expired_root.get();
    } catch (const std::logic_error&) {
        rejected_as_expired = true;
    }
    RUVIA_CHECK(rejected_as_expired);
    loops.stop();
    loops.join();

    RUVIA_CHECK_EQ(checker.calls_, 0);
    RUVIA_CHECK_EQ(checker.invalid_calls_, 0);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
}

RUVIA_TEST(scoped_operation_cold_frame_drop_allows_reentrant_parent_join) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::operation_scope parent_scope;
    counting_resource resource;
    int expired_count = 0;
    std::size_t bytes_at_expire = std::numeric_limits<std::size_t>::max();
    test_scoped_capability capability(parent_scope, expired_count, &resource, &bytes_at_expire);
    static_cast<void>(capability);
    reentrant_retirement_stats stats;

    auto root = loop.start(run_reentrant_cold_drop(
        worker_value, parent_scope, resource, expired_count, stats));
    loops.start();
    root.get();

    RUVIA_CHECK(stats.child_spawned_);
    RUVIA_CHECK(stats.pending_operations_at_spawn_);
    RUVIA_CHECK_EQ(stats.expirations_at_spawn_, 0);
    RUVIA_CHECK(stats.bytes_at_spawn_ > 0U);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(stats.allocations_after_join_, 0U);
    RUVIA_CHECK_EQ(stats.bytes_after_join_, 0U);
    RUVIA_CHECK_EQ(stats.expirations_after_join_, 1);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
    RUVIA_CHECK(!parent_scope.has_pending_operations());
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_completion_clears_borrowed_start_check) {
    ruvia::operation_scope scope;
    checker_target checker;
    {
        auto operation = ruvia::make_scoped_operation(
            scope, complete_immediately(), &check_target, &checker);
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        auto first_root = loop.start(await_scoped_operation(operation));
        loops.start();
        first_root.get();
        RUVIA_CHECK_EQ(checker.calls_, 2);
        checker.alive_ = false;

        auto repeated_root = loop.start(await_scoped_operation(operation));
        bool rejected_as_completed = false;
        try {
            repeated_root.get();
        } catch (const std::logic_error&) {
            rejected_as_completed = true;
        }
        RUVIA_CHECK(rejected_as_completed);
        loops.stop();
        loops.join();
    }
    RUVIA_CHECK_EQ(checker.calls_, 2);
    RUVIA_CHECK_EQ(checker.invalid_calls_, 0);
    scope.close();
}

RUVIA_TEST(scoped_operation_rejects_empty_task_without_starting) {
    ruvia::operation_scope scope;
    auto source = complete_immediately();
    auto retained = std::move(source);
    {
        auto operation = ruvia::make_scoped_operation(scope, std::move(source));
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        auto first_root = loop.start(await_scoped_operation(operation));
        auto repeated_root = loop.start(await_scoped_operation(operation));
        loops.start();
        for (auto* root : {&first_root, &repeated_root}) {
            bool rejected = false;
            try {
                root->get();
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }
        RUVIA_CHECK(scope.has_pending_operations());
        loops.stop();
        loops.join();
    }
    RUVIA_CHECK(!scope.has_pending_operations());
    static_cast<void>(retained);
}

RUVIA_TEST(scoped_operation_scope_drain_allows_reentrant_join_after_all_frames_release) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    ruvia::operation_scope scope;
    counting_resource resource;
    int expired_count = 0;
    std::size_t bytes_at_expire = std::numeric_limits<std::size_t>::max();
    test_scoped_capability capability(scope, expired_count, &resource, &bytes_at_expire);
    reentrant_retirement_stats stats;
    auto root = loop.start(run_reentrant_cold_drop(
        worker_value, scope, resource, expired_count, stats, true));
    loops.start();
    root.get();
    RUVIA_CHECK(stats.child_spawned_);
    RUVIA_CHECK(stats.pending_operations_at_spawn_);
    RUVIA_CHECK_EQ(stats.expirations_at_spawn_, 0);
    RUVIA_CHECK(stats.bytes_at_spawn_ > 0U);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(stats.allocations_after_join_, 0U);
    RUVIA_CHECK_EQ(stats.bytes_after_join_, 0U);
    RUVIA_CHECK_EQ(stats.expirations_after_join_, 1);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK(!scope.has_pending_operations());
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_join_rescans_after_frame_cleanup_drops_a_sibling) {
    ruvia::operation_scope scope;
    counting_resource resource;
    int expired_count = 0;
    std::size_t bytes_at_expire = std::numeric_limits<std::size_t>::max();
    test_scoped_capability capability(scope, expired_count, &resource, &bytes_at_expire);
    std::unique_ptr<ruvia::scoped_operation<void>> sibling(
        new auto(ruvia::make_scoped_operation(scope,
            cold_with_payload(std::pmr::string(1024, 'p', &resource)))));
    const auto sibling_allocations = resource.in_use_allocations_;
    auto operation = ruvia::make_scoped_operation(scope,
        cold_with_sibling_drop(sibling_drop_input(resource, sibling)));
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 2 * sibling_allocations);
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    auto joined = loop.start(join_and_observe_bytes(scope, resource));
    loops.start();
    RUVIA_CHECK_EQ(joined.get(), 0U);
    RUVIA_CHECK(sibling == nullptr);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(bytes_at_expire, 0U);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
    RUVIA_CHECK(!scope.has_pending_operations());
    loops.stop();
    loops.join();
    static_cast<void>(operation);
}

RUVIA_TEST(scoped_capabilities_copy_move_and_unregister_without_expiring_other_owners) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    test_scoped_capability source(scope, expired_count);
    test_scoped_capability moved(std::move(source));
    {
        test_scoped_capability discarded(moved);
    }
    test_scoped_capability copied(moved);
    RUVIA_CHECK_EQ(expired_count, 0);
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 2);

    test_scoped_capability expired_copy(copied);
    for (const auto* capability : {&source, &moved, &copied, &expired_copy}) {
        bool rejected = false;
        try {
            capability->use();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 2);
}

RUVIA_TEST(scoped_capability_cleanup_can_destroy_its_owner_and_a_sibling) {
    struct capability_owner final {
        capability_owner(ruvia::operation_scope& scope, counting_resource& resource,
            int& expired_count, bool& expired_before_cleanup,
            std::unique_ptr<capability_owner>* self,
            std::unique_ptr<capability_owner>* sibling)
            : payload_(1024, 'p', &resource),
              expired_count_(expired_count),
              expired_before_cleanup_(expired_before_cleanup),
              self_(self),
              sibling_(sibling),
              registration_(scope, this, &capability_owner::expire) {}

        static void expire(void* target) noexcept {
            auto& owner_value = *static_cast<capability_owner*>(target);
            ++owner_value.expired_count_;
            try {
                owner_value.registration_.require_active();
            } catch (const std::logic_error&) {
                owner_value.expired_before_cleanup_ = true;
            }
            auto* self = owner_value.self_;
            auto* sibling = owner_value.sibling_;
            if (sibling != nullptr) {
                sibling->reset();
            }
            if (self != nullptr) {
                self->reset();
            }
        }

        std::pmr::string payload_;
        int& expired_count_;
        bool& expired_before_cleanup_;
        std::unique_ptr<capability_owner>* self_;
        std::unique_ptr<capability_owner>* sibling_;
        ruvia::scoped_capability_registration registration_;
    };

    ruvia::operation_scope scope;
    counting_resource resource;
    int expired_count = 0;
    bool expired_before_cleanup = false;
    std::unique_ptr<capability_owner> self;
    auto sibling = std::make_unique<capability_owner>(
        scope, resource, expired_count, expired_before_cleanup, nullptr, nullptr);
    self = std::make_unique<capability_owner>(
        scope, resource, expired_count, expired_before_cleanup, &self, &sibling);
    scope.close();
    RUVIA_CHECK(self == nullptr);
    RUVIA_CHECK(sibling == nullptr);
    RUVIA_CHECK(expired_before_cleanup);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(resource.in_use_allocations_, 0U);
    RUVIA_CHECK_EQ(resource.in_use_bytes_, 0U);
}
