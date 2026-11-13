#pragma once

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/disposition.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"

namespace ruvia {

// The completion argument of async_start(). Void tasks complete with
// std::exception_ptr, which satisfies Asio's disposition concept directly;
// value-returning tasks complete with the value first and the failure second,
// passed by rvalue reference so over-aligned results are never passed by value
// on MSVC.
template <typename t_type>
    requires detail::asio_task_result<t_type>
using task_completion_result = std::conditional_t<std::is_void_v<t_type>, std::exception_ptr,
    std::variant<t_type, std::exception_ptr>>;

}  // namespace ruvia

namespace ruvia::detail {

// Owns the started task and the completion handler, allocated with the
// handler's associated allocator, until delivery on its associated executor.
template <typename t_type, typename handler_type>
class task_completion_state final {
public:
    using handler_allocator_type = asio::associated_allocator_t<handler_type>;
    using state_allocator_type = typename std::allocator_traits<handler_allocator_type>::template rebind_alloc<
        task_completion_state>;
    using state_allocator_traits_type = std::allocator_traits<state_allocator_type>;

    static task_completion_state* create(task<t_type>&& task_value, handler_type&& handler_value) {
        handler_allocator_type handler_allocator(asio::get_associated_allocator(handler_value));
        state_allocator_type state_allocator(handler_allocator);
        auto* state_value = state_allocator_traits_type::allocate(state_allocator, 1);
        try {
            state_allocator_traits_type::construct(state_allocator, state_value, std::move(task_value),
                std::forward<handler_type>(handler_value), std::move(handler_allocator));
        } catch (...) {
            state_allocator_traits_type::deallocate(state_allocator, state_value, 1);
            throw;
        }
        return state_value;
    }

    task_completion_state(
        task<t_type>&& task_value, handler_type&& handler_value, handler_allocator_type allocator_value)
        : task_(std::move(task_value)),
          handler_(std::forward<handler_type>(handler_value)),
          allocator_(std::move(allocator_value)) {}

    void start() {
        task_.coroutine_.promise().control_.set_completion(this, &task_completion_state::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state_value = static_cast<task_completion_state*>(raw);
        auto executor = asio::get_associated_executor(state_value->handler_);
        auto completion_allocator = state_value->allocator_;
        state_owner_type owner_value(state_value, state_deleter_type{state_allocator_type(state_value->allocator_)});
        asio::post(executor, asio::bind_allocator(std::move(completion_allocator),
                                 [owner_value = std::move(owner_value)]() mutable {
                                     deliver(std::move(owner_value));
                                 }));
    }

private:
    struct state_deleter_type final {
        void operator()(task_completion_state* state_value) noexcept {
            state_allocator_traits_type::destroy(allocator_, state_value);
            state_allocator_traits_type::deallocate(allocator_, state_value, 1);
        }

        state_allocator_type allocator_;
    };

    using state_owner_type = std::unique_ptr<task_completion_state, state_deleter_type>;

    static void deliver(state_owner_type owner_value) {
        // The delivery owner outlives the result and every callback argument.
        // An owning completion handler can publish external readiness in its
        // destructor only after moved-from result storage has been reclaimed.
        auto handler = std::move(owner_value->handler_);
        owner_value->task_.prepare_direct_completion();
        auto result_value = [&owner_value]() -> task_completion_result<t_type> {
            try {
                if constexpr (std::is_void_v<t_type>) {
                    owner_value->task_.coroutine_.promise().result();
                    return {};
                } else {
                    return task_completion_result<t_type>(
                        std::in_place_index<0>, owner_value->task_.coroutine_.promise().result());
                }
            } catch (...) {
                if constexpr (std::is_void_v<t_type>) {
                    return std::current_exception();
                } else {
                    return task_completion_result<t_type>(std::in_place_index<1>, std::current_exception());
                }
            }
        }();
        owner_value->task_.finish_direct_completion();
        owner_value.reset();
        std::move(handler)(std::move(result_value));
    }

    task<t_type> task_;
    handler_type handler_;
    handler_allocator_type allocator_;
};

template <typename t_type>
    requires asio_task_result<t_type> && (!std::is_void_v<t_type>)
[[nodiscard]] asio::awaitable<t_type> await_non_void_task(task<t_type> task_value) {
    auto result_value = co_await async_start(std::move(task_value), asio::use_awaitable);
    if (result_value.index() == 1) {
        std::rethrow_exception(std::get<1>(result_value));
    }
    co_return std::move(std::get<0>(result_value));
}

}  // namespace ruvia::detail

namespace ruvia {

// Starts a lazy task as an Asio asynchronous operation. The task starts
// inline on the calling thread; its completion is delivered through the
// handler's associated executor with a task_completion_result<T>, after the
// coroutine frame has been reclaimed. The operation state is allocated with
// the handler's associated allocator. Any completion token works: a callback,
// use_awaitable, use_future, deferred, or a custom token.
template <typename t_type, typename completion_token_type>
    requires detail::asio_task_result<t_type>
auto async_start(task<t_type>&& task_value, completion_token_type&& token) {
    task_value.prepare_direct_start();
    using completion_signature_type =
        std::conditional_t<std::is_void_v<t_type>, void(std::exception_ptr), void(task_completion_result<t_type>&&)>;
    return asio::async_initiate<completion_token_type, completion_signature_type>(
        [](auto&& handler, task<t_type> started_task) {
            using handler_type = std::decay_t<decltype(handler)>;
            auto* state_value = detail::task_completion_state<t_type, handler_type>::create(
                std::move(started_task), std::forward<decltype(handler)>(handler));
            state_value->start();
        },
        token, std::move(task_value));
}

// Adapts a lazy task for an Asio coroutine owner such as co_spawn. A failed
// task rethrows its exception from the awaitable. Void tasks use Asio's
// std::exception_ptr disposition, so use_awaitable produces the awaitable<void>
// directly without a wrapper coroutine frame.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    if constexpr (std::is_void_v<t_type>) {
        return async_start(std::move(task_value), asio::use_awaitable);
    } else {
        return detail::await_non_void_task(std::move(task_value));
    }
}

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields. Void completions carry
// only the error code: the result accessors and the two-argument factory exist
// only for a non-void result_type.
template <typename result_type>
class asio_completion final {
    using result_storage_type = std::conditional_t<std::is_void_v<result_type>, std::monostate, result_type>;

public:
    asio_completion(const asio_completion&) = default;
    asio_completion(asio_completion&&) = default;
    asio_completion& operator=(const asio_completion&) = delete;
    asio_completion& operator=(asio_completion&&) = delete;

