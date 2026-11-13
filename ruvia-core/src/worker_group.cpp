#include "ruvia/core/worker_group.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_runtime.h"

#include "event_loop_state.h"

namespace ruvia {
worker_group::worker_group() = default;

worker_group::~worker_group() {
    for (auto* member : members_) {
        member->unbind_group(*this);
    }
}

void worker_group::add(worker_runtime& member) {
    const std::lock_guard lock(mutex_);
    if (sealed_ || stopping_) {
        throw std::logic_error("worker group members must be added before start or stop");
    }
    members_.push_back(&member);
    try {
        member.bind_group(*this);
    } catch (...) {
        members_.pop_back();
        throw;
    }
}

void worker_group::launch() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (auto* member : members_) {
            if (!launch(*member)) {
                return;
            }
        }
    } catch (...) {
        const auto launch_failure = std::current_exception();
        (void)join(launch_failure);
        std::rethrow_exception(launch_failure);
    }
}

bool worker_group::start() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (auto* member : members_) {
            if (!launch(*member)) {
                return settle(false, "ready");
            }
        }
        for (auto* member : members_) {
            if (!settle(member->wait_until_ready(), "ready")) {
                return false;
            }
        }
        const bool reached = settle(true, "ready");
        if (reached) {
            const std::lock_guard lock(mutex_);
            ready_ = true;
        }
        return reached;
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

bool worker_group::serve() {
    {
        const std::lock_guard lock(mutex_);
        if (!ready_) {
            throw std::logic_error("worker group must be ready before it serves");
        }
        if (serve_requested_) {
            throw std::logic_error("worker group can only serve once");
        }
        serve_requested_ = true;
    }
    try {
        for (auto* member : members_) {
            if (stop_requested()) {
                return settle(false, "serving");
            }
            member->request_serve();
        }
        for (auto* member : members_) {
            if (!settle(member->wait_until_serving(), "serving")) {
                return false;
            }
        }
        return settle(true, "serving");
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

void worker_group::member_failed(worker_runtime& member, std::exception_ptr failure) noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
            failed_member_ = &member;
        }
    }
    request_stop();
}

void worker_group::member_stopping() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            member_stopped_ = true;
        }
    }
    request_stop();
}

void worker_group::request_stop() noexcept {
    bool dispatch = false;
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            stopping_ = true;
            stop_dispatching_ = true;
            dispatch = true;
        }
    }
    // Publish group stop before notifying members, which can reenter it.
    // Every caller closes admission on all members; repeats are nonwaiting
    // even while the winning dispatch is notifying an earlier member.
    for (auto* member : members_) {
        member->request_stop();
    }
    if (!dispatch) {
        return;
    }
    const std::lock_guard lock(mutex_);
    stop_dispatching_ = false;
    // Notify before releasing the mutex: a waking join may retire this owner.
    stop_changed_.notify_all();
}

bool worker_group::stop_requested() const noexcept {
    const std::lock_guard lock(mutex_);
    return stopping_;
}

std::exception_ptr worker_group::join(std::exception_ptr primary) noexcept {
    request_stop();
    {
        std::unique_lock lock(mutex_);
        stop_changed_.wait(lock, [this] { return !stop_dispatching_ && !joining_; });
        if (joined_) {
            return join_failure_;
        }
        joining_ = true;
    }
    for (auto* member : members_) {
        try {
            member->join();
        } catch (...) {
            // join() only rejects a member's own thread, which can never
            // own the lifecycle; a stranded owner cannot be recovered.
            std::terminate();
        }
    }
    // The returned failure is settled before any report, so it is never also
    // reported here: the caller's primary, else the first member failure in
    // registration order.
    if (primary == nullptr) {
        for (auto* member : members_) {
            if (auto failure = member->recorded_failure(); failure != nullptr) {
                primary = std::move(failure);
                break;
            }
        }
    }
    // Workers commonly fail for one shared cause (an unreachable dependency),
    // so equivalent failures collapse into a single line with their count.
    // Each member's failure is read once with ownership: it is returned as the
    // primary, collapsed into it, or reported here.
    for (std::size_t index = 0; index != members_.size(); ++index) {
        const auto failure = members_[index]->take_failure();
        if (failure == nullptr || equivalent_failures(failure, primary)) {
            continue;
        }
        bool reported = false;
        for (std::size_t earlier = 0; earlier != index && !reported; ++earlier) {
            reported = equivalent_failures(members_[earlier]->recorded_failure(), failure);
        }
        if (reported) {
            continue;
        }
        std::size_t occurrences = 1;
        for (auto later = index + 1; later != members_.size(); ++later) {
            if (equivalent_failures(members_[later]->recorded_failure(), failure)) {
                ++occurrences;
            }
        }
        report_unhandled_failure("additional worker", failure, occurrences);
    }
    const std::lock_guard lock(mutex_);
    join_failure_ = std::move(primary);
    joining_ = false;
    joined_ = true;
    stop_changed_.notify_all();
    return join_failure_;
}

