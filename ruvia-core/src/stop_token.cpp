#include "ruvia/core/stop_token.h"

namespace ruvia {

namespace detail {

stop_state_ref stop_state::create() {
    return stop_state_ref(new stop_state());
}

void stop_state::destroy() noexcept {
    delete this;
}

void stop_state::request_stop() noexcept {
    std::unique_lock lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
        return;
    }
    flags_.fetch_or(requested_flag, std::memory_order_release);
    requester_ = std::this_thread::get_id();
    while (head_ != nullptr) {
        auto* const node = head_;
        head_ = node->next_;
        if (head_ != nullptr) {
            head_->previous_ = nullptr;
        }
        node->next_ = nullptr;
        running_ = node;
        lock.unlock();
        // The callback may destroy the node, and an unlink waiting on another
        // thread frees it once running_ moves on: the node is not touched again.
        node->invoke();
        lock.lock();
        running_ = nullptr;
        if (std::exchange(run_awaited_, false)) {
            run_epoch_.fetch_add(1, std::memory_order_relaxed);
            run_epoch_.notify_all();
        }
    }
}

bool stop_state::link(stop_callback_node& node, std::atomic<stop_registration_phase>& phase) noexcept {
    std::lock_guard lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
        return false;
    }
    node.linked_ = true;
    auto expected = stop_registration_phase::registering;
    if (!phase.compare_exchange_strong(expected, stop_registration_phase::registered,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        node.linked_ = false;
        if (expected != stop_registration_phase::reset_pending) {
            std::terminate();
        }
        return false;
    }
    node.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &node;
    }
    head_ = &node;
    return true;
}

void stop_state::unlink(stop_callback_node& node) noexcept {
    std::unique_lock lock(mutex_);
    if (head_ == &node || node.previous_ != nullptr) {
        if (node.previous_ != nullptr) {
            node.previous_->next_ = node.next_;
        } else {
            head_ = node.next_;
        }
        if (node.next_ != nullptr) {
            node.next_->previous_ = node.previous_;
        }
        node.previous_ = nullptr;
        node.next_ = nullptr;
        return;
    }
    // Never linked, or its run already returned.
    if (running_ != &node) {
        return;
    }
    // Destroyed from inside its own run: waiting would deadlock, and the
    // requester never touches the node after its run.
    if (requester_ == std::this_thread::get_id()) {
        return;
    }
    // The mutex orders the callback's effects before the observed running_ change.
    while (running_ == &node) {
        run_awaited_ = true;
        const auto epoch = run_epoch_.load(std::memory_order_relaxed);
        lock.unlock();
        run_epoch_.wait(epoch, std::memory_order_relaxed);
        lock.lock();
    }
}

}  // namespace detail

// Request deadlines create a source per request on every worker. The mimalloc
// resource has no shared pooling layer and outlives every observer, including
// tokens retired after the allocating worker has exited.
stop_source::stop_source()
    : state_(detail::stop_state::create()) {}

namespace {

[[nodiscard]] bool unique_parent(std::span<const stop_token> parents, std::size_t index) noexcept {
    if (!parents[index].stoppable()) {
        return false;
    }
    for (std::size_t previous = 0; previous != index; ++previous) {
        if (parents[previous] == parents[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t extra_parent_count(std::span<const stop_token> parents) noexcept {
    if (parents.size() <= 2) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            return 0;
        }
        count += unique_parent(parents, index);
    }
    return count > 2 ? count - 2 : 0;
}

}  // namespace

stop_group::stop_group(std::span<const stop_token> parents)
    : extra_parents_(extra_parent_count(parents)) {
    // Capture the state, never this: downstream callbacks may destroy the
    // group, including the parent registration currently invoking us.
    const auto register_parent = [this](const stop_token& parent, stop_registration& registration) {
        parent.register_callback(registration, [state = output_.state_] {
            state->request_stop();
        });
    };
    if (!extra_parents_.empty()) {
        source_.emplace();
        output_ = source_->token();
        std::size_t slot = 0;
        for (std::size_t index = 0; index != parents.size(); ++index) {
            if (!unique_parent(parents, index)) {
                continue;
            }
            if (slot < parents_.size()) {
                register_parent(parents[index], parents_[slot]);
            } else {
                register_parent(parents[index], extra_parents_[slot - parents_.size()]);
            }
            ++slot;
        }
        return;
    }
    const stop_token* first = nullptr;
    const stop_token* second = nullptr;
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            output_ = parents[index];
            return;
        }
        if (unique_parent(parents, index)) {
            if (first == nullptr) {
                first = &parents[index];
            } else {
                second = &parents[index];
            }
        }
    }
    if (first == nullptr) {
        return;
    }
    if (second == nullptr) {
        output_ = *first;
        return;
    }
    source_.emplace();
    output_ = source_->token();
    register_parent(*first, parents_[0]);
    register_parent(*second, parents_[1]);
}

}  // namespace ruvia
