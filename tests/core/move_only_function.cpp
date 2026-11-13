#include "ruvia/core/move_only_function.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <type_traits>
#include <utility>

namespace ruvia::test::move_only_function_tests {

struct counted_delete final {
    int* destroyed_;

    void operator()(int* value) const noexcept {
        ++*destroyed_;
        delete value;
    }
};

using callback = ruvia::move_only_function<int()>;
using payload = std::unique_ptr<int, counted_delete>;

struct reentry_state final {
    callback* destination_{nullptr};
    callback replacement_;
    int expected_current_{0};
    int reentries_{0};
};

struct replacing_target final {
    reentry_state* state_;
    payload value_;

    replacing_target(reentry_state* state, int value, int& destroyed)
        : state_(state),
          value_(new int(value), {&destroyed}) {}

    replacing_target(replacing_target&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          value_(std::move(other.value_)) {}

    ~replacing_target() {
        if (state_ != nullptr) {
            auto* const state = state_;
            ++state->reentries_;
            EXPECT_TRUE(static_cast<bool>(*state->destination_));
            if (*state->destination_) {
                EXPECT_EQ((*state->destination_)(), state->expected_current_);
            }
            *state->destination_ = std::move(state->replacement_);
        }
    }

    int operator()() const noexcept {
        return *value_;
    }
};

callback make_replacing_target(int value, int& destroyed, bool heap_target, reentry_state* state = nullptr) {
    if (heap_target) {
        return callback([target = replacing_target(state, value, destroyed), padding = std::array<char, 64>{}] {
            (void)padding;
            return target();
        });
    }
    return callback(replacing_target(state, value, destroyed));
}

struct destroy_owner_delete final {
    std::unique_ptr<callback>* owner_;
    int* destroyed_;

    void operator()(int* value) const noexcept {
        ++*destroyed_;
        delete value;
        owner_->reset();
    }
};

struct callback_lifetime final {
    bool* alive_;

    explicit callback_lifetime(bool& alive) noexcept
        : alive_(&alive) {
        alive = true;
    }

    callback_lifetime(callback_lifetime&& other) noexcept
        : alive_(std::exchange(other.alive_, nullptr)) {}

    ~callback_lifetime() {
        if (alive_ != nullptr) {
            *alive_ = false;
        }
    }
};

struct clear_reentry_target final {
    callback_lifetime lifetime_;
    reentry_state* state_;

    explicit clear_reentry_target(bool& alive, reentry_state* state = nullptr) noexcept
        : lifetime_(alive),
          state_(state) {}

    clear_reentry_target(clear_reentry_target&& other) noexcept
        : lifetime_(std::move(other.lifetime_)),
          state_(std::exchange(other.state_, nullptr)) {}

    ~clear_reentry_target() {
        if (auto* const state = std::exchange(state_, nullptr)) {
            ++state->reentries_;
            EXPECT_FALSE(static_cast<bool>(*state->destination_));
            *state->destination_ = std::move(state->replacement_);
        }
    }

