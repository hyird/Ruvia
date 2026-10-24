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
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_group.h"
#include "ruvia/core/worker_handle.h"

#include "event_loop_state.h"

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
            const std::weak_ptr<detail::event_loop_state> weak_loop = loop;
            loop->failure_sink_ = [weak_owner, weak_loop](std::exception_ptr failure) noexcept {
                if (const auto owner = weak_owner.lock()) {
                    if (const auto locked_loop = weak_loop.lock()) {
                        owner->report_failure(*locked_loop, std::move(failure));
                        return;
                    }
                }
                report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
            };
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
            });
            group_.add(*stable_runtime);
        }
    }

    void report_failure(detail::event_loop_state& loop, std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        {
            const std::lock_guard lock(failure_mutex_);
            if (joined_) {
                report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
                return;
            }
        }
        auto& runtime = loop.owned_runtime();
        if (!runtime.fail(failure)) {
            if (!equivalent_failures(failure, runtime.failure())) {
                report_unhandled_failure("additional event loop failure", failure);
            }
        }
        stop();
    }

    void start() {
        group_.launch();
    }

    void stop() noexcept {
        group_.request_stop();
        for (const auto& loop : loops_) {
            loop->stop();
        }
    }

    [[nodiscard]] std::exception_ptr join() noexcept {
        auto failure = group_.join();
        for (const auto& loop : loops_) {
            detail::worker_handle_access::wait_for_reservations(loop->runtime_.handle());
        }
        const std::lock_guard lock(failure_mutex_);
        if (joined_) {
            return nullptr;
        }
        joined_ = true;
        return failure;
    }

    std::pmr::vector<std::shared_ptr<detail::event_loop_state>> loops_{process_resource()};
    worker_group group_{process_resource()};
    std::mutex failure_mutex_;
    bool joined_{false};
    std::atomic<std::size_t> next_index_{0};
};

event_loop_pool::event_loop_pool(event_loop_pool_options options)
    : impl_(std::allocate_shared<impl_type>(
          std::pmr::polymorphic_allocator<impl_type>(process_resource()), options)) {
    impl_->bind_callbacks(impl_);
}

event_loop_pool::~event_loop_pool() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.handle().is_current(); })) {
        std::terminate();
    }
    impl_->stop();
    if (const auto failure = impl_->join()) {
        // Destroying a pool that was never joined explicitly makes this the
        // only place its first failure is ever observed, and a destructor
        // cannot rethrow it further. Report rather than end here.
        report_unhandled_failure("event loop pool", failure);
    }
}

void event_loop_pool::start() {
    impl_->start();
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
    if (const auto failure = impl_->join()) {
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