    [[nodiscard]] static asio_completion completed(std::error_code error_code) noexcept
        requires std::is_void_v<result_type>
    {
        return asio_completion(error_code, result_storage_type{});
    }

    [[nodiscard]] static asio_completion completed(std::error_code error_code, result_storage_type result_value)
        requires(!std::is_void_v<result_type>)
    {
        return asio_completion(error_code, std::move(result_value));
    }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool succeeded() const noexcept {
        return !error_code_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return static_cast<bool>(error_code_);
    }

    [[nodiscard]] const result_storage_type& result() const& noexcept
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    const result_storage_type& result() const&& = delete;

    [[nodiscard]] result_storage_type& result() & noexcept
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    result_storage_type& result() && = delete;

    [[nodiscard]] result_storage_type take_result() &&
        requires(!std::is_void_v<result_type>)
    {
        return std::move(result_);
    }

private:
    asio_completion(std::error_code error_code, result_storage_type&& result_value) noexcept(
        std::is_nothrow_move_constructible_v<result_storage_type>)
        : error_code_(error_code),
          result_(std::move(result_value)) {}

    std::error_code error_code_;
    [[no_unique_address]] result_storage_type result_;
};

// The single-use awaiter of one completion-based Asio operation, as returned
// by async_asio(). It owns the initiation and the completion state until the
// operation finishes, and is neither copyable, movable, nor addressable; an
// owner that keeps it beyond one await-expression constructs it in place
// (for example std::optional::emplace) from the initiation.
template <typename result_type, typename initiate_type>
class asio_completion_awaiter final {
public:
    explicit asio_completion_awaiter(initiate_type initiate)
        : initiate_(std::move(initiate)) {}
    asio_completion_awaiter(const asio_completion_awaiter&) = delete;
    asio_completion_awaiter& operator=(const asio_completion_awaiter&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the exception propagates from the await-expression
    // directly ([expr.await]/5), without an exception_ptr side channel.
    //
    // An asio initiation may complete synchronously (for example a closed
    // socket or a resolver cache hit), invoking the completion handler before
    // await_suspend returns. Resuming the continuation from inside that window
    // would run the coroutine while it is still suspended-by-await_suspend and
    // potentially destroy its frame before this function returns. The
    // suspend_race_state records the completion instead; the return value of
    // await_suspend then reports the actual race order, so a synchronous
    // completion is consumed by await_resume without resuming at all.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        if constexpr (std::is_void_v<result_type>) {
            initiate_([this, continuation](std::error_code ec, auto&&...) mutable {
                if (state_.complete(asio_completion<void>::completed(ec))) {
                    continuation.resume();
                }
            });
        } else {
            initiate_([this, continuation](std::error_code ec, result_type result_value) mutable {
                if (state_.complete(asio_completion<result_type>::completed(ec, std::move(result_value)))) {
                    continuation.resume();
                }
            });
        }
        return state_.suspend(continuation);
    }

    [[nodiscard]] asio_completion<result_type> await_resume() {
        return state_.take_value();
    }