bool worker_group::launch(worker_runtime& member) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return false;
        }
    }
    // A concurrent stop (group-wide or of this member) can win the runtime's
    // launch transition; start() then reports it instead of launching.
    return member.start();
}

void worker_group::wait_for_stop_dispatch() noexcept {
    std::unique_lock lock(mutex_);
    stop_changed_.wait(lock, [this] { return !stop_dispatching_; });
}

void worker_group::rethrow_phase_failure(std::exception_ptr fallback) {
    request_stop();
    wait_for_stop_dispatch();
    auto failure = observe_phase_failure();
    std::rethrow_exception(failure ? failure : std::move(fallback));
}

std::exception_ptr worker_group::observe_phase_failure() noexcept {
    std::exception_ptr failure;
    worker_runtime* member = nullptr;
    {
        const std::lock_guard lock(mutex_);
        failure = failure_;
        member = failed_member_;
    }
    if (member != nullptr) {
        // A phase waiter may be on a different, healthy member. Observe only
        // the exception actually delivered, preserving other failure reports.
        static_cast<void>(member->take_failure());
    }
    return failure;
}

bool worker_group::settle(bool reached, const char* phase) {
    bool was_stopping = false;
    {
        const std::lock_guard lock(mutex_);
        was_stopping = stopping_;
        if (reached && !stopping_ && !failure_) {
            return true;
        }
    }
    request_stop();
    wait_for_stop_dispatch();
    auto failure = observe_phase_failure();
    bool member_stopped = false;
    {
        const std::lock_guard lock(mutex_);
        member_stopped = member_stopped_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (member_stopped || (!reached && !was_stopping)) {
        throw std::runtime_error(std::string("worker stopped before the group was ") + phase);
    }
    return false;
}

}  // namespace ruvia

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
            loops_.push_back(std::make_shared<detail::event_loop_state>(
                options.queue_capacity_, options.blocking_pool_));
            loops_.back()->install_shutdown_registration();
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
                        locked_loop->complete_stop_admission();
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
        bool joined = false;
        {
            const std::lock_guard lock(failure_mutex_);
            joined = joined_;
        }
        if (joined) {
            report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
            return;
        }
        if (!loop.owned_runtime().fail(failure)) {
            if (!equivalent_failures(failure, loop.recorded_failure())) {
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
            detail::worker_ref_access::wait_for_reservations(loop->runtime_.worker());
        }
        const std::lock_guard lock(failure_mutex_);
        if (joined_) {
            return nullptr;
        }
        joined_ = true;
        return failure;
    }

    std::vector<std::shared_ptr<detail::event_loop_state>> loops_;
    worker_group group_;
    std::mutex failure_mutex_;
    bool joined_{false};
    std::atomic<std::size_t> next_index_{0};
};

event_loop_pool::event_loop_pool(event_loop_pool_options options)
    : impl_(std::make_shared<impl_type>(options)) {
    impl_->bind_callbacks(impl_);
}

event_loop_pool::~event_loop_pool() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.worker().is_current(); })) {
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
            impl_->loops_, [](const auto& loop) { return loop->runtime_.worker().is_current(); })) {
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
