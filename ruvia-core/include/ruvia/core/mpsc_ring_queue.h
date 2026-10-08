#pragma once

#include <concepts>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "ruvia/core/spsc_ring_queue.h"

namespace ruvia {

// Bounded multi-producer storage. Publication and consumption serialize through
// one mutex and reuse the same ring implementation as local/SPSC queues. The
// lock may also protect the owner's admission/waiter state, so checking close
// and publishing a value remain one operation without adding a second lock.
// The consumer and every producer must retire before this owner is destroyed.
template <std::move_constructible value_type>
class mpsc_ring_queue final {
public:
    class locked_access final {
    public:
        locked_access(const locked_access&) = delete;
        locked_access& operator=(const locked_access&) = delete;
        locked_access(locked_access&&) = delete;
        locked_access& operator=(locked_access&&) = delete;

        [[nodiscard]] value_type* front() noexcept {
            auto* slot = owner_.storage_.front();
            return slot ? std::addressof(**slot) : nullptr;
        }
        void pop() noexcept {
            auto* slot = owner_.storage_.front();
            if (!slot) {
                std::terminate();
            }
            slot->reset();
            owner_.storage_.pop();
        }
        template <typename... argument_types>
            requires std::constructible_from<value_type, argument_types...>
        [[nodiscard]] bool try_emplace(argument_types&&... arguments) {
            auto* slot = owner_.storage_.prepare_push();
            if (!slot) {
                return false;
            }
            try {
                slot->emplace(std::forward<argument_types>(arguments)...);
            } catch (...) {
                owner_.storage_.cancel_push();
                throw;
            }
            owner_.storage_.commit_push();
            return true;
        }
        [[nodiscard]] bool try_push(value_type&& value) {
            return try_emplace(std::move(value));
        }
        [[nodiscard]] bool try_push(const value_type& value)
            requires std::copy_constructible<value_type>
        {
            return try_emplace(value);
        }
        [[nodiscard]] bool try_pop(value_type& value) noexcept(std::is_nothrow_move_assignable_v<value_type>)
            requires std::is_move_assignable_v<value_type>
        {
            auto* item = front();
            if (!item) {
                return false;
            }
            value = std::move(*item);
            pop();
            return true;
        }

    private:
        explicit locked_access(mpsc_ring_queue& owner)
            : owner_(owner),
              lock_(owner.mutex_) {}
        mpsc_ring_queue& owner_;
        std::unique_lock<std::mutex> lock_;
        friend class mpsc_ring_queue;
    };

    explicit mpsc_ring_queue(std::size_t capacity, std::pmr::memory_resource* resource = nullptr)
        : storage_(capacity, resource) {}
    mpsc_ring_queue(const mpsc_ring_queue&) = delete;
    mpsc_ring_queue& operator=(const mpsc_ring_queue&) = delete;
    mpsc_ring_queue(mpsc_ring_queue&&) = delete;
    mpsc_ring_queue& operator=(mpsc_ring_queue&&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.capacity();
    }
    [[nodiscard]] locked_access lock() {
        return locked_access(*this);
    }
    // Borrowed by owner lifecycle and wait registration; outlives their access.
    [[nodiscard]] std::mutex& synchronization_mutex() noexcept {
        return mutex_;
    }
    [[nodiscard]] bool try_push(value_type&& value) {
        return lock().try_push(std::move(value));
    }
    [[nodiscard]] bool try_push(const value_type& value)
        requires std::copy_constructible<value_type>
    {
        return lock().try_push(value);
    }
    [[nodiscard]] bool try_pop(value_type& value)
        requires std::is_move_assignable_v<value_type>
    {
        return lock().try_pop(value);
    }

private:
    local_ring_queue<std::optional<value_type>> storage_;
    std::mutex mutex_;
};

}  // namespace ruvia
