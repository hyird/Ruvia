#include "ruvia/core/local_ring_queue.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "failing_memory_resource.h"

namespace {

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

}  // namespace

TEST(Ruvia, local_ring_queue_fifo_full_recovery_and_repeated_slot_wrap) {
    for (const std::size_t capacity : {1, 3, 5, 8}) {
        ruvia::local_ring_queue<std::size_t> queue(capacity);
        EXPECT_EQ(queue.capacity(), capacity);
        EXPECT_TRUE(queue.empty());
        EXPECT_TRUE(!queue.full());
        EXPECT_EQ(queue.size(), std::size_t{0});
        const auto& const_queue = queue;
        EXPECT_TRUE(const_queue.front() == nullptr);
        std::size_t next_input = 1;
        std::size_t next_output = 1;
        for (std::size_t round = 0; round != 1000; ++round) {
            for (std::size_t index = 0; index != capacity; ++index) {
                EXPECT_TRUE(queue.try_push(next_input++));
            }
            EXPECT_TRUE(queue.full());
            EXPECT_TRUE(!queue.empty());
            EXPECT_EQ(queue.size(), capacity);
            EXPECT_TRUE(const_queue.front() != nullptr);
            EXPECT_TRUE(queue.prepare_push() == nullptr);
            EXPECT_TRUE(!queue.try_push(std::size_t{0}));

            const auto released = (capacity + 1) / 2;
            std::size_t value = 0;
            for (std::size_t index = 0; index != released; ++index) {
                EXPECT_TRUE(queue.try_pop(value));
                EXPECT_EQ(value, next_output++);
            }
            for (std::size_t index = 0; index != released; ++index) {
                const auto input = next_input++;
                EXPECT_TRUE(queue.try_push(input));
            }
            EXPECT_TRUE(queue.prepare_push() == nullptr);
            for (std::size_t index = 0; index != capacity; ++index) {
                EXPECT_TRUE(queue.try_pop(value));
                EXPECT_EQ(value, next_output++);
            }
            EXPECT_TRUE(queue.front() == nullptr);
            value = 91;
            EXPECT_TRUE(!queue.try_pop(value));
            EXPECT_EQ(value, std::size_t{91});
        }
        EXPECT_EQ(next_input, next_output);
    }
}

TEST(Ruvia, local_ring_queue_preparation_is_unpublished_and_cancel_reuses_capacity) {
    for (const std::size_t capacity : {1, 3}) {
        ruvia::local_ring_queue<int> queue(capacity);
        queue.cancel_push();
        for (int attempt_value = 0; attempt_value != 100; ++attempt_value) {
            auto* slot = queue.prepare_push();
            EXPECT_TRUE(slot != nullptr);
            if (!slot) {
                return;
            }
            *slot = attempt_value;
            EXPECT_TRUE(queue.front() == nullptr);
            int value = -1;
            EXPECT_TRUE(!queue.try_pop(value));
            EXPECT_EQ(value, -1);
            queue.cancel_push();
            queue.cancel_push();
            EXPECT_TRUE(queue.front() == nullptr);
        }
        auto* slot = queue.prepare_push();
        EXPECT_TRUE(slot != nullptr);
        if (!slot) {
            return;
        }
        *slot = 173;
        queue.commit_push();
        EXPECT_TRUE(queue.front() != nullptr);
        int value = 0;
        EXPECT_TRUE(queue.try_pop(value));
        EXPECT_EQ(value, 173);
        EXPECT_TRUE(queue.front() == nullptr);
    }
}

TEST(Ruvia, local_ring_queue_front_borrow_prevents_slot_overwrite) {
    ruvia::local_ring_queue<int> queue(3);
    EXPECT_TRUE(queue.try_push(11));
    const auto* held = queue.front();
    EXPECT_TRUE(held != nullptr);
    if (!held) {
        return;
    }
    EXPECT_TRUE(queue.try_push(22));
    EXPECT_TRUE(queue.try_push(33));
    for (unsigned attempt_value = 0; attempt_value != 50; ++attempt_value) {
        EXPECT_TRUE(queue.prepare_push() == nullptr);
        EXPECT_TRUE(!queue.try_push(44));
        EXPECT_EQ(*held, 11);
        EXPECT_TRUE(queue.front() == held);
    }
    queue.pop();
    EXPECT_TRUE(queue.try_push(44));
    int value = 0;
    for (const int expected : {22, 33, 44}) {
        EXPECT_TRUE(queue.try_pop(value));
        EXPECT_EQ(value, expected);
    }
    EXPECT_TRUE(queue.front() == nullptr);
}

