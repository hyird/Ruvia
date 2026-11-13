#include "ruvia/core/worker_wait.h"

#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"

#include "worker_task_fixture.h"

namespace ruvia::test::worker_wait_tests {

// A worker-bound counting semaphore composed only from public building
// blocks: the shared state owns the wait slot and its shutdown registration,
// and each acquire owns a worker_single_wait_awaiter.
struct semaphore_state final {
    semaphore_state(worker_ref worker_value, std::size_t permits) noexcept
        : worker_(std::move(worker_value)),
          permits_(permits) {}

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }

    [[nodiscard]] worker_wait_slot<std::monostate, semaphore_state>& wait_slot() noexcept {
        return wait_slot_;
    }

    void worker_stopping() noexcept {
        auto lock = lock_state();
        stopping_ = true;
        wait_slot_.complete(worker_wait_status::worker_stopping);
    }

    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    std::size_t permits_;
    bool stopping_{false};
    worker_wait_slot<std::monostate, semaphore_state> wait_slot_;
};

static_assert(worker_wait_state<semaphore_state, std::monostate>);
static_assert(worker_shutdown_observer<semaphore_state>);

// The concept names the shutdown registration member, not just the observer.
struct unregistered_state final {
    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] std::unique_lock<std::mutex> lock_state();
    [[nodiscard]] worker_wait_slot<std::monostate, unregistered_state>& wait_slot() noexcept;
    void worker_stopping() noexcept;
    worker_shutdown_registration registration_;
};
static_assert(!worker_wait_state<unregistered_state, std::monostate>);

class worker_semaphore final {
public:
    class acquire_operation final {
    public:
        acquire_operation(acquire_operation&&) noexcept = default;
        acquire_operation(const acquire_operation&) = delete;
        acquire_operation& operator=(const acquire_operation&) = delete;
        acquire_operation& operator=(acquire_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("semaphore acquire must run on its worker");
            auto& state = wait_.state();
            auto lock = state.lock_state();
            if (state.stopping_) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (state.permits_ != 0) {
                --state.permits_;
                wait_.complete_ready(worker_wait_result<std::monostate>::with_value(std::monostate{}));
                return true;
            }
            return wait_.complete_or_publish("semaphore supports one pending acquire");
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<std::monostate> await_resume() {
            return wait_.take_result();
        }

    private:
        friend class worker_semaphore;

        acquire_operation(std::shared_ptr<semaphore_state> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<std::monostate, semaphore_state> wait_;
    };

    worker_semaphore(const worker_ref& worker_value, std::size_t permits)
        : state_(make_worker_wait_state<semaphore_state>(worker_value, permits)) {}

    [[nodiscard]] acquire_operation acquire(operation_options options = {}) {
        return acquire_operation(state_, std::move(options));
    }

    // Any thread: hands the permit to the pending acquire, or banks it.
    void release() {
        auto lock = state_->lock_state();
        if (state_->wait_slot().occupied()) {
            state_->wait_slot().complete(worker_wait_result<std::monostate>::with_value(std::monostate{}));
        } else {
            ++state_->permits_;
        }
    }

private:
    std::shared_ptr<semaphore_state> state_;
};

struct semaphore_outcomes final {
    bool permit_{false};
    bool nonpositive_rejected_{false};
    bool timed_out_{false};
    bool deadline_passed_{false};
    bool cancelled_{false};
    bool released_{false};
};

task<void> exercise_semaphore(
    const worker_ref& worker_value, std::binary_semaphore& released, semaphore_outcomes& outcomes) {
    worker_semaphore semaphore(worker_value, 1);
    outcomes.permit_ = (co_await semaphore.acquire()).has_value();
    try {
        static_cast<void>(semaphore.acquire({.timeout_ = std::chrono::milliseconds(0)}));
    } catch (const std::invalid_argument&) {
        outcomes.nonpositive_rejected_ = true;
    }
    outcomes.timed_out_ =
        (co_await semaphore.acquire({.timeout_ = std::chrono::milliseconds(5)})).status() == worker_wait_status::timed_out;
    outcomes.deadline_passed_ =
        (co_await semaphore.acquire({.timeout_ = operation_timeout(std::chrono::steady_clock::time_point{})})).status() ==
        worker_wait_status::timed_out;

    stop_source source;
    static_cast<void>(worker_value.post([&source] { source.request_stop(); }));
    outcomes.cancelled_ =
        (co_await semaphore.acquire({.timeout_ = std::chrono::seconds(30), .stop_token_ = source.token()})).status() ==
        worker_wait_status::cancelled;

    std::thread releaser([&] {
        released.acquire();
        semaphore.release();
    });
    static_cast<void>(worker_value.post([&released] { released.release(); }));
    const auto handed_off = co_await semaphore.acquire({.timeout_ = std::chrono::seconds(30)});
    releaser.join();
    outcomes.released_ = handed_off.has_value();
}

task<void> wait_through_stop(event_loop_attachment& attachment, worker_semaphore& semaphore, bool& stopping) {
    asio::post(attachment.loop().executor(), [&attachment] { attachment.stop(); });
    stopping = (co_await semaphore.acquire()).status() == worker_wait_status::worker_stopping;
}

TEST(Ruvia, PublicWorkerWaitBlocksComposeBoundSemaphore) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    std::binary_semaphore released{0};
    semaphore_outcomes outcomes;
    run_worker_tasks(attachment, exercise_semaphore(worker_value, released, outcomes));
    EXPECT_TRUE(outcomes.permit_);
    EXPECT_TRUE(outcomes.nonpositive_rejected_);
    EXPECT_TRUE(outcomes.timed_out_);
    EXPECT_TRUE(outcomes.deadline_passed_);
    EXPECT_TRUE(outcomes.cancelled_);
    EXPECT_TRUE(outcomes.released_);
}

TEST(Ruvia, PublicWorkerWaitBlocksObserveWorkerStopping) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    worker_semaphore semaphore(worker_value, 0);
    bool stopping = false;
    run_worker_tasks(attachment, wait_through_stop(attachment, semaphore, stopping));
    EXPECT_TRUE(stopping);
    EXPECT_THROW(worker_semaphore(worker_value, 0), std::runtime_error);
}

