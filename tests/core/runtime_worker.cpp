#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
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

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/runtime_lifecycle.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_runtime_context.h"
#include "ruvia/core/worker_signal.h"

#include "worker_selection.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};

    [[nodiscard]] bool owns(const void* pointer, std::size_t bytes_value) const noexcept {
        const auto block_record = live_blocks_.find(const_cast<void*>(pointer));
        return block_record != live_blocks_.end() && block_record->second >= bytes_value;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_blocks_.size();
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* const block = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        try {
            live_blocks_.emplace(block, bytes_value);
        } catch (...) {
            std::pmr::new_delete_resource()->deallocate(block, bytes_value, alignment);
            throw;
        }
        ++allocations_;
        return block;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        live_blocks_.erase(pointer);
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::unordered_map<void*, std::size_t> live_blocks_{std::pmr::new_delete_resource()};
};

struct post_payload final {
    explicit post_payload(std::pmr::memory_resource* resource)
        : bytes_(resource) {
        bytes_.resize(4096, 'x');
    }
    post_payload(post_payload&&) noexcept = default;
    post_payload(const post_payload&) = delete;
    std::pmr::vector<char> bytes_;
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
        std::pmr::memory_resource* resource)
        : state_(&state_value),
          payload_(resource) {
        payload_.resize(4096, 'x');
    }
    heap_reentrant_post_callable(const heap_reentrant_post_callable& other)
        : state_(other.state_),
          payload_(other.payload_, other.payload_.get_allocator().resource()) {
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
    std::pmr::vector<char> payload_;
};

struct reentrant_worker_post_state final {
    std::optional<ruvia::worker_handle> worker_;
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
    explicit heap_worker_callable(reentrant_worker_post_state& state_value, std::pmr::memory_resource* resource)
        : state_(&state_value),
          payload_(resource) {
        payload_.resize(4096, 'h');
    }
    heap_worker_callable(const heap_worker_callable& other)
        : state_(other.state_),
          payload_(other.payload_, other.payload_.get_allocator().resource()) {
        state_->retire();
    }
    heap_worker_callable(heap_worker_callable&&) noexcept = default;
    void operator()() {
        state_->payload_valid_ = payload_.size() == 4096 && payload_.front() == 'h' &&
                                 payload_.back() == 'h';
    }
    reentrant_worker_post_state* state_;
    std::pmr::vector<char> payload_;
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

bool test_worker_handle_callable_lifetime() {
    const auto run_rejected = [](ruvia::post_result_type rejected, bool& payload_valid) {
        if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr) {
            return false;
        }
        auto task_value = std::move(rejected).take_rejected();
        ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
        if (recovery.loop(0).post(std::move(task_value)) != ruvia::post_status::accepted) {
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
        state.worker_.emplace(state.pool_->loop(0).handle());
        std::optional<inline_worker_callable> input;
        input.emplace(state);
        auto rejected = state.worker_->post(std::move(*input));
        if (!state.retired_ || state.worker_ || state.pool_ || !run_rejected(std::move(rejected), state.payload_valid_)) {
            return false;
        }
    }

    counting_resource resource;
    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).handle());
        std::optional<heap_worker_callable> input;
        input.emplace(state, &resource);
        auto rejected = state.worker_->post(*input);
        input.reset();
        if (!state.retired_ || state.worker_ || state.pool_ ||
            !run_rejected(std::move(rejected), state.payload_valid_) ||
            resource.allocations_ != resource.deallocations_) {
            return false;
        }
    }

    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).handle());
        ruvia::move_only_function<void()> input{erased_worker_callable(state)};
        state.erase_on_destroy_ = true;
        auto rejected = state.worker_->post(std::move(input));
        if (!state.retired_ || state.worker_ || state.pool_ ||
            !run_rejected(std::move(rejected), state.payload_valid_)) {
            return false;
        }
    }
    return resource.allocations_ == resource.deallocations_;
}

struct queue_destructor_state final {
    const ruvia::worker_handle* worker_{nullptr};
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
    const ruvia::worker_handle& worker_;
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
    ruvia::worker_runtime_context runtime(context, 1);
    cancellation_observation observed;
    cancellation_owner owner_value{runtime.handle(), observed};
    auto queue = ruvia::make_worker_cancellation_target(owner_value, runtime.handle());
    ruvia::stop_source source;
    auto registration = source.token().register_callback(
        ruvia::worker_cancellation_post(queue, 7));
    unsigned normal_calls = 0;
    const auto accepted = runtime.handle().post([&normal_calls] { ++normal_calls; });
    if (!accepted.accepted()) {
        return false;
    }
    if (close) {
        runtime.close();
    }
    const auto rejected = runtime.handle().post([] {});
    if (rejected.status() != (close ? ruvia::post_status::worker_stopping : ruvia::post_status::queue_full)) {
        return false;
    }
    source.request_stop();
    if (observed.calls_ != 0) {
        return false;
    }
    runtime.run();
    queue->detach(owner_value);
    return observed.calls_ == 1 && observed.operation_id_ == 7 && observed.on_worker_ &&
           normal_calls == 1;
}

bool test_queued_cancellation_releases_queue_after_owner_retirement() {
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    cancellation_observation observed;
    auto owner_value = std::make_unique<cancellation_owner>(runtime.handle(), observed);
    auto queue = ruvia::make_worker_cancellation_target(*owner_value, runtime.handle());
    std::weak_ptr weak_queue(queue);
    ruvia::stop_source source;
    auto registration = source.token().register_callback(
        ruvia::worker_cancellation_post(queue, 9));
    source.request_stop();
    registration.reset();
    queue->detach(*owner_value);
    owner_value.reset();
    queue.reset();
    const bool retained_by_post = !weak_queue.expired();
    runtime.detach();
    context.run();
    return retained_by_post && observed.owner_destroyed_ && observed.calls_ == 0 &&
           weak_queue.expired();
}

bool test_cancellation_after_endpoint_detach_does_not_touch_retired_owner() {
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    cancellation_observation observed;
    auto owner_value = std::make_unique<cancellation_owner>(runtime.handle(), observed);
    auto queue = ruvia::make_worker_cancellation_target(*owner_value, runtime.handle());
    ruvia::stop_source source;
    auto registration = source.token().register_callback(
        ruvia::worker_cancellation_post(queue, 11));
    queue->detach(*owner_value);
    owner_value.reset();
    runtime.detach();
    source.request_stop();
    return observed.owner_destroyed_ && observed.calls_ == 0 && context.run() == 0 &&
           queue.use_count() == 1;
}