    int operator()() const noexcept {
        return lifetime_.alive_ != nullptr && *lifetime_.alive_;
    }
};

}  // namespace ruvia::test::move_only_function_tests

TEST(Ruvia, move_assignment_extracts_source_owned_by_old_target) {
    using callback = ruvia::move_only_function<void()>;
    using payload = std::unique_ptr<int, ruvia::test::move_only_function_tests::counted_delete>;

    for (const bool heap_target : {false, true}) {
        SCOPED_TRACE(heap_target);
        int calls = 0;
        int destroyed = 0;
        auto nested = std::make_unique<callback>(
            [value = payload(new int(42), {&destroyed}), &calls] { calls += *value; });
        auto* const source = nested.get();
        callback destination;
        if (heap_target) {
            destination = callback([owned = std::move(nested), padding = std::array<char, 64>{}] {
                (void)owned;
                (void)padding;
            });
        } else {
            destination = callback([owned = std::move(nested)] { (void)owned; });
        }

        destination = std::move(*source);
        EXPECT_EQ(destroyed, 0);
        destination();
        EXPECT_EQ(calls, 42);
        destination = nullptr;
        EXPECT_EQ(destroyed, 1);
    }
}

TEST(Ruvia, move_assignment_reentrant_replacements_release_each_target) {
    using namespace ruvia::test::move_only_function_tests;

    for (const bool heap_target : {false, true}) {
        for (const int reentry_count : {1, 4}) {
            SCOPED_TRACE(heap_target);
            SCOPED_TRACE(reentry_count);
            int destroyed = 0;
            callback destination;
            std::array<reentry_state, 4> states;
            for (int index = 0; index < reentry_count; ++index) {
                states[index].destination_ = &destination;
                states[index].expected_current_ = index + 2;
                auto* const next = index + 2 < reentry_count ? &states[index + 2] : nullptr;
                states[index].replacement_ = make_replacing_target(index + 3, destroyed, heap_target, next);
            }
            destination = make_replacing_target(1, destroyed, heap_target, &states[0]);
            callback source = make_replacing_target(2, destroyed, heap_target, reentry_count > 1 ? &states[1] : nullptr);

            destination = std::move(source);

            EXPECT_FALSE(static_cast<bool>(source));
            ASSERT_TRUE(static_cast<bool>(destination));
            EXPECT_EQ(destination(), reentry_count + 2);
            EXPECT_EQ(destroyed, reentry_count + 1);
            for (int index = 0; index < reentry_count; ++index) {
                EXPECT_EQ(states[index].reentries_, 1);
                EXPECT_FALSE(static_cast<bool>(states[index].replacement_));
            }
            destination = nullptr;
            EXPECT_EQ(destroyed, reentry_count + 2);
        }
    }
}

TEST(Ruvia, move_assignment_retirement_can_destroy_destination_owner) {
    using namespace ruvia::test::move_only_function_tests;
    using owner_payload = std::unique_ptr<int, destroy_owner_delete>;

    for (const bool heap_target : {false, true}) {
        SCOPED_TRACE(heap_target);
        int destroyed = 0;
        auto owner = std::make_unique<callback>();
        if (heap_target) {
            *owner = callback([value = owner_payload(new int(1), {&owner, &destroyed}), padding = std::array<char, 64>{}] {
                (void)padding;
                return *value;
            });
        } else {
            *owner = callback([value = owner_payload(new int(1), {&owner, &destroyed})] { return *value; });
        }
        callback source = make_replacing_target(2, destroyed, heap_target);

        *owner = std::move(source);

        EXPECT_EQ(owner, nullptr);
        EXPECT_FALSE(static_cast<bool>(source));
        EXPECT_EQ(destroyed, 2);
    }
}

TEST(Ruvia, null_assignment_preserves_reentrant_replacement_lifetime) {
    using namespace ruvia::test::move_only_function_tests;

    for (const bool heap_target : {false, true}) {
        SCOPED_TRACE(heap_target);
        bool original_alive = false;
        bool replacement_alive = false;
        callback destination;
        reentry_state state;
        state.destination_ = &destination;
        state.replacement_ = callback(clear_reentry_target(replacement_alive));
        if (heap_target) {
            destination = callback([target = clear_reentry_target(original_alive, &state), padding = std::array<char, 64>{}] {
                (void)padding;
                return target();
            });
        } else {
            destination = callback(clear_reentry_target(original_alive, &state));
        }

        destination = nullptr;

        EXPECT_FALSE(original_alive);
        EXPECT_TRUE(replacement_alive);
        EXPECT_EQ(state.reentries_, 1);
        EXPECT_FALSE(static_cast<bool>(state.replacement_));
        ASSERT_TRUE(static_cast<bool>(destination));
        EXPECT_EQ(destination(), 1);
        destination = nullptr;
        EXPECT_FALSE(replacement_alive);
    }
}

TEST(Ruvia, noexcept_function_preserves_mutable_target_through_const_owner) {
    using callback = ruvia::move_only_function<int() noexcept>;
    const auto throwing = [] { return 0; };
    static_assert(!std::is_constructible_v<callback, decltype(throwing)>);
    static_assert(std::is_nothrow_invocable_r_v<int, const callback&>);

    callback source([count = std::make_unique<int>(4)]() mutable noexcept {
        return ++*count;
    });
    const callback destination(std::move(source));
    EXPECT_FALSE(source);
    EXPECT_EQ(destination(), 5);
    EXPECT_EQ(destination(), 6);
}
