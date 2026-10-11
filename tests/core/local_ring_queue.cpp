#include "ruvia/core/local_ring_queue.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "failing_memory_resource.h"
#include "test_harness.h"

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

RUVIA_TEST(local_ring_queue_fifo_full_recovery_and_repeated_slot_wrap) {
    for (const std::size_t capacity : {1, 3, 5, 8}) {
        ruvia::local_ring_queue<std::size_t> queue(capacity);
        RUVIA_CHECK_EQ(queue.capacity(), capacity);
        std::size_t next_input = 1;
        std::size_t next_output = 1;
        for (std::size_t round = 0; round != 1000; ++round) {
            for (std::size_t index = 0; index != capacity; ++index) {
                RUVIA_CHECK(queue.try_push(next_input++));
            }
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
            RUVIA_CHECK(queue.prepare_push() == nullptr);
            for (std::size_t index = 0; index != capacity; ++index) {
                RUVIA_CHECK(queue.try_pop(value));
                RUVIA_CHECK_EQ(value, next_output++);
            }
            RUVIA_CHECK(queue.front() == nullptr);
            value = 91;
            RUVIA_CHECK(!queue.try_pop(value));
            RUVIA_CHECK_EQ(value, std::size_t{91});
        }
        RUVIA_CHECK_EQ(next_input, next_output);
    }
}

RUVIA_TEST(local_ring_queue_preparation_is_unpublished_and_cancel_reuses_capacity) {
    for (const std::size_t capacity : {1, 3}) {
        ruvia::local_ring_queue<int> queue(capacity);
        queue.cancel_push();
        for (int attempt_value = 0; attempt_value != 100; ++attempt_value) {
            auto* slot = queue.prepare_push();
            RUVIA_CHECK(slot != nullptr);
            if (!slot) {
                return;
            }
            *slot = attempt_value;
            RUVIA_CHECK(queue.front() == nullptr);
            int value = -1;
            RUVIA_CHECK(!queue.try_pop(value));
            RUVIA_CHECK_EQ(value, -1);
            queue.cancel_push();
            queue.cancel_push();
            RUVIA_CHECK(queue.front() == nullptr);
        }
        auto* slot = queue.prepare_push();
        RUVIA_CHECK(slot != nullptr);
        if (!slot) {
            return;
        }
        *slot = 173;
        queue.commit_push();
        RUVIA_CHECK(queue.front() != nullptr);
        int value = 0;
        RUVIA_CHECK(queue.try_pop(value));
        RUVIA_CHECK_EQ(value, 173);
        RUVIA_CHECK(queue.front() == nullptr);
    }
}

RUVIA_TEST(local_ring_queue_front_borrow_prevents_slot_overwrite) {
    ruvia::local_ring_queue<int> queue(3);
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
    RUVIA_CHECK(queue.front() == nullptr);
}

RUVIA_TEST(local_ring_queue_moves_payload_and_pop_keeps_slot_constructed) {
    std::size_t live = 0;
    {
        ruvia::local_ring_queue<std::unique_ptr<retained_payload>> queue(1);
        auto input = std::make_unique<retained_payload>(live, 73);
        RUVIA_CHECK(queue.try_push(std::move(input)));
        RUVIA_CHECK(!input);
        auto* slot = queue.front();
        RUVIA_CHECK(slot != nullptr);
        if (!slot) {
            return;
        }
        std::unique_ptr<retained_payload> output = std::move(*slot);
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
        RUVIA_CHECK(queue.front() == nullptr);
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

RUVIA_TEST(local_ring_queue_pmr_storage_reclaims_all_slots_without_hot_path_allocation) {
    tracked_slot::reset();
    failing_memory_resource resource;
    {
        ruvia::local_ring_queue<tracked_slot> queue(5, &resource);
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

RUVIA_TEST(local_ring_queue_startup_allocation_failure_leaves_resource_reusable) {
    failing_memory_resource resource;
    resource.fail_after(0);
    bool failed = false;
    try {
        ruvia::local_ring_queue<int> queue(3, &resource);
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    {
        ruvia::local_ring_queue<int> queue(3, &resource);
        RUVIA_CHECK(queue.try_push(61));
        int output = 0;
        RUVIA_CHECK(queue.try_pop(output));
        RUVIA_CHECK_EQ(output, 61);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(local_ring_queue_partial_slot_construction_failure_reclaims_completed_slots) {
    failing_memory_resource resource;
    for (std::size_t failure = 1; failure != 6; ++failure) {
        tracked_slot::reset(failure);
        bool failed = false;
        try {
            ruvia::local_ring_queue<tracked_slot> queue(5, &resource);
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
        ruvia::local_ring_queue<tracked_slot> queue(5, &resource);
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

RUVIA_TEST(local_ring_queue_zero_capacity_is_rejected) {
    failing_memory_resource resource;
    bool rejected = false;
    try {
        ruvia::local_ring_queue<int> queue(0, &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(local_ring_queue_throwing_assignment_cancels_hidden_reservation_and_preserves_fifo) {
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
    RUVIA_CHECK(queue.front() == nullptr);
}
