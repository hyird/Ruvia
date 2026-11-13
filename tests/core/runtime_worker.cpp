#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/root_task.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"

namespace ruvia::test::runtime_worker_tests {

static_assert(std::is_constructible_v<worker_signal, worker_ref&>);
static_assert(std::is_constructible_v<worker_signal, const worker_ref&>);
static_assert(!std::is_constructible_v<worker_signal, worker_ref&&>);
static_assert(!std::is_constructible_v<worker_signal, const worker_ref&&>);
static_assert(std::is_constructible_v<task_scope, worker_ref&>);
static_assert(std::is_constructible_v<task_scope, const worker_ref&>);
static_assert(!std::is_constructible_v<task_scope, worker_ref&&>);
static_assert(!std::is_constructible_v<task_scope, const worker_ref&&>);

struct root_assignment_result final {
    explicit root_assignment_result(int value, std::function<void()>* on_retire = nullptr,
        std::unique_ptr<root_task<root_assignment_result>> child = {}) noexcept
        : value_(value),
          on_retire_(on_retire),
          child_(std::move(child)) {}
    root_assignment_result(root_assignment_result&& other) noexcept
        : value_(other.value_),
          on_retire_(std::exchange(other.on_retire_, nullptr)),
          child_(std::move(other.child_)) {}
    root_assignment_result(const root_assignment_result&) = delete;
    root_assignment_result& operator=(const root_assignment_result&) = delete;
    root_assignment_result& operator=(root_assignment_result&&) = delete;
    ~root_assignment_result() {
        if (on_retire_ != nullptr && *on_retire_) {
            auto callback = std::exchange(*on_retire_, {});
            callback();
        }
    }

    int value_;
    std::function<void()>* on_retire_;
    std::unique_ptr<root_task<root_assignment_result>> child_;
};

task<root_assignment_result> make_root_assignment_result(root_assignment_result value, bool fail = false) {
    if (fail) {
        throw std::runtime_error("expected root task failure");
    }
    co_return std::move(value);
}

TEST(Ruvia, WorkerRootMoveAssignmentCapturesSourceBeforeRetiringOldResult) {
    std::function<void()> retirement;
    event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    auto source = loop.start(make_root_assignment_result(root_assignment_result(42)));
    auto replacement = loop.start(make_root_assignment_result(root_assignment_result(99)));
    auto destination = loop.start(make_root_assignment_result(root_assignment_result(7, &retirement)));
    loops.start();
    source.wait();
    replacement.wait();
    destination.wait();
    loops.stop();
    loops.join();

    bool retired = false;
    retirement = [&] {
        retired = true;
        source = std::move(replacement);
    };
    destination = std::move(source);
    EXPECT_TRUE(retired);
    EXPECT_EQ(std::move(destination).get().value_, 42);
    ASSERT_TRUE(source.valid());
    EXPECT_EQ(std::move(source).get().value_, 99);
}

TEST(Ruvia, WorkerRootMoveAssignmentCanTakeSourceOwnedByOldResult) {
    for (const bool fail : {false, true}) {
        SCOPED_TRACE(fail);
        std::function<void()> retirement;
        event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
        const auto loop = loops.loop(0);
        auto child = std::make_unique<root_task<root_assignment_result>>(
            loop.start(make_root_assignment_result(root_assignment_result(42), fail)));
        auto* const source = child.get();
        auto destination = loop.start(make_root_assignment_result(
            root_assignment_result(7, &retirement, std::move(child))));
        loops.start();
        source->wait();
        destination.wait();
        loops.stop();
        loops.join();

        bool retired = false;
        retirement = [&] { retired = true; };
        destination = std::move(*source);
        EXPECT_TRUE(retired);
        if (fail) {
            EXPECT_THROW(static_cast<void>(std::move(destination).get()), std::runtime_error);
        } else {
            EXPECT_EQ(std::move(destination).get().value_, 42);
        }
        EXPECT_FALSE(destination.valid());
    }
}

}  // namespace ruvia::test::runtime_worker_tests

namespace {

struct payload_tracker final {
    void record_acquire() noexcept {
        ++allocations_;
        ++live_;
    }

    void record_release() noexcept {
        ++deallocations_;
        --live_;
    }

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocations_;
    }

    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return deallocations_;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_;
    }

    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    std::size_t live_{0};
};

struct tracked_bytes final {
    tracked_bytes(payload_tracker* tracker, std::size_t count, char fill)
        : tracker_(tracker),
          bytes_(count, fill) {
        if (tracker_ != nullptr) {
            tracker_->record_acquire();
        }
    }

    tracked_bytes(const tracked_bytes& other)
        : tracker_(other.tracker_),
          bytes_(other.bytes_) {
        if (tracker_ != nullptr && !bytes_.empty()) {
            tracker_->record_acquire();
        }
    }

    tracked_bytes(tracked_bytes&& other) noexcept
        : tracker_(std::exchange(other.tracker_, nullptr)),
          bytes_(std::move(other.bytes_)) {}

    tracked_bytes& operator=(const tracked_bytes&) = delete;
    tracked_bytes& operator=(tracked_bytes&&) = delete;

    ~tracked_bytes() {
        if (tracker_ != nullptr && !bytes_.empty()) {
            tracker_->record_release();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] char front() const noexcept {
        return bytes_.front();
    }

    [[nodiscard]] char back() const noexcept {
        return bytes_.back();
    }

    payload_tracker* tracker_{nullptr};
    std::vector<char> bytes_;
};

struct post_payload final {
    explicit post_payload(payload_tracker* resource)
        : bytes_(resource, 4096, 'x') {}
    post_payload(post_payload&&) noexcept = default;
    post_payload(const post_payload&) = delete;
    tracked_bytes bytes_;
};

struct reentrant_post_state final {
    ruvia::event_loop* loop_;
    std::unique_ptr<ruvia::event_loop_pool>* pool_;
    std::promise<void>* completion_;
    std::atomic_bool* payload_valid_;