private:
    initiate_type initiate_;
    detail::suspend_race_state<asio_completion<result_type>> state_;
};

// Adapts a completion-based Asio operation for a Ruvia coroutine. The
// initiation receives one handler taking (std::error_code[, result_type]).
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return asio_completion_awaiter<result_type, std::decay_t<initiate_type>>(std::forward<initiate_type>(initiate));
}

}  // namespace ruvia

namespace ruvia {

class event_loop;

namespace detail {

struct event_loop_state;

// Routes an unobserved root-task failure through the loop's own failure sink,
// borrowed from the live loop state instead of copied into every task. A
// retired loop falls back to the unhandled-failure diagnostic.
void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept;

class root_task_completion final {
public:
    root_task_completion(worker_ref worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : worker_(std::move(worker_value)),
          loop_(std::move(loop_value)) {}

    root_task_completion(const root_task_completion&) = delete;
    root_task_completion& operator=(const root_task_completion&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        const std::lock_guard lock(mutex_);
        return owner_alive_;
    }

    [[nodiscard]] bool ready() const noexcept {
        const std::lock_guard lock(mutex_);
        return complete_;
    }

    void wait() const {
        require_foreign_thread();
        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this] { return complete_; });
    }

    [[nodiscard]] bool wait_until(std::chrono::steady_clock::time_point deadline) const {
        require_foreign_thread();
        std::unique_lock lock(mutex_);
        return completed_.wait_until(lock, deadline, [this] { return complete_; });
    }

    void abandon() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (!owner_alive_) {
                return;
            }
            owner_alive_ = false;
            if (complete_) {
                unobserved = failure_;
            }
        }
        report(std::move(unobserved));
    }

    void stage_failure(std::exception_ptr failure) noexcept {
        const std::lock_guard lock(mutex_);
        if (complete_) {
            std::terminate();
        }
        failure_ = std::move(failure);
    }

    // Publish only after the delivery owner has destroyed all intermediate
    // result objects and reclaimed their moved-from storage.
    void publish_completion() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (complete_) {
                std::terminate();
            }
            complete_ = true;
            if (!owner_alive_) {
                unobserved = failure_;
            }
        }
        completed_.notify_all();
        report(std::move(unobserved));
    }

    void complete_failure(std::exception_ptr failure) noexcept {
        stage_failure(std::move(failure));
        publish_completion();
    }

    [[nodiscard]] std::exception_ptr consume_failure() {
        wait();
        std::lock_guard lock(mutex_);
        if (!owner_alive_) {
            throw std::logic_error("root task result was already consumed");
        }
        owner_alive_ = false;
        return failure_;
    }

private:
    void require_foreign_thread() const {
        if (worker_.is_current()) {
            throw std::logic_error("cannot wait for a root task on its event loop");
        }
    }

    void report(std::exception_ptr failure) const noexcept {
        if (!failure) {
            return;
        }
        report_root_task_failure(loop_, std::move(failure));
    }

    worker_ref worker_;
    std::weak_ptr<event_loop_state> loop_;
    mutable std::mutex mutex_;
    mutable std::condition_variable completed_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool owner_alive_{true};
};

struct root_task_void_result final {};

template <typename t_type>
class root_task_state final {
public:
    root_task_state(worker_ref worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : completion_(std::move(worker_value), std::move(loop_value)) {}

    // The type-independent completion protocol; only the result is typed here.
    [[nodiscard]] root_task_completion& completion() noexcept {
        return completion_;
    }
    [[nodiscard]] const root_task_completion& completion() const noexcept {
        return completion_;
    }

    template <typename value_type>
        requires std::same_as<std::remove_cvref_t<value_type>, std::remove_cv_t<t_type>>
    void stage_value(value_type&& value) {
        value_.emplace(std::forward<value_type>(value));
    }

    void stage_value() noexcept
        requires std::is_void_v<t_type>
    {}

    t_type get() {
        auto failure = completion_.consume_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if constexpr (!std::is_void_v<t_type>) {
            struct retire_value final {
                std::optional<t_type>& value_;
                ~retire_value() {
                    value_.reset();
                }
            } retire{value_};
            return std::move(*value_);
        }
    }

private:
    root_task_completion completion_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<t_type>, root_task_void_result, std::optional<t_type>> value_;
};

}  // namespace detail