struct counting_observer final {
    void worker_stopping() noexcept {
        ++stopping_;
    }
    void worker_stopping_complete() noexcept {
        complete_after_stopping_ = stopping_ == 1;
    }

    worker_shutdown_registration registration_;
    int stopping_{0};
    bool complete_after_stopping_{false};
};

TEST(Ruvia, WorkerShutdownRegistrationRunsBothPhasesOnce) {
    auto observer = std::make_shared<counting_observer>();
    auto dropped = std::make_shared<counting_observer>();
    {
        asio::io_context io_context;
        auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        EXPECT_EQ(worker_value.register_shutdown(observer, &counting_observer::registration_),
            worker_registration_status::registered);
        EXPECT_THROW(static_cast<void>(worker_value.register_shutdown(observer, &counting_observer::registration_)),
            std::logic_error);
        EXPECT_EQ(worker_value.register_shutdown(dropped, &counting_observer::registration_),
            worker_registration_status::registered);
        // Destroying the owner unlinks it before the worker stops.
        dropped.reset();
        attachment.stop();
        io_context.run();
        EXPECT_EQ(worker_value.register_shutdown(std::make_shared<counting_observer>(), &counting_observer::registration_),
            worker_registration_status::worker_stopping);
    }
    EXPECT_EQ(observer->stopping_, 1);
    EXPECT_TRUE(observer->complete_after_stopping_);
    EXPECT_EQ(worker_ref{}.register_shutdown(observer, &counting_observer::registration_),
        worker_registration_status::worker_stopping);
}

TEST(Ruvia, WorkerCompletionLaneBypassesFullQueue) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    int ran = 0;
    ASSERT_TRUE(worker_value.post([&ran] { ++ran; }).accepted());
    ASSERT_EQ(worker_value.post([&ran] { ++ran; }).status(), post_status::queue_full);
    EXPECT_TRUE(worker_value.defer_completion([&ran] { ran += 10; }));
    EXPECT_THROW(static_cast<void>(worker_value.defer_completion(move_only_function<void()>{})), std::invalid_argument);
    io_context.run_one();
    io_context.poll();
    EXPECT_EQ(ran, 11);
    attachment.stop();
    io_context.run();
    bool destroyed = false;
    struct destruction_flag final {
        bool* destroyed_;
        destruction_flag(const destruction_flag&) = delete;
        destruction_flag(destruction_flag&& other) noexcept
            : destroyed_(std::exchange(other.destroyed_, nullptr)) {}
        explicit destruction_flag(bool& destroyed) noexcept
            : destroyed_(&destroyed) {}
        ~destruction_flag() {
            if (destroyed_ != nullptr) {
                *destroyed_ = true;
            }
        }
    };
    EXPECT_FALSE(worker_value.defer_completion([flag = destruction_flag(destroyed)] {}));
    EXPECT_TRUE(destroyed);
    EXPECT_FALSE(worker_ref{}.defer_completion([] {}));
}

TEST(Ruvia, WorkerWaitResultFactoriesFixCategory) {
    const auto value = worker_wait_result<int>::with_value(7);
    EXPECT_EQ(value.status(), worker_wait_status::value);
    EXPECT_EQ(value.value(), 7);
    const auto timed_out = worker_wait_result<int>::with_status(worker_wait_status::timed_out);
    EXPECT_EQ(timed_out.status(), worker_wait_status::timed_out);
    EXPECT_FALSE(timed_out.has_value());
    EXPECT_THROW(static_cast<void>(worker_wait_result<int>::with_status(worker_wait_status::value)), std::invalid_argument);
}

TEST(Ruvia, BlockingResultFactoriesFixCategory) {
    auto value = blocking_result<int>::with_value(5);
    EXPECT_TRUE(value.completed());
    EXPECT_EQ(std::move(value).take_value(), 5);
    const auto void_value = blocking_result<void>::with_value();
    EXPECT_TRUE(void_value.completed());
    const auto failed = blocking_result<int>::with_error(std::make_exception_ptr(std::runtime_error("failed")));
    EXPECT_TRUE(failed.failed());
    EXPECT_EQ(failed.status(), blocking_status::completed);
    EXPECT_THROW(static_cast<void>(failed.value()), std::runtime_error);
    const auto rejected = blocking_result<int>::with_status(blocking_status::queue_full);
    EXPECT_EQ(rejected.status(), blocking_status::queue_full);
    EXPECT_THROW(static_cast<void>(rejected.value()), blocking_operation_rejected);
    EXPECT_THROW(static_cast<void>(blocking_result<int>::with_status(blocking_status::completed)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(blocking_result<int>::with_error(nullptr)), std::invalid_argument);
}

}  // namespace ruvia::test::worker_wait_tests