    void shut_down_loop() noexcept {
        *loop_ = ruvia::event_loop{};
        pool_->reset();
    }
};

struct inline_reentrant_post_callable final {
    explicit inline_reentrant_post_callable(reentrant_post_state& state_value) noexcept
        : state_(&state_value) {
        payload_.fill('x');
    }
    inline_reentrant_post_callable(const inline_reentrant_post_callable& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {
        state_->shut_down_loop();
    }
    inline_reentrant_post_callable(inline_reentrant_post_callable&& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {
        state_->shut_down_loop();
    }
    ~inline_reentrant_post_callable() {
        payload_.fill('\0');
    }

    void operator()() {
        state_->payload_valid_->store(payload_.size() == 16 && payload_.front() == 'x' &&
                                          payload_.back() == 'x',
            std::memory_order_release);
        state_->completion_->set_value();
    }

    reentrant_post_state* state_;
    std::array<char, 16> payload_{};
};

struct heap_reentrant_post_callable final {
    explicit heap_reentrant_post_callable(reentrant_post_state& state_value,
        payload_tracker* resource)
        : state_(&state_value),
          payload_(resource, 4096, 'x') {}
    heap_reentrant_post_callable(const heap_reentrant_post_callable& other)
        : state_(other.state_),
          payload_(other.payload_) {
        state_->shut_down_loop();
    }
    heap_reentrant_post_callable(heap_reentrant_post_callable&& other) noexcept
        : state_(other.state_),
          payload_(std::move(other.payload_)) {
        state_->shut_down_loop();
    }
    ~heap_reentrant_post_callable() = default;

    void operator()() {
        state_->payload_valid_->store(payload_.size() == 4096 && payload_.front() == 'x' &&
                                          payload_.back() == 'x',
            std::memory_order_release);
        state_->completion_->set_value();
    }

    reentrant_post_state* state_;
    tracked_bytes payload_;
};

struct reentrant_worker_post_state final {
    std::optional<ruvia::worker_ref> worker_;
    std::unique_ptr<ruvia::event_loop_pool> pool_;
    bool retired_{false};
    bool payload_valid_{false};
    bool erase_on_destroy_{false};

    void retire() noexcept {
        if (retired_) {
            return;
        }
        retired_ = true;
        worker_.reset();
        pool_.reset();
    }
};

struct inline_worker_callable final {
    explicit inline_worker_callable(reentrant_worker_post_state& state_value) noexcept
        : state_(&state_value) {
        payload_.fill('i');
    }
    inline_worker_callable(const inline_worker_callable&) = delete;
    inline_worker_callable(inline_worker_callable&& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {
        state_->retire();
    }
    void operator()() {
        state_->payload_valid_ = payload_.front() == 'i' && payload_.back() == 'i';
    }
    reentrant_worker_post_state* state_;
    std::array<char, 16> payload_{};
};

struct heap_worker_callable final {
    explicit heap_worker_callable(reentrant_worker_post_state& state_value, payload_tracker* resource)
        : state_(&state_value),
          payload_(resource, 4096, 'h') {}
    heap_worker_callable(const heap_worker_callable& other)
        : state_(other.state_),
          payload_(other.payload_) {
        state_->retire();
    }
    heap_worker_callable(heap_worker_callable&&) noexcept = default;
    void operator()() {
        state_->payload_valid_ = payload_.size() == 4096 && payload_.front() == 'h' &&
                                 payload_.back() == 'h';
    }
    reentrant_worker_post_state* state_;
    tracked_bytes payload_;
};

struct erased_worker_callable final {
    explicit erased_worker_callable(reentrant_worker_post_state& state_value) noexcept
        : state_(&state_value) {
        payload_.fill('e');
    }
    erased_worker_callable(const erased_worker_callable&) = delete;
    erased_worker_callable(erased_worker_callable&& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {}
    ~erased_worker_callable() {
        if (state_->erase_on_destroy_) {
            state_->retire();
        }
    }
    void operator()() {
        state_->payload_valid_ = payload_.front() == 'e' && payload_.back() == 'e';
    }
    reentrant_worker_post_state* state_;
    std::array<char, 16> payload_{};
};

bool test_worker_ref_callable_lifetime() {
    const auto run_rejected = [](ruvia::post_outcome<> rejected, bool& payload_valid) {
        if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr) {
            return false;
        }
        auto task_value = std::move(rejected).take_rejected();
        ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
        if (recovery.loop(0).worker().post(std::move(task_value)) != ruvia::post_status::accepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        return payload_valid;
    };

    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).worker());
        std::optional<inline_worker_callable> input;
        input.emplace(state);
        auto rejected = state.worker_->post(std::move(*input));
        if (!state.retired_ || state.worker_ || state.pool_ || !run_rejected(std::move(rejected), state.payload_valid_)) {
            return false;
        }
    }

    payload_tracker resource;
    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).worker());
        std::optional<heap_worker_callable> input;
        input.emplace(state, &resource);
        auto rejected = state.worker_->post(*input);
        input.reset();
        if (!state.retired_ || state.worker_ || state.pool_ ||
            !run_rejected(std::move(rejected), state.payload_valid_) ||
            resource.allocation_count() != resource.deallocation_count()) {
            return false;
        }
    }

    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).worker());
        ruvia::move_only_function<void()> input{erased_worker_callable(state)};
        state.erase_on_destroy_ = true;
        auto rejected = state.worker_->post(std::move(input));
        if (!state.retired_ || state.worker_ || state.pool_ ||
            !run_rejected(std::move(rejected), state.payload_valid_)) {
            return false;
        }
    }
    if (resource.allocation_count() != resource.deallocation_count()) {
        return false;
    }

    struct throw_on_copy final {
        explicit throw_on_copy(payload_tracker* resource_value, const std::atomic<bool>& fail)
            : bytes_(resource_value, 4096, 'c'),
              fail_(fail) {}
        throw_on_copy(const throw_on_copy& other)
            : bytes_(other.bytes_),
              fail_(other.fail_) {
            if (fail_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("copy failed");
            }
        }
        throw_on_copy(throw_on_copy&&) noexcept = default;
        void operator()() const {}
        tracked_bytes bytes_;
        const std::atomic<bool>& fail_;
    };
    struct throw_on_move final {
        explicit throw_on_move(payload_tracker* resource_value, const std::atomic<bool>& fail)
            : bytes_(resource_value, 4096, 'm'),
              fail_(fail) {}
        throw_on_move(throw_on_move&& other)
            : bytes_(std::move(other.bytes_)),
              fail_(other.fail_) {
            if (fail_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("move failed");
            }
        }
        void operator()() const {}
        tracked_bytes bytes_;
        const std::atomic<bool>& fail_;
    };

    asio::io_context exception_context;
    auto exception_attachment = ruvia::attach_event_loop(exception_context, {.queue_capacity_ = 1});
    const auto exception_worker = exception_attachment.loop().worker();
    payload_tracker exception_resource;
    // Keep failure injection observable at runtime, including under whole-program optimization.
    std::atomic<bool> fail_construction{true};
    {
        throw_on_copy copy_input(&exception_resource, fail_construction);
        throw_on_move move_input(&exception_resource, fail_construction);
        bool copy_threw = false;
        bool move_threw = false;
        try {
            static_cast<void>(exception_worker.post(copy_input));
        } catch (const std::runtime_error&) {
            copy_threw = true;
        }
        try {
            static_cast<void>(exception_worker.post(std::move(move_input)));
        } catch (const std::runtime_error&) {
            move_threw = true;
        }
        fail_construction.store(false, std::memory_order_relaxed);
        const auto after_exception = exception_worker.post(copy_input);
        exception_context.poll();
        if (!copy_threw || !move_threw || !exception_worker.valid() || !exception_worker.accepting() ||
            after_exception != ruvia::post_status::accepted) {
            return false;
        }
    }
    if (exception_resource.allocation_count() != exception_resource.deallocation_count()) {
        return false;
    }

    struct worker_reentry_state final {
        std::optional<ruvia::worker_ref>* source_;
        std::optional<asio::io_context>* context_;
        ruvia::event_loop_attachment* attachment_;
        bool detach_;
        bool retired_{false};
        bool payload_valid_{false};
        payload_tracker* resource_;

        void retire() noexcept {
            if (retired_) {
                return;
            }
            retired_ = true;
            source_->reset();
            if (detach_) {
                // No root task or cleanup is outstanding, so destroying the
                // context retires and detaches the worker synchronously.
                context_->reset();
            } else {
                attachment_->stop();
            }
        }
    };
    struct reentrant_worker_callable final {
        explicit reentrant_worker_callable(worker_reentry_state& state_value)
            : state_(&state_value),
              payload_(state_value.resource_, 4096, 'v') {}
        reentrant_worker_callable(const reentrant_worker_callable& other)
            : state_(other.state_),
              payload_(other.payload_) {
            state_->retire();
        }
        reentrant_worker_callable(reentrant_worker_callable&& other) noexcept
            : state_(other.state_),
              payload_(std::move(other.payload_)) {
            state_->retire();
        }
        void operator()() {
            state_->payload_valid_ = payload_.size() == 4096 && payload_.front() == 'v' &&
                                     payload_.back() == 'v';
        }
        worker_reentry_state* state_;
        tracked_bytes payload_;
    };

    const auto exercise_reentry = [](bool detach, bool use_copy) {
        payload_tracker reentry_resource;
        std::optional<asio::io_context> context;
        context.emplace();
        auto attachment = ruvia::attach_event_loop(*context, {.queue_capacity_ = 1});
        const auto worker_value = attachment.loop().worker();
        std::optional<ruvia::worker_ref> source;
        source.emplace(worker_value);
        worker_reentry_state state_value{.source_ = &source,
            .context_ = &context,
            .attachment_ = &attachment,
            .detach_ = detach,
            .resource_ = &reentry_resource};
        std::optional<reentrant_worker_callable> input;
        input.emplace(state_value);
        auto rejected = use_copy ? source->post(*input) : source->post(std::move(*input));
        if (!state_value.retired_ || source || worker_value.valid() == detach ||
            worker_value.accepting() || rejected != ruvia::post_status::worker_stopping ||
            rejected.rejected() == nullptr) {
            return false;
        }
        auto task_value = std::move(rejected).take_rejected();
        ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
        if (recovery.loop(0).worker().post(std::move(task_value)) != ruvia::post_status::accepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        input.reset();
        return state_value.payload_valid_ && reentry_resource.allocation_count() == reentry_resource.deallocation_count();
    };
    if (!exercise_reentry(false, true) || !exercise_reentry(true, false)) {
        return false;
    }

    struct reserved_move_state final {
        const ruvia::worker_ref* worker_;
        ruvia::event_loop_attachment* attachment_;
        payload_tracker* resource_;
        int moves_{0};
        bool reservation_observed_{false};
        bool ran_{false};
    };
    struct inline_reserved_move_callable final {
        explicit inline_reserved_move_callable(reserved_move_state& state_value)
            : state_(&state_value),
              payload_(new char[16]) {
            state_->resource_->record_acquire();
            std::fill_n(payload_.get(), 16, 'r');
        }
        inline_reserved_move_callable(inline_reserved_move_callable&& other) noexcept
            : state_(other.state_),
              payload_(std::move(other.payload_)) {
            // This move transfers the callable into its already reserved node;
            // abandoning that node may cause additional cleanup moves after this point.
            if (++state_->moves_ == 3) {
                state_->reservation_observed_ =
                    state_->worker_->post([] {}) == ruvia::post_status::queue_full;
                state_->attachment_->stop();
            }
        }
        ~inline_reserved_move_callable() {
            if (payload_ != nullptr) {
                state_->resource_->record_release();
            }
        }
        void operator()() {
            state_->ran_ = payload_ != nullptr && payload_[0] == 'r' && payload_[15] == 'r';
        }

        reserved_move_state* state_;
        std::unique_ptr<char[]> payload_;
    };

    // Stopping inside the reserved move keeps the admitted callback. Draining
    // the context runs it; destroying the unrun context detaches the worker and
    // releases it without invoking it.
    const auto exercise_reserved_move = [](bool detach) {
        payload_tracker reserved_resource;
        std::optional<asio::io_context> context;
        context.emplace();
        auto attachment = ruvia::attach_event_loop(*context, {.queue_capacity_ = 1});
        const auto worker_value = attachment.loop().worker();
        reserved_move_state state_value{
            .worker_ = &worker_value, .attachment_ = &attachment, .resource_ = &reserved_resource};
        inline_reserved_move_callable input(state_value);
        const auto submitted = worker_value.post(std::move(input));
        if (submitted != ruvia::post_status::accepted || !state_value.reservation_observed_ ||
            reserved_resource.allocation_count() != 1) {
            return false;
        }
        if (detach) {
            context.reset();
            if (state_value.ran_ || worker_value.valid() || reserved_resource.deallocation_count() != 1) {
                return false;
            }
        } else {
            context->run();
            if (!state_value.ran_ || worker_value.valid() || reserved_resource.deallocation_count() != 1) {
                return false;
            }
        }
        return reserved_resource.allocation_count() == reserved_resource.deallocation_count();
    };
    if (!exercise_reserved_move(false) || !exercise_reserved_move(true)) {
        return false;
    }

    constexpr std::size_t repeated_posts = 64;
    payload_tracker repeated_resource;
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = repeated_posts + 2});
    const auto worker_value = attachment.loop().worker();
    bool accepted_payload_valid = false;
    if (!worker_value.valid() || !worker_value.accepting() ||
        worker_value.post([] {}) != ruvia::post_status::accepted) {
        return false;
    }
    auto accepted = worker_value.post([payload = post_payload(&repeated_resource), &accepted_payload_valid]() mutable {
        accepted_payload_valid = payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                 payload.bytes_.back() == 'x';
    });
    if (accepted != ruvia::post_status::accepted) {
        return false;
    }
    for (std::size_t index = 0; index < repeated_posts; ++index) {
        auto posted = worker_value.post([payload = post_payload(&repeated_resource)]() mutable {
            if (payload.bytes_.size() != 4096 || payload.bytes_.front() != 'x') {
                std::terminate();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    // Stop drains every accepted post before the attachment retires.
    attachment.stop();
    context.run();
    if (!accepted_payload_valid || repeated_resource.allocation_count() != repeated_resource.deallocation_count()) {
        return false;
    }

    asio::io_context full_context;
    auto full_attachment = ruvia::attach_event_loop(full_context, {.queue_capacity_ = 1});
    const auto full_worker = full_attachment.loop().worker();
    payload_tracker rejected_resource;
    bool retained_payload_valid = false;
    if (full_worker.post([] {}) != ruvia::post_status::accepted) {
        return false;
    }
    auto full = full_worker.post([payload = post_payload(&rejected_resource), &retained_payload_valid]() mutable {
        retained_payload_valid = payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                 payload.bytes_.back() == 'x';
    });
    if (full != ruvia::post_status::queue_full || full.rejected() == nullptr) {
        return false;
    }
    auto retained = std::move(full).take_rejected();
    for (int attempt_value = 0; attempt_value < 3; ++attempt_value) {
        auto again = full_worker.post(std::move(retained));
        if (again != ruvia::post_status::queue_full || again.rejected() == nullptr ||
            rejected_resource.allocation_count() == rejected_resource.deallocation_count()) {
            return false;
        }
        retained = std::move(again).take_rejected();
    }
    full_context.poll();
    const auto retried = full_worker.post(std::move(retained));
    if (retried != ruvia::post_status::accepted) {
        return false;
    }
    full_attachment.stop();
    full_context.run();
    if (!retained_payload_valid || rejected_resource.allocation_count() != rejected_resource.deallocation_count()) {
        return false;
    }

    asio::io_context closed_context;
    auto closed_attachment = ruvia::attach_event_loop(closed_context, {.queue_capacity_ = 1});
    const auto closed_worker = closed_attachment.loop().worker();
    closed_attachment.stop();
    if (!closed_worker.valid() || closed_worker.accepting() ||
        closed_worker.post([] {}) != ruvia::post_status::worker_stopping) {
        return false;
    }
    closed_attachment.run();
    if (closed_worker.valid() || closed_worker.accepting() || closed_attachment.valid()) {
        return false;
    }
    ruvia::worker_ref invalid;
    return !invalid.valid() && !invalid.accepting() &&
           invalid.post([] {}) == ruvia::post_status::worker_stopping &&
           repeated_resource.allocation_count() > 0 &&
           repeated_resource.allocation_count() == repeated_resource.deallocation_count();
}

bool test_post_factory_keeps_endpoint_through_owner_retirement() {
    auto context = std::make_unique<asio::io_context>();
    std::optional<ruvia::event_loop_attachment> attachment;
    attachment.emplace(ruvia::attach_event_loop(*context, {.queue_capacity_ = 1}));
    auto worker = attachment->loop().worker();
    auto payload = std::make_shared<int>(42);
    const std::weak_ptr<int> observed = payload;
    bool ran = false;
    const auto status = worker.post_factory(
        [owned = std::move(payload), &attachment, &worker, &ran]() mutable -> ruvia::move_only_function<void()> {
            attachment.reset();
            worker = ruvia::worker_ref{};
            return [owned = std::move(owned), &ran] { ran = *owned == 42; };
        });
    // Destroying the unrun context detaches the worker, which abandons the
    // admitted callback and releases its owned inputs.
    context.reset();
    return status == ruvia::post_status::accepted && !attachment && !worker.valid() &&
           !ran && observed.expired();
}

bool test_post_factory_exception_releases_admission() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    const auto expected = std::make_error_code(std::errc::argument_out_of_domain);
    bool propagated = false;
    try {
        static_cast<void>(worker_value.post_factory(
            [&expected]() -> ruvia::move_only_function<void()> { throw std::system_error(expected); }));
    } catch (const std::system_error& error) {
        propagated = error.code() == expected;
    }
    bool ran = false;
    const auto status = worker_value.post_factory(
        [&ran]() -> ruvia::move_only_function<void()> { return [&ran] { ran = true; }; });
    attachment.stop();
    context.run();
    return propagated && status == ruvia::post_status::accepted && ran;
}

bool test_post_factory_close_drains_admitted_callback() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    int calls = 0;
    const auto status = worker_value.post_factory([&]() -> ruvia::move_only_function<void()> {
        attachment.stop();
        return [&calls] { ++calls; };
    });
    bool rejected_factory_called = false;
    const auto rejected = worker_value.post_factory([&]() -> ruvia::move_only_function<void()> {
        rejected_factory_called = true;
        return [] {};
    });
    context.run();
    return status == ruvia::post_status::accepted && calls == 1 &&
           rejected == ruvia::post_status::worker_stopping && !rejected_factory_called;
}

struct queue_destructor_state final {
    const ruvia::worker_ref* worker_{nullptr};
    ruvia::event_loop_attachment* attachment_{nullptr};
    int destroyed_{0};
    bool ran_{false};
};

class queue_destructor_callback final {
public:
    explicit queue_destructor_callback(queue_destructor_state& state_value) noexcept
        : state_(&state_value) {}
    queue_destructor_callback(const queue_destructor_callback&) = delete;
    queue_destructor_callback(queue_destructor_callback&&) noexcept = default;

    ~queue_destructor_callback() {
        static_cast<void>(state_->worker_->post([] {}));
        ++state_->destroyed_;
    }

    void operator()() const {
        state_->ran_ = true;
        state_->attachment_->stop();
    }

private:
    queue_destructor_state* state_;
};

struct cancellation_observation final {
    unsigned calls_{0};
    std::uint64_t operation_id_{0};
    bool on_worker_{false};
    bool owner_destroyed_{false};
};

struct cancellation_owner final {
    const ruvia::worker_ref& worker_;
    cancellation_observation& observed_;

    ~cancellation_owner() {
        observed_.owner_destroyed_ = true;
    }

    void cancel_operation_by_id(std::uint64_t operation_id) noexcept {
        ++observed_.calls_;
        observed_.operation_id_ = operation_id;
        observed_.on_worker_ = worker_.is_current();
    }
};

bool test_cancellation_reaches_worker_with_saturated_or_closed_queue(bool close) {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    cancellation_observation observed;
    cancellation_owner owner_value{worker_value, observed};
    auto queue = ruvia::make_worker_cancellation_target(owner_value, worker_value);
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, ruvia::worker_cancellation_post(queue.get(), 7));
    unsigned normal_calls = 0;
    const auto accepted = worker_value.post([&normal_calls] { ++normal_calls; });
    if (!accepted.accepted()) {
        return false;
    }
    if (close) {
        attachment.stop();
    }
    const auto rejected = worker_value.post([] {});
    if (rejected.status() != (close ? ruvia::post_status::worker_stopping : ruvia::post_status::queue_full)) {
        return false;
    }
    source.request_stop();
    if (observed.calls_ != 0) {
        return false;
    }
    // Retirement drains the queued post and the deferred cancellation first.
    attachment.stop();
    attachment.run();
    queue->detach(owner_value);
    return observed.calls_ == 1 && observed.operation_id_ == 7 && observed.on_worker_ &&
           normal_calls == 1;
}

