#include "ruvia/core/worker_group.h"

#include <exception>
#include <stdexcept>
#include <string>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_resource.h"

namespace ruvia {
namespace {

// The end of the tier that starts at `begin`.
template <typename members_type>
[[nodiscard]] std::size_t tier_end(const members_type& members, std::size_t begin) noexcept {
    auto end = begin;
    while (end < members.size() && members[end].tier_ == members[begin].tier_) {
        ++end;
    }
    return end;
}

}  // namespace

worker_group::worker_group(std::pmr::memory_resource* resource)
    : members_(pmr_resource_or_default(resource)) {}

worker_group::~worker_group() {
    for (const auto& member : members_) {
        member.runtime_->unbind_group(*this);
    }
}

void worker_group::add(worker_runtime& member, std::size_t tier) {
    const std::lock_guard lock(mutex_);
    if (sealed_ || stopping_) {
        throw std::logic_error("worker group members must be added before start or stop");
    }
    if (!members_.empty() &&
        (tier < members_.back().tier_ || tier - members_.back().tier_ > 1)) {
        throw std::invalid_argument("worker group tiers must be added in order");
    }
    members_.push_back({.runtime_ = &member, .tier_ = tier});
    try {
        member.bind_group(*this);
    } catch (...) {
        members_.pop_back();
        throw;
    }
}

bool worker_group::start() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_ && !stopping_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (std::size_t begin = 0; begin < members_.size();) {
            const auto end = tier_end(members_, begin);
            for (auto index = begin; index != end; ++index) {
                if (!launch(*members_[index].runtime_)) {
                    return settle(false, "ready");
                }
            }
            for (auto index = begin; index != end; ++index) {
                if (!settle(members_[index].runtime_->wait_until_ready(), "ready")) {
                    return false;
                }
            }
            begin = end;
        }
        return settle(true, "ready");
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

bool worker_group::serve() {
    try {
        for (auto end = members_.size(); end != 0;) {
            auto begin = end - 1;
            while (begin != 0 && members_[begin - 1].tier_ == members_[end - 1].tier_) {
                --begin;
            }
            for (auto index = begin; index != end; ++index) {
                if (stop_requested()) {
                    return settle(false, "serving");
                }
                members_[index].runtime_->request_serve();
            }
            for (auto index = begin; index != end; ++index) {
                if (!settle(members_[index].runtime_->wait_until_serving(), "serving")) {
                    return false;
                }
            }
            end = begin;
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
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        stop_dispatching_ = true;
    }
    // Runtime stop may synchronously call an unlaunched member's abandon hook.
    // Publish the group stop first, then let hooks query/reenter it unlocked.
    for (const auto& member : members_) {
        member.runtime_->request_stop();
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
    for (std::size_t begin = 0; begin < members_.size();) {
        const auto end = tier_end(members_, begin);
        if (begin != 0) {
            // Every producer feeding this tier has joined.
            for (auto index = begin; index != end; ++index) {
                members_[index].runtime_->finalize();
            }
        }
        for (auto index = begin; index != end; ++index) {
            try {
                members_[index].runtime_->join();
            } catch (...) {
                // join() only rejects a member's own thread, which can never
                // own the lifecycle; a stranded owner cannot be recovered.
                std::terminate();
            }
        }
        begin = end;
    }
    // The returned failure is settled before any report, so it is never also
    // reported here: the caller's primary, else the first member failure in
    // tier order.
    if (primary == nullptr) {
        for (const auto& member : members_) {
            if (auto failure = member.runtime_->failure(); failure != nullptr) {
                primary = std::move(failure);
                break;
            }
        }
    }
    // Workers commonly fail for one shared cause (an unreachable dependency),
    // so equivalent failures collapse into a single line with their count.
    for (std::size_t index = 0; index != members_.size(); ++index) {
        const auto failure = members_[index].runtime_->failure();
        if (failure == nullptr || equivalent_failures(failure, primary)) {
            continue;
        }
        bool reported = false;
        for (std::size_t earlier = 0; earlier != index && !reported; ++earlier) {
            reported = equivalent_failures(members_[earlier].runtime_->failure(), failure);
        }
        if (reported) {
            continue;
        }
        std::size_t occurrences = 1;
        for (auto later = index + 1; later != members_.size(); ++later) {
            if (equivalent_failures(members_[later].runtime_->failure(), failure)) {
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