template <typename t_type>
class [[nodiscard]] root_task final {
public:
    root_task(const root_task&) = delete;
    root_task& operator=(const root_task&) = delete;

    root_task(root_task&& other) noexcept
        : state_(std::move(other.state_)) {}

    root_task& operator=(root_task&& other) noexcept {
        if (this != &other) {
            // The old result may own or replace the source during retirement.
            root_task incoming(std::move(other));
            state_.swap(incoming.state_);
        }
        return *this;
    }

    ~root_task() {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ && state_->completion().valid();
    }

    // True once the task has completed and its result has not been consumed.
    [[nodiscard]] bool ready() const noexcept {
        return state_ && state_->completion().valid() && state_->completion().ready();
    }

    // Blocks a thread other than the loop's own until completion.
    void wait() const {
        require_state().completion().wait();
    }

    // Bounded wait(): returns whether the task completed in time. A relative
    // bound starts now; an absolute deadline that has passed only reports the
    // current state.
    [[nodiscard]] bool wait_for(operation_timeout timeout) const {
        return require_state().completion().wait_until(timeout.deadline_from(std::chrono::steady_clock::now()));
    }

    // Consumes the result, so only an rvalue extracts it:
    // std::move(root).get(). User result moves run without the state mutex
    // and may re-enter this root_task. The object must remain alive for the
    // entire synchronous call; replacing its owned state during a move is
    // supported.
    decltype(auto) get() && {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        auto state_value = state_;
        struct retire_state final {
            std::shared_ptr<detail::root_task_state<t_type>>& state_;
            const std::shared_ptr<detail::root_task_state<t_type>>& expected_;
            ~retire_state() {
                if (state_ == expected_ && !expected_->completion().valid()) {
                    state_.reset();
                }
            }
        } retire{state_, state_value};
        if constexpr (std::is_void_v<t_type>) {
            state_value->get();
            return;
        } else {
            return state_value->get();
        }
    }
    void get() & = delete;

private:
    explicit root_task(std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : state_(std::move(state_value)) {}

    [[nodiscard]] const detail::root_task_state<t_type>& require_state() const {
        if (!state_) {
            throw std::logic_error("root task has no state");
        }
        return *state_;
    }

    void reset() noexcept {
        if (auto state_value = std::exchange(state_, nullptr)) {
            state_value->completion().abandon();
        }
    }

    std::shared_ptr<detail::root_task_state<t_type>> state_;
    friend class event_loop;
};

}  // namespace ruvia

namespace ruvia {

namespace detail {
struct event_loop_state;
class event_loop_stop_listener;
template <typename t_type>
struct root_task_launch_guard;
}  // namespace detail

// Thrown by event_loop::start() when the loop refuses a task: status()
// distinguishes a full admission queue from a stopping or retired loop. Also
// the failure of an admitted root task whose launch was discarded because
// the loop stopped before running it.
class event_loop_start_rejected final : public std::runtime_error {
public:
    [[nodiscard]] post_status status() const noexcept {
        return status_;
    }

private:
    friend class event_loop;
    template <typename>
    friend struct detail::root_task_launch_guard;

    explicit event_loop_start_rejected(post_status status);

    post_status status_;
};

// The outcome of event_loop::try_start(): accepted owns the started
// root_task (value()/take_value()); a rejection owns the unconsumed cold task
// (rejected()/take_rejected()) with its post_status.
template <typename t_type>
using event_loop_start_result =
    worker_delivery_result<task<t_type>, post_status, post_status::accepted, root_task<t_type>>;

namespace detail {

// drain() destroys remaining queue closures without invoking them when
// a posted task throws. Own that destructor path so wait()/get() cannot
// hang and an unobserved failure still reaches the loop sink.
template <typename t_type>
struct root_task_launch_guard final {
    std::optional<task<t_type>> task_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;

    root_task_launch_guard(task<t_type> task_value, std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : task_(std::move(task_value)),
          completion_(std::move(state_value)) {}
    root_task_launch_guard(const root_task_launch_guard&) = delete;
    root_task_launch_guard& operator=(const root_task_launch_guard&) = delete;
    root_task_launch_guard(root_task_launch_guard&& other) noexcept
        : task_(std::move(other.task_)),
          completion_(std::move(other.completion_)) {}
    root_task_launch_guard& operator=(root_task_launch_guard&&) = delete;
    ~root_task_launch_guard() {
        // A rejected or discarded launch still owns a cold frame.
        // Retire it before waking a caller that may reclaim its inputs.
        task_.reset();
        if (completion_ != nullptr) {
            completion_->completion().complete_failure(
                std::make_exception_ptr(event_loop_start_rejected(post_status::worker_stopping)));
        }
    }
    void release() noexcept {
        task_.reset();
        completion_.reset();
    }
};
template <typename t_type>
struct root_task_completion_handler final {
    using executor_type = asio::io_context::executor_type;

