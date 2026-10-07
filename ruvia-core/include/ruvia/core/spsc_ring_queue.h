#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia {

enum class ring_synchronization { local,
    cross_thread };

template <typename value_type>
struct ring_batch final {
    std::span<value_type> first;
    std::span<value_type> second;

    [[nodiscard]] std::size_t size() const noexcept {
        return first.size() + second.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return first.empty();
    }
};

#if defined(_MSC_VER)
#pragma warning(push)
// Cache-line isolation deliberately pads the queue and its endpoint states.
#pragma warning(disable : 4324)
#endif

// One bounded storage/publication implementation. Local endpoints cooperate on
// one thread; cross-thread endpoints each have exactly one owning thread.
// All slots stay constructed and no queue operation allocates after startup.
// Payload assignment may allocate or throw independently of the queue.
//
// Producer operations are endpoint-affine. A successful preparation reserves
// one linear, unpublished batch: it cannot be prepared again or used concurrently.
// commit_push(count) publishes its prefix and cancels the remainder; cancel_push
// cancels the whole reservation. Either action invalidates all prepared borrows.
// Direct prepared-slot assignment failure requires cancel before re-preparing;
// try_push automatically cancels its hidden reservation before rethrowing.
// Consumer borrows remain valid until pop consumes their slots. A batch may be
// partially consumed, but no consumed borrow may subsequently be used.
// Both endpoints and every borrow must retire before destruction. empty() is a
// snapshot, also usable after publication stops or both endpoints retire; it does
// not mutate endpoint caches. No notification/lifetime management is provided.
template <typename value_type, ring_synchronization synchronization>
class basic_ring_queue final {
public:
    // Null selects Ruvia's process resource. The resource must outlive the queue.
    explicit basic_ring_queue(std::size_t capacity, std::pmr::memory_resource* resource = nullptr)
        : owned_storage_(allocate_storage(capacity, resource)),
          storage_(owned_storage_.get(), capacity) {}

    // Borrow constructed storage; never independently access it while in use.
    explicit basic_ring_queue(std::span<value_type> storage)
        : owned_storage_(nullptr, storage_deleter{}),
          storage_(storage) {
        if (storage.empty()) {
            throw std::invalid_argument("ring queue capacity must be nonzero");
        }
    }

    basic_ring_queue(const basic_ring_queue&) = delete;
    basic_ring_queue& operator=(const basic_ring_queue&) = delete;
    basic_ring_queue(basic_ring_queue&&) = delete;
    basic_ring_queue& operator=(basic_ring_queue&&) = delete;
    ~basic_ring_queue() = default;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.size();
    }

    [[nodiscard]] bool has_capacity() const noexcept {
        return push_capacity() != 0;
    }

    [[nodiscard]] ring_batch<value_type> prepare_push_batch(std::size_t max_count) noexcept {
        if (producer_.prepared_ != 0) {
            std::terminate();
        }
        const auto count = std::min(max_count, push_capacity(max_count));
        producer_.prepared_ = count;
        return make_batch(storage_, producer_.index_, count);
    }

    [[nodiscard]] value_type* prepare_push() noexcept {
        const auto batch = prepare_push_batch(1);
        return batch.empty() ? nullptr : batch.first.data();
    }

    void commit_push(std::size_t count = 1) noexcept {
        if (producer_.prepared_ == 0 || count > producer_.prepared_) {
            std::terminate();
        }
        producer_.prepared_ = 0;
        advance_index(producer_.index_, count);
        publish(producer_.sequence_, load_own(producer_.sequence_) + count);
    }

    void cancel_push() noexcept {
        producer_.prepared_ = 0;
    }

    [[nodiscard]] bool try_push(const value_type& value) noexcept(std::is_nothrow_copy_assignable_v<value_type>) {
        return push_value(value);
    }

    [[nodiscard]] bool try_push(value_type&& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        return push_value(std::move(value));
    }

    [[nodiscard]] ring_batch<value_type> front_batch(std::size_t max_count) noexcept {
        return make_batch(storage_, consumer_.index_, std::min(max_count, pop_available(max_count)));
    }

    [[nodiscard]] ring_batch<const value_type> front_batch(std::size_t max_count) const noexcept {
        return make_batch(std::span<const value_type>(storage_), consumer_.index_, std::min(max_count, pop_available(max_count)));
    }

    [[nodiscard]] value_type* front() noexcept {
        const auto batch = front_batch(1);
        return batch.empty() ? nullptr : batch.first.data();
    }

    [[nodiscard]] const value_type* front() const noexcept {
        const auto batch = front_batch(1);
        return batch.empty() ? nullptr : batch.first.data();
    }

    void pop(std::size_t count = 1) noexcept {
        if (count > pop_available(count)) {
            std::terminate();
        }
        advance_index(consumer_.index_, count);
        publish(consumer_.sequence_, load_own(consumer_.sequence_) + count);
    }

    [[nodiscard]] bool try_pop(value_type& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        if (!front()) {
            return false;
        }
        value = std::move(storage_[consumer_.index_]);
        pop();
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return load_peer(consumer_.sequence_) == load_peer(producer_.sequence_);
    }

