#include "ruvia/core/stop_token.h"

#include <memory_resource>
#include <new>

namespace ruvia {

namespace detail {

stop_state_ref stop_state::create(std::pmr::memory_resource& resource) {
    void* storage = resource.allocate(sizeof(stop_state), alignof(stop_state));
    return stop_state_ref(::new (storage) stop_state(resource));
}

void stop_state::destroy() noexcept {
    auto* const resource = resource_;
    this->~stop_state();
    resource->deallocate(this, sizeof(stop_state), alignof(stop_state));
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
        bool destroyed = false;
        node->destroyed_ = &destroyed;
        running_ = node;
        lock.unlock();
        node->callback_->invoke();
        // A node the callback destroyed is never touched again. Otherwise the
        // completion store is the last access: a destructor waiting on another
        // thread may free the node as soon as it observes it.
        if (!destroyed) {
            node->completed_.store(true, std::memory_order_release);
        }
        lock.lock();
        running_ = nullptr;
    }
}

bool stop_state::link(stop_callback_node& node) noexcept {
    std::lock_guard lock(mutex_);
    if ((flags_.load(std::memory_order_relaxed) & requested_flag) != 0) {
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
    // Destroyed from inside its own run: the requester must not touch it again.
    if (requester_ == std::this_thread::get_id()) {
        *node.destroyed_ = true;
        return;
    }
    lock.unlock();
    // Callbacks are short and post their real work, so the wait is brief.
    while (!node.completed_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}  // namespace detail

// Request deadlines create a source per request on every worker. The process
// pool would serialize those workers on its shared lock, so the state comes
// from new/delete, whose small-object path is thread-local. The resource is
// never destroyed, so a state may retire on any thread at any time.
stop_source::stop_source()
    : state_(detail::stop_state::create(*std::pmr::new_delete_resource())) {}

}  // namespace ruvia