bool test_queued_cancellation_releases_queue_after_owner_retirement() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    cancellation_observation observed;
    auto owner_value = std::make_unique<cancellation_owner>(worker_value, observed);
    auto queue = ruvia::make_worker_cancellation_target(*owner_value, worker_value);
    std::weak_ptr weak_queue(queue);
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, ruvia::worker_cancellation_post(queue.get(), 9));
    source.request_stop();
    registration.reset();
    queue->detach(*owner_value);
    owner_value.reset();
    queue.reset();
    const bool retained_by_post = !weak_queue.expired();
    attachment.stop();
    attachment.run();
    return retained_by_post && observed.owner_destroyed_ && observed.calls_ == 0 &&
           weak_queue.expired();
}

bool test_cancellation_after_endpoint_detach_does_not_touch_retired_owner() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    cancellation_observation observed;
    auto owner_value = std::make_unique<cancellation_owner>(worker_value, observed);
    auto queue = ruvia::make_worker_cancellation_target(*owner_value, worker_value);
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, ruvia::worker_cancellation_post(queue.get(), 11));
    queue->detach(*owner_value);
    owner_value.reset();
    // Retiring the attachment detaches the endpoint before the stop request.
    attachment.stop();
    attachment.run();
    if (worker_value.valid()) {
        return false;
    }
    source.request_stop();
    context.restart();
    return observed.owner_destroyed_ && observed.calls_ == 0 && context.run() == 0;
}

bool test_attachment_escaped_worker_detaches_on_retirement() {
    asio::io_context context;
    std::optional<ruvia::worker_ref> escaped_worker;
    {
        auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 8});
        const auto loop = attachment.loop();
        if (&loop.io_context() != &context || !loop.worker().valid()) {
            return false;
        }
        escaped_worker.emplace(loop.worker());
    }
    asio::post(context, [] {});
    context.run();
    return escaped_worker && !escaped_worker->valid();
}

bool test_queue_callable_destruction_can_inspect_worker() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker_value = attachment.loop().worker();
    queue_destructor_state state_value{.worker_ = &worker_value, .attachment_ = &attachment};
    const auto submitted = worker_value.post(queue_destructor_callback(state_value));
    if (!submitted.accepted()) {
        return false;
    }
    attachment.run();
    return state_value.ran_ && state_value.destroyed_ > 0;
}

bool test_event_loop_post_borrow_and_rejected_callable_ownership() {
    constexpr std::size_t repeated_posts = 64;
    payload_tracker resource;
    std::atomic_bool invalid_payload_valid{false};
    std::atomic_size_t repeated_valid{0};
    std::atomic_size_t repeated_invalid{0};
    std::promise<void> repeated_completed;
    auto repeated_completion = repeated_completed.get_future();
    ruvia::event_loop_pool accepting({.loop_count_ = 1, .queue_capacity_ = repeated_posts + 1});
    const auto loop = accepting.loop(0);

    auto invalid = ruvia::event_loop{}.worker().post(
        [payload = post_payload(&resource), &invalid_payload_valid]() mutable {
            invalid_payload_valid.store(payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                            payload.bytes_.back() == 'x',
                std::memory_order_release);
        });
    if (invalid != ruvia::post_status::worker_stopping || invalid.rejected() == nullptr ||
        resource.allocation_count() == resource.deallocation_count()) {
        return false;
    }
    auto invalid_task = std::move(invalid).take_rejected();
    if (resource.allocation_count() == resource.deallocation_count() ||
        loop.worker().post(std::move(invalid_task)) != ruvia::post_status::accepted) {
        return false;
    }

    for (std::size_t i = 0; i < repeated_posts; ++i) {
        auto posted = accepting.loop(0).worker().post([payload = post_payload(&resource), i, &repeated_valid,
                                                          &repeated_invalid, &repeated_completed]() mutable {
            if (payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                payload.bytes_.back() == 'x') {
                repeated_valid.fetch_add(1, std::memory_order_relaxed);
            } else {
                repeated_invalid.fetch_add(1, std::memory_order_relaxed);
            }
            if (i + 1 == repeated_posts) {
                repeated_completed.set_value();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    accepting.start();
    repeated_completion.wait();
    accepting.stop();
    accepting.join();
    if (!invalid_payload_valid.load(std::memory_order_acquire) ||
        repeated_valid.load(std::memory_order_relaxed) != repeated_posts ||
        repeated_invalid.load(std::memory_order_relaxed) != 0 ||
        resource.allocation_count() != resource.deallocation_count()) {
        return false;
    }

    ruvia::event_loop closed;
    {
        ruvia::event_loop_pool stopped({.loop_count_ = 1, .queue_capacity_ = 1});
        closed = stopped.loop(0);
        stopped.join();
    }
    std::atomic_bool closed_payload_valid{false};
    auto rejected = closed.worker().post(
        [payload = post_payload(&resource), &closed_payload_valid]() mutable {
            closed_payload_valid.store(payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                           payload.bytes_.back() == 'x',
                std::memory_order_release);
        });
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        resource.allocation_count() == resource.deallocation_count()) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    if (resource.allocation_count() == resource.deallocation_count()) {
        return false;
    }
    std::promise<void> recovered;
    auto recovered_future = recovered.get_future();
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    auto recovered_post = recovery.loop(0).worker().post(
        [task = std::move(retry), &recovered]() mutable {
            task();
            recovered.set_value();
        });
    if (!recovered_post.accepted()) {
        return false;
    }
    recovery.start();
    recovered_future.wait();
    recovery.stop();
    recovery.join();
    return closed_payload_valid.load(std::memory_order_acquire) &&
           resource.allocation_count() == resource.deallocation_count();
}

bool test_event_loop_post_protects_reentrant_inline_move() {
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::atomic_bool payload_valid{false};
    auto original_pool = std::make_unique<ruvia::event_loop_pool>(
        ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
    auto loop = original_pool->loop(0);
    reentrant_post_state state_value{.loop_ = &loop,
        .pool_ = &original_pool,
        .completion_ = &completed,
        .payload_valid_ = &payload_valid};
    std::optional<inline_reentrant_post_callable> input;
    input.emplace(state_value);

    auto rejected = loop.worker().post(std::move(*input));
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        original_pool != nullptr || loop.worker().valid()) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    input.reset();
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    if (recovery.loop(0).worker().post(std::move(retry)) != ruvia::post_status::accepted) {
        return false;
    }
    recovery.start();
    if (completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        recovery.stop();
        recovery.join();
        return false;
    }
    recovery.stop();
    recovery.join();
    return payload_valid.load(std::memory_order_acquire);
}

bool test_event_loop_post_protects_reentrant_heap_copy() {
    payload_tracker resource;
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::atomic_bool payload_valid{false};
    auto original_pool = std::make_unique<ruvia::event_loop_pool>(
        ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
    auto loop = original_pool->loop(0);
    reentrant_post_state state_value{.loop_ = &loop,
        .pool_ = &original_pool,
        .completion_ = &completed,
        .payload_valid_ = &payload_valid};
    std::optional<heap_reentrant_post_callable> input;
    input.emplace(state_value, &resource);
    const auto callable_storage = resource.allocation_count() - resource.deallocation_count();
    if (callable_storage == 0) {
        return false;
    }

    auto rejected = loop.worker().post(*input);
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        original_pool != nullptr || loop.worker().valid()) {
        return false;
    }
    input.reset();
    if (resource.allocation_count() - resource.deallocation_count() != callable_storage) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    if (resource.allocation_count() - resource.deallocation_count() != callable_storage) {
        return false;
    }
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    if (recovery.loop(0).worker().post(std::move(retry)) != ruvia::post_status::accepted) {
        return false;
    }
    recovery.start();
    if (completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        recovery.stop();
        recovery.join();
        return false;
    }
    recovery.stop();
    recovery.join();
    return payload_valid.load(std::memory_order_acquire) &&
           resource.allocation_count() == resource.deallocation_count();
}

bool test_post_outcome_invariants_and_empty_callbacks() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    // Results come only from real deliveries: an accepted one has nothing to return.
    auto accepted = loop.worker().post([] {});
    bool accepted_take_rejected = false;
    try {
        static_cast<void>(std::move(accepted).take_rejected());
    } catch (const std::logic_error&) {
        accepted_take_rejected = true;
    }
    auto full = loop.worker().post([] {});
    bool full_returns_task = full.status() == ruvia::post_status::queue_full && full.rejected() != nullptr &&
                             static_cast<bool>(std::move(full).take_rejected()) && full.rejected() == nullptr;

    bool empty_post = false;
    using null_callback_type = void (*)();
    try {
        static_cast<void>(loop.worker().post(static_cast<null_callback_type>(nullptr)));
    } catch (const std::invalid_argument&) {
        empty_post = true;
    }
    bool empty_stop_callback = false;
    using null_stop_callback_type = ruvia::move_only_function<ruvia::task<void>()>;
    try {
        static_cast<void>(loop.on_stop(null_stop_callback_type{}));
    } catch (const std::invalid_argument&) {
        empty_stop_callback = true;
    }
    loops.join();
    return accepted.accepted() && accepted_take_rejected && full_returns_task && empty_post && empty_stop_callback;
}

ruvia::task<void> wait_for_signal(ruvia::worker_signal& signal, bool& resumed,
    std::size_t& remaining, ruvia::event_loop_attachment& attachment) {
    {
        auto discarded_cold_wait = signal.wait();
        static_cast<void>(discarded_cold_wait);
    }
    co_await signal.wait();
    resumed = true;
    if (--remaining == 0) {
        attachment.stop();
    }
}

bool test_worker_signal_is_worker_affine() {
    bool invalid_worker_rejected = false;
    ruvia::worker_ref invalid_worker;
    try {
        ruvia::worker_signal invalid(invalid_worker);
    } catch (const std::invalid_argument&) {
        invalid_worker_rejected = true;
    }

    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto worker_ref_value = attachment.loop().worker();
    ruvia::worker_signal first_signal(worker_ref_value);
    ruvia::worker_signal second_signal(worker_ref_value);
    const bool worker_borrowed = &first_signal.worker() == &worker_ref_value;
    bool wait_creation_rejected = false;
    try {
        static_cast<void>(first_signal.wait());
    } catch (const std::logic_error&) {
        wait_creation_rejected = true;
    }
    // notify() shares wait()'s affinity contract: a wrong thread throws, and
    // the signal stays usable (no pending wake is recorded).
    bool notify_rejected = false;
    try {
        first_signal.notify();
    } catch (const std::logic_error&) {
        notify_rejected = true;
    }
    bool first_resumed = false;
    bool second_resumed = false;
    std::size_t remaining = 2;
    asio::co_spawn(io_context,
        ruvia::as_awaitable(
            wait_for_signal(first_signal, first_resumed, remaining, attachment)),
        asio::detached);
    asio::co_spawn(io_context,
        ruvia::as_awaitable(
            wait_for_signal(second_signal, second_resumed, remaining, attachment)),
        asio::detached);
    asio::post(io_context, [&] {
        first_signal.notify();
        second_signal.notify();
    });
    io_context.run();
    return invalid_worker_rejected && worker_borrowed && wait_creation_rejected && notify_rejected && first_resumed &&
           second_resumed;
}

ruvia::task<void> wait_signal_once(ruvia::worker_signal& signal, bool& resumed) {
    co_await signal.wait();
    resumed = true;
}

ruvia::task<void> exercise_signal_pending_latch(
    ruvia::worker_signal& signal, ruvia::event_loop_attachment& attachment, bool& success) {
    signal.notify();
    signal.notify();
    {
        auto cold_wait = signal.wait();
        static_cast<void>(cold_wait);
    }
    auto completed_wait = signal.wait();
    co_await completed_wait;
    bool repeated_wait_rejected = false;
    try {
        co_await completed_wait;
    } catch (const std::logic_error&) {
        repeated_wait_rejected = true;
    }

    bool resumed = false;
    ruvia::task_scope scope(signal.worker());
    scope.spawn(wait_signal_once(signal, resumed));
    const bool second_wait_suspended = !resumed;
    signal.notify();
    signal.notify();
    co_await scope.join();
    co_await signal.wait();
    success = second_wait_suspended && resumed && repeated_wait_rejected;
    attachment.stop();
}

bool test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker_value = attachment.loop().worker();
    ruvia::worker_signal signal(worker_value);
    bool success = false;
    asio::co_spawn(context,
        ruvia::as_awaitable(exercise_signal_pending_latch(signal, attachment, success)),
        asio::detached);
    context.run();
    return success;
}

ruvia::task<void> release_signal_after_wait(const ruvia::worker_ref& worker,
    ruvia::event_loop_attachment& attachment, bool& success) {
    for (const bool deferred : {false, true}) {
        auto signal = std::make_unique<ruvia::worker_signal>(worker);
        auto wait = signal->wait();
        if (deferred) {
            auto loop = attachment.loop();
            asio::post(loop.io_context(), [&signal] { signal->notify(); });
        } else {
            signal->notify();
        }
        co_await wait;
        signal.reset();
    }
    success = true;
    attachment.stop();
}

bool test_completed_signal_wait_releases_owner() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker = attachment.loop().worker();
    bool success = false;
    asio::co_spawn(context, ruvia::as_awaitable(release_signal_after_wait(worker, attachment, success)), asio::detached);
    context.run();
    return success;
}