private:
    using cursor = std::conditional_t<synchronization == ring_synchronization::cross_thread, std::atomic<std::size_t>, std::size_t>;
    static_assert(synchronization != ring_synchronization::cross_thread || std::atomic<std::size_t>::is_always_lock_free);

    [[nodiscard]] static std::size_t load_own(const cursor& value) noexcept {
        if constexpr (synchronization == ring_synchronization::cross_thread) {
            return value.load(std::memory_order_relaxed);
        } else {
            return value;
        }
    }

    [[nodiscard]] static std::size_t load_peer(const cursor& value) noexcept {
        if constexpr (synchronization == ring_synchronization::cross_thread) {
            return value.load(std::memory_order_acquire);
        } else {
            return value;
        }
    }

    static void publish(cursor& target, std::size_t value) noexcept {
        if constexpr (synchronization == ring_synchronization::cross_thread) {
            target.store(value, std::memory_order_release);
        } else {
            target = value;
        }
    }

    [[nodiscard]] std::size_t push_capacity(std::size_t requested = 1) const noexcept {
        const auto produced = load_own(producer_.sequence_);
        if (capacity() - (produced - producer_.peer_sequence_) < requested) {
            producer_.peer_sequence_ = load_peer(consumer_.sequence_);
        }
        return capacity() - (produced - producer_.peer_sequence_);
    }

    [[nodiscard]] std::size_t pop_available(std::size_t requested = 1) const noexcept {
        const auto consumed = load_own(consumer_.sequence_);
        if (consumer_.peer_sequence_ - consumed < requested) {
            consumer_.peer_sequence_ = load_peer(producer_.sequence_);
        }
        return consumer_.peer_sequence_ - consumed;
    }

    template <typename slot_type>
    [[nodiscard]] static ring_batch<slot_type> make_batch(std::span<slot_type> storage, std::size_t index, std::size_t count) noexcept {
        const auto first_count = std::min(count, storage.size() - index);
        return {storage.subspan(index, first_count), storage.first(count - first_count)};
    }

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
            resource = detail::processResource();
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

    void advance_index(std::size_t& index, std::size_t count) noexcept {
        const auto remaining = capacity() - index;
        index = count >= remaining ? count - remaining : index + count;
    }

    // Physical indices wrap independently of unsigned publication counters;
    // rollover therefore also works for non-power-of-two capacities.
    struct alignas(64) producer_state final {
        cursor sequence_{0};
        mutable std::size_t peer_sequence_{0};
        std::size_t index_{0};
        std::size_t prepared_{0};
    };

    struct alignas(64) consumer_state final {
        cursor sequence_{0};
        mutable std::size_t peer_sequence_{0};
        std::size_t index_{0};
    };

    storage_owner owned_storage_;
    std::span<value_type> storage_;
    producer_state producer_;
    consumer_state consumer_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <typename value_type>
using spsc_ring_queue = basic_ring_queue<value_type, ring_synchronization::cross_thread>;

template <typename value_type>
using local_ring_queue = basic_ring_queue<value_type, ring_synchronization::local>;

}  // namespace ruvia