bool test_worker_runtime_context_owns_stable_detached_endpoint() {
    asio::io_context context;
    std::optional<ruvia::worker_handle> escaped_handle;
    {
        ruvia::worker_runtime_context runtime(context, 8);
        const auto* handle_address = &runtime.handle();
        if (&runtime.io_context() != &context || handle_address != &runtime.handle() ||
            !runtime.handle().valid()) {
            return false;
        }
        escaped_handle.emplace(runtime.handle());
        runtime.detach();
        if (runtime.handle().valid() || escaped_handle->valid()) {
            return false;
        }
    }
    asio::post(context, [] {});
    context.run();
    return escaped_handle && !escaped_handle->valid();
}

bool test_queue_callable_destruction_can_inspect_worker() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker_value = attachment.loop().handle();
    queue_destructor_state state_value{.worker_ = &worker_value, .attachment_ = &attachment};
    const auto submitted = worker_value.post(queue_destructor_callback(state_value));
    if (!submitted.accepted()) {
        return false;
    }
    attachment.run();
    return state_value.ran_ && state_value.destroyed_ > 0;
}

bool test_queue_factory_rollback_and_detach() {
    asio::io_context context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(context, 1);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    bool thrown = false;
    try {
        static_cast<void>((worker_value).post_factory([]() -> ruvia::move_only_function<void()> {
            throw std::runtime_error("factory failed");
        }));
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    bool empty = false;
    try {
        static_cast<void>((worker_value).post_factory([] { return ruvia::move_only_function<void()>(); }));
    } catch (const std::invalid_argument&) {
        empty = true;
    }
    bool recovered = (worker_value).post_factory([] { return ruvia::move_only_function<void()>([] {}); }) ==
                     ruvia::post_status::accepted;
    context.run();

    bool raw_stack_rejected = false;
    ruvia::detail::worker_dispatcher raw_stack(context, 1);
    try {
        static_cast<void>(raw_stack.post([] {}));
    } catch (const std::bad_weak_ptr&) {
        raw_stack_rejected = true;
    }

    bool abandoned_ran = false;
    bool abandoned_destroyed = false;
    struct probe final {
        bool* ran_;
        bool* destroyed_;
        probe(bool& ran_value, bool& destroyed_value)
            : ran_(&ran_value),
              destroyed_(&destroyed_value) {}
        probe(probe&& other) noexcept
            : ran_(other.ran_),
              destroyed_(std::exchange(other.destroyed_, nullptr)) {}
        ~probe() {
            if (destroyed_ != nullptr) {
                *destroyed_ = true;
            }
        }
        void operator()() {
            *ran_ = true;
        }
    };
    const auto detached = std::make_shared<ruvia::detail::worker_dispatcher>(context, 1);
    const auto detached_worker = ruvia::detail::worker_handle_access::make(detached);
    const auto status = (detached_worker).post_factory([detached, &abandoned_ran, &abandoned_destroyed] {
        detached->detach_context();
        return ruvia::move_only_function<void()>(
            probe{abandoned_ran, abandoned_destroyed});
    });
    return thrown && empty && recovered && raw_stack_rejected &&
           status == ruvia::post_status::accepted && !abandoned_ran && abandoned_destroyed;
}

bool test_queue_factory_can_finish_after_detach() {
    asio::io_context context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(context, 1);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    std::promise<void> entered;
    std::promise<void> resume;
    auto resumed = resume.get_future();
    std::atomic_int destroyed{0};
    bool ran = false;
    auto status = ruvia::post_status::worker_stopping;
    struct payload final {
        std::atomic_int* destroyed_;
        ~payload() {
            destroyed_->fetch_add(1);
        }
    };
    std::jthread producer_value([&] {
        status = (worker_value).post_factory([&] {
            auto payload_value = std::make_unique<payload>(&destroyed);
            entered.set_value();
            resumed.wait();
            return ruvia::move_only_function<void()>(
                [payload_value = std::move(payload_value), &ran] { ran = true; });
        });
    });
    entered.get_future().wait();
    dispatcher->detach_context();
    const bool retained_while_reserved = destroyed.load() == 0;
    resume.set_value();
    producer_value.join();
    context.run();
    return retained_while_reserved && status == ruvia::post_status::accepted && !ran &&
           destroyed.load() == 1;
}

bool test_event_loop_post_borrow_and_rejected_callable_ownership() {
    constexpr std::size_t repeated_posts = 64;
    counting_resource resource;
    std::atomic_bool invalid_payload_valid{false};
    std::atomic_size_t repeated_valid{0};
    std::atomic_size_t repeated_invalid{0};
    std::promise<void> repeated_completed;
    auto repeated_completion = repeated_completed.get_future();
    ruvia::event_loop_pool accepting({.loop_count_ = 1, .queue_capacity_ = repeated_posts + 1});
    const auto loop = accepting.loop(0);

    auto invalid = ruvia::event_loop{}.post(
        [payload = post_payload(&resource), &invalid_payload_valid]() mutable {
            invalid_payload_valid.store(payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                            payload.bytes_.back() == 'x',
                std::memory_order_release);
        });
    if (invalid != ruvia::post_status::worker_stopping || invalid.rejected() == nullptr ||
        resource.allocations_ == resource.deallocations_) {
        return false;
    }
    auto invalid_task = std::move(invalid).take_rejected();
    if (resource.allocations_ == resource.deallocations_ ||
        loop.post(std::move(invalid_task)) != ruvia::post_status::accepted) {
        return false;
    }

    for (std::size_t i = 0; i < repeated_posts; ++i) {
        auto posted = accepting.loop(0).post([payload = post_payload(&resource), i, &repeated_valid,
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
        resource.allocations_ != resource.deallocations_) {
        return false;
    }

    ruvia::event_loop closed;
    {
        ruvia::event_loop_pool stopped({.loop_count_ = 1, .queue_capacity_ = 1});
        closed = stopped.loop(0);
        stopped.join();
    }
    std::atomic_bool closed_payload_valid{false};
    auto rejected = closed.post(
        [payload = post_payload(&resource), &closed_payload_valid]() mutable {
            closed_payload_valid.store(payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                           payload.bytes_.back() == 'x',
                std::memory_order_release);
        });
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        resource.allocations_ == resource.deallocations_) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    if (resource.allocations_ == resource.deallocations_) {
        return false;
    }
    std::promise<void> recovered;
    auto recovered_future = recovered.get_future();
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    auto recovered_post = recovery.loop(0).post(
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
           resource.allocations_ == resource.deallocations_;
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

    auto rejected = loop.post(std::move(*input));
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        original_pool != nullptr || loop.valid()) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    input.reset();
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    if (recovery.loop(0).post(std::move(retry)) != ruvia::post_status::accepted) {
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
    counting_resource resource;
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
    const auto callable_storage = resource.allocations_ - resource.deallocations_;
    if (callable_storage == 0) {
        return false;
    }

    auto rejected = loop.post(*input);
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        original_pool != nullptr || loop.valid()) {
        return false;
    }
    input.reset();
    if (resource.allocations_ - resource.deallocations_ != callable_storage) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    if (resource.allocations_ - resource.deallocations_ != callable_storage) {
        return false;
    }
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    if (recovery.loop(0).post(std::move(retry)) != ruvia::post_status::accepted) {
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
           resource.allocations_ == resource.deallocations_;
}

bool test_worker_submission_view_lifecycle_and_rejection() {
    struct throw_on_copy final {
        explicit throw_on_copy(std::pmr::memory_resource* resource)
            : bytes_(resource) {
            bytes_.resize(4096, 'c');
        }
        throw_on_copy(const throw_on_copy& other)
            : bytes_(other.bytes_, other.bytes_.get_allocator().resource()) {
            throw std::runtime_error("copy failed");
        }
        throw_on_copy(throw_on_copy&&) noexcept = default;
        void operator()() const {}
        std::pmr::vector<char> bytes_;
    };
    struct throw_on_move final {
        explicit throw_on_move(std::pmr::memory_resource* resource)
            : bytes_(resource) {
            bytes_.resize(4096, 'm');
        }
        throw_on_move(throw_on_move&& other)
            : bytes_(std::move(other.bytes_)) {
            throw std::runtime_error("move failed");
        }
        void operator()() const {}
        std::pmr::vector<char> bytes_;
    };

    asio::io_context exception_context;
    ruvia::worker_runtime_context exception_runtime(exception_context, 1);
    const auto exception_view = exception_runtime.submission();
    counting_resource exception_resource;
    {
        throw_on_copy copy_input(&exception_resource);
        throw_on_move move_input(&exception_resource);
        bool copy_threw = false;
        bool move_threw = false;
        try {
            static_cast<void>(exception_view.post(copy_input));
        } catch (const std::runtime_error&) {
            copy_threw = true;
        }
        try {
            static_cast<void>(exception_view.post(std::move(move_input)));
        } catch (const std::runtime_error&) {
            move_threw = true;
        }
        const auto after_exception = exception_view.post([] {});
        if (!copy_threw || !move_threw || !exception_view.valid() || !exception_view.accepting() ||
            after_exception != ruvia::post_status::accepted) {
            return false;
        }
    }
    if (exception_resource.allocations_ != exception_resource.deallocations_) {
        return false;
    }

    struct view_reentry_state final {
        std::optional<ruvia::worker_submission_view>* source_;
        ruvia::worker_runtime_context* runtime_;
        bool detach_;
        bool retired_{false};
        bool payload_valid_{false};
        counting_resource* resource_;

        void retire() noexcept {
            if (retired_) {
                return;
            }
            retired_ = true;
            source_->reset();
            if (detach_) {
                runtime_->detach();
            } else {
                runtime_->close();
            }
        }
    };
    struct reentrant_view_callable final {
        explicit reentrant_view_callable(view_reentry_state& state_value)
            : state_(&state_value),
              payload_(state_value.resource_) {
            payload_.resize(4096, 'v');
        }
        reentrant_view_callable(const reentrant_view_callable& other)
            : state_(other.state_),
              payload_(other.payload_, other.payload_.get_allocator().resource()) {
            state_->retire();
        }
        reentrant_view_callable(reentrant_view_callable&& other) noexcept
            : state_(other.state_),
              payload_(std::move(other.payload_)) {
            state_->retire();
        }
        void operator()() {
            state_->payload_valid_ = payload_.size() == 4096 && payload_.front() == 'v' &&
                                     payload_.back() == 'v';
        }
        view_reentry_state* state_;
        std::pmr::vector<char> payload_;
    };

    const auto exercise_reentry = [](bool detach, bool use_copy) {
        counting_resource resource;
        asio::io_context context;
        ruvia::worker_runtime_context runtime(context, 1);
        std::optional<ruvia::worker_submission_view> source;
        source.emplace(runtime.submission());
        view_reentry_state state_value{.source_ = &source, .runtime_ = &runtime, .detach_ = detach, .resource_ = &resource};
        std::optional<reentrant_view_callable> input;
        input.emplace(state_value);
        auto rejected = use_copy ? source->post(*input) : source->post(std::move(*input));
        if (!state_value.retired_ || source || !runtime.submission().valid() ||
            runtime.submission().accepting() || rejected != ruvia::post_status::worker_stopping ||
            rejected.rejected() == nullptr) {
            return false;
        }
        auto task_value = std::move(rejected).take_rejected();
        ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
        if (recovery.loop(0).post(std::move(task_value)) != ruvia::post_status::accepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        input.reset();
        return state_value.payload_valid_ && resource.allocations_ == resource.deallocations_;
    };
    if (!exercise_reentry(false, true) || !exercise_reentry(true, false)) {
        return false;
    }

    struct reserved_move_state final {
        ruvia::worker_runtime_context* runtime_;
        counting_resource* resource_;
        bool detach_;
        int moves_{0};
        bool reservation_observed_{false};
        bool ran_{false};
    };
    struct inline_reserved_move_callable final {
        explicit inline_reserved_move_callable(reserved_move_state& state_value)
            : state_(&state_value),
              payload_(static_cast<char*>(state_value.resource_->allocate(16, alignof(char)))) {
            std::fill_n(payload_, 16, 'r');
        }
        inline_reserved_move_callable(inline_reserved_move_callable&& other) noexcept
            : state_(other.state_),
              payload_(std::exchange(other.payload_, nullptr)) {
            // This move transfers the callable into its already reserved node;
            // detach may cause additional cleanup moves after this point.
            if (++state_->moves_ == 3) {
                state_->reservation_observed_ =
                    state_->runtime_->submission().post([] {}) == ruvia::post_status::queue_full;
                if (state_->detach_) {
                    state_->runtime_->detach();
                } else {
                    state_->runtime_->close();
                }
            }
        }
        ~inline_reserved_move_callable() {
            if (payload_ != nullptr) {
                state_->resource_->deallocate(payload_, 16, alignof(char));
            }
        }
        void operator()() {
            state_->ran_ = payload_ != nullptr && payload_[0] == 'r' && payload_[15] == 'r';
        }

        reserved_move_state* state_;
        char* payload_;
    };
    static_assert(sizeof(inline_reserved_move_callable) <= 3 * sizeof(void*));
    static_assert(std::is_nothrow_move_constructible_v<inline_reserved_move_callable>);

    const auto exercise_reserved_move = [](bool detach) {
        counting_resource resource;
        asio::io_context context;
        ruvia::worker_runtime_context runtime(context, 1);
        reserved_move_state state_value{.runtime_ = &runtime, .resource_ = &resource, .detach_ = detach};
        inline_reserved_move_callable input(state_value);
        const auto submitted = runtime.submission().post(std::move(input));
        if (submitted != ruvia::post_status::accepted || !state_value.reservation_observed_ ||
            resource.allocations_ != 1) {
            return false;
        }
        if (detach) {
            if (state_value.ran_ || resource.deallocations_ != 1) {
                return false;
            }
        } else {
            context.run();
            if (!state_value.ran_ || resource.deallocations_ != 1) {
                return false;
            }
        }
        return resource.allocations_ == resource.deallocations_;
    };
    if (!exercise_reserved_move(false) || !exercise_reserved_move(true)) {
        return false;
    }

    constexpr std::size_t repeated_posts = 64;
    counting_resource resource;
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, repeated_posts + 2);
    const auto view = runtime.submission();
    bool accepted_payload_valid = false;
    if (!view.valid() || !view.accepting() || view.post([] {}) != ruvia::post_status::accepted) {
        return false;
    }
    auto accepted = view.post([payload = post_payload(&resource), &accepted_payload_valid]() mutable {
        accepted_payload_valid = payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                 payload.bytes_.back() == 'x';
    });
    if (accepted != ruvia::post_status::accepted) {
        return false;
    }
    for (std::size_t index = 0; index < repeated_posts; ++index) {
        auto posted = view.post([payload = post_payload(&resource)]() mutable {
            if (payload.bytes_.size() != 4096 || payload.bytes_.front() != 'x') {
                std::terminate();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    context.run();
    if (!accepted_payload_valid || resource.allocations_ != resource.deallocations_) {
        return false;
    }

    asio::io_context full_context;
    ruvia::worker_runtime_context full_runtime(full_context, 1);
    const auto full_view = full_runtime.submission();
    counting_resource rejected_resource;
    bool retained_payload_valid = false;
    if (full_view.post([] {}) != ruvia::post_status::accepted) {
        return false;
    }
    auto full = full_view.post([payload = post_payload(&rejected_resource), &retained_payload_valid]() mutable {
        retained_payload_valid = payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                 payload.bytes_.back() == 'x';
    });
    if (full != ruvia::post_status::queue_full || full.rejected() == nullptr) {
        return false;
    }
    auto retained = std::move(full).take_rejected();
    for (int attempt_value = 0; attempt_value < 3; ++attempt_value) {
        auto again = full_view.post(std::move(retained));
        if (again != ruvia::post_status::queue_full || again.rejected() == nullptr ||
            rejected_resource.allocations_ == rejected_resource.deallocations_) {
            return false;
        }
        retained = std::move(again).take_rejected();
    }
    full_context.run();
    full_context.restart();
    const auto retried = full_view.post(std::move(retained));
    if (retried != ruvia::post_status::accepted) {
        return false;
    }
    full_context.run();
    if (!retained_payload_valid || rejected_resource.allocations_ != rejected_resource.deallocations_) {
        return false;
    }

    asio::io_context closed_context;
    ruvia::worker_runtime_context closed_runtime(closed_context, 1);
    auto closed_view = closed_runtime.submission();
    closed_runtime.close();
    if (!closed_view.valid() || closed_view.accepting() ||
        closed_view.post([] {}) != ruvia::post_status::worker_stopping) {
        return false;
    }
    closed_runtime.detach();
    if (!closed_view.valid() || closed_view.accepting()) {
        return false;
    }
    ruvia::worker_submission_view invalid;
    return !invalid.valid() && !invalid.accepting() &&
           invalid.post([] {}) == ruvia::post_status::worker_stopping && resource.allocations_ > 0 &&
           resource.allocations_ == resource.deallocations_;
}

bool test_post_outcome_invariants_and_empty_callbacks() {
    bool accepted_take_rejected = false;
    try {
        static_cast<void>(std::move(ruvia::post_result_type::accept()).take_rejected());
    } catch (const std::logic_error&) {
        accepted_take_rejected = true;
    }

    bool accepted_rejection_status = false;
    try {
        static_cast<void>(ruvia::post_result_type::reject(ruvia::post_status::accepted, [] {}));
    } catch (const std::invalid_argument&) {
        accepted_rejection_status = true;
    }

    bool empty_rejected_task = false;
    try {
        static_cast<void>(ruvia::post_result_type::reject(ruvia::post_status::queue_full, {}));
    } catch (const std::invalid_argument&) {
        empty_rejected_task = true;
    }

    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    bool empty_post = false;
    using null_callback_type = void (*)();
    try {
        static_cast<void>(loop.post(static_cast<null_callback_type>(nullptr)));
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
    return accepted_take_rejected && accepted_rejection_status && empty_rejected_task && empty_post &&
           empty_stop_callback;
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
    ruvia::worker_handle invalid_worker;
    try {
        ruvia::worker_signal invalid(invalid_worker);
    } catch (const std::invalid_argument&) {
        invalid_worker_rejected = true;
    }

    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::worker_signal first_signal(worker_handle_value);
    ruvia::worker_signal second_signal(worker_handle_value);
    const bool worker_borrowed = &first_signal.worker() == &worker_handle_value;
    bool wait_creation_rejected = false;
    try {
        static_cast<void>(first_signal.wait());
    } catch (const std::logic_error&) {
        wait_creation_rejected = true;
    }
    bool first_resumed = false;
    bool second_resumed = false;
    std::size_t remaining = 2;
    asio::co_spawn(io_context,
        ruvia::detail::task_as_awaitable(
            wait_for_signal(first_signal, first_resumed, remaining, attachment)),
        asio::detached);
    asio::co_spawn(io_context,
        ruvia::detail::task_as_awaitable(
            wait_for_signal(second_signal, second_resumed, remaining, attachment)),
        asio::detached);
    asio::post(io_context, [&] {
        first_signal.notify();
        second_signal.notify();
    });
    io_context.run();
    return invalid_worker_rejected && worker_borrowed && wait_creation_rejected && first_resumed && second_resumed;
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
    co_await signal.wait();

    bool resumed = false;
    ruvia::task_scope scope(signal.worker());
    scope.spawn(wait_signal_once(signal, resumed));
    const bool second_wait_suspended = !resumed;
    signal.notify();
    signal.notify();
    co_await scope.join();
    co_await signal.wait();
    success = second_wait_suspended && resumed;
    attachment.stop();
}

bool test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_signal signal(worker_value);
    bool success = false;
    asio::co_spawn(context,
        ruvia::detail::task_as_awaitable(exercise_signal_pending_latch(signal, attachment, success)),
        asio::detached);
    context.run();
    return success;
}

bool test_worker_signal_has_no_arbitrary_waiter_limit() {
    constexpr std::size_t waiter_count = 16;
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::worker_signal signal(worker_handle_value);
    std::array<bool, waiter_count> resumed{};
    std::size_t remaining = waiter_count;
    for (std::size_t index = 0; index < resumed.size(); ++index) {
        asio::co_spawn(io_context,
            ruvia::detail::task_as_awaitable(
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

ruvia::task<void> start_cold_signal_wait(ruvia::task<void> cold_wait, bool& rejected) {
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
    const auto owner_handle = owner_attachment.loop().handle();
    ruvia::worker_signal signal(owner_handle);
    std::optional<ruvia::task<void>> cold_wait;

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
        ruvia::detail::task_as_awaitable(start_cold_signal_wait(std::move(*cold_wait), rejected)),
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
    if (!first.valid() || first.id() == 0 || first.id() == second.id() || first.is_current()) {
        return false;
    }
    constexpr std::string_view key = "device-42";
    if (loops.loop_for(key).id() != loops.loop_for(ruvia::detail::worker_selection_hash(key)).id()) {
        return false;
    }
    if (&first.io_context() != &first.executor().context() || first.handle().id() != first.id()) {
        return false;
    }
    asio::ip::tcp::socket tcp(first.io_context());
    asio::ip::udp::socket udp(first.io_context());
    if (&tcp.get_executor().context() != &first.io_context() ||
        &udp.get_executor().context() != &first.io_context()) {
        return false;
    }

    std::promise<bool> completed;
    auto result_value = completed.get_future();
    std::atomic_bool stop_callback_ran{false};
    std::atomic_bool stop_callback_on_loop{false};
    auto stop_registration_value = first.on_stop([&]() -> ruvia::task<void> {
        stop_callback_on_loop = first.is_current();
        stop_callback_ran = true;
        co_return;
    });
    auto move_only = std::make_unique<int>(42);
    if (first.post([worker = first, value = std::move(move_only),
                       completed = std::move(completed)]() mutable {
            completed.set_value(worker.is_current() && *value == 42);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    const bool success = result_value.get();
    loops.stop();
    loops.join();
    return success && stop_registration_value.valid() && stop_callback_ran && stop_callback_on_loop &&
           first.post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_bounded_queue() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 2});
    const auto worker_value = loops.loop(0);
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
        if (!attachment.valid() || !loop.valid() || &loop.io_context() != &io_context) {
            return false;
        }

        asio::ip::tcp::socket tcp(loop.executor());
        asio::ip::udp::socket udp(loop.executor());
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
            stop_callback_on_loop = loop.is_current();
            stop_callback_ran = true;
            co_return;
        });
        if (loop.post([loop, completed = std::move(completed)]() mutable {
                completed.set_value(loop.is_current());
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
            !stop_callback_on_loop || loop.valid() ||
            loop.post([] {}) != ruvia::post_status::worker_stopping) {
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
    ruvia::worker_handle worker_value, std::atomic_bool& finished) {
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
        co_await finish_shutdown_cleanup(first.handle(), first_cleanup_finished);
    });
    auto second_cleanup = second.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(second.handle(), second_cleanup_finished);
    });
    std::promise<void> reported;
    auto reported_result = reported.get_future();
    if (first.post([first, &reported] {
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
    const bool all_closed = !first.accepting() && !second.accepting();
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
    root.get();

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

bool test_pool_failure_report_works_after_queue_closure() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool queued_work_ran{false};
    if (loop.post([&] { queued_work_ran.store(true, std::memory_order_release); }) !=
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
    return failure_rethrown && queued_work_ran.load(std::memory_order_acquire) && !loop.valid();
}

bool test_attachment_report_failure_keeps_native_run_ownership() {
    const auto run = [](bool wrapped_run) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context);
        const auto loop = attachment.loop();
        std::atomic_bool cleanup_finished{false};
        auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
            co_await finish_shutdown_cleanup(loop.handle(), cleanup_finished);
        });
        bool report_returned = false;
        bool unrelated_work_ran = false;
        asio::post(io_context, [&] {
            loop.report_failure(std::make_exception_ptr(std::runtime_error("attached report")));
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
               cleanup_finished.load(std::memory_order_acquire) && !native_run_threw && !loop.valid();
    };
    return run(true) && run(false);
}

ruvia::task<void> failed_abandoned_attachment_root() {
    throw std::runtime_error("abandoned attachment root failed");
    co_return;
}

bool test_abandoned_attachment_root_failure_retires(bool wrapped_run) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto loop = attachment.loop();
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
           !loop.valid() && context_access_rejected;
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
    return cleanup.valid() && cleanup_ran && failure_propagated && !loop.valid();
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
    const bool post_rejected = loop.post([] {}) == ruvia::post_status::worker_stopping;
    attachment.stop();
    return io_context_rejected && executor_rejected && post_rejected && !attachment.valid() &&
           !loop.valid();
}

bool test_join_stops_running_pool() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    loops.start();
    loops.join();
    return !loop.accepting() && loop.post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_failure_propagation() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stop_callback_ran{false};
    std::atomic_bool stop_callback_on_loop{false};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_callback_on_loop = loop.is_current();
        stop_callback_ran = true;
        co_return;
    });
    struct listener final : ruvia::detail::worker_shutdown_listener {
        void worker_stopping() noexcept override {
            notified_ = true;
        }
        bool notified_{false};
    };
    const auto listener_value = std::make_shared<listener>();
    ruvia::detail::worker_handle_access::register_shutdown_listener(loop.handle(), listener_value);
    if (loop.post([] { throw std::runtime_error("posted task failed"); }) !=
        ruvia::post_status::accepted) {
        return false;
    }
    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stop_registration_value.valid() && stop_callback_ran && stop_callback_on_loop &&
               listener_value->notified_ && std::string_view(error.what()) == "posted task failed";
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
        if (!first.is_current()) {
            stops_on_owners.store(false, std::memory_order_relaxed);
        }
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    auto second_stop = second.on_stop([&]() -> ruvia::task<void> {
        if (!second.is_current()) {
            stops_on_owners.store(false, std::memory_order_relaxed);
        }
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    if (first.post([&] {
            if (!first.is_current()) {
                tasks_on_owners.store(false, std::memory_order_relaxed);
            }
            task_calls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::post_status::accepted ||
        second.post([&] {
            if (!second.is_current()) {
                tasks_on_owners.store(false, std::memory_order_relaxed);
            }
            task_calls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.join();
    const bool rejected_after_join = first.post([] {}) == ruvia::post_status::worker_stopping &&
                                     second.post([] {}) == ruvia::post_status::worker_stopping;
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
        stop_on_owner.store(loop.is_current(), std::memory_order_release);
        co_return;
    });
    if (loop.post([] { throw std::runtime_error("pre-start task failed"); }) !=
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
    if (loop.post([&] {
            bool rejected = false;
            try {
                loops.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                           "cannot join an event loop pool from one of its workers";
            }
            completed.set_value(rejected && loop.is_current());
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
        failed_stop_on_owner.store(failed_loop.is_current(), std::memory_order_release);
        failed_stop_calls.fetch_add(1, std::memory_order_relaxed);
        asio::post(failed_loop.io_context(),
            [] { throw std::runtime_error("secondary shutdown handler failed"); });
        asio::post(failed_loop.io_context(), [&] {
            shutdown_continuation_drained.store(failed_loop.is_current(), std::memory_order_release);
        });
        co_return;
    });
    auto peer_stop = peer_loop.on_stop([&]() -> ruvia::task<void> {
        peer_stop_on_owner.store(peer_loop.is_current(), std::memory_order_release);
        peer_stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    asio::post(failed_loop.io_context(), [] { throw std::runtime_error("executor handler failed"); });
    if (failed_loop.post([probe = std::make_unique<abandon_probe>(abandoned_queue_destroyed),
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

bool test_expired_handle() {
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
    return !loop.valid() && !loop.accepting() && context_rejected &&
           loop.post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_escaped_worker_handle_becomes_detached_endpoint() {
    ruvia::worker_handle worker;
    ruvia::worker_id_type live_id = 0;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
        worker = loops.loop(0).handle();
        live_id = worker.id();
        if (!worker.valid() || live_id == 0) {
            return false;
        }
    }

    bool internal_defer_rejected = false;
    try {
        ruvia::detail::worker_handle_access::defer(worker, [] {});
    } catch (const std::runtime_error&) {
        internal_defer_rejected = true;
    }
    return !worker.valid() && !worker.accepting() && !worker.is_current() && worker.id() == 0 &&
           worker.post([] {}) == ruvia::post_status::worker_stopping && internal_defer_rejected;
}

bool test_failure_destroys_abandoned_queue_tasks() {
    struct destruction_probe final {
        explicit destruction_probe(bool& value) noexcept
            : destroyed_(&value) {}
        bool* destroyed_;
        ~destruction_probe() {
            *destroyed_ = true;
        }
    };

    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 2);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    bool queued_task_destroyed = false;
    if (worker_value.post([] { throw std::runtime_error("stop queue drain"); }) !=
            ruvia::post_status::accepted ||
        worker_value.post([probe = std::make_unique<destruction_probe>(queued_task_destroyed)] {}) !=
            ruvia::post_status::accepted) {
        return false;
    }
    try {
        io_context.run();
    } catch (const std::runtime_error&) {
    }
    if (!queued_task_destroyed) {
        return false;
    }
    dispatcher->detach_context();
    return queued_task_destroyed && !worker_value.valid();
}

bool test_abandoned_root_task_completes_when_queue_drain_fails() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    if (loop.post([] { throw std::runtime_error("boom"); }) != ruvia::post_status::accepted) {
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
        static_cast<void>(root.get());
        return false;
    } catch (const std::runtime_error& error) {
        return std::string_view(error.what()) == "event loop is stopping";
    } catch (...) {
        return false;
    }
}

bool test_dispatcher_lifecycle_hooks_are_worker_affine() {
    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 1);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    bool startup_on_worker = false;
    bool failure_on_worker = false;
    bool shutdown_on_worker = false;
    bool received_startup_failure = false;

    dispatcher->run_context(
        [&] {
            startup_on_worker = worker_value.is_current();
            throw std::runtime_error("worker startup failed");
        },
        [&](std::exception_ptr failure) noexcept {
            failure_on_worker = worker_value.is_current();
            try {
                std::rethrow_exception(failure);
            } catch (const std::runtime_error& error) {
                received_startup_failure = std::string_view(error.what()) == "worker startup failed";
            } catch (...) {
            }
        },
        [&]() noexcept { shutdown_on_worker = worker_value.is_current(); });
    dispatcher->detach_context();
    return startup_on_worker && failure_on_worker && shutdown_on_worker && received_startup_failure;
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

bool test_stop_listener_arrival_during_shutdown_notification_batch() {
    struct blocking_listener final : ruvia::detail::worker_shutdown_listener {
        blocking_listener(std::promise<void>& entered_promise,
            std::shared_future<void> release_signal) noexcept
            : entered_(&entered_promise),
              release_(std::move(release_signal)) {}

        std::promise<void>* entered_;
        std::shared_future<void> release_;

        void worker_stopping() noexcept override {
            entered_->set_value();
            release_.wait();
        }
    };

    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    std::promise<void> listener_entered;
    auto listener_entered_result = listener_entered.get_future();
    std::promise<void> release_listener;
    auto blocking_listener_value = std::make_shared<blocking_listener>(
        listener_entered, release_listener.get_future().share());
    ruvia::detail::worker_handle_access::register_shutdown_listener(loop.handle(), blocking_listener_value);

    std::atomic_bool stop_callback_ran{false};
    auto stop_callback_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_callback_ran.store(true, std::memory_order_release);
        co_return;
    });
    asio::post(loop.io_context(), [] { throw std::runtime_error("first shutdown trigger"); });
    loops.start();
    if (listener_entered_result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        release_listener.set_value();
        loops.join();
        return false;
    }

    loops.stop();
    const bool loop_still_attached_during_batch = loop.valid();
    bool root_admission_closed = false;
    try {
        static_cast<void>(loop.start(failed_abandoned_attachment_root()));
    } catch (const std::runtime_error&) {
        root_admission_closed = true;
    }
    release_listener.set_value();
    bool failure_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        failure_rethrown = std::string_view(error.what()) == "first shutdown trigger";
    } catch (...) {
    }
    return loop_still_attached_during_batch && root_admission_closed && failure_rethrown &&
           stop_callback_ran.load(std::memory_order_acquire) && stop_callback_value.valid() && !loop.valid();
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
    const auto worker_value = loop.handle();
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

ruvia::task<void> wait_for_shutdown_timer(const ruvia::worker_handle& worker_value, bool& child_done) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)));
    child_done = true;
    co_return;
}

ruvia::task<void> nested_shutdown_scope(const ruvia::worker_handle& worker_value, bool& child_done,
    bool& middle_done) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(wait_for_shutdown_timer(worker_value, child_done));
    co_await scope.join();
    middle_done = true;
}

ruvia::task<void> root_shutdown_scope(const ruvia::worker_handle& worker_value,
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

struct root_pmr_result final {
    explicit root_pmr_result(std::pmr::memory_resource* resource)
        : values_(resource) {
        values_.resize(32, 42);
    }
    root_pmr_result(root_pmr_result&&) noexcept = default;
    root_pmr_result(const root_pmr_result&) = delete;
    std::pmr::vector<int> values_;
};

ruvia::task<root_pmr_result> make_root_pmr_result(std::pmr::memory_resource* resource) {
    co_return root_pmr_result(resource);
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
    static_cast<void>(root->get());
    const bool replacement_survived = root->valid();
    if (replacement_survived) {
        static_cast<void>(root->get());
    }
    loops.join();
    return !valid_during_move && replacement_survived && !root->valid();
}

ruvia::task<const int> immutable_root_result(const ruvia::worker_handle& worker_value) {
    co_return worker_value.is_current() ? 17 : -1;
}

bool test_root_task_delivers_const_result() {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto worker_value = loops.loop(0).handle();
    auto root = loops.loop(0).start(immutable_root_result(worker_value));
    loops.start();
    const auto value = root.get();
    loops.stop();
    loops.join();
    return value == 17 && !root.valid();
}

bool test_root_task_result_owns_pmr_storage_past_pool_retirement() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    counting_resource resource;
    auto root = loop.start(make_root_pmr_result(&resource));
    loops.start();
    root.wait();
    loops.stop();
    loops.join();
    bool retained = false;
    {
        auto result_value = root.get();
        const auto payload_bytes = result_value.values_.capacity() * sizeof(int);
        const bool payload_retained = resource.owns(result_value.values_.data(), payload_bytes);
        const bool values_valid = result_value.values_.size() == 32 &&
                                  std::all_of(result_value.values_.begin(), result_value.values_.end(),
                                      [](int value) { return value == 42; });
        retained = values_valid && result_value.values_.get_allocator().resource() == &resource &&
                   payload_retained && resource.live_allocations() > 0;
    }
    return retained && resource.live_allocations() == 0 &&
           resource.allocations_ == resource.deallocations_;
}

bool test_root_tasks_join_nested_scopes_during_stop() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
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
        root.get();
    } catch (...) {
        return false;
    }
    return child_done && middle_done && root_done;
}

bool test_lifecycle_transitions_are_monotonic() {
    using lifecycle_type = ruvia::runtime_lifecycle;
    lifecycle_type lifecycle;
    lifecycle.complete_stop();
    if (lifecycle.state() != lifecycle_type::state_type::ready || !lifecycle.start()) {
        return false;
    }
    lifecycle.complete_stop();
    if (lifecycle.state() != lifecycle_type::state_type::running || lifecycle.start() ||
        !lifecycle.request_stop() || lifecycle.state() != lifecycle_type::state_type::stopping ||
        lifecycle.request_stop()) {
        return false;
    }

    lifecycle.complete_stop();
    return lifecycle.state() == lifecycle_type::state_type::stopped && !lifecycle.request_stop() &&
           lifecycle.state() == lifecycle_type::state_type::stopped && !lifecycle.start();
}

bool test_concurrent_stop_has_one_initiator() {
    using lifecycle_type = ruvia::runtime_lifecycle;
    constexpr std::size_t thread_count = 16;
    lifecycle_type lifecycle;
    if (!lifecycle.start()) {
        return false;
    }

    std::atomic<std::size_t> initiators{0};
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool start = false;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([&] {
                {
                    std::unique_lock lock(gate_mutex);
                    gate_changed.wait(lock, [&] { return start; });
                }
                if (lifecycle.request_stop()) {
                    initiators.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    } catch (...) {
        {
            std::lock_guard lock(gate_mutex);
            start = true;
        }
        gate_changed.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        throw;
    }
    {
        std::lock_guard lock(gate_mutex);
        start = true;
    }
    gate_changed.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }

    lifecycle.complete_stop();
    return initiators.load(std::memory_order_relaxed) == 1 &&
           lifecycle.state() == lifecycle_type::state_type::stopped && !lifecycle.request_stop();
}

}  // namespace

int main() {
    const auto run = [](const char* name, bool (*test)()) {
        std::printf("[ RUN ] %s\n", name);
        std::fflush(stdout);
        const bool passed = test();
        std::printf("[%s] %s\n", passed ? " ok " : "FAIL", name);
        std::fflush(stdout);
        return passed;
    };
    return run("event_loop_post_borrow_and_rejected_callable_ownership",
               test_event_loop_post_borrow_and_rejected_callable_ownership) &&
                   run("event_loop_post_protects_reentrant_inline_move",
                       test_event_loop_post_protects_reentrant_inline_move) &&
                   run("event_loop_post_protects_reentrant_heap_copy",
                       test_event_loop_post_protects_reentrant_heap_copy) &&
                   run("worker_handle_callable_lifetime", test_worker_handle_callable_lifetime) &&
                   run("worker_submission_view_lifecycle_and_rejection",
                       test_worker_submission_view_lifecycle_and_rejection) &&
                   run("post_outcome_invariants_and_empty_callbacks",
                       test_post_outcome_invariants_and_empty_callbacks) &&
                   run("worker_runtime_context_owns_stable_detached_endpoint",
                       test_worker_runtime_context_owns_stable_detached_endpoint) &&
                   run("queue_callable_destruction_can_inspect_worker",
                       test_queue_callable_destruction_can_inspect_worker) &&
                   run("queue_factory_rollback_and_detach",
                       test_queue_factory_rollback_and_detach) &&
                   run("queue_factory_can_finish_after_detach",
                       test_queue_factory_can_finish_after_detach) &&
                   run("worker_signal_is_worker_affine", test_worker_signal_is_worker_affine) &&
                   run("worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake",
                       test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake) &&
                   run("worker_signal_has_no_waiter_limit",
                       test_worker_signal_has_no_arbitrary_waiter_limit) &&
                   run("worker_signal_rechecks_cold_wait_affinity",
                       test_worker_signal_rechecks_affinity_when_cold_wait_starts) &&
                   run("dispatch_and_affinity", test_dispatch_and_affinity) &&
                   run("bounded_queue", test_bounded_queue) &&
                   run("cancellation_with_full_queue", [] { return test_cancellation_reaches_worker_with_saturated_or_closed_queue(false); }) &&
                   run("cancellation_with_closed_queue", [] { return test_cancellation_reaches_worker_with_saturated_or_closed_queue(true); }) &&
                   run("queued_cancellation_after_owner_retirement", test_queued_cancellation_releases_queue_after_owner_retirement) &&
                   run("cancellation_after_endpoint_detach", test_cancellation_after_endpoint_detach_does_not_touch_retired_owner) &&
                   run("external_event_loop_attachment", test_external_event_loop_attachment) &&
                   run("attachment_run_failure_retires_through_async_cleanup",
                       test_attachment_run_failure_retires_through_async_cleanup) &&
                   run("abandoned_attachment_root_failure_retires_native_run",
                       [] { return test_abandoned_attachment_root_failure_retires(false); }) &&
                   run("abandoned_attachment_root_failure_retires_attachment_run",
                       [] { return test_abandoned_attachment_root_failure_retires(true); }) &&
                   run("pool_reported_failure_stops_every_loop_and_join_rethrows",
                       test_pool_reported_failure_stops_every_loop_and_join_rethrows) &&
                   run("pool_failure_handoff_during_destructor_report",
                       test_pool_failure_handoff_during_destructor_report) &&
                   run("pool_failure_report_works_after_queue_closure",
                       test_pool_failure_report_works_after_queue_closure) &&
                   run("attachment_report_failure_preserves_native_run_ownership",
                       test_attachment_report_failure_keeps_native_run_ownership) &&
                   run("external_attachment_retains_state_until_cleanup",
                       test_external_attachment_retains_state_until_context_cleanup) &&
                   run("external_attachment_handles_context_destruction",
                       test_external_attachment_handles_context_destruction) &&
                   run("join_stops_running_pool", test_join_stops_running_pool) &&
                   run("failure_propagation", test_failure_propagation) &&
                   run("join_before_start_drains_on_owners", test_join_before_start_drains_on_owners) &&
                   run("stop_before_start_propagates_failure",
                       test_stop_before_start_propagates_failure) &&
                   run("join_rejects_pool_worker", test_join_rejects_pool_worker) &&
                   run("executor_failure_drains_shutdown_on_owners",
                       test_executor_failure_drains_shutdown_on_owners) &&
                   run("expired_handle", test_expired_handle) &&
                   run("escaped_worker_handle_detaches",
                       test_escaped_worker_handle_becomes_detached_endpoint) &&
                   run("failure_destroys_abandoned_queue_tasks",
                       test_failure_destroys_abandoned_queue_tasks) &&
                   run("abandoned_root_task_completes_when_queue_drain_fails",
                       test_abandoned_root_task_completes_when_queue_drain_fails) &&
                   run("dispatcher_lifecycle_hooks_are_worker_affine",
                       test_dispatcher_lifecycle_hooks_are_worker_affine) &&
                   run("stop_callback_failure_reaches_join", test_stop_callback_failure_reaches_join) &&
                   run("stop_callback_factory_failure_keeps_other_callbacks_running",
                       test_stop_callback_factory_failure_does_not_skip_other_callbacks) &&
                   run("stop_listener_arrival_waits_for_shutdown_notification_batch",
                       test_stop_listener_arrival_during_shutdown_notification_batch) &&
                   run("async_stop_callbacks_start_together_and_keep_captures_alive",
                       test_async_stop_callbacks_start_together_and_keep_captures_alive) &&
                   run("root_tasks_join_nested_scopes_during_stop",
                       test_root_tasks_join_nested_scopes_during_stop) &&
                   run("root_result_moves_are_reentrant_and_preserve_owner",
                       test_root_task_result_moves_outside_lock_and_preserves_reentrant_owner) &&
                   run("root_result_owns_pmr_storage_past_pool_retirement",
                       test_root_task_result_owns_pmr_storage_past_pool_retirement) &&
                   run("root_task_delivers_const_result",
                       test_root_task_delivers_const_result) &&
                   run("lifecycle_transitions_are_monotonic",
                       test_lifecycle_transitions_are_monotonic) &&
                   run("concurrent_stop_has_one_initiator", test_concurrent_stop_has_one_initiator)
               ? 0
               : 1;
}