bool test_worker_signal_has_no_arbitrary_waiter_limit() {
    constexpr std::size_t waiter_count = 16;
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto worker_ref_value = attachment.loop().worker();
    ruvia::worker_signal signal(worker_ref_value);
    std::array<bool, waiter_count> resumed{};
    std::size_t remaining = waiter_count;
    for (std::size_t index = 0; index < resumed.size(); ++index) {
        asio::co_spawn(io_context,
            ruvia::as_awaitable(
                wait_for_signal(signal, resumed[index], remaining, attachment)),
            asio::detached);
    }

    asio::post(io_context, [&] { signal.notify(); });
    io_context.run();
    for (const bool value : resumed) {
        if (!value) {
            return false;
        }
    }
    return true;
}

ruvia::task<void> start_cold_signal_wait(ruvia::worker_signal::wait_operation cold_wait, bool& rejected) {
    try {
        co_await std::move(cold_wait);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

bool test_worker_signal_rechecks_affinity_when_cold_wait_starts() {
    asio::io_context owner_context;
    asio::io_context other_context;
    auto owner_attachment = ruvia::attach_event_loop(owner_context);
    auto other_attachment = ruvia::attach_event_loop(other_context);
    const auto owner_worker = owner_attachment.loop().worker();
    ruvia::worker_signal signal(owner_worker);
    std::optional<ruvia::worker_signal::wait_operation> cold_wait;

    asio::post(owner_context, [&] {
        cold_wait.emplace(signal.wait());
        owner_attachment.stop();
    });
    owner_context.run();
    if (!cold_wait.has_value()) {
        return false;
    }

    bool rejected = false;
    asio::co_spawn(other_context,
        ruvia::as_awaitable(start_cold_signal_wait(std::move(*cold_wait), rejected)),
        asio::detached);
    asio::post(other_context, [&] { other_attachment.stop(); });
    other_context.run();
    cold_wait.reset();
    return rejected;
}

bool test_dispatch_and_affinity() {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    if (!first.worker().valid() || first.worker().id() == 0 || first.worker().id() == second.worker().id() ||
        first.worker().is_current()) {
        return false;
    }
    constexpr std::string_view key = "device-42";
    if (loops.loop_for(key).worker().id() != loops.loop_for(key).worker().id()) {
        return false;
    }
    if (&first.io_context() != &first.executor().context()) {
        return false;
    }
    asio::ip::tcp::socket tcp(first.worker().executor());
    asio::ip::udp::socket udp(first.worker().executor());
    if (&tcp.get_executor().context() != &first.io_context() ||
        &udp.get_executor().context() != &first.io_context()) {
        return false;
    }

    std::promise<bool> completed;
    auto result_value = completed.get_future();
    std::atomic_bool stop_callback_ran{false};
    std::atomic_bool stop_callback_on_loop{false};
    auto stop_registration_value = first.on_stop([&]() -> ruvia::task<void> {
        stop_callback_on_loop = first.worker().is_current();
        stop_callback_ran = true;
        co_return;
    });
    auto move_only = std::make_unique<int>(42);
    if (first.worker().post([loop = first, value = std::move(move_only),
                                completed = std::move(completed)]() mutable {
            completed.set_value(loop.worker().is_current() && *value == 42);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    const bool success = result_value.get();
    loops.stop();
    loops.join();
    return success && stop_registration_value.valid() && stop_callback_ran && stop_callback_on_loop &&
           first.worker().post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_bounded_queue() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 2});
    const auto worker_value = loops.loop(0).worker();
    std::atomic<int> calls{0};
    std::promise<void> completed;
    auto result_value = completed.get_future();
    std::promise<void> retried;
    auto retried_result = retried.get_future();

    if (worker_value.post([&] { ++calls; }) != ruvia::post_status::accepted || worker_value.post([&] {
            ++calls;
            completed.set_value();
        }) != ruvia::post_status::accepted) {
        return false;
    }
    auto rejected = worker_value.post([value = std::make_unique<int>(9), &calls, &retried] {
        calls.fetch_add(*value);
        retried.set_value();
    });
    if (rejected != ruvia::post_status::queue_full || rejected.rejected() == nullptr) {
        return false;
    }

    loops.start();
    result_value.get();
    if (worker_value.post(std::move(rejected).take_rejected()) != ruvia::post_status::accepted) {
        return false;
    }
    retried_result.get();
    loops.stop();
    loops.join();
    bool recovered_after_stop = false;
    auto stopped = worker_value.post([&recovered_after_stop] { recovered_after_stop = true; });
    if (stopped != ruvia::post_status::worker_stopping || stopped.rejected() == nullptr) {
        return false;
    }
    auto stopped_task = std::move(stopped).take_rejected();
    stopped_task();
    return calls.load() == 11 && recovered_after_stop;
}

bool test_external_event_loop_attachment() {
    asio::io_context io_context;
    {
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
        const auto loop = attachment.loop();
        if (!attachment.valid() || !loop.worker().valid() || &loop.io_context() != &io_context) {
            return false;
        }

        asio::ip::tcp::socket tcp(loop.worker().executor());
        asio::ip::udp::socket udp(loop.worker().executor());
        if (&tcp.get_executor().context() != &io_context ||
            &udp.get_executor().context() != &io_context) {
            return false;
        }

        bool duplicate_rejected = false;
        try {
            auto duplicate = ruvia::attach_event_loop(io_context);
        } catch (const std::invalid_argument&) {
            duplicate_rejected = true;
        }
        if (!duplicate_rejected) {
            return false;
        }

        std::promise<bool> completed;
        auto result_value = completed.get_future();
        std::atomic_bool stop_callback_ran{false};
        std::atomic_bool stop_callback_on_loop{false};
        auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
            stop_callback_on_loop = loop.worker().is_current();
            stop_callback_ran = true;
            co_return;
        });
        if (loop.worker().post([loop, completed = std::move(completed)]() mutable {
                completed.set_value(loop.worker().is_current());
            }) != ruvia::post_status::accepted) {
            return false;
        }

        std::thread external_thread([&] { attachment.run(); });
        bool dispatched_on_external_thread = false;
        try {
            dispatched_on_external_thread = result_value.get();
            attachment.stop();
            external_thread.join();
        } catch (...) {
            attachment.stop();
            if (external_thread.joinable()) {
                external_thread.join();
            }
            throw;
        }
        if (!dispatched_on_external_thread || !stop_registration_value.valid() || !stop_callback_ran ||
            !stop_callback_on_loop || loop.worker().valid() ||
            loop.worker().post([] {}) != ruvia::post_status::worker_stopping) {
            return false;
        }
    }

    io_context.restart();
    {
        auto attachment = ruvia::attach_event_loop(io_context);
        attachment.stop();
        attachment.run();
    }

    io_context.restart();
    try {
        auto invalid = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 0});
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

ruvia::task<void> finish_shutdown_cleanup(
    ruvia::worker_ref worker_value, std::atomic_bool& finished) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    finished.store(true, std::memory_order_release);
    co_return;
}

bool test_pool_reported_failure_stops_every_loop_and_join_rethrows() {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic_bool first_cleanup_finished{false};
    std::atomic_bool second_cleanup_finished{false};
    auto first_cleanup = first.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(first.worker(), first_cleanup_finished);
    });
    auto second_cleanup = second.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(second.worker(), second_cleanup_finished);
    });
    std::promise<void> reported;
    auto reported_result = reported.get_future();
    if (first.worker().post([first, &reported] {
            first.report_failure(std::make_exception_ptr(std::runtime_error("reported runtime failure")));
            reported.set_value();
        }) != ruvia::post_status::accepted) {
        return false;
    }
    loops.start();
    if (reported_result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        loops.join();
        return false;
    }
    const bool all_closed = !first.worker().accepting() && !second.worker().accepting();
    bool original_failure_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        original_failure_rethrown = std::string_view(error.what()) == "reported runtime failure";
    }
    return first_cleanup.valid() && second_cleanup.valid() && all_closed &&
           first_cleanup_finished.load(std::memory_order_acquire) &&
           second_cleanup_finished.load(std::memory_order_acquire) && original_failure_rethrown;
}

struct failure_report_gate final {
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool released_{false};
    std::atomic_size_t calls_{0};
};

class gated_failure final : public std::exception {
public:
    explicit gated_failure(failure_report_gate& gate_value) noexcept
        : gate_(gate_value) {}