TEST(Ruvia, local_ring_queue_moves_payload_and_pop_keeps_slot_constructed) {
    std::size_t live = 0;
    {
        ruvia::local_ring_queue<std::unique_ptr<retained_payload>> queue(1);
        auto input = std::make_unique<retained_payload>(live, 73);
        EXPECT_TRUE(queue.try_push(std::move(input)));
        EXPECT_TRUE(!input);
        auto* slot = queue.front();
        EXPECT_TRUE(slot != nullptr);
        if (!slot) {
            return;
        }
        std::unique_ptr<retained_payload> output = std::move(*slot);
        queue.pop();
        EXPECT_TRUE(output != nullptr);
        if (!output) {
            return;
        }
        EXPECT_EQ(output->value_, 73);
        EXPECT_EQ(live, std::size_t{1});
        output.reset();
        EXPECT_EQ(live, std::size_t{0});

        EXPECT_TRUE(queue.try_push(std::make_unique<retained_payload>(live, 89)));
        queue.pop();
        EXPECT_TRUE(queue.front() == nullptr);
        EXPECT_EQ(live, std::size_t{1});
        queue.cancel_push();
        EXPECT_EQ(live, std::size_t{1});
        EXPECT_TRUE(queue.try_push(std::make_unique<retained_payload>(live, 97)));
        EXPECT_EQ(live, std::size_t{1});
        const auto* front = queue.front();
        EXPECT_TRUE(front && *front);
        if (front && *front) {
            EXPECT_EQ((*front)->value_, 97);
        }
    }
    EXPECT_EQ(live, std::size_t{0});
}

TEST(Ruvia, local_ring_queue_pmr_storage_reclaims_all_slots_without_hot_path_allocation) {
    tracked_slot::reset();
    failing_memory_resource resource;
    {
        ruvia::local_ring_queue<tracked_slot> queue(5, &resource);
        EXPECT_EQ(resource.live_allocations(), std::size_t{1});
        EXPECT_EQ(tracked_slot::live_, std::size_t{5});
        resource.fail_after(0);
        for (std::size_t round = 0; round != 100; ++round) {
            for (std::size_t index = 0; index != queue.capacity(); ++index) {
                auto* slot = queue.prepare_push();
                EXPECT_TRUE(slot != nullptr);
                if (!slot) {
                    return;
                }
                slot->value_ = round * queue.capacity() + index;
                queue.commit_push();
            }
            for (std::size_t index = 0; index != queue.capacity(); ++index) {
                const auto* value = queue.front();
                EXPECT_TRUE(value != nullptr);
                if (!value) {
                    return;
                }
                EXPECT_EQ(value->value_, round * queue.capacity() + index);
                queue.pop();
            }
        }
        EXPECT_EQ(tracked_slot::attempts_, std::size_t{5});
        EXPECT_EQ(tracked_slot::destroyed_, std::size_t{0});
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(tracked_slot::live_, std::size_t{0});
    EXPECT_EQ(tracked_slot::destroyed_, std::size_t{5});
}

TEST(Ruvia, local_ring_queue_startup_allocation_failure_leaves_resource_reusable) {
    failing_memory_resource resource;
    resource.fail_after(0);
    bool failed = false;
    try {
        ruvia::local_ring_queue<int> queue(3, &resource);
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    {
        ruvia::local_ring_queue<int> queue(3, &resource);
        EXPECT_TRUE(queue.try_push(61));
        int output = 0;
        EXPECT_TRUE(queue.try_pop(output));
        EXPECT_EQ(output, 61);
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, local_ring_queue_partial_slot_construction_failure_reclaims_completed_slots) {
    failing_memory_resource resource;
    for (std::size_t failure = 1; failure != 6; ++failure) {
        tracked_slot::reset(failure);
        bool failed = false;
        try {
            ruvia::local_ring_queue<tracked_slot> queue(5, &resource);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(tracked_slot::attempts_, failure);
        EXPECT_EQ(tracked_slot::live_, std::size_t{0});
        EXPECT_EQ(tracked_slot::destroyed_, failure - 1);
        EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    }
    tracked_slot::reset();
    {
        ruvia::local_ring_queue<tracked_slot> queue(5, &resource);
        auto* slot = queue.prepare_push();
        EXPECT_TRUE(slot != nullptr);
        if (!slot) {
            return;
        }
        slot->value_ = 109;
        queue.commit_push();
        const auto* front = queue.front();
        EXPECT_TRUE(front != nullptr);
        if (front) {
            EXPECT_EQ(front->value_, std::size_t{109});
        }
    }
    EXPECT_EQ(tracked_slot::destroyed_, std::size_t{5});
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, local_ring_queue_zero_capacity_is_rejected) {
    failing_memory_resource resource;
    bool rejected = false;
    try {
        ruvia::local_ring_queue<int> queue(0, &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, local_ring_queue_throwing_assignment_cancels_hidden_reservation_and_preserves_fifo) {
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
    EXPECT_TRUE(queue.try_push(throwing_value{1}));
    bool failed = false;
    try {
        EXPECT_TRUE(queue.try_push(throwing_value{2, true}));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    auto* prepared = queue.prepare_push();
    EXPECT_TRUE(prepared != nullptr);
    prepared->value_ = 3;
    queue.commit_push();
    const throwing_value fourth{4};
    EXPECT_TRUE(queue.try_push(fourth));
    throwing_value output;
    for (const int expected : {1, 3, 4}) {
        EXPECT_TRUE(queue.try_pop(output));
        EXPECT_EQ(output.value_, expected);
    }
    EXPECT_TRUE(queue.front() == nullptr);
}
