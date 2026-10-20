#include "ruvia/core/event_loop_pool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/runtime_lifecycle.h"
#include "ruvia/core/worker_selection.h"

#include "event_loop_state.h"
#include "first_failure.h"

namespace ruvia {
namespace {

std::size_t default_loop_count() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

}  // namespace

struct event_loop_pool::impl_type final {
    explicit impl_type(event_loop_pool_options options) {
        const auto count = options.loop_count_ == 0 ? default_loop_count() : options.loop_count_;
        if (options.queue_capacity_ == 0) {
            throw std::invalid_argument("event loop queue capacity must be greater than zero");
        }
        loops_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            loops_.push_back(std::allocate_shared<detail::event_loop_state>(
                std::pmr::polymorphic_allocator<detail::event_loop_state>(process_resource()),
                options.queue_capacity_));
            loops_.back()->install_shutdown_hook();
        }
    }

    void bind_callbacks(const std::shared_ptr<impl_type>& self) {
        const std::weak_ptr<impl_type> weak_owner = self;
        for (const auto& loop : loops_) {
            loop->failure_sink_ = [weak_owner](std::exception_ptr failure) {
                if (const auto owner = weak_owner.lock()) {
                    owner->report_failure(std::move(failure));
                } else {
                    report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
                }
            };
            const std::weak_ptr<detail::event_loop_state> weak_loop = loop;
            auto* const stable_runtime = &loop->owned_runtime();
            stable_runtime->configure({
                .stop_admission_ = [weak_loop, stable_runtime] {
                    if (const auto locked_loop = weak_loop.lock()) {
                        locked_loop->stop();
                    } else {
                        // Construction rollback can destroy an unpublished
                        // state before its core owner drains this control.
                        stable_runtime->finalize();
                    } },
                .failure_ = [weak_owner](std::exception_ptr failure) noexcept {
                    if (const auto owner = weak_owner.lock()) {
                        owner->report_failure(std::move(failure));
                    } else {
                        report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
                    } },
            });
        }
    }

    // Keeps the first failure for join() while the pool observes its loops;
    // join() rethrows only that one, so distinct later failures are reported.
    // After pool retirement nobody can rethrow, so failures are reported.
    void record_failure(std::exception_ptr failure) noexcept {
        std::exception_ptr first;
        {
            const std::lock_guard lock(failure_mutex_);
            if (!observed_) {
                first = nullptr;
            } else if (failure_.record(failure, false)) {
                return;
            } else {
                first = failure_.first();
            }
        }
        if (first == nullptr) {
            report_unhandled_failure("event loop failure after pool retirement", failure);
        } else if (!equivalent_failures(failure, first)) {
            report_unhandled_failure("additional event loop failure", failure);
        }
    }

    void report_failure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        record_failure(std::move(failure));
        stop();
    }

    void stop() noexcept {
        if (!lifecycle_.request_stop()) {
            return;
        }
        for (const auto& loop : loops_) {
            loop->stop();
        }
    }

    [[nodiscard]] bool start() noexcept {
        return lifecycle_.start();
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return lifecycle_.state() != runtime_lifecycle::state_type::running;
    }

    void complete_stop() noexcept {
        lifecycle_.complete_stop();
    }

    [[nodiscard]] std::exception_ptr take_failure() noexcept {
        const std::lock_guard lock(failure_mutex_);
        return failure_.take_unobserved();
    }

    void detach_pool_observer() noexcept {
        std::exception_ptr remaining;
        {
            const std::lock_guard lock(failure_mutex_);
            observed_ = false;
            remaining = failure_.take_unobserved();
        }
        if (remaining) {
            report_unhandled_failure("event loop failure during pool retirement", remaining);
        }
    }

    // A pool stop (requested directly or by a failing loop) closes the owner
    // lifecycle before it stops any loop, and can win a cold loop's launch
    // transition. Launching then ends; join() reports the stop's outcome.
    [[nodiscard]] bool launch(worker_runtime& runtime) {
        return !stop_requested() && runtime.start();
    }

    void join_loops() {
        for (const auto& loop : loops_) {
            loop->owned_runtime().join();
        }
    }

    std::pmr::vector<std::shared_ptr<detail::event_loop_state>> loops_{process_resource()};
    runtime_lifecycle lifecycle_;
    std::mutex failure_mutex_;
    first_failure failure_;
    bool observed_{true};
    std::atomic<std::size_t> next_index_{0};
};

event_loop_pool::event_loop_pool(event_loop_pool_options options)
    : impl_(std::allocate_shared<impl_type>(
          std::pmr::polymorphic_allocator<impl_type>(process_resource()), options)) {
    impl_->bind_callbacks(impl_);
}

event_loop_pool::~event_loop_pool() {
    stop();
    try {
        join();
    } catch (...) {
        // Destroying a pool that was never joined explicitly makes this the
        // only place its first failure is ever rethrown, and a destructor
        // cannot rethrow it further. Report rather than end here.
        report_unhandled_failure("event loop pool", std::current_exception());
    }
    impl_->detach_pool_observer();
}

void event_loop_pool::start() {
    if (!impl_->start()) {
        throw std::logic_error("event loop pool can only be started once");
    }
    try {
        for (const auto& loop : impl_->loops_) {
            if (!impl_->launch(loop->owned_runtime())) {
                return;
            }
        }
    } catch (...) {
        const auto launch_failure = std::current_exception();
        impl_->stop();
        impl_->join_loops();
        impl_->complete_stop();
        std::rethrow_exception(launch_failure);
    }
}

void event_loop_pool::stop() noexcept {
    impl_->stop();
}

void event_loop_pool::join() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.handle().is_current(); })) {
        throw std::logic_error("cannot join an event loop pool from one of its workers");
    }
    impl_->stop();
    impl_->join_loops();
    for (const auto& loop : impl_->loops_) {
        detail::worker_handle_access::wait_for_reservations(loop->runtime_.handle());
    }
    impl_->complete_stop();

    if (const auto failure = impl_->take_failure()) {
        std::rethrow_exception(failure);
    }
}

std::size_t event_loop_pool::loop_count() const noexcept {
    return impl_->loops_.size();
}

event_loop event_loop_pool::loop(std::size_t index) const {
    if (index >= impl_->loops_.size()) {
        throw std::out_of_range("event loop index is out of range");
    }
    return event_loop(impl_->loops_[index]);
}

event_loop event_loop_pool::next_loop() noexcept {
    const auto index = impl_->next_index_.fetch_add(1, std::memory_order_relaxed);
    return event_loop(impl_->loops_[index % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::uint64_t key) const noexcept {
    return event_loop(impl_->loops_[key % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::string_view key) const noexcept {
    return loop_for(worker_selection_hash(key));
}

}  // namespace ruvia