    const char* what() const noexcept override {
        gate_.calls_.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(gate_.mutex_);
        gate_.entered_ = true;
        gate_.changed_.notify_all();
        gate_.changed_.wait(lock, [this] { return gate_.released_; });
        return "gated first pool failure";
    }

private:
    failure_report_gate& gate_;
};

class counted_failure final : public std::exception {
public:
    explicit counted_failure(std::shared_ptr<std::atomic_size_t> calls) noexcept
        : calls_(std::move(calls)) {}

    const char* what() const noexcept override {
        calls_->fetch_add(1, std::memory_order_relaxed);
        return "handed-off pool failure";
    }

private:
    std::shared_ptr<std::atomic_size_t> calls_;
};

ruvia::task<void> report_pool_root_failure(ruvia::event_loop loop, failure_report_gate& gate_value) {
    std::exception_ptr failure;
    try {
        throw gated_failure(gate_value);
    } catch (...) {
        failure = std::current_exception();
    }
    loop.report_failure(std::move(failure));
    co_return;
}

bool test_pool_failure_handoff_during_destructor_report() {
    failure_report_gate gate;
    auto second_failure_calls = std::make_shared<std::atomic_size_t>(0);
    std::exception_ptr second_failure;
    try {
        throw counted_failure(second_failure_calls);
    } catch (...) {
        second_failure = std::current_exception();
    }
    auto pool = std::make_unique<ruvia::event_loop_pool>(
        ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = pool->loop(0);
    auto root = loop.start(report_pool_root_failure(loop, gate));
    pool->start();
    root.wait();
    std::move(root).get();

    std::thread destroyer([&] { pool.reset(); });
    bool first_report_entered = false;
    {
        std::unique_lock lock(gate.mutex_);
        first_report_entered = gate.changed_.wait_for(
            lock, std::chrono::seconds(5), [&] { return gate.entered_; });
    }
    if (first_report_entered) {
        std::thread reporter([&] {
            loop.report_failure(second_failure);
        });
        reporter.join();
    }
    {
        const std::lock_guard lock(gate.mutex_);
        gate.released_ = true;
    }
    gate.changed_.notify_all();
    destroyer.join();

    return first_report_entered && !pool && gate.calls_.load(std::memory_order_relaxed) == 1 &&
           second_failure_calls->load(std::memory_order_relaxed) == 1;
}

bool test_pool_reports_distinct_failure_from_another_loop() {
    auto second_failure_calls = std::make_shared<std::atomic_size_t>(0);
    std::exception_ptr second_failure;
    try {
        throw counted_failure(second_failure_calls);
    } catch (...) {
        second_failure = std::current_exception();
    }
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 1});
    loops.loop(0).report_failure(std::make_exception_ptr(std::runtime_error("first loop failure")));
    loops.loop(1).report_failure(second_failure);
    bool first_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "first loop failure";
    }
    return first_rethrown && second_failure_calls->load(std::memory_order_relaxed) == 1;
}

bool test_worker_run_reports_failure_after_delivered_one() {
    auto second_failure_calls = std::make_shared<std::atomic_size_t>(0);
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    asio::post(io_context, [] { throw std::runtime_error("first handler failed"); });
    asio::post(io_context, [second_failure_calls] { throw counted_failure(second_failure_calls); });
    bool first_rethrown = false;
    try {
        attachment.run();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "first handler failed";
    }
    return first_rethrown && second_failure_calls->load(std::memory_order_relaxed) == 1;
}

bool test_pool_failure_report_works_after_queue_closure() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool queued_work_ran{false};
    if (loop.worker().post([&] { queued_work_ran.store(true, std::memory_order_release); }) !=
        ruvia::post_status::accepted) {
        return false;
    }
    loops.stop();
    loop.report_failure(std::make_exception_ptr(std::runtime_error("late runtime failure")));
    bool failure_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        failure_rethrown = std::string_view(error.what()) == "late runtime failure";
    }
    return failure_rethrown && queued_work_ran.load(std::memory_order_acquire) && !loop.worker().valid();
}

bool test_attachment_report_failure_keeps_native_run_ownership() {
    const auto run = [](bool wrapped_run, bool receive_failure) {
        asio::io_context io_context;
        const auto expected = std::make_exception_ptr(std::runtime_error("attached report"));
        std::exception_ptr received;
        int reports = 0;
        ruvia::event_loop_attachment_options options;
        if (receive_failure) {
            options.failure_ = [&](std::exception_ptr failure) {
                received = std::move(failure);
                ++reports;
            };
        }
        auto attachment = ruvia::attach_event_loop(io_context, std::move(options));
        const auto loop = attachment.loop();
        std::atomic_bool cleanup_finished{false};
        auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
            co_await finish_shutdown_cleanup(loop.worker(), cleanup_finished);
        });
        bool report_returned = false;
        bool unrelated_work_ran = false;
        asio::post(io_context, [&] {
            loop.report_failure(expected);
            report_returned = true;
            asio::post(io_context, [&] { unrelated_work_ran = true; });
        });
        bool native_run_threw = false;
        try {
            if (wrapped_run) {
                attachment.run();
            } else {
                io_context.run();
            }
        } catch (...) {
            native_run_threw = true;
        }
        return cleanup.valid() && report_returned && unrelated_work_ran &&
               cleanup_finished.load(std::memory_order_acquire) && !native_run_threw && !loop.worker().valid() &&
               reports == (receive_failure ? 1 : 0) && received == (receive_failure ? expected : nullptr);
    };
    return run(true, false) && run(false, false) && run(true, true) && run(false, true);
}

ruvia::task<void> failed_abandoned_attachment_root() {
    throw std::runtime_error("abandoned attachment root failed");
    co_return;
}

bool test_abandoned_attachment_root_failure_retires(bool wrapped_run) {
    asio::io_context io_context;
    int root_reports = 0;
    int stop_reports = 0;
    bool reports_on_runner = true;
    const auto runner = std::this_thread::get_id();
    auto attachment = ruvia::attach_event_loop(io_context, {
                                                               .failure_ = [&](std::exception_ptr failure) {
                                                                   reports_on_runner = reports_on_runner && std::this_thread::get_id() == runner;
                                                                   try {
                                                                       std::rethrow_exception(failure);
                                                                   } catch (const std::runtime_error& error) {
                                                                       if (std::string_view(error.what()) == "abandoned attachment root failed") {
                                                                           ++root_reports;
                                                                       } else if (std::string_view(error.what()) == "attachment stop failed") {
                                                                           ++stop_reports;
                                                                       }
                                                                   }
                                                               },
                                                           });
    const auto loop = attachment.loop();
    std::atomic_bool cleanup_finished{false};
    auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(loop.worker(), cleanup_finished);
        throw std::runtime_error("attachment stop failed");
    });
    std::atomic_bool unrelated_work_ran{false};
    asio::post(io_context, [&] { unrelated_work_ran.store(true, std::memory_order_release); });
    {
        auto root = loop.start(failed_abandoned_attachment_root());
    }

    if (wrapped_run) {
        attachment.run();
    } else {
        io_context.run();
    }

    bool context_access_rejected = false;
    try {
        static_cast<void>(loop.io_context());
    } catch (const std::logic_error&) {
        context_access_rejected = true;
    }
    return unrelated_work_ran.load(std::memory_order_acquire) && !attachment.valid() &&
           !loop.worker().valid() && context_access_rejected && root_reports == 1 && stop_reports == 1 &&
           reports_on_runner && cleanup.valid() && cleanup_finished.load(std::memory_order_acquire);
}

bool test_attachment_run_failure_retires_through_async_cleanup() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto loop = attachment.loop();
    bool cleanup_ran = false;
    auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
        cleanup_ran = true;
        co_return;
    });
    asio::post(io_context, [] { throw std::runtime_error("attached handler failed"); });
    bool failure_propagated = false;
    try {
        attachment.run();
    } catch (const std::runtime_error& error) {
        failure_propagated = std::string_view(error.what()) == "attached handler failed";
    }
    return cleanup.valid() && cleanup_ran && failure_propagated && !loop.worker().valid();
}

bool test_external_attachment_retains_state_until_context_cleanup() {
    asio::io_context io_context;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool entered = false;
    bool release = false;
    std::atomic_bool stop_callback_ran{false};
    ruvia::event_loop_stop_registration stop_registration;
    std::thread external_thread;

    {
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
        stop_registration = attachment.loop().on_stop([&]() -> ruvia::task<void> {
            stop_callback_ran.store(true, std::memory_order_release);
            co_return;
        });
        asio::post(io_context, [&] {
            {
                const std::lock_guard lock(gate_mutex);
                entered = true;
            }
            gate_changed.notify_one();
            std::unique_lock lock(gate_mutex);
            gate_changed.wait(lock, [&] { return release; });
        });
        external_thread = std::thread([&] { io_context.run(); });
        {
            std::unique_lock lock(gate_mutex);
            gate_changed.wait(lock, [&] { return entered; });
        }
    }

    bool duplicate_rejected_while_cleanup_is_pending = false;
    try {
        auto duplicate = ruvia::attach_event_loop(io_context);
    } catch (const std::invalid_argument&) {
        duplicate_rejected_while_cleanup_is_pending = true;
    }

    {
        const std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_changed.notify_one();
    external_thread.join();
    stop_registration.reset();

    bool reattached_after_cleanup = false;
    try {
        io_context.restart();
        auto replacement = ruvia::attach_event_loop(io_context);
        replacement.stop();
        replacement.run();
        reattached_after_cleanup = true;
    } catch (...) {
    }
    return duplicate_rejected_while_cleanup_is_pending &&
           stop_callback_ran.load(std::memory_order_acquire) && reattached_after_cleanup;
}

bool test_external_attachment_handles_context_destruction() {
    auto io_context = std::make_unique<asio::io_context>();
    {
        // Register the timer service before Ruvia's external-context service;
        // shutdown must remain safe regardless of Asio service registration
        // order.
        asio::steady_timer timer(*io_context);
    }
    auto attachment = ruvia::attach_event_loop(*io_context);
    const auto loop = attachment.loop();
    io_context.reset();

    bool io_context_rejected = false;
    try {
        static_cast<void>(loop.io_context());
    } catch (const std::logic_error&) {
        io_context_rejected = true;
    }
    bool executor_rejected = false;
    try {
        static_cast<void>(loop.executor());
    } catch (const std::logic_error&) {
        executor_rejected = true;
    }
    bool worker_executor_rejected = false;
    try {
        static_cast<void>(loop.worker().executor());
    } catch (const std::logic_error&) {
        worker_executor_rejected = true;
    }
    const bool post_rejected = loop.worker().post([] {}) == ruvia::post_status::worker_stopping;
    attachment.stop();
    return io_context_rejected && executor_rejected && worker_executor_rejected && post_rejected && !attachment.valid() &&
           !loop.worker().valid();
}

bool test_stopped_attachment_with_queued_post_handles_context_destruction() {
    auto marker = std::make_shared<int>(0);
    const std::weak_ptr<int> observed = marker;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context);
        // A plain post holds no retirement lease, so stop() parks its idle
        // finalization behind it; destroying the unrun context abandons both.
        if (attachment.loop().worker().post([marker = std::move(marker)] {}) != ruvia::post_status::accepted) {
            return false;
        }
        attachment.stop();
    }
    return observed.expired();
}

bool test_join_stops_running_pool() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    loops.start();
    loops.join();
    return !loop.worker().accepting() && loop.worker().post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_failure_propagation() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stop_callback_ran{false};
    std::atomic_bool stop_callback_on_loop{false};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_callback_on_loop = loop.worker().is_current();
        stop_callback_ran = true;
        co_return;
    });
    if (loop.worker().post([] { throw std::runtime_error("posted task failed"); }) !=
        ruvia::post_status::accepted) {
        return false;
    }
    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stop_registration_value.valid() && stop_callback_ran && stop_callback_on_loop &&
               std::string_view(error.what()) == "posted task failed";
    }
    return false;
}

