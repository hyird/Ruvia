#pragma once

#include <memory>

namespace ruvia::detail {

template <typename node_type>
class waiter_list;

// The intrusive FIFO link of one waiter. A waiter derives from it publicly and
// is linked into at most one waiter_list; the list's owner serializes access.
template <typename node_type>
class waiter_link {
protected:
    waiter_link() noexcept = default;
    ~waiter_link() = default;
    waiter_link(const waiter_link&) = delete;
    waiter_link& operator=(const waiter_link&) = delete;

    [[nodiscard]] bool queued() const noexcept {
        return queued_;
    }

private:
    friend class waiter_list<node_type>;

    node_type* previous_{nullptr};
    node_type* next_{nullptr};
    bool queued_{false};
};

// FIFO of waiting operations, linked through their own waiter_link, so
// queueing never allocates.
template <typename node_type>
class waiter_list final {
public:
    [[nodiscard]] node_type* front() const noexcept {
        return head_;
    }

    void push_back(node_type& waiter) noexcept {
        auto& link = link_of(waiter);
        link.previous_ = tail_;
        link.next_ = nullptr;
        link.queued_ = true;
        if (tail_ != nullptr) {
            link_of(*tail_).next_ = std::addressof(waiter);
        } else {
            head_ = std::addressof(waiter);
        }
        tail_ = std::addressof(waiter);
    }

    // Returns false when the waiter is not queued.
    bool remove(node_type& waiter) noexcept {
        auto& link = link_of(waiter);
        if (!link.queued_) {
            return false;
        }
        if (link.previous_ != nullptr) {
            link_of(*link.previous_).next_ = link.next_;
        } else {
            head_ = link.next_;
        }
        if (link.next_ != nullptr) {
            link_of(*link.next_).previous_ = link.previous_;
        } else {
            tail_ = link.previous_;
        }
        link.previous_ = nullptr;
        link.next_ = nullptr;
        link.queued_ = false;
        return true;
    }

    // The first queued waiter matching `predicate`, or null.
    template <typename predicate_type>
    [[nodiscard]] node_type* find_if(predicate_type predicate) const noexcept {
        for (auto* waiter = head_; waiter != nullptr; waiter = link_of(*waiter).next_) {
            if (predicate(*waiter)) {
                return waiter;
            }
        }
        return nullptr;
    }

private:
    [[nodiscard]] static waiter_link<node_type>& link_of(node_type& waiter) noexcept {
        return waiter;
    }

    node_type* head_{nullptr};
    node_type* tail_{nullptr};
};

}  // namespace ruvia::detail
