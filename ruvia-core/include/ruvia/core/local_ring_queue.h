#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

// Bounded single-thread FIFO ring. All slots are constructed at startup and stay
// constructed; no queue operation allocates afterwards. Payload assignment may
// allocate or throw independently of the queue. Callers that share it across
// threads must serialize every operation themselves.
//
// prepare_push reserves one unpublished slot: it cannot be prepared again until
// commit_push publishes it or cancel_push releases it. Direct prepared-slot
// assignment failure requires cancel before re-preparing; try_push cancels its
// hidden reservation before rethrowing. A front() borrow stays valid until pop.
template <typename value_type>
class local_ring_queue final {
public:
    // Null selects Ruvia's process resource. The resource must outlive the queue.
    explicit local_ring_queue(std::size_t capacity, std::pmr::memory_resource* resource = nullptr)
        : storage_(allocate_storage(capacity, resource)) {}

    local_ring_queue(const local_ring_queue&) = delete;
    local_ring_queue& operator=(const local_ring_queue&) = delete;
    local_ring_queue(local_ring_queue&&) = delete;
    local_ring_queue& operator=(local_ring_queue&&) = delete;
    ~local_ring_queue() = default;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.get_deleter().capacity_;
    }

    [[nodiscard]] value_type* prepare_push() noexcept {
        if (prepared_) {
            std::terminate();
        }
        if (size_ == capacity()) {
            return nullptr;
        }
        prepared_ = true;
        return storage_.get() + slot_index(size_);
    }

    void commit_push() noexcept {
        if (!prepared_) {
            std::terminate();
        }
        prepared_ = false;
        ++size_;
    }

    void cancel_push() noexcept {
        prepared_ = false;
    }

    [[nodiscard]] bool try_push(const value_type& value) noexcept(std::is_nothrow_copy_assignable_v<value_type>) {
        return push_value(value);
    }

    [[nodiscard]] bool try_push(value_type&& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        return push_value(std::move(value));
    }

    [[nodiscard]] value_type* front() noexcept {
        return size_ == 0 ? nullptr : storage_.get() + head_;
    }

    void pop() noexcept {
        if (size_ == 0) {
            std::terminate();
        }
        head_ = slot_index(1);
        --size_;
    }

    [[nodiscard]] bool try_pop(value_type& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        auto* slot = front();
        if (!slot) {
            return false;
        }
        value = std::move(*slot);
        pop();
        return true;
    }

private:
    struct storage_deleter final {
        std::pmr::memory_resource* resource_{nullptr};
        std::size_t capacity_{0};
        std::size_t constructed_{0};

        void operator()(value_type* storage) const noexcept {
            std::destroy_n(storage, constructed_);
            std::pmr::polymorphic_allocator<value_type>(resource_).deallocate(storage, capacity_);
        }
    };

    using storage_owner = std::unique_ptr<value_type, storage_deleter>;

    [[nodiscard]] static storage_owner allocate_storage(std::size_t capacity, std::pmr::memory_resource* resource) {
        if (capacity == 0) {
            throw std::invalid_argument("ring queue capacity must be nonzero");
        }
        if (!resource) {
            resource = process_resource();
        }
        std::pmr::polymorphic_allocator<value_type> allocator(resource);
        storage_owner storage(allocator.allocate(capacity), storage_deleter{resource, capacity, 0});
        auto& constructed = storage.get_deleter().constructed_;
        while (constructed != capacity) {
            std::uninitialized_construct_using_allocator(storage.get() + constructed, allocator);
            ++constructed;
        }
        return storage;
    }

    // Physical index of the slot `offset` positions after the head; offset <= capacity.
    [[nodiscard]] std::size_t slot_index(std::size_t offset) const noexcept {
        const auto remaining = capacity() - head_;
        return offset >= remaining ? offset - remaining : head_ + offset;
    }

    template <typename input_type>
    [[nodiscard]] bool push_value(input_type&& value) noexcept(std::is_nothrow_assignable_v<value_type&, input_type&&>) {
        auto* slot = prepare_push();
        if (!slot) {
            return false;
        }
        if constexpr (std::is_nothrow_assignable_v<value_type&, input_type&&>) {
            *slot = std::forward<input_type>(value);
        } else {
            try {
                *slot = std::forward<input_type>(value);
            } catch (...) {
                cancel_push();
                throw;
            }
        }
        commit_push();
        return true;
    }

    storage_owner storage_;
    std::size_t head_{0};
    std::size_t size_{0};
    bool prepared_{false};
};

}  // namespace ruvia