bool test_join_before_start_drains_on_owners() {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 1});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic<unsigned> task_calls{0};
    std::atomic<unsigned> stop_calls{0};
    std::atomic_bool tasks_on_owners{true};
    std::atomic_bool stops_on_owners{true};

    auto first_stop = first.on_stop([&]() -> ruvia::task<void> {
        if (!first.worker().is_current()) {
            stops_on_owners.store(false, std::memory_order_relaxed);
        }
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    auto second_stop = second.on_stop([&]() -> ruvia::task<void> {
        if (!second.worker().is_current()) {
            stops_on_owners.store(false, std::memory_order_relaxed);
        }
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    if (first.worker().post([&] {
            if (!first.worker().is_current()) {
                tasks_on_owners.store(false, std::memory_order_relaxed);
            }
            task_calls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::post_status::accepted ||
        second.worker().post([&] {
            if (!second.worker().is_current()) {
                tasks_on_owners.store(false, std::memory_order_relaxed);
            }
            task_calls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.join();
    const bool rejected_after_join = first.worker().post([] {}) == ruvia::post_status::worker_stopping &&
                                     second.worker().post([] {}) == ruvia::post_status::worker_stopping;
    return first_stop.valid() && second_stop.valid() && rejected_after_join &&
           task_calls.load(std::memory_order_relaxed) == 2 &&
           stop_calls.load(std::memory_order_relaxed) == 2 &&
           tasks_on_owners.load(std::memory_order_relaxed) &&
           stops_on_owners.load(std::memory_order_relaxed);
}

bool test_stop_before_start_propagates_failure() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stop_on_owner{false};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_on_owner.store(loop.worker().is_current(), std::memory_order_release);
        co_return;
    });
    if (loop.worker().post([] { throw std::runtime_error("pre-start task failed"); }) !=
        ruvia::post_status::accepted) {
        return false;
    }

    loops.stop();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stop_registration_value.valid() && stop_on_owner.load(std::memory_order_acquire) &&
               std::string_view(error.what()) == "pre-start task failed";
    }
    return false;
}

bool test_join_rejects_pool_worker() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::promise<bool> completed;
    auto result_value = completed.get_future();
    if (loop.worker().post([&] {
            bool rejected = false;
            try {
                loops.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                           "cannot join an event loop pool from one of its workers";
            }
            completed.set_value(rejected && loop.worker().is_current());
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    const bool rejected = result_value.get();
    loops.stop();
    loops.join();
    return rejected;
}

bool test_executor_failure_drains_shutdown_on_owners() {
    struct abandon_probe final {
        explicit abandon_probe(std::atomic_bool& destroyed) noexcept
            : destroyed_(&destroyed) {}
        ~abandon_probe() {
            destroyed_->store(true, std::memory_order_release);
        }
        std::atomic_bool* destroyed_;
    };

    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 2});
    const auto failed_loop = loops.loop(0);
    const auto peer_loop = loops.loop(1);
    std::atomic<unsigned> failed_stop_calls{0};
    std::atomic<unsigned> peer_stop_calls{0};
    std::atomic_bool failed_stop_on_owner{false};
    std::atomic_bool peer_stop_on_owner{false};
    std::atomic_bool shutdown_continuation_drained{false};
    std::atomic_bool abandoned_queue_ran{false};
    std::atomic_bool abandoned_queue_destroyed{false};

    auto failed_stop = failed_loop.on_stop([&]() -> ruvia::task<void> {
        failed_stop_on_owner.store(failed_loop.worker().is_current(), std::memory_order_release);
        failed_stop_calls.fetch_add(1, std::memory_order_relaxed);
        asio::post(failed_loop.io_context(),
            [] { throw std::runtime_error("secondary shutdown handler failed"); });
        asio::post(failed_loop.io_context(), [&] {
            shutdown_continuation_drained.store(failed_loop.worker().is_current(), std::memory_order_release);
        });
        co_return;
    });
    auto peer_stop = peer_loop.on_stop([&]() -> ruvia::task<void> {
        peer_stop_on_owner.store(peer_loop.worker().is_current(), std::memory_order_release);
        peer_stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    asio::post(failed_loop.io_context(), [] { throw std::runtime_error("executor handler failed"); });
    if (failed_loop.worker().post([probe = std::make_unique<abandon_probe>(abandoned_queue_destroyed),
                                      &abandoned_queue_ran] {
            abandoned_queue_ran.store(true, std::memory_order_release);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return failed_stop.valid() && peer_stop.valid() &&
               std::string_view(error.what()) == "executor handler failed" &&
               failed_stop_calls.load(std::memory_order_relaxed) == 1 &&
               peer_stop_calls.load(std::memory_order_relaxed) == 1 &&
               failed_stop_on_owner.load(std::memory_order_acquire) &&
               peer_stop_on_owner.load(std::memory_order_acquire) &&
               shutdown_continuation_drained.load(std::memory_order_acquire) &&
               !abandoned_queue_ran.load(std::memory_order_acquire) &&
               abandoned_queue_destroyed.load(std::memory_order_acquire);
    }
    return false;
}

bool test_expired_loop() {
    ruvia::event_loop loop;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
        loop = loops.loop(0);
    }
    bool context_rejected = false;
    try {
        static_cast<void>(loop.io_context());
    } catch (const std::logic_error&) {
        context_rejected = true;
    }
    return !loop.worker().valid() && !loop.worker().accepting() && context_rejected &&
           loop.worker().post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_escaped_worker_ref_becomes_detached_endpoint() {
    ruvia::worker_ref worker;
    ruvia::worker_id_type live_id = 0;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
        worker = loops.loop(0).worker();
        live_id = worker.id();
        if (!worker.valid() || live_id == 0) {
            return false;
        }
    }

    return !worker.valid() && !worker.accepting() && !worker.is_current() && worker.id() == 0 &&
           worker.post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_abandoned_root_task_completes_when_queue_drain_fails() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    if (loop.worker().post([] { throw std::runtime_error("boom"); }) != ruvia::post_status::accepted) {
        return false;
    }

    auto root = loop.start([]() -> ruvia::task<int> { co_return 42; }());
    loops.start();

    bool join_threw = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        join_threw = std::string_view(error.what()) == "boom";
    } catch (...) {
    }
    if (!join_threw) {
        return false;
    }

    try {
        static_cast<void>(std::move(root).get());
        return false;
    } catch (const ruvia::event_loop_start_rejected& error) {
        return error.status() == ruvia::post_status::worker_stopping;
    } catch (...) {
        return false;
    }
}

// A stop callback runs after every caller that could have received its
// exception is gone. Dropping it would make a failed cleanup invisible, so the
// pool records it as its first failure and join() rethrows it.
bool test_stop_callback_factory_failure_does_not_skip_other_callbacks() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    std::atomic_bool coroutine_callback_ran{false};
    auto factory_failure = loop.on_stop([]() -> ruvia::task<void> {
        throw std::runtime_error("stop factory failed");
    });
    auto coroutine_callback = loop.on_stop([&]() -> ruvia::task<void> {
        coroutine_callback_ran.store(true, std::memory_order_release);
        co_return;
    });

    loops.start();
    loops.stop();
    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop factory failed";
    } catch (...) {
    }
    return rethrown && coroutine_callback_ran.load(std::memory_order_acquire) &&
           factory_failure.valid() && coroutine_callback.valid();
}

bool test_stop_callback_failure_reaches_join() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 2});
    const auto loop = loops.loop(0);
    std::atomic<unsigned> stop_calls{0};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("stop callback failed");
        co_return;
    });

    loops.start();
    loops.stop();

    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop callback failed";
    } catch (...) {
    }
    return rethrown && stop_calls.load(std::memory_order_relaxed) == 1;
}

bool test_async_stop_callbacks_start_together_and_keep_captures_alive() {
    struct callback final {
        std::atomic<unsigned>* started_;
        std::atomic<unsigned>* finished_;
        ruvia::worker_signal* signal_;
        bool notifier_;
        int value_;
        int* observed_;

        ruvia::task<void> operator()() {
            started_->fetch_add(1, std::memory_order_release);
            if (notifier_) {
                signal_->notify();
            } else {
                co_await signal_->wait();
            }
            *observed_ += value_;
            finished_->fetch_add(1, std::memory_order_release);
            co_return;
        }
    };

    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.worker();
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> finished{0};
    ruvia::worker_signal signal(worker_value);
    int observed_value = 0;
    auto first = loop.on_stop(callback{&started, &finished, &signal, false, 1, &observed_value});
    auto second = loop.on_stop(callback{&started, &finished, &signal, true, 2, &observed_value});
    loops.start();
    loops.stop();
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (started.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::yield();
    }
    first.reset();
    second.reset();
    loops.join();
    return started.load(std::memory_order_acquire) == 2 &&
           finished.load(std::memory_order_acquire) == 2 && observed_value == 3;
}

bool test_reset_stop_registration_cancels_deferred_unstarted_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    bool ran = false;
    auto reg = attachment.loop().on_stop([&]() -> ruvia::task<void> {
        ran = true;
        co_return;
    });
    attachment.stop();
    reg.reset();
    attachment.run();
    return !ran && !reg.valid();
}

bool test_retired_stop_registration_cancels_deferred_callback(bool destroy) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    bool ran = false;
    auto capture = std::make_shared<int>(42);
    const std::weak_ptr<int> lifetime(capture);
    std::optional<ruvia::event_loop_stop_registration> registration;
    registration.emplace(attachment.loop().on_stop(
        [capture = std::move(capture), &ran]() -> ruvia::task<void> {
            ran = *capture == 42;
            co_return;
        }));
    attachment.stop();
    if (destroy) {
        registration.reset();
    } else {
        *registration = ruvia::event_loop_stop_registration{};
    }
    const bool released_before_run = lifetime.expired();
    attachment.run();
    return released_before_run && !ran && (!registration || !registration->valid());
}

bool test_move_stop_registration_preserves_incoming_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    int observed = 0;
    auto old_capture = std::make_shared<int>(1);
    const std::weak_ptr<int> old_lifetime(old_capture);
    auto destination = attachment.loop().on_stop(
        [capture = std::move(old_capture), &observed]() -> ruvia::task<void> {
            observed += *capture;
            co_return;
        });
    auto new_capture = std::make_shared<int>(10);
    const std::weak_ptr<int> new_lifetime(new_capture);
    auto source = attachment.loop().on_stop(
        [capture = std::move(new_capture), &observed]() -> ruvia::task<void> {
            observed += *capture;
            co_return;
        });
    attachment.stop();
    destination = std::move(source);
    auto& alias = destination;
    destination = std::move(alias);
    auto transferred = std::move(destination);
    const bool ownership_transferred = !source.valid() && !destination.valid() && transferred.valid() &&
                                       old_lifetime.expired() && !new_lifetime.expired();
    attachment.run();
    return ownership_transferred && observed == 10 && new_lifetime.expired();
}

bool test_stop_registration_reentrant_retirement_preserves_replacement() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    int observed = 0;
    ruvia::event_loop_stop_registration destination;
    auto source = std::make_unique<ruvia::event_loop_stop_registration>(
        attachment.loop().on_stop([&]() -> ruvia::task<void> {
            observed += 10;
            co_return;
        }));
    auto* owned_source = source.get();
    auto replacement = attachment.loop().on_stop([&]() -> ruvia::task<void> {
        observed += 100;
        co_return;
    });
    struct callback final {
        std::unique_ptr<ruvia::event_loop_stop_registration> source_;
        ruvia::event_loop_stop_registration* destination_;
        ruvia::event_loop_stop_registration* replacement_;
        int* observed_;
        callback(std::unique_ptr<ruvia::event_loop_stop_registration> source,
            ruvia::event_loop_stop_registration& destination,
            ruvia::event_loop_stop_registration& replacement,
            int& observed)
            : source_(std::move(source)),
              destination_(&destination),
              replacement_(&replacement),
              observed_(&observed) {}
        callback(callback&&) noexcept = default;
        ~callback() {
            if (source_) {
                *destination_ = std::move(*replacement_);
            }
        }
        ruvia::task<void> operator()() {
            *observed_ += 1;
            co_return;
        }
    };
    destination = attachment.loop().on_stop(callback(std::move(source), destination, replacement, observed));
    attachment.stop();
    destination = std::move(*owned_source);
    const bool replacement_transferred = destination.valid() && !replacement.valid();
    attachment.run();
    return replacement_transferred && observed == 100;
}