    executor_type executor_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;
    std::shared_ptr<void> retirement_lease_;
    bool delivered_{false};

    root_task_completion_handler(executor_type executor_value, std::shared_ptr<detail::root_task_state<t_type>> state_value,
        std::shared_ptr<void> lease_value) noexcept
        : executor_(std::move(executor_value)),
          completion_(std::move(state_value)),
          retirement_lease_(std::move(lease_value)) {}
    root_task_completion_handler(const root_task_completion_handler&) = delete;
    root_task_completion_handler& operator=(const root_task_completion_handler&) = delete;
    root_task_completion_handler(root_task_completion_handler&& other) noexcept
        : executor_(std::move(other.executor_)),
          completion_(std::move(other.completion_)),
          retirement_lease_(std::move(other.retirement_lease_)),
          delivered_(std::exchange(other.delivered_, false)) {}
    root_task_completion_handler& operator=(root_task_completion_handler&&) = delete;
    ~root_task_completion_handler() {
        if (delivered_) {
            completion_->completion().publish_completion();
        }
    }
    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }
    void operator()(task_completion_result<t_type>&& result_value) noexcept {
        try {
            if constexpr (std::is_void_v<t_type>) {
                if (result_value != nullptr) {
                    completion_->completion().stage_failure(std::move(result_value));
                } else {
                    completion_->stage_value();
                }
            } else if (result_value.index() == 1) {
                completion_->completion().stage_failure(std::get<1>(result_value));
            } else {
                completion_->stage_value(std::move(std::get<0>(result_value)));
            }
        } catch (...) {
            completion_->completion().stage_failure(std::current_exception());
        }
        delivered_ = true;
    }
};

}  // namespace detail

class event_loop_stop_registration final {
public:
    event_loop_stop_registration() noexcept = default;
    ~event_loop_stop_registration();

