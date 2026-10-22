#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_runtime_context.h"

#include "test_harness.h"
#include "worker_task_fixture.h"

namespace worker_resume_test {

// Suspends and has the worker resume the coroutine, asked from the worker
// itself or from a foreign thread. The step is recorded negated once the
// request returned, which is before the resumption when it is never inline.
struct deferred_resume final {
    const ruvia::worker_handle& worker_;
    std::vector<int>& order_;
    int step_;
    bool foreign_{false};

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const {
        if (foreign_) {
            std::jthread([this, continuation] { worker_.defer_resume(continuation); }).join();
        } else {
            worker_.defer_resume(continuation);
        }
        order_.push_back(-step_);
    }
    void await_resume() const noexcept {}
};

ruvia::task<void> resume_twice(const ruvia::worker_handle& worker, std::vector<int>& order, int first, bool& on_worker) {
    order.push_back(first);
    co_await deferred_resume{worker, order, first};
    on_worker = on_worker && worker.is_current();
    order.push_back(first + 1);
    co_await deferred_resume{worker, order, first + 2, true};
    on_worker = on_worker && worker.is_current();
    order.push_back(first + 3);
}

[[nodiscard]] bool before(const std::vector<int>& order, int first, int second) {
    const auto left = std::ranges::find(order, first);
    const auto right = std::ranges::find(order, second);
    return left != order.end() && right != order.end() && left < right;
}

}  // namespace worker_resume_test

// A deferred resumption runs on the worker in a later turn, never inside the
// request, whether it was asked on the worker or from another thread, and
// requests from the worker's own thread resume in request order -- under the
// default event loop and the single-owner one production workers use.
RUVIA_TEST(worker_handle_defers_resumption_to_its_worker) {
    using worker_resume_test::before;
    for (const int hint : {ASIO_CONCURRENCY_HINT_DEFAULT, ASIO_CONCURRENCY_HINT_UNSAFE_IO}) {
        asio::io_context context(hint);
        ruvia::worker_runtime_context runtime(context, 1);
        const auto worker_value = runtime.handle();
        std::vector<int> order;
        bool on_worker = true;
        ruvia::test::run_worker_tasks(runtime, worker_resume_test::resume_twice(worker_value, order, 10, on_worker),
            worker_resume_test::resume_twice(worker_value, order, 20, on_worker));
        RUVIA_CHECK(on_worker);
        RUVIA_CHECK_EQ(order.size(), std::size_t{10});
        for (const int first : {10, 20}) {
            RUVIA_CHECK(before(order, -first, first + 1));
            RUVIA_CHECK(before(order, -(first + 2), first + 3));
        }
        RUVIA_CHECK(before(order, 11, 21));
    }
}
