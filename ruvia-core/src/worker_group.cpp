#include "ruvia/core/worker_group.h"

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

void worker_group::add(worker_runtime& member, std::size_t tier) {
    if (!members_.empty() && tier != members_.back().tier_ && tier != members_.back().tier_ + 1) {
        throw std::invalid_argument("worker group tiers must be added in order");
    }
    members_.push_back({.runtime_ = &member, .tier_ = tier});
}

bool worker_group::start() {
    for (std::size_t begin = 0; begin < members_.size();) {
        const auto end = tier_end(members_, begin);
        for (auto index = begin; index != end; ++index) {
            if (!launch(*members_[index].runtime_)) {
                return false;
            }
        }
        for (auto index = begin; index != end; ++index) {
            if (!settle(members_[index].runtime_->wait_until_ready(), "ready")) {
                return false;
            }
        }
        begin = end;
    }
    return !stop_requested();
}

bool worker_group::serve() {
    for (auto end = members_.size(); end != 0;) {
        auto begin = end - 1;
        while (begin != 0 && members_[begin - 1].tier_ == members_[end - 1].tier_) {
            --begin;
        }
        for (auto index = begin; index != end; ++index) {
            if (stop_requested()) {
                return false;
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
    return !stop_requested();
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

std::exception_ptr worker_group::join() noexcept {
    request_stop();
    {
        std::unique_lock lock(mutex_);
        stop_changed_.wait(lock, [this] { return !stop_dispatching_; });
    }
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
    std::exception_ptr first;
    for (const auto& member : members_) {
        auto failure = member.runtime_->failure();
        if (failure == nullptr) {
            continue;
        }
        if (first == nullptr) {
            first = std::move(failure);
        } else {
            report_unhandled_failure("additional worker failure", std::move(failure));
        }
    }
    return first;
}

bool worker_group::launch(worker_runtime& member) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return false;
        }
    }
    try {
        member.start();
    } catch (const std::logic_error&) {
        // A concurrent group stop can win the runtime's launch transition.
        if (stop_requested() && member.state() != runtime_lifecycle::state_type::running) {
            return false;
        }
        throw;
    }
    return true;
}

bool worker_group::settle(bool reached, const char* phase) const {
    if (reached) {
        return true;
    }
    if (stop_requested()) {
        return false;
    }
    throw std::runtime_error(std::string("worker stopped before the group was ") + phase);
}

}  // namespace ruvia