    event_loop_stop_registration(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration& operator=(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration(event_loop_stop_registration&&) noexcept = default;
    event_loop_stop_registration& operator=(event_loop_stop_registration&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;

private:
    explicit event_loop_stop_registration(
        std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept;

    std::shared_ptr<detail::event_loop_stop_listener> listener_;
    friend class event_loop;
};

// A stable handle to one Ruvia runtime and its Asio execution context. event_loop
// does not own a thread: event_loop_pool or event_loop_attachment owns and drives
// the runtime that produced it.
class event_loop final {
public:
    event_loop() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;

    // A retired pool loop or attachment is invalid; accessing its context or
    // obtaining a fresh executor then throws std::logic_error. Previously
    // obtained executors must not outlive the loop's physical context storage.
    [[nodiscard]] asio::io_context& io_context() const&;
    asio::io_context& io_context() const&& = delete;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    // The loop's worker endpoint: admission state, identity, posting, timers,
    // and the completion lane. Rvalue loops return a copy: the worker_ref must not
    // outlive its loop owner.
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    [[nodiscard]] worker_ref worker() const&& noexcept;

    // Reports a terminal runtime failure without throwing on the caller's
    // thread. Pool owners retain the first failure for join(); attachments
    // route it to their configured failure callback or the unhandled-failure
    // diagnostic, then retire without throwing from the host's run().
    void report_failure(std::exception_ptr failure) const noexcept;

    // Loop-level cold-path cleanup of an already owned resource; it belongs
    // to no admitted operation and is not new business work. Bypasses bounded
    // admission but takes retirement protection, so it can still return false
    // after retirement has begun. Accepted callbacks and their owned inputs
    // retire before the loop can release its execution context. Contrast
    // worker_ref::defer_completion(), the completion, cancellation, and
    // resume lane of an operation the worker already admitted.
    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] bool defer_cleanup(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.defer_cleanup_task(move_only_function<void()>(std::forward<fn_type>(fn)));
    }

    // Starts one lazy task on this loop. Accepted: the loop owns the started
    // coroutine until completion and the result holds its structured
    // completion owner; abandoning that root_task never destroys a suspended
    // frame, and an abandoned failure is routed to the loop failure sink.
    // Rejected: a full admission queue (queue_full) or a stopping or retired
    // loop (worker_stopping) returns the unconsumed cold task. Admission is
    // reserved before the task moves, so a rejection never consumes it. Throws
    // only for allocation failure.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] event_loop_start_result<t_type> try_start(task<t_type> task_value) const {
        using result_type = event_loop_start_result<t_type>;
        auto retirement_lease = acquire_root_lease();
        if (!retirement_lease) {
            return detail::worker_delivery_access::reject<result_type>(post_status::worker_stopping, std::move(task_value));
        }
        auto completion = std::make_shared<detail::root_task_state<t_type>>(worker(), state_);
        const auto bound_executor = executor();
        const auto status = worker().post_factory([&]() -> move_only_function<void()> {
            return [bound_executor, retirement_lease = std::move(retirement_lease),
                       launch = detail::root_task_launch_guard<t_type>(std::move(task_value), completion)]() mutable {
                try {
                    async_start(std::move(*launch.task_),
                        detail::root_task_completion_handler<t_type>(bound_executor, launch.completion_, std::move(retirement_lease)));
                    launch.release();
                } catch (...) {
                    auto failed_completion = std::move(launch.completion_);
                    launch.release();
                    failed_completion->completion().complete_failure(std::current_exception());
                }
            };
        });
        if (status != post_status::accepted) {
            return detail::worker_delivery_access::reject<result_type>(status, std::move(task_value));
        }
        return detail::worker_delivery_access::accept<result_type>(root_task<t_type>(std::move(completion)));
    }

    // The local strict form of try_start(): a rejection destroys the cold task
    // and throws event_loop_start_rejected carrying its status.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] root_task<t_type> start(task<t_type> task_value) const {
        auto started = try_start(std::move(task_value));
        if (!started.accepted()) {
            throw event_loop_start_rejected(started.status());
        }
        return std::move(started).take_value();
    }

    // Stop cleanup is asynchronous and joined before runtime retirement. The
    // callable owner remains address-stable until its returned task and
    // completion handler have both finished. Resetting, replacing or destroying
    // the registration only unregisters cleanup that has not started.
    // Cancelling an unstarted callback moves and destroys its target outside
    // the listener lock; those operations may request loop stop.
    template <typename fn_type>
        requires std::invocable<fn_type&> &&
                 std::same_as<std::invoke_result_t<fn_type&>, task<void>> &&
                 std::move_constructible<std::decay_t<fn_type>>
    [[nodiscard]] event_loop_stop_registration on_stop(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.register_stop_callback(
            move_only_function<task<void>()>(std::forward<fn_type>(fn)));
    }

private:
    explicit event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept;
    [[nodiscard]] event_loop_stop_registration register_stop_callback(
        move_only_function<task<void>()> callback) const;
    [[nodiscard]] std::shared_ptr<void> acquire_root_lease() const;
    [[nodiscard]] bool defer_cleanup_task(move_only_function<void()> task) const;

    std::shared_ptr<detail::event_loop_state> state_;
    friend class event_loop_pool;
    friend class event_loop_attachment;
    friend class worker_runtime;
};

struct event_loop_attachment_options final {
    std::size_t queue_capacity_{1024};
    // Borrowed pool for run_blocking() and client host lookups on this loop;
    // it must outlive the attachment. Null leaves the loop without one.
    blocking_pool* blocking_pool_{nullptr};
    // Owns the receiver for each reported failure (including abandoned root
    // tasks and failed stop callbacks). Empty uses report_unhandled_failure.
    // Invoked synchronously, outside runtime locks, on the reporting thread:
    // normally the context thread, but report_failure() or abandoning an
    // already-completed root may report from another thread. Calls may overlap
    // or reenter; the receiver must synchronize any shared mutable state.
    // Requests attachment stop before invocation. The receiver may destroy
    // the attachment or loop handles; its target stays alive through invocation
    // and is retained until the last loop-state owner releases it. Borrowed
    // captures must remain alive that long; do not capture an owning loop.
    // Receiver exceptions go to report_unhandled_failure, never escape run(),
    // and do not interrupt retirement or recursively invoke this receiver.
    move_only_function<void(std::exception_ptr)> failure_{};
};

// Binds a worker to an io_context the caller owns. The caller drives that
// context on exactly one thread, either directly with io_context::run() or
// with event_loop_attachment::run(), which also installs Ruvia's worker
// identity. The attachment never calls io_context::stop() and never restarts
// or joins a context it does not own.
//
// Retirement runs the same stop/finalize sequence as an event_loop_pool loop.
// stop(), or destroying the attachment, closes admission at once, from any
// thread, including while another thread is inside run(). Stop callbacks,
// root tasks, and deferred cleanup then drain, timers are cancelled, and the
// worker retires on the context's thread. Until then the attachment holds
// work on the context, so the caller keeps running it: a plain run() returns
// once retirement and the caller's own work are done. valid() turns false
// when retirement completes, for callers whose own work keeps run() busy.
//
// Destroying the io_context before retirement completes is valid only while
// no root task, stop callback, or deferred cleanup is outstanding; the
// context's shutdown then retires the worker synchronously. Destroying it
// with such work outstanding violates this contract: the shutdown reports a
// std::logic_error to the unhandled-failure diagnostic and terminates,
// because that work is owned by handlers that will never run.
//
// After retirement, event_loop handles become invalid; io_context() and
// executor() throw std::logic_error. The external io_context remains owned
// by its caller.
class event_loop_attachment final {
public:
    ~event_loop_attachment();

