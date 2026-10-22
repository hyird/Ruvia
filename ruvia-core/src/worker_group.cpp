#include "ruvia/core/worker_group.h"

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_runtime.h"

namespace ruvia {
worker_group::worker_group(std::pmr::memory_resource* resource)
    : members_(pmr_resource_or_default(resource)) {}

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
    if (joined_) {
        return join_failure_;
    }
    request_stop();
    wait_for_stop_dispatch();
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
            if (auto failure = member->failure(); failure != nullptr) {
                primary = std::move(failure);
                break;
            }
        }
    }
    // Workers commonly fail for one shared cause (an unreachable dependency),
    // so equivalent failures collapse into a single line with their count.
    for (std::size_t index = 0; index != members_.size(); ++index) {
        const auto failure = members_[index]->failure();
        if (failure == nullptr || equivalent_failures(failure, primary)) {
            continue;
        }
        bool reported = false;
        for (std::size_t earlier = 0; earlier != index && !reported; ++earlier) {
            reported = equivalent_failures(members_[earlier]->failure(), failure);
        }
        if (reported) {
            continue;
        }
        std::size_t occurrences = 1;
        for (auto later = index + 1; later != members_.size(); ++later) {
            if (equivalent_failures(members_[later]->failure(), failure)) {
                ++occurrences;
            }
        }
        report_unhandled_failure("additional worker", failure, occurrences);
    }
    join_failure_ = std::move(primary);
    joined_ = true;
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
        static_cast<void>(member->failure());
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