bool test_retired_started_stop_registration_keeps_capture(bool destroy, bool fail) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    ruvia::worker_signal resume(loop.worker());
    bool retained_while_suspended = false;
    bool finished = false;
    int observed = 0;
    auto capture = std::make_shared<int>(42);
    const std::weak_ptr<int> lifetime(capture);
    std::optional<ruvia::event_loop_stop_registration> registration;
    registration.emplace(loop.on_stop(
        [&, capture = std::move(capture)]() -> ruvia::task<void> {
            asio::post(loop.executor(), [&] {
                if (destroy) {
                    registration.reset();
                } else {
                    *registration = ruvia::event_loop_stop_registration{};
                }
                retained_while_suspended = !finished && !lifetime.expired();
                resume.notify();
            });
            co_await resume.wait();
            observed = *capture;
            finished = true;
            if (fail) {
                throw std::runtime_error("retired stop callback");
            }
        }));
    loops.start();
    loops.stop();
    bool reported_failure = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "retired stop callback") {
            return false;
        }
        reported_failure = true;
    }
    return retained_while_suspended && finished && observed == 42 && lifetime.expired() &&
           reported_failure == fail && (!registration || !registration->valid());
}

bool test_stop_registration_move_can_stop_attachment() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    bool armed = false;
    bool ran = false;
    struct callback final {
        ruvia::event_loop_attachment* attachment_;
        bool* armed_;
        bool* ran_;
        callback(ruvia::event_loop_attachment& owner, bool& enabled, bool& invoked)
            : attachment_(&owner),
              armed_(&enabled),
              ran_(&invoked) {}
        callback(const callback&) = delete;
        callback(callback&& other) noexcept
            : attachment_(other.attachment_),
              armed_(other.armed_),
              ran_(other.ran_) {
            if (*armed_) {
                attachment_->stop();
            }
        }
        ~callback() {
            if (*armed_) {
                attachment_->stop();
            }
        }
        ruvia::task<void> operator()() {
            *ran_ = true;
            co_return;
        }
    };
    auto reg = attachment.loop().on_stop(callback(attachment, armed, ran));
    armed = true;
    reg.reset();
    attachment.run();
    return !ran && !reg.valid();
}

ruvia::task<void> wait_for_shutdown_timer(const ruvia::worker_ref& worker_value, bool& child_done) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)));
    child_done = true;
    co_return;
}

ruvia::task<void> nested_shutdown_scope(const ruvia::worker_ref& worker_value, bool& child_done,
    bool& middle_done) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(wait_for_shutdown_timer(worker_value, child_done));
    co_await scope.join();
    middle_done = true;
}

ruvia::task<void> root_shutdown_scope(const ruvia::worker_ref& worker_value,
    std::promise<void>& started, bool& child_done, bool& middle_done, bool& root_done) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(nested_shutdown_scope(worker_value, child_done, middle_done));
    started.set_value();
    co_await scope.join();
    root_done = true;
}

struct root_get_move_probe final {
    std::optional<ruvia::root_task<root_get_move_probe>>* owner_{};
    std::optional<ruvia::root_task<root_get_move_probe>>* replacement_{};
    std::atomic_bool* trigger_{};
    bool* valid_during_move_{};

    root_get_move_probe() = default;
    root_get_move_probe(std::optional<ruvia::root_task<root_get_move_probe>>* root_owner,
        std::optional<ruvia::root_task<root_get_move_probe>>* root_replacement,
        std::atomic_bool* move_trigger, bool* observed_validity)
        : owner_(root_owner),
          replacement_(root_replacement),
          trigger_(move_trigger),
          valid_during_move_(observed_validity) {}
    root_get_move_probe(root_get_move_probe&& other) noexcept
        : owner_(other.owner_),
          replacement_(other.replacement_),
          trigger_(other.trigger_),
          valid_during_move_(other.valid_during_move_) {
        if (trigger_ != nullptr && trigger_->exchange(false, std::memory_order_acq_rel)) {
            *valid_during_move_ = owner_->value().valid();
            if (replacement_ != nullptr) {
                *owner_ = std::move(*replacement_);
            }
        }
    }
    root_get_move_probe& operator=(root_get_move_probe&&) = delete;
    root_get_move_probe(const root_get_move_probe&) = delete;
    root_get_move_probe& operator=(const root_get_move_probe&) = delete;
};

ruvia::task<root_get_move_probe> return_root_get_move_probe(root_get_move_probe value) {
    co_return std::move(value);
}

struct root_heap_result final {
    explicit root_heap_result(payload_tracker* resource)
        : tracker_(resource),
          values_(32, 42),
          elements_(values_.data()) {
        if (tracker_ != nullptr) {
            tracker_->record_acquire();
        }
    }
    root_heap_result(root_heap_result&& other) noexcept
        : tracker_(std::exchange(other.tracker_, nullptr)),
          values_(std::move(other.values_)),
          elements_(other.elements_) {}
    root_heap_result(const root_heap_result&) = delete;
    ~root_heap_result() {
        if (tracker_ != nullptr) {
            tracker_->record_release();
        }
    }
    payload_tracker* tracker_{nullptr};
    std::vector<int> values_;
    const int* elements_{nullptr};
};

ruvia::task<root_heap_result> make_root_heap_result(payload_tracker* resource) {
    co_return root_heap_result(resource);
}

bool test_root_task_result_moves_outside_lock_and_preserves_reentrant_owner() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    std::optional<ruvia::root_task<root_get_move_probe>> root;
    std::optional<ruvia::root_task<root_get_move_probe>> replacement;
    std::atomic_bool trigger{false};
    bool valid_during_move = true;
    root.emplace(loop.start(return_root_get_move_probe(
        root_get_move_probe{&root, &replacement, &trigger, &valid_during_move})));
    replacement.emplace(loop.start(return_root_get_move_probe(
        root_get_move_probe{&root, nullptr, &trigger, &valid_during_move})));
    loops.start();
    root->wait();
    replacement->wait();
    trigger.store(true, std::memory_order_release);
    static_cast<void>(std::move(*root).get());
    const bool replacement_survived = root->valid();
    if (replacement_survived) {
        static_cast<void>(std::move(*root).get());
    }
    loops.join();
    return !valid_during_move && replacement_survived && !root->valid();
}

ruvia::task<const int> immutable_root_result(const ruvia::worker_ref& worker_value) {
    co_return worker_value.is_current() ? 17 : -1;
}

bool test_root_task_delivers_const_result() {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto worker_value = loops.loop(0).worker();
    auto root = loops.loop(0).start(immutable_root_result(worker_value));
    loops.start();
    const auto value = std::move(root).get();
    loops.stop();
    loops.join();
    return value == 17 && !root.valid();
}

bool test_root_task_result_owns_heap_storage_past_pool_retirement() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    payload_tracker resource;
    auto root = loop.start(make_root_heap_result(&resource));
    loops.start();
    root.wait();
    loops.stop();
    loops.join();
    bool retained = false;
    {
        auto result_value = std::move(root).get();
        const bool payload_retained = result_value.values_.data() == result_value.elements_;
        const bool values_valid = result_value.values_.size() == 32 &&
                                  std::all_of(result_value.values_.begin(), result_value.values_.end(),
                                      [](int value) { return value == 42; });
        retained = values_valid && payload_retained && resource.live_allocations() > 0;
    }
    return retained && resource.live_allocations() == 0 &&
           resource.allocation_count() == resource.deallocation_count();
}

bool test_root_tasks_join_nested_scopes_during_stop() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.worker();
    std::promise<void> started;
    auto started_result = started.get_future();
    bool child_done = false;
    bool middle_done = false;
    bool root_done = false;
    auto root = loop.start(root_shutdown_scope(
        worker_value, started, child_done, middle_done, root_done));
    loops.start();
    if (started_result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        loops.join();
        return false;
    }
    loops.stop();
    loops.join();
    try {
        std::move(root).get();
    } catch (...) {
        return false;
    }
    return child_done && middle_done && root_done;
}

bool test_draining_post_can_schedule_defer_cleanup_during_stop() {
    ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    bool post_ran = false;
    bool cleanup_scheduled = false;
    bool cleanup_ran = false;
    if (loop.worker().post([loop, worker_value, &post_ran, &cleanup_scheduled, &cleanup_ran] {
            post_ran = worker_value.is_current();
            cleanup_scheduled = loop.defer_cleanup([worker_value, &cleanup_ran] {
                cleanup_ran = worker_value.is_current();
            });
        }) != ruvia::post_status::accepted) {
        return false;
    }
    pool.stop();
    pool.join();
    return post_ran && cleanup_scheduled && cleanup_ran;
}

}  // namespace

TEST(Ruvia, WorkerPostBorrowAndRejectedCallableOwnership) {
    EXPECT_TRUE(test_event_loop_post_borrow_and_rejected_callable_ownership());
}
TEST(Ruvia, WorkerPostProtectsReentrantInlineMove) {
    EXPECT_TRUE(test_event_loop_post_protects_reentrant_inline_move());
}
TEST(Ruvia, WorkerPostProtectsReentrantHeapCopy) {
    EXPECT_TRUE(test_event_loop_post_protects_reentrant_heap_copy());
}
TEST(Ruvia, WorkerRefCallableLifetime) {
    EXPECT_TRUE(test_worker_ref_callable_lifetime());
}
TEST(Ruvia, WorkerPostFactoryKeepsEndpointThroughOwnerRetirement) {
    EXPECT_TRUE(test_post_factory_keeps_endpoint_through_owner_retirement());
}
TEST(Ruvia, WorkerPostFactoryExceptionReleasesAdmission) {
    EXPECT_TRUE(test_post_factory_exception_releases_admission());
}
TEST(Ruvia, WorkerPostFactoryCloseDrainsAdmittedCallback) {
    EXPECT_TRUE(test_post_factory_close_drains_admitted_callback());
}
TEST(Ruvia, WorkerPostOutcomeInvariantsAndEmptyCallbacks) {
    EXPECT_TRUE(test_post_outcome_invariants_and_empty_callbacks());
}
TEST(Ruvia, WorkerAttachmentEscapedRefDetachesOnRetirement) {
    EXPECT_TRUE(test_attachment_escaped_worker_detaches_on_retirement());
}
TEST(Ruvia, WorkerQueueCallableDestructionCanInspectWorker) {
    EXPECT_TRUE(test_queue_callable_destruction_can_inspect_worker());
}
TEST(Ruvia, WorkerSignalIsWorkerAffine) {
    EXPECT_TRUE(test_worker_signal_is_worker_affine());
}
TEST(Ruvia, WorkerSignalPendingLatchSurvivesDiscardAndWake) {
    EXPECT_TRUE(test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake());
}
TEST(Ruvia, WorkerCompletedSignalWaitReleasesOwner) {
    EXPECT_TRUE(test_completed_signal_wait_releases_owner());
}
TEST(Ruvia, WorkerSignalHasNoWaiterLimit) {
    EXPECT_TRUE(test_worker_signal_has_no_arbitrary_waiter_limit());
}
TEST(Ruvia, WorkerSignalRechecksColdWaitAffinity) {
    EXPECT_TRUE(test_worker_signal_rechecks_affinity_when_cold_wait_starts());
}
TEST(Ruvia, WorkerDispatchAndAffinity) {
    EXPECT_TRUE(test_dispatch_and_affinity());
}
TEST(Ruvia, WorkerBoundedQueue) {
    EXPECT_TRUE(test_bounded_queue());
}
TEST(Ruvia, WorkerCancellationWithFullQueue) {
    EXPECT_TRUE(test_cancellation_reaches_worker_with_saturated_or_closed_queue(false));
}
TEST(Ruvia, WorkerCancellationWithClosedQueue) {
    EXPECT_TRUE(test_cancellation_reaches_worker_with_saturated_or_closed_queue(true));
}
TEST(Ruvia, WorkerQueuedCancellationAfterOwnerRetirement) {
    EXPECT_TRUE(test_queued_cancellation_releases_queue_after_owner_retirement());
}
TEST(Ruvia, WorkerCancellationAfterEndpointDetach) {
    EXPECT_TRUE(test_cancellation_after_endpoint_detach_does_not_touch_retired_owner());
}
TEST(Ruvia, WorkerExternalEventLoopAttachment) {
    EXPECT_TRUE(test_external_event_loop_attachment());
}
TEST(Ruvia, WorkerAttachmentRunFailureRetiresThroughAsyncCleanup) {
    EXPECT_TRUE(test_attachment_run_failure_retires_through_async_cleanup());
}
TEST(Ruvia, WorkerAbandonedAttachmentFailureRetiresNativeRun) {
    EXPECT_TRUE(test_abandoned_attachment_root_failure_retires(false));
}
TEST(Ruvia, WorkerAbandonedAttachmentFailureRetiresAttachmentRun) {
    EXPECT_TRUE(test_abandoned_attachment_root_failure_retires(true));
}
TEST(Ruvia, WorkerPoolReportedFailureStopsEveryLoopAndJoinRethrows) {
    EXPECT_TRUE(test_pool_reported_failure_stops_every_loop_and_join_rethrows());
}
TEST(Ruvia, WorkerPoolFailureHandoffDuringDestructorReport) {
    EXPECT_TRUE(test_pool_failure_handoff_during_destructor_report());
}
TEST(Ruvia, WorkerPoolReportsDistinctFailureFromAnotherLoop) {
    EXPECT_TRUE(test_pool_reports_distinct_failure_from_another_loop());
}
TEST(Ruvia, WorkerRunReportsFailureAfterDeliveredOne) {
    EXPECT_TRUE(test_worker_run_reports_failure_after_delivered_one());
}
TEST(Ruvia, WorkerPoolFailureReportWorksAfterQueueClosure) {
    EXPECT_TRUE(test_pool_failure_report_works_after_queue_closure());
}
TEST(Ruvia, WorkerAttachmentReportFailurePreservesNativeRunOwnership) {
    EXPECT_TRUE(test_attachment_report_failure_keeps_native_run_ownership());
}
TEST(Ruvia, WorkerAttachmentOnlyReportsUnconsumedRootFailures) {
    asio::io_context context;
    int reports = 0;
    std::thread::id reporting_thread;
    std::exception_ptr received;
    auto attachment = ruvia::attach_event_loop(context, {
                                                            .failure_ = [&](std::exception_ptr failure) {
                                                                ++reports;
                                                                reporting_thread = std::this_thread::get_id();
                                                                received = std::move(failure);
                                                            },
                                                        });
    const auto loop = attachment.loop();
    {
        auto consumed = loop.start(failed_abandoned_attachment_root());
        context.poll();
        EXPECT_THROW(std::move(consumed).get(), std::runtime_error);
    }
    EXPECT_EQ(reports, 0);
    EXPECT_TRUE(loop.worker().accepting());

    auto abandoned = std::make_optional(loop.start(failed_abandoned_attachment_root()));
    context.poll();
    std::thread::id abandoning_thread;
    std::thread reporter([&] {
        abandoning_thread = std::this_thread::get_id();
        abandoned.reset();
    });
    reporter.join();
    EXPECT_EQ(reports, 1);
    EXPECT_EQ(reporting_thread, abandoning_thread);
    EXPECT_NE(received, nullptr);
    EXPECT_FALSE(loop.worker().accepting());
    context.run();
    EXPECT_FALSE(attachment.valid());
}