    event_loop_attachment(const event_loop_attachment&) = delete;
    event_loop_attachment& operator=(const event_loop_attachment&) = delete;
    // Move construction transfers one attachment without touching its context.
    // Move assignment would implicitly stop the target attachment, so it stays
    // deleted and ownership transfer remains explicit.
    event_loop_attachment(event_loop_attachment&& other) noexcept;
    event_loop_attachment& operator=(event_loop_attachment&& other) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event_loop loop() const noexcept;
    // Runs the attached context on the current thread with Ruvia's worker
    // identity installed. A callback may destroy the attachment during run();
    // the caller-owned context must remain alive until this call returns.
    void run();
    void stop() noexcept;

private:
    explicit event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept;

    std::shared_ptr<detail::event_loop_state> state_;
    friend event_loop_attachment attach_event_loop(asio::io_context&, event_loop_attachment_options);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller retains
// ownership of the context's unrelated work and stop/restart policy; stop()
// retires only Ruvia-managed work and never cancels unrelated operations.
[[nodiscard]] event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options = {});

}  // namespace ruvia

namespace ruvia {

class blocking_pool;

struct event_loop_pool_options final {
    std::size_t loop_count_{0};
    std::size_t queue_capacity_{1024};
    // One borrowed pool shared by every loop for run_blocking() and client
    // host lookups; it must outlive the pool. Null leaves the loops without one.
    blocking_pool* blocking_pool_{nullptr};
};

class event_loop_pool final {
public:
    explicit event_loop_pool(event_loop_pool_options options = {});
    ~event_loop_pool();

    event_loop_pool(const event_loop_pool&) = delete;
    event_loop_pool& operator=(const event_loop_pool&) = delete;
    event_loop_pool(event_loop_pool&&) = delete;
    event_loop_pool& operator=(event_loop_pool&&) = delete;

    void start();
    void stop() noexcept;
    // Stops the pool and waits for every worker to finish its managed cleanup.
    // If join() happens before start(), it creates short-lived owner threads to
    // drain accepted work and run stop callbacks. Loop handles become invalid
    // once joined. Calling join() from a pool worker throws logic_error before
    // stopping the pool.
    void join();

    [[nodiscard]] std::size_t loop_count() const noexcept;
    [[nodiscard]] event_loop loop(std::size_t index) const;
    [[nodiscard]] event_loop next_loop() noexcept;
    [[nodiscard]] event_loop loop_for(std::uint64_t key) const noexcept;
    [[nodiscard]] event_loop loop_for(std::string_view key) const noexcept;

private:
    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

}  // namespace ruvia

namespace ruvia {

class event_loop;
class worker_runtime;

struct worker_notification_options final {
    // Bounds the Windows native wake-pipe connection wait (at most one minute).
    // Synchronous setup system calls are not hard-real-time bounded.
    timeout_duration startup_timeout_{std::chrono::seconds(5)};
};

// coalesced means an existing pending notification covers this call;
// notifications are latched, not counted.
enum class worker_notification_status : std::uint8_t {
    notified,
    coalesced,
    closed,
};

enum class worker_notification_wait_status : std::uint8_t {
    notified,
    closed,
};

namespace detail {
class worker_notification_state;
}  // namespace detail

// A startup-created, address-stable, cross-thread latched wake channel for one
// worker runtime. The native wake pair is created at construction; the receiver
// Asio handle is adopted on the worker's first wait. The notification borrows
// its worker runtime or event loop, which must outlive it. Before destruction, every
// notify() call must have returned and no future call may begin. close()
// rejects new notifications but does not join producers. The owner worker must
// explicitly close the notification and join every suspended wait before its
// runtime stops. A cold, never-awaited instance can be destroyed directly.
class worker_notification final {
public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] worker_notification_wait_status await_resume();

    private:
        explicit wait_operation(worker_notification& owner_value) noexcept;

        worker_notification* owner_;
        friend class worker_notification;
    };

