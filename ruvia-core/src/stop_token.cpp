#include "ruvia/core/stop_token.h"

#include <memory_resource>
#include <new>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

namespace detail {

stop_state_ref stop_state::create() {
    void* storage = process_resource()->allocate(sizeof(stop_state), alignof(stop_state));
    return stop_state_ref(::new (storage) stop_state());
}

void stop_state::destroy() noexcept {
    this->~stop_state();
    process_resource()->deallocate(this, sizeof(stop_state), alignof(stop_state));
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
        node->linked_ = false;
        running_ = node;
        lock.unlock();
        // The callback may destroy the node, and an unlink waiting on another
        // thread frees it once running_ moves on: the node is not touched again.
        node->callback_->invoke();
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
    auto expected = stop_registration_phase::registering;
    if (!phase.compare_exchange_strong(expected, stop_registration_phase::registered,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
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
    node.linked_ = true;
    return true;
}

void stop_state::unlink(stop_callback_node& node) noexcept {
    std::unique_lock lock(mutex_);
    if (node.linked_) {
        if (node.previous_ != nullptr) {
            node.previous_->next_ = node.next_;
        } else {
            head_ = node.next_;
        }
        if (node.next_ != nullptr) {
            node.next_->previous_ = node.previous_;
        }
        node.linked_ = false;
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
    : extra_parents_(extra_parent_count(parents), process_resource()) {
    std::size_t count = 0;
    std::array<std::size_t, 2> first_two_indices{};
    for (std::size_t index = 0; index != parents.size(); ++index) {
        if (parents[index].stop_requested()) {
            output_ = parents[index];
            return;
        }
        if (unique_parent(parents, index)) {
            output_ = parents[index];
            if (count < first_two_indices.size()) {
                first_two_indices[count] = index;
            }
            ++count;
        }
    }
    if (count < 2) {
        return;
    }
    source_.emplace();
    output_ = source_->token();
    // Capture the state, never this: downstream callbacks may destroy the
    // group, including the parent registration currently invoking us.
    const auto register_parent = [this](const stop_token& parent, stop_registration& registration) {
        parent.register_callback(registration, [state = output_.state_] {
            state->request_stop();
        });
    };
    register_parent(parents[first_two_indices[0]], parents_[0]);
    register_parent(parents[first_two_indices[1]], parents_[1]);
    if (count == 2) {
        return;
    }
    std::size_t extra_slot = 0;
    for (std::size_t index = first_two_indices[1] + 1; index != parents.size(); ++index) {
        if (unique_parent(parents, index)) {
            register_parent(parents[index], extra_parents_[extra_slot++]);
        }
    }
}

}  // namespace ruvia