TEST(Ruvia, WorkerAttachmentFailureReceiverSurvivesReentrantOwnerDestructionAndThrow) {
    asio::io_context context;
    std::optional<ruvia::event_loop_attachment> attachment;
    std::optional<ruvia::event_loop> loop;
    auto lifetime = std::make_shared<int>(42);
    const std::weak_ptr<int> observed = lifetime;
    auto diagnostic_calls = std::make_shared<std::atomic_size_t>(0);
    const auto first = std::make_exception_ptr(std::runtime_error("first attached failure"));
    const auto nested = std::make_exception_ptr(std::runtime_error("nested attached failure"));
    int reports = 0;
    bool target_survived = false;
    bool unrelated_work_ran = false;
    std::atomic_bool cleanup_finished{false};
    attachment.emplace(ruvia::attach_event_loop(context, {
                                                             .failure_ = [&, owned = std::make_unique<int>(42), lifetime = std::move(lifetime)](std::exception_ptr failure) {
                                                                 ++reports;
                                                                 if (failure == first) {
                                                                     loop->report_failure(nested);
                                                                     target_survived = !attachment && !loop && !observed.expired() && *owned == *lifetime;
                                                                 } else {
                                                                     EXPECT_EQ(failure, nested);
                                                                     attachment.reset();
                                                                     loop.reset();
                                                                     throw counted_failure(diagnostic_calls);
                                                                 }
                                                             },
                                                         }));
    loop.emplace(attachment->loop());
    const auto worker = loop->worker();
    auto cleanup = loop->on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(worker, cleanup_finished);
    });
    asio::post(context, [&] {
        loop->report_failure(first);
        asio::post(context, [&] { unrelated_work_ran = true; });
    });
    attachment->run();
    EXPECT_EQ(reports, 2);
    EXPECT_TRUE(target_survived);
    EXPECT_EQ(diagnostic_calls->load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(cleanup_finished.load(std::memory_order_acquire));
    EXPECT_TRUE(unrelated_work_ran);
    EXPECT_FALSE(worker.valid());
    EXPECT_TRUE(observed.expired());
}

TEST(Ruvia, WorkerExternalAttachmentRetainsStateUntilCleanup) {
    EXPECT_TRUE(test_external_attachment_retains_state_until_context_cleanup());
}
TEST(Ruvia, WorkerExternalAttachmentHandlesContextDestruction) {
    EXPECT_TRUE(test_external_attachment_handles_context_destruction());
}
TEST(Ruvia, WorkerStoppedAttachmentQueuedPostHandlesContextDestruction) {
    EXPECT_TRUE(test_stopped_attachment_with_queued_post_handles_context_destruction());
}
TEST(Ruvia, WorkerJoinStopsRunningPool) {
    EXPECT_TRUE(test_join_stops_running_pool());
}
TEST(Ruvia, WorkerFailurePropagation) {
    EXPECT_TRUE(test_failure_propagation());
}
TEST(Ruvia, WorkerJoinBeforeStartDrainsOnOwners) {
    EXPECT_TRUE(test_join_before_start_drains_on_owners());
}
TEST(Ruvia, WorkerStopBeforeStartPropagatesFailure) {
    EXPECT_TRUE(test_stop_before_start_propagates_failure());
}
TEST(Ruvia, WorkerJoinRejectsPoolWorker) {
    EXPECT_TRUE(test_join_rejects_pool_worker());
}
TEST(Ruvia, WorkerExecutorFailureDrainsShutdownOnOwners) {
    EXPECT_TRUE(test_executor_failure_drains_shutdown_on_owners());
}
TEST(Ruvia, WorkerExpiredLoop) {
    EXPECT_TRUE(test_expired_loop());
}
TEST(Ruvia, WorkerEscapedRefDetaches) {
    EXPECT_TRUE(test_escaped_worker_ref_becomes_detached_endpoint());
}
TEST(Ruvia, WorkerAbandonedRootTaskCompletesWhenQueueDrainFails) {
    EXPECT_TRUE(test_abandoned_root_task_completes_when_queue_drain_fails());
}
TEST(Ruvia, WorkerStopCallbackFailureReachesJoin) {
    EXPECT_TRUE(test_stop_callback_failure_reaches_join());
}
TEST(Ruvia, WorkerStopCallbackFactoryFailureKeepsOtherCallbacksRunning) {
    EXPECT_TRUE(test_stop_callback_factory_failure_does_not_skip_other_callbacks());
}
TEST(Ruvia, WorkerAsyncStopCallbacksStartTogetherAndKeepCapturesAlive) {
    EXPECT_TRUE(test_async_stop_callbacks_start_together_and_keep_captures_alive());
}
TEST(Ruvia, WorkerResetStopRegistrationCancelsDeferredUnstartedCallback) {
    EXPECT_TRUE(test_reset_stop_registration_cancels_deferred_unstarted_callback());
}
TEST(Ruvia, WorkerDestroyedStopRegistrationCancelsDeferredCallback) {
    EXPECT_TRUE(test_retired_stop_registration_cancels_deferred_callback(true));
}
TEST(Ruvia, WorkerOverwrittenStopRegistrationCancelsDeferredCallback) {
    EXPECT_TRUE(test_retired_stop_registration_cancels_deferred_callback(false));
}
TEST(Ruvia, WorkerMovedStopRegistrationPreservesIncomingCallback) {
    EXPECT_TRUE(test_move_stop_registration_preserves_incoming_callback());
}
TEST(Ruvia, WorkerStopRegistrationReentrantRetirementPreservesReplacement) {
    EXPECT_TRUE(test_stop_registration_reentrant_retirement_preserves_replacement());
}
TEST(Ruvia, WorkerDestroyedStopRegistrationKeepsStartedCallbackAlive) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(true, false));
}
TEST(Ruvia, WorkerOverwrittenStopRegistrationKeepsStartedCallbackAlive) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(false, false));
}
TEST(Ruvia, WorkerDestroyedStopRegistrationPreservesStartedCallbackFailure) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(true, true));
}
TEST(Ruvia, WorkerOverwrittenStopRegistrationPreservesStartedCallbackFailure) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(false, true));
}
TEST(Ruvia, WorkerStopRegistrationMoveCanStopAttachment) {
    EXPECT_TRUE(test_stop_registration_move_can_stop_attachment());
}
TEST(Ruvia, WorkerRootTasksJoinNestedScopesDuringStop) {
    EXPECT_TRUE(test_root_tasks_join_nested_scopes_during_stop());
}
TEST(Ruvia, WorkerRootResultMovesAreReentrantAndPreserveOwner) {
    EXPECT_TRUE(test_root_task_result_moves_outside_lock_and_preserves_reentrant_owner());
}
TEST(Ruvia, WorkerRootResultOwnsHeapStoragePastPoolRetirement) {
    EXPECT_TRUE(test_root_task_result_owns_heap_storage_past_pool_retirement());
}
TEST(Ruvia, WorkerRootTaskDeliversConstResult) {
    EXPECT_TRUE(test_root_task_delivers_const_result());
}
TEST(Ruvia, WorkerDrainingPostCanScheduleDeferCleanupDuringStop) {
    EXPECT_TRUE(test_draining_post_can_schedule_defer_cleanup_during_stop());
}

#if defined(_WIN32)
TEST(Ruvia, WorkerNativeThreadWaitDoesNotWaitForRunningOrColdCleanup) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    const auto caller = pool.loop(0);
    for (const bool started : {false, true}) {
        SCOPED_TRACE(started);
        ruvia::worker_runtime runtime;
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        std::promise<void> release;
        const auto released = release.get_future().share();
        struct release_shutdown final {
            std::promise<void>* release_;
            ~release_shutdown() {
                if (release_) {
                    release_->set_value();
                }
            }
        } unblock{&release};
        runtime.configure({.startup_ = [&] { runtime.mark_ready(); },
            .shutdown_ = [&] {
                entered.set_value();
                (void)released.wait_for(std::chrono::seconds(10)); }});
        if (started) {
            ASSERT_TRUE(runtime.start());
            ASSERT_TRUE(runtime.wait_until_ready());
        }
        const auto prepare = [&]() -> ruvia::task<std::uintptr_t> {
            co_return co_await runtime.prepare_thread_join(caller.worker());
        };
        const auto native = caller.start(prepare()).get();
        ASSERT_NE(native, std::uintptr_t{0});
        const std::unique_ptr<void, decltype(&::CloseHandle)> wait_descriptor(
            reinterpret_cast<HANDLE>(native), &::CloseHandle);
        ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(::WaitForSingleObject(wait_descriptor.get(), 0), WAIT_TIMEOUT);
        release.set_value();
        unblock.release_ = nullptr;
        ASSERT_EQ(::WaitForSingleObject(wait_descriptor.get(), 5000), WAIT_OBJECT_0);
        runtime.join();
        EXPECT_EQ(runtime.state(), ruvia::worker_runtime_state::stopped);
        EXPECT_EQ(caller.start(prepare()).get(), std::uintptr_t{0});
    }
    pool.join();
}
#endif