    explicit worker_notification(
        worker_runtime& runtime, worker_notification_options options = {});
    // Borrows the loop's worker; the loop must remain address-stable and alive.
    explicit worker_notification(
        const event_loop& loop, worker_notification_options options = {});
    worker_notification(event_loop&&, worker_notification_options = {}) = delete;
    worker_notification(const event_loop&&, worker_notification_options = {}) = delete;
    ~worker_notification();

    worker_notification(const worker_notification&) = delete;
    worker_notification& operator=(const worker_notification&) = delete;
    worker_notification(worker_notification&&) = delete;
    worker_notification& operator=(worker_notification&&) = delete;

    // Safe from any thread. The method updates its atomic latch and performs a
    // nonblocking native write; an existing latch or proven full channel coalesces.
    [[nodiscard]] worker_notification_status notify() noexcept;

    // Returns the one stable awaiter; waiting is worker-affine and permits one
    // outstanding wait. Constructing or discarding a cold task around wait()
    // does not register an operation. A notification racing with registration
    // may resume asynchronously.
    [[nodiscard]] wait_operation& wait() & noexcept;
    wait_operation& wait() && = delete;

    // Worker-affine and nonblocking. A pending wait completes as closed after
    // Asio delivers and drains its cancellation completion.
    void close();

private:
    using state_pointer = std::unique_ptr<detail::worker_notification_state>;

    explicit worker_notification(state_pointer state) noexcept;

    state_pointer state_;
    wait_operation wait_awaiter_;
};

}  // namespace ruvia

namespace ruvia {
namespace detail {

template <typename t_type>
inline constexpr bool is_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool is_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

template <typename t_type>
concept temporary_owning_char_string =
    is_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

template <typename range_type>
concept temporary_owning_byte_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, std::byte>;

}  // namespace detail

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}
template <detail::temporary_owning_char_string string_type>
std::span<const std::byte> as_bytes(string_type&&) = delete;

[[nodiscard]] inline std::string_view as_chars(std::span<const std::byte> bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}
template <detail::temporary_owning_byte_range range_type>
std::string_view as_chars(range_type&&) = delete;

}  // namespace ruvia

namespace ruvia {

// UTF-8 text describing an error code, for every Ruvia message that embeds one.
// On Windows, message() of std::system_category() and asio::system_category()
// is FormatMessageA output in the process ANSI code page (for example GBK), which
// would corrupt UTF-8 responses, JSON and logs; codes of those categories are
// therefore formatted with FormatMessageW and converted to UTF-8, or described
// as "<category>:<value>" when the system has no message. Every other category,
// and every category on POSIX, uses message() unchanged.
[[nodiscard]] std::string error_code_text(const std::error_code& error);

// what() of a std::system_error with its error-code text produced by
// error_code_text. The supported standard libraries build what() as
// "<what_arg>: <code().message()>"; when what() does not end with that
// message, only error_code_text(code()) is returned.
[[nodiscard]] std::string system_error_text(const std::system_error& error);
// The last resort for a failure that has nowhere else to go: a detached task or
// a callback threw, no application sink was configured to receive it, and no
// caller will ever rethrow it. Writing one line to stderr is the only remaining
// way to keep it observable.
//
// Discarding it instead would make the fault undiagnosable, and terminating
// would turn one local failure into a whole-process one. Every layer routes its
// unowned failures here so a Ruvia process never fails silently, and so the
// diagnostic looks the same wherever it came from:
//
//     ruvia: <context> failed: <what>
//
// `context` names the failing unit ("web connection", "event loop stop callback").
// Writing is best effort -- stderr may be closed, full, or redirected -- but
// nothing beyond this point could report the failure either way.
//
// A lifecycle owner that rethrows one failure among several (application::run)
// reports only the ones it does not rethrow; the caller owns the rethrown one.
//
// Rate limited: a fault that fails every connection at once would otherwise
// bury the first, informative failure under its consequences, and a blocked
// stderr would slow the recovery it is describing. Excess reports are counted
// and the count travels with the next line that gets through, so the volume is
// bounded without the flood becoming invisible. An application that wants every
// failure should install its layer's sink instead of relying on this.
//
// `occurrences` > 1 reports that many equivalent failures (see
// equivalent_failures) as one line, so one root cause shared by several units
// is not repeated once per unit.
//
// A std::system_error is described by system_error_text,
// so the line stays UTF-8 on Windows.
void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences = 1) noexcept;

// True for the same exception object, or for two std::exception objects of the
// same dynamic type with the same what(): the same fault seen by several units.
[[nodiscard]] bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept;

}  // namespace ruvia

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::optional<asio::ip::address> parse_ip_address(std::string_view text);
// Close a socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace ruvia
