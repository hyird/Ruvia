#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include <asio/io_context.hpp>

#include "event_loop.h"
#include "operation_scope.h"
#include "task_scope.h"
#include "worker_task_fixture.h"

namespace {
thread_local ruvia::detail::operation_scope* expire_on_allocate{};
thread_local bool fail_allocation{};
}  // namespace

// Intercept scalar operator new only in this executable, keeping Core's mimalloc linkage.
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t bytes) {
    if (std::exchange(fail_allocation, false)) {
        throw std::bad_alloc();
    }
    if (auto* scope = std::exchange(expire_on_allocate, nullptr)) {
        scope->close();
    }
    return __real__Znwm(bytes);
}

namespace ruvia::test::scoped_spawn_allocation_tests {

task<void> count_run(std::shared_ptr<int> lifetime, int& runs) {
    static_cast<void>(lifetime);
    ++runs;
    co_return;
}

task<void> verify_spawn_rollback(const worker_ref& worker) {
    task_scope children(worker);
    detail::operation_scope operations;
    int runs = 0;
    auto lifetime = std::make_shared<int>(0);
    auto operation = operations.attach(count_run(lifetime, runs));
    expire_on_allocate = &operations;
    bool expired = false;
    try {
        children.spawn(std::move(operation));
    } catch (const std::logic_error&) {
        expired = true;
    }
    expire_on_allocate = nullptr;
    EXPECT_TRUE(expired);
    EXPECT_TRUE(children.empty());
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(lifetime.use_count(), 1);

    auto allocation_failure = count_run(lifetime, runs);
    fail_allocation = true;
    bool failed = false;
    try {
        children.spawn(std::move(allocation_failure));
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    fail_allocation = false;
    EXPECT_TRUE(failed);
    EXPECT_TRUE(children.empty());
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(lifetime.use_count(), 1);

    children.spawn(count_run(lifetime, runs));
    co_await children.join();
    EXPECT_TRUE(children.empty());
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(lifetime.use_count(), 1);
}

}  // namespace ruvia::test::scoped_spawn_allocation_tests

TEST(Ruvia, TaskScopeRollsBackSpawnBeforeStartingChild) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    ruvia::test::run_worker_tasks(attachment, ruvia::test::scoped_spawn_allocation_tests::verify_spawn_rollback(worker));
}
