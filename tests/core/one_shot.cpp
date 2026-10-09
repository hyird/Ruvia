#include "ruvia/core/one_shot.h"

#include <chrono>
#include <concepts>
#include <memory>
#include <memory_resource>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_task_fixture.h"

namespace {

class throwing_move final {
public:
    explicit throwing_move(int value) noexcept
        : value_(value) {}

    throwing_move(const throwing_move&) = delete;
    throwing_move& operator=(const throwing_move&) = delete;
    // This fixture intentionally models a move that can throw.
    throwing_move(throwing_move&& other) noexcept(false) {
        if (throw_on_move_) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline bool throw_on_move_{false};

private:
    int value_{0};
};

ruvia::task<void> wait_for_value(ruvia::one_shot_receiver<int>& receiver, int expected, bool& success) {
    const auto result_value = co_await receiver.wait_for(std::chrono::seconds(1));
    success = result_value.status() == ruvia::worker_wait_status::value && result_value.has_value() &&
              result_value.value() == expected;
}

ruvia::task<void> wait_for_worker_stopping(ruvia::worker_runtime_context& runtime,
    ruvia::one_shot_receiver<int>& receiver, bool& success) {
    asio::post(runtime.io_context(), [&runtime] { runtime.close(); });
    const auto result_value = co_await receiver.wait();
    success = result_value.status() == ruvia::worker_wait_status::worker_stopping && !result_value.has_value();
}

ruvia::task<void> wait_until_closed(ruvia::one_shot_receiver<int>& receiver, bool& success) {
    const auto result_value = co_await receiver.wait_for(std::chrono::seconds(1));
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<ruvia::worker_wait_result<int>> make_cold_wait_after_receiver_close(
    ruvia::worker_handle worker_value, bool timed) {
    auto [completion, receiver] = ruvia::make_one_shot<int>(std::move(worker_value));
    if (timed) {
        return receiver.wait_for(std::chrono::seconds(1));
    }
    return receiver.wait();
}

ruvia::task<void> verify_cold_receiver_tasks(ruvia::task<ruvia::worker_wait_result<int>> wait,
    ruvia::task<ruvia::worker_wait_result<int>> timed_wait, bool& success) {
    const auto cold_closed = co_await std::move(wait);
    const auto timed_cold_closed = co_await std::move(timed_wait);
    success = cold_closed.status() == ruvia::worker_wait_status::closed &&
              timed_cold_closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> exercise(ruvia::worker_handle worker_value, bool& success) {
    {
        auto [completion, receiver] = ruvia::make_one_shot<throwing_move>(worker_value);
        bool move_failed = false;
        throwing_move::throw_on_move_ = true;
        try {
            static_cast<void>(completion.complete(throwing_move(5)));
        } catch (const std::runtime_error&) {
            move_failed = true;
        }
        throwing_move::throw_on_move_ = false;
        if (!move_failed || !completion.complete(throwing_move(6)).accepted()) {
            co_return;
        }
        const auto result_value = co_await receiver.wait();
        if (!result_value.has_value() || result_value.value().value() != 6) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        const auto completed = completion.complete(7);
        const auto duplicate = completion.complete(8);
        if (!completed.accepted() ||
            duplicate.status() != ruvia::one_shot_complete_status::already_completed ||
            duplicate.rejected() == nullptr || *duplicate.rejected() != 8) {
            co_return;
        }
        const auto result_value = co_await receiver.wait();
        if (!result_value.has_value() || result_value.value() != 7) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        const auto timeout = co_await receiver.wait_for(std::chrono::milliseconds(1));
        if (timeout.status() != ruvia::worker_wait_status::timed_out ||
            !completion.complete(9).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.has_value() || late.value() != 9) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        static_cast<void>(worker_value.post([&source] { source.request_stop(); }));
        const auto cancelled = co_await receiver.wait_for(std::chrono::seconds(1), source.token());
        if (cancelled.status() != ruvia::worker_wait_status::cancelled ||
            !completion.complete(11).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.has_value() || late.value() != 11) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        source.request_stop();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        const auto cancelled = co_await receiver.wait_for(std::chrono::seconds(1), source.token());
        if (cancelled.status() != ruvia::worker_wait_status::cancelled ||
            !completion.complete(12).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.has_value() || late.value() != 12) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        receiver.close();
        const auto closed = co_await receiver.wait();
        const auto rejected = completion.complete(10);
        if (closed.status() != ruvia::worker_wait_status::closed ||
            rejected.status() != ruvia::one_shot_complete_status::receiver_closed ||
            rejected.rejected() == nullptr || *rejected.rejected() != 10) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        bool pending_closed = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn(wait_until_closed(receiver, pending_closed));
        bool duplicate_rejected = false;
        try {
            static_cast<void>(co_await receiver.wait());
        } catch (const std::logic_error&) {
            duplicate_rejected = true;
        }
        receiver.close();
        co_await scope.join();
        if (!pending_closed || !duplicate_rejected ||
            completion.complete(13).status() != ruvia::one_shot_complete_status::receiver_closed) {
            co_return;
        }
    }

    auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
    auto active_receiver = std::move(receiver);
    ruvia::task_scope scope(worker_value);
    scope.spawn(wait_for_value(active_receiver, 42, success));
    const bool completed = completion.complete(42).accepted();
    if (!completed) {
        active_receiver.close();
    }
    scope.request_stop();
    co_await scope.join();
    success = success && completed;
}

}  // namespace

int main() {
    bool success = false;
    bool cold_receiver_tasks_safe = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto cold_wait = make_cold_wait_after_receiver_close(worker_value, false);
        auto timed_cold_wait = make_cold_wait_after_receiver_close(worker_value, true);
        ruvia::test::run_worker_tasks(runtime, exercise(worker_value, success),
            verify_cold_receiver_tasks(std::move(cold_wait), std::move(timed_cold_wait),
                cold_receiver_tasks_safe));
        runtime.detach();
    }

    bool worker_stopping = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        ruvia::test::run_worker_tasks(runtime, wait_for_worker_stopping(runtime, receiver, worker_stopping));
        runtime.detach();
    }

    bool cross_thread_value = false;
    bool cross_thread_completed = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        std::binary_semaphore receiver_scheduled{0};
        asio::post(io_context, [&receiver_scheduled] { receiver_scheduled.release(); });
        std::thread completer([&] {
            receiver_scheduled.acquire();
            cross_thread_completed = completion.complete(77).accepted();
        });
        ruvia::test::run_worker_tasks(runtime, wait_for_value(receiver, 77, cross_thread_value));
        completer.join();
        runtime.detach();
    }
    const bool all_passed = success && cold_receiver_tasks_safe && worker_stopping && cross_thread_value &&
                            cross_thread_completed;
    return all_passed ? 0 : 1;
}
