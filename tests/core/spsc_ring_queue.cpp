#include "ruvia/core/spsc_ring_queue.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/channel_lifecycle.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

class default_resource_scope final {
public:
    explicit default_resource_scope(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}

    ~default_resource_scope() {
        std::pmr::set_default_resource(previous_);
    }

    default_resource_scope(const default_resource_scope&) = delete;
    default_resource_scope& operator=(const default_resource_scope&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

struct tracked_slot final {
    inline static std::size_t attempts_{0};
    inline static std::size_t live_{0};
    inline static std::size_t destroyed_{0};
    inline static std::size_t fail_on_attempt_{0};

    tracked_slot() {
        ++attempts_;
        if (attempts_ == fail_on_attempt_) {
            throw std::runtime_error("requested slot construction failure");
        }
        ++live_;
    }

    ~tracked_slot() {
        --live_;
        ++destroyed_;
    }

    tracked_slot(const tracked_slot&) = delete;
    tracked_slot& operator=(const tracked_slot&) = delete;
    tracked_slot(tracked_slot&&) = delete;
    tracked_slot& operator=(tracked_slot&&) = delete;

    static void reset(std::size_t fail_on_attempt = 0) noexcept {
        attempts_ = 0;
        live_ = 0;
        destroyed_ = 0;
        fail_on_attempt_ = fail_on_attempt;
    }

    std::size_t value_{0};
};

struct retained_payload final {
    retained_payload(std::size_t& live, int value) noexcept
        : live_(live),
          value_(value) {
        ++live_;
    }

    ~retained_payload() {
        --live_;
    }

    std::size_t& live_;
    int value_;
};

struct sequence_packet final {
    std::size_t sequence_{0};
    std::array<std::uint64_t, 8> payload_{};
};

}  // namespace

RUVIA_TEST(spsc_ring_queue_fifo_full_recovery_and_repeated_slot_wrap) {
    for (const std::size_t capacity : {1, 3, 5, 8}) {
        ruvia::spsc_ring_queue<std::size_t> queue(capacity);
        RUVIA_CHECK_EQ(queue.capacity(), capacity);
        std::size_t next_input = 1;
        std::size_t next_output = 1;
        for (std::size_t round = 0; round != 1000; ++round) {
            for (std::size_t index = 0; index != capacity; ++index) {
                RUVIA_CHECK(queue.has_capacity());
                RUVIA_CHECK(queue.try_push(next_input++));
            }
            RUVIA_CHECK(!queue.has_capacity());
            RUVIA_CHECK(queue.prepare_push() == nullptr);
            RUVIA_CHECK(!queue.try_push(std::size_t{0}));

            const auto released = (capacity + 1) / 2;
            std::size_t value = 0;
            for (std::size_t index = 0; index != released; ++index) {
                RUVIA_CHECK(queue.try_pop(value));
                RUVIA_CHECK_EQ(value, next_output++);
            }
            for (std::size_t index = 0; index != released; ++index) {
                const auto input = next_input++;
                RUVIA_CHECK(queue.try_push(input));
            }
            RUVIA_CHECK(!queue.has_capacity());
            for (std::size_t index = 0; index != capacity; ++index) {
                RUVIA_CHECK(queue.try_pop(value));
                RUVIA_CHECK_EQ(value, next_output++);
            }
            RUVIA_CHECK(queue.empty());
            RUVIA_CHECK(queue.front() == nullptr);
            value = 91;
            RUVIA_CHECK(!queue.try_pop(value));
            RUVIA_CHECK_EQ(value, std::size_t{91});
        }
        RUVIA_CHECK_EQ(next_input, next_output);
    }
}

RUVIA_TEST(spsc_ring_queue_preparation_is_unpublished_and_cancel_reuses_capacity) {
    for (const std::size_t capacity : {1, 3}) {
        ruvia::spsc_ring_queue<int> queue(capacity);
        queue.cancel_push();
        for (int attempt_value = 0; attempt_value != 100; ++attempt_value) {
            auto* slot = queue.prepare_push();
            RUVIA_CHECK(slot != nullptr);
            if (!slot) {
                return;
            }
            *slot = attempt_value;
            RUVIA_CHECK(queue.empty());
            RUVIA_CHECK(queue.front() == nullptr);
            int value = -1;
            RUVIA_CHECK(!queue.try_pop(value));
            RUVIA_CHECK_EQ(value, -1);
            queue.cancel_push();
            queue.cancel_push();
            RUVIA_CHECK(queue.empty());
            RUVIA_CHECK(queue.has_capacity());
        }
        auto* slot = queue.prepare_push();
        RUVIA_CHECK(slot != nullptr);
        if (!slot) {
            return;
        }
        *slot = 173;
        queue.commit_push();
        RUVIA_CHECK(!queue.empty());
        int value = 0;
        RUVIA_CHECK(queue.try_pop(value));
        RUVIA_CHECK_EQ(value, 173);
        RUVIA_CHECK(queue.empty());
        RUVIA_CHECK(queue.has_capacity());
    }
}

RUVIA_TEST(spsc_ring_queue_front_borrow_prevents_slot_overwrite) {
    ruvia::spsc_ring_queue<int> queue(3);
    RUVIA_CHECK(queue.try_push(11));
    const auto* held = queue.front();
    RUVIA_CHECK(held != nullptr);
    if (!held) {
        return;
    }
    RUVIA_CHECK(queue.try_push(22));
    RUVIA_CHECK(queue.try_push(33));
    for (unsigned attempt_value = 0; attempt_value != 50; ++attempt_value) {
        RUVIA_CHECK(queue.prepare_push() == nullptr);
        RUVIA_CHECK(!queue.try_push(44));
        RUVIA_CHECK_EQ(*held, 11);
        RUVIA_CHECK(queue.front() == held);
    }
    queue.pop();
    RUVIA_CHECK(queue.try_push(44));
    int value = 0;
    for (const int expected : {22, 33, 44}) {
        RUVIA_CHECK(queue.try_pop(value));
        RUVIA_CHECK_EQ(value, expected);
    }
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(spsc_ring_queue_moves_payload_and_pop_keeps_slot_constructed) {
    std::size_t live = 0;
    {
        ruvia::spsc_ring_queue<std::unique_ptr<retained_payload>> queue(1);
        auto input = std::make_unique<retained_payload>(live, 73);
        RUVIA_CHECK(queue.try_push(std::move(input)));
        RUVIA_CHECK(!input);
        const auto batch = queue.front_batch(1);
        RUVIA_CHECK(!batch.empty());
        std::unique_ptr<retained_payload> output = std::move(batch.first_[0]);
        queue.pop();
        RUVIA_CHECK(output != nullptr);
        if (!output) {
            return;
        }
        RUVIA_CHECK_EQ(output->value_, 73);
        RUVIA_CHECK_EQ(live, std::size_t{1});
        output.reset();
        RUVIA_CHECK_EQ(live, std::size_t{0});

        RUVIA_CHECK(queue.try_push(std::make_unique<retained_payload>(live, 89)));
        queue.pop();
        RUVIA_CHECK(queue.empty());
        RUVIA_CHECK_EQ(live, std::size_t{1});
        queue.cancel_push();
        RUVIA_CHECK_EQ(live, std::size_t{1});
        RUVIA_CHECK(queue.try_push(std::make_unique<retained_payload>(live, 97)));
        RUVIA_CHECK_EQ(live, std::size_t{1});
        const auto* front = queue.front();
        RUVIA_CHECK(front && *front);
        if (front && *front) {
            RUVIA_CHECK_EQ((*front)->value_, 97);
        }
    }
    RUVIA_CHECK_EQ(live, std::size_t{0});
}

RUVIA_TEST(spsc_ring_queue_borrowed_storage_neither_allocates_nor_retires_payloads) {
    tracked_slot::reset();
    failing_memory_resource resource;
    {
        std::array<tracked_slot, 3> storage;
        tracked_slot::fail_on_attempt_ = tracked_slot::attempts_ + 1;
        resource.fail_after(0);
        default_resource_scope default_resource(resource);
        {
            ruvia::spsc_ring_queue<tracked_slot> queue{std::span<tracked_slot>(storage)};
            for (std::size_t round = 1; round != 101; ++round) {
                auto* slot = queue.prepare_push();
                RUVIA_CHECK(slot != nullptr);
                if (!slot) {
                    return;
                }
                slot->value_ = round;
                queue.commit_push();
                const auto* value = queue.front();
                RUVIA_CHECK(value != nullptr);
                if (!value) {
                    return;
                }
                RUVIA_CHECK_EQ(value->value_, round);
                queue.pop();
            }
        }
        RUVIA_CHECK_EQ(tracked_slot::attempts_, std::size_t{3});
        RUVIA_CHECK_EQ(tracked_slot::live_, std::size_t{3});
        RUVIA_CHECK_EQ(tracked_slot::destroyed_, std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(tracked_slot::live_, std::size_t{0});
    RUVIA_CHECK_EQ(tracked_slot::destroyed_, std::size_t{3});
    tracked_slot::reset();
}

RUVIA_TEST(spsc_ring_queue_pmr_storage_reclaims_all_slots_without_hot_path_allocation) {
    tracked_slot::reset();
    failing_memory_resource resource;
    {
        ruvia::spsc_ring_queue<tracked_slot> queue(5, &resource);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{1});
        RUVIA_CHECK_EQ(tracked_slot::live_, std::size_t{5});
        resource.fail_after(0);
        for (std::size_t round = 0; round != 100; ++round) {
            for (std::size_t index = 0; index != queue.capacity(); ++index) {
                auto* slot = queue.prepare_push();
                RUVIA_CHECK(slot != nullptr);
                if (!slot) {
                    return;
                }
                slot->value_ = round * queue.capacity() + index;
                queue.commit_push();
            }
            for (std::size_t index = 0; index != queue.capacity(); ++index) {
                const auto* value = queue.front();
                RUVIA_CHECK(value != nullptr);
                if (!value) {
                    return;
                }
                RUVIA_CHECK_EQ(value->value_, round * queue.capacity() + index);
                queue.pop();
            }
        }
        RUVIA_CHECK_EQ(tracked_slot::attempts_, std::size_t{5});
        RUVIA_CHECK_EQ(tracked_slot::destroyed_, std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(tracked_slot::live_, std::size_t{0});
    RUVIA_CHECK_EQ(tracked_slot::destroyed_, std::size_t{5});
}

RUVIA_TEST(spsc_ring_queue_startup_allocation_failure_leaves_resource_reusable) {
    failing_memory_resource resource;
    resource.fail_after(0);
    bool failed = false;
    try {
        ruvia::spsc_ring_queue<int> queue(3, &resource);
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    {
        ruvia::spsc_ring_queue<int> queue(3, &resource);
        RUVIA_CHECK(queue.try_push(61));
        int output = 0;
        RUVIA_CHECK(queue.try_pop(output));
        RUVIA_CHECK_EQ(output, 61);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(spsc_ring_queue_partial_slot_construction_failure_reclaims_completed_slots) {
    failing_memory_resource resource;
    for (std::size_t failure = 1; failure != 6; ++failure) {
        tracked_slot::reset(failure);
        bool failed = false;
        try {
            ruvia::spsc_ring_queue<tracked_slot> queue(5, &resource);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(tracked_slot::attempts_, failure);
        RUVIA_CHECK_EQ(tracked_slot::live_, std::size_t{0});
        RUVIA_CHECK_EQ(tracked_slot::destroyed_, failure - 1);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    tracked_slot::reset();
    {
        ruvia::spsc_ring_queue<tracked_slot> queue(5, &resource);
        auto* slot = queue.prepare_push();
        RUVIA_CHECK(slot != nullptr);
        if (!slot) {
            return;
        }
        slot->value_ = 109;
        queue.commit_push();
        const auto* front = queue.front();
        RUVIA_CHECK(front != nullptr);
        if (front) {
            RUVIA_CHECK_EQ(front->value_, std::size_t{109});
        }
    }
    RUVIA_CHECK_EQ(tracked_slot::destroyed_, std::size_t{5});
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(spsc_ring_queue_zero_capacity_rejects_both_storage_modes) {
    failing_memory_resource resource;
    bool owning_rejected = false;
    try {
        ruvia::spsc_ring_queue<int> queue(0, &resource);
    } catch (const std::invalid_argument&) {
        owning_rejected = true;
    }
    bool borrowed_rejected = false;
    try {
        ruvia::spsc_ring_queue<int> queue{std::span<int>{}};
    } catch (const std::invalid_argument&) {
        borrowed_rejected = true;
    }
    RUVIA_CHECK(owning_rejected);
    RUVIA_CHECK(borrowed_rejected);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(spsc_ring_queue_cross_thread_fifo_publishes_complete_payloads) {
    constexpr std::size_t packet_count = 50000;
    for (const std::size_t capacity : {1, 3, 127}) {
        ruvia::spsc_ring_queue<sequence_packet> queue(capacity);
        std::atomic<bool> stop{false};
        bool producer_completed = false;
        const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::thread producer_value([&] {
            std::size_t sequence = 1;
            while (sequence <= packet_count) {
                auto batch = queue.prepare_push_batch(std::min(std::size_t{17}, packet_count - sequence + 1));
                if (batch.empty()) {
                    if (stop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline_value) {
                        return;
                    }
                    std::this_thread::yield();
                    continue;
                }
                for (auto slots : {batch.first_, batch.second_}) {
                    for (auto& slot : slots) {
                        slot.sequence_ = sequence;
                        for (std::size_t index = 0; index != slot.payload_.size(); ++index) {
                            slot.payload_[index] = static_cast<std::uint64_t>(sequence) * 17 + index;
                        }
                        ++sequence;
                    }
                }
                queue.commit_push(batch.size());
            }
            producer_completed = true;
        });

        bool fifo_valid = true;
        std::size_t received_value = 0;
        while (received_value != packet_count && std::chrono::steady_clock::now() < deadline_value) {
            const auto batch = queue.front_batch(11);
            if (batch.empty()) {
                std::this_thread::yield();
                continue;
            }
            for (auto slots : {batch.first_, batch.second_}) {
                for (const auto& packet : slots) {
                    ++received_value;
                    fifo_valid = fifo_valid && packet.sequence_ == received_value;
                    for (std::size_t index = 0; index != packet.payload_.size(); ++index) {
                        fifo_valid = fifo_valid && packet.payload_[index] == static_cast<std::uint64_t>(received_value) * 17 + index;
                    }
                }
            }
            queue.pop(batch.size());
            if (!fifo_valid) {
                break;
            }
        }
        stop.store(true, std::memory_order_relaxed);
        producer_value.join();
        RUVIA_CHECK(producer_completed);
        RUVIA_CHECK(fifo_valid);
        RUVIA_CHECK_EQ(received_value, packet_count);
        RUVIA_CHECK(queue.empty());
    }
}

RUVIA_TEST(local_ring_batch_wrap_partial_publication_and_cancel) {
    ruvia::local_ring_queue<int> queue(5);
    auto batch = queue.prepare_push_batch(4);
    RUVIA_CHECK_EQ(batch.size(), std::size_t{4});
    for (std::size_t index = 0; index != batch.first_.size(); ++index) {
        batch.first_[index] = static_cast<int>(index + 1);
    }
    queue.commit_push(3);
    const auto held = queue.front_batch(2);
    RUVIA_CHECK_EQ(held.first_[0], 1);
    RUVIA_CHECK_EQ(held.first_[1], 2);
    auto tail = queue.prepare_push_batch(5);
    RUVIA_CHECK_EQ(tail.first_.size(), std::size_t{2});
    tail.first_[0] = 4;
    tail.first_[1] = 5;
    queue.commit_push(2);
    RUVIA_CHECK_EQ(held.first_[0], 1);
    RUVIA_CHECK_EQ(held.first_[1], 2);
    queue.pop(2);
    batch = queue.prepare_push_batch(5);
    RUVIA_CHECK_EQ(batch.size(), std::size_t{2});
    queue.cancel_push();
    queue.pop(2);
    batch = queue.prepare_push_batch(4);
    RUVIA_CHECK_EQ(batch.first_.size(), std::size_t{4});
    for (std::size_t index = 0; index != batch.size(); ++index) {
        batch.first_[index] = static_cast<int>(index + 6);
    }
    queue.commit_push(4);
    const auto wrapped = queue.front_batch(5);
    RUVIA_CHECK_EQ(wrapped.first_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(wrapped.second_.size(), std::size_t{4});
    RUVIA_CHECK_EQ(wrapped.first_[0], 5);
    for (std::size_t index = 0; index != wrapped.second_.size(); ++index) {
        RUVIA_CHECK_EQ(wrapped.second_[index], static_cast<int>(index + 6));
    }
    queue.pop(5);
    RUVIA_CHECK(queue.empty());
    batch = queue.prepare_push_batch(4);
    RUVIA_CHECK_EQ(batch.first_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(batch.second_.size(), std::size_t{3});
    batch.first_[0] = 10;
    batch.second_[0] = 11;
    queue.commit_push(2);
    const auto prefix = queue.front_batch(5);
    RUVIA_CHECK_EQ(prefix.size(), std::size_t{2});
    RUVIA_CHECK_EQ(prefix.first_[0], 10);
    RUVIA_CHECK_EQ(prefix.second_[0], 11);
    queue.pop(2);
    RUVIA_CHECK(queue.prepare_push_batch(0).empty());
    RUVIA_CHECK(queue.front_batch(0).empty());
}

RUVIA_TEST(buffer_pool_local_raii_moves_rebinds_and_reclaims_out_of_order) {
    failing_memory_resource resource;
    {
        ruvia::buffer_pool pool(3, 32, &resource);
        resource.fail_after(0);
        auto first = pool.try_acquire();
        auto second = pool.try_acquire();
        auto third = pool.try_acquire();
        RUVIA_CHECK(first && second && third);
        RUVIA_CHECK(!pool.try_acquire());
        auto* borrowed = first->bytes().data();
        borrowed[0] = std::byte{42};
        second->reset();
        auto reused = pool.try_acquire();
        RUVIA_CHECK(reused);
        RUVIA_CHECK_EQ(borrowed[0], std::byte{42});
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{3});
        first->reset();
        third->reset();
        reused->reset();
        RUVIA_CHECK_EQ(pool.available(), pool.capacity());
        try {
            auto exceptional = pool.try_acquire();
            throw std::runtime_error("operation failed after acquiring buffer");
        } catch (const std::runtime_error&) {
        }
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(buffer_pool_credit_callback_transfers_without_reclaiming_on_consumer) {
    failing_memory_resource resource;
    {
        ruvia::buffer_pool pool(3, 32, &resource);
        ruvia::spsc_ring_queue<ruvia::buffer_credit> credits(3, &resource);
        resource.fail_after(0);
        const auto publish_credit = [](void* context_value, ruvia::buffer_credit credit) noexcept {
            auto& queue = *static_cast<ruvia::spsc_ring_queue<ruvia::buffer_credit>*>(context_value);
            if (!queue.try_push(std::move(credit))) {
                std::terminate();
            }
        };
        auto first = pool.try_acquire();
        auto second = pool.try_acquire();
        auto third = pool.try_acquire();
        first->set_return_callback({&credits, publish_credit});
        second->set_return_callback({&credits, publish_credit});
        third->set_return_callback({&credits, publish_credit});
        const auto index = first->index();
        std::thread consumer([first = std::move(*first), second = std::move(*second), third = std::move(*third)]() mutable {
            first.bytes()[0] = std::byte{91};
            second.reset();
            third = std::move(first);
            third.reset();
        });
        consumer.join();
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{3});
        ruvia::buffer_credit credit;
        std::size_t returned = 0;
        while (credits.try_pop(credit)) {
            pool.reclaim(std::move(credit));
            ++returned;
        }
        RUVIA_CHECK_EQ(returned, std::size_t{3});
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
        auto reused = pool.try_acquire();
        RUVIA_CHECK_EQ(reused->index(), index);
        RUVIA_CHECK_EQ(reused->bytes()[0], std::byte{91});
        pool.reclaim(reused->release_credit());
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(buffer_pool_partial_startup_allocation_failure_rolls_back) {
    for (std::size_t successful_allocations = 0; successful_allocations != 3; ++successful_allocations) {
        failing_memory_resource resource;
        resource.fail_after(successful_allocations);
        bool failed = false;
        try {
            ruvia::buffer_pool pool(3, 32, &resource);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(channel_lifecycle_stop_rejects_new_admission_and_preserves_started_work) {
    ruvia::spsc_channel_lifecycle lifecycle;
    auto admission = lifecycle.try_admit();
    RUVIA_CHECK(admission);
    std::thread consumer([&] {
        lifecycle.request_stop();
    });
    consumer.join();
    RUVIA_CHECK(lifecycle.stop_requested());
    RUVIA_CHECK(!lifecycle.try_admit());
    lifecycle.close();
    RUVIA_CHECK(!lifecycle.producer_closed());
    admission->reset();
    RUVIA_CHECK(lifecycle.producer_closed());
    RUVIA_CHECK(!lifecycle.consumer_finalized());
    std::thread final_consumer([&] {
        if (lifecycle.producer_closed()) {
            lifecycle.finalize();
        }
    });
    final_consumer.join();
    RUVIA_CHECK(lifecycle.consumer_finalized());
}

RUVIA_TEST(ring_queue_throwing_assignment_cancels_hidden_reservation_and_preserves_fifo) {
    struct throwing_value final {
        int value_{0};
        bool fail_{false};
        throwing_value& operator=(const throwing_value& input) {
            if (input.fail_) {
                throw std::runtime_error("payload assignment failed");
            }
            value_ = input.value_;
            return *this;
        }
    };
    ruvia::local_ring_queue<throwing_value> queue(3);
    RUVIA_CHECK(queue.try_push(throwing_value{1}));
    bool failed = false;
    try {
        RUVIA_CHECK(queue.try_push(throwing_value{2, true}));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    auto* prepared = queue.prepare_push();
    RUVIA_CHECK(prepared != nullptr);
    prepared->value_ = 3;
    queue.commit_push();
    const throwing_value fourth{4};
    RUVIA_CHECK(queue.try_push(fourth));
    throwing_value output;
    for (const int expected : {1, 3, 4}) {
        RUVIA_CHECK(queue.try_pop(output));
        RUVIA_CHECK_EQ(output.value_, expected);
    }
    RUVIA_CHECK(queue.empty());
}
