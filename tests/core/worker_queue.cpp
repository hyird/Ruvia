#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_wait.h"
TEST(Ruvia, worker_queue_yields_to_io_context_between_batches) {
    struct reposting_task final {
        const ruvia::worker_ref* worker_;
        ruvia::event_loop_attachment* attachment_;
        const bool* foreign_ran_;
        unsigned* runs_;
        void operator()() const {
            ++*runs_;
            if (!*foreign_ran_ && *runs_ < 1000) {
                static_cast<void>(worker_->post(*this));
            } else {
                attachment_->stop();
            }
        }
    };
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    bool foreign_ran{};
    unsigned runs{};
    EXPECT_TRUE(worker_value.post(reposting_task{&worker_value, &attachment, &foreign_ran, &runs}).accepted());
    asio::post(context, [&foreign_ran] { foreign_ran = true; });
    attachment.run();
    // The repost lands behind the plain handler queued after the first batch.
    EXPECT_TRUE(foreign_ran);
    EXPECT_EQ(runs, 2u);
}

TEST(Ruvia, worker_cancellation_registration_retires_before_slot_reuse) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned accepted_{};
        unsigned stale_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++accepted_;
            } else {
                ++stale_;
            }
        }
    } state;
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    auto target = ruvia::make_worker_cancellation_target(state, worker_value);
    using registration = ruvia::worker_cancellation_registration<ruvia::worker_cancellation_target<owner>>;
    using target_owner = std::shared_ptr<ruvia::worker_cancellation_target<owner>>;
    static_assert(std::is_constructible_v<registration, target_owner&, std::uint64_t&>);
    static_assert(std::is_constructible_v<registration, const target_owner&, std::uint64_t&>);
    static_assert(!std::is_constructible_v<registration, target_owner&&, std::uint64_t&>);
    static_assert(!std::is_constructible_v<registration, const target_owner&&, std::uint64_t&>);
    std::optional<registration> current;
    ruvia::stop_source old_stop;
    ruvia::stop_source new_stop;
    std::uint64_t old_id{};
    bool identity_cleared{};
    EXPECT_TRUE(worker_value.post([&] {
                                current.emplace(target, state.identity_);
                                old_id = current->id();
                                current->arm(old_stop.token());
                                // Foreign cancellation is queued behind this owner turn.
                                std::jthread requester([&] { old_stop.request_stop(); });
                                requester.join();
                                current.reset();
                                identity_cleared = state.identity_ == 0;
                                current.emplace(target, state.identity_);
                                current->arm(new_stop.token());
                            })
            .accepted());
    // Drain the ready handlers, including the queued foreign cancellation,
    // while the loop stays attached for the second turn.
    context.poll();
    EXPECT_TRUE(identity_cleared);
    EXPECT_TRUE(state.identity_ != 0 && state.identity_ != old_id);
    EXPECT_EQ(state.accepted_, 0u);
    EXPECT_EQ(state.stale_, 1u);
    EXPECT_TRUE(worker_value.post([&] {
                                new_stop.request_stop();
                                current.reset();
                                target->detach(state);
                                attachment.stop();
                            })
            .accepted());
    attachment.run();
    EXPECT_EQ(state.accepted_, 1u);
    EXPECT_EQ(state.identity_, std::uint64_t{0});
}

TEST(Ruvia, worker_cancellation_registration_handles_precancel_and_exception_unwind) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned cancelled_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++cancelled_;
            }
        }
    } state;
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    auto target = ruvia::make_worker_cancellation_target(state, worker_value);
    ruvia::stop_source source;
    source.request_stop();
    bool unwound = false;
    EXPECT_TRUE(worker_value.post([&] {
                                try {
                                    ruvia::worker_cancellation_registration registration(target, state.identity_);
                                    registration.arm(source.token());
                                    throw std::runtime_error("operation failed");
                                } catch (const std::runtime_error&) {
                                    unwound = state.identity_ == 0;
                                }
                                target->detach(state);
                                attachment.stop();
                            })
            .accepted());
    attachment.run();
    EXPECT_TRUE(unwound);
    EXPECT_EQ(state.cancelled_, 1u);
}

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <utility>
#include <vector>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"

#include "worker_task_fixture.h"

namespace worker_resume_test {

// Suspends and has the worker resume the coroutine, asked from the worker
// itself or from a foreign thread. The step is recorded negated once the
// request returned, which is before the resumption when it is never inline.
struct deferred_resume final {
    const ruvia::worker_ref& worker_;
    std::vector<int>& order_;
    int step_;
    bool foreign_{false};

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const {
        if (foreign_) {
            std::jthread([this, continuation] { worker_.defer_resume(continuation); }).join();
        } else {
            worker_.defer_resume(continuation);
        }
        order_.push_back(-step_);
    }
    void await_resume() const noexcept {}
};

ruvia::task<void> resume_twice(const ruvia::worker_ref& worker, std::vector<int>& order, int first, bool& on_worker) {
    order.push_back(first);
    co_await deferred_resume{worker, order, first};
    on_worker = on_worker && worker.is_current();
    order.push_back(first + 1);
    co_await deferred_resume{worker, order, first + 2, true};
    on_worker = on_worker && worker.is_current();
    order.push_back(first + 3);
}

[[nodiscard]] bool before(const std::vector<int>& order, int first, int second) {
    const auto left = std::ranges::find(order, first);
    const auto right = std::ranges::find(order, second);
    return left != order.end() && right != order.end() && left < right;
}

}  // namespace worker_resume_test

// A deferred resumption runs on the worker in a later turn, never inside the
// request, whether it was asked on the worker or from another thread, and
// requests from the worker's own thread resume in request order -- under the
// default event loop and the single-owner one production workers use.
TEST(Ruvia, worker_ref_defers_resumption_to_its_worker) {
    using worker_resume_test::before;
    for (const int hint : {ASIO_CONCURRENCY_HINT_DEFAULT, ASIO_CONCURRENCY_HINT_UNSAFE_IO}) {
        asio::io_context context(hint);
        auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
        const auto worker_value = attachment.loop().worker();
        std::vector<int> order;
        bool on_worker = true;
        ruvia::test::run_worker_tasks(attachment,
            worker_resume_test::resume_twice(worker_value, order, 10, on_worker),
            worker_resume_test::resume_twice(worker_value, order, 20, on_worker));
        EXPECT_TRUE(on_worker);
        EXPECT_EQ(order.size(), std::size_t{10});
        for (const int first : {10, 20}) {
            EXPECT_TRUE(before(order, -first, first + 1));
            EXPECT_TRUE(before(order, -(first + 2), first + 3));
        }
        EXPECT_TRUE(before(order, 11, 21));
    }
}

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
