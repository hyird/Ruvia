#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/root_task.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

namespace detail {
struct event_loop_state;
class event_loop_stop_listener;
}  // namespace detail

class event_loop_stop_registration final {
public:
    event_loop_stop_registration() noexcept = default;
    ~event_loop_stop_registration() = default;

    event_loop_stop_registration(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration& operator=(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration(event_loop_stop_registration&&) noexcept = default;
    event_loop_stop_registration& operator=(event_loop_stop_registration&&) noexcept = default;

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
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;

    // A retired pool loop or attachment is invalid; accessing its context or
    // obtaining a fresh executor then throws std::logic_error. Previously
    // obtained executors must not outlive the loop's physical context storage.
    [[nodiscard]] asio::io_context& io_context() const&;
    asio::io_context& io_context() const&& = delete;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    [[nodiscard]] worker_handle handle() const noexcept;

    // Reports a terminal runtime failure without throwing on the caller's
    // thread. Pool owners retain the first failure for join(); attachments
    // route it to the existing unhandled-failure diagnostic.
    void report_failure(std::exception_ptr failure) const noexcept;

    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result_type post(fn_type&& fn) const {
        return dispatch_handle().post(std::forward<fn_type>(fn));
    }

    // Cold-path cleanup for an already owned resource, not new business work.
    // Bypasses bounded admission but acquires retirement protection; returns
    // false once final retirement begins. Accepted callbacks and their owned
    // inputs retire before the loop can release its execution context.
    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] bool defer_cleanup(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.defer_cleanup_task(move_only_function<void()>(std::forward<fn_type>(fn)));
    }

    // Starts one lazy task on this loop and returns its structured completion
    // owner. The loop owns the started coroutine until completion; abandoning
    // the root_task never destroys a suspended frame, and an abandoned failure
    // is routed to the loop failure sink.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] root_task<t_type> start(task<t_type> task_value) const {
        auto retirement_lease = acquire_root_lease();
        if (!retirement_lease) {
            throw std::runtime_error("event loop is stopping");
        }
        auto completion = std::allocate_shared<detail::root_task_state<t_type>>(
            std::pmr::polymorphic_allocator<detail::root_task_state<t_type>>(process_resource()),
            handle(), state_);
        const auto bound_executor = executor();
        // drain() destroys remaining queue closures without invoking them when
        // a posted task throws. Own that destructor path so wait()/get() cannot
        // hang and an unobserved failure still reaches the loop sink.
        struct launch_guard final {
            std::optional<task<t_type>> task_;
            std::shared_ptr<detail::root_task_state<t_type>> completion_;

            launch_guard(task<t_type> task_value, std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
                : task_(std::move(task_value)),
                  completion_(std::move(state_value)) {}
            launch_guard(const launch_guard&) = delete;
            launch_guard& operator=(const launch_guard&) = delete;
            launch_guard(launch_guard&& other) noexcept
                : task_(std::move(other.task_)),
                  completion_(std::move(other.completion_)) {}
            launch_guard& operator=(launch_guard&&) = delete;
            ~launch_guard() {
                // A rejected or discarded launch still owns a cold frame.
                // Retire it before waking a caller that may reclaim its inputs.
                task_.reset();
                if (completion_ != nullptr) {
                    completion_->complete_failure(
                        std::make_exception_ptr(std::runtime_error("event loop is stopping")));
                }
            }
            void release() noexcept {
                task_.reset();
                completion_.reset();
            }
        };
        struct root_completion final {
            using executor_type = asio::io_context::executor_type;

            executor_type executor_;
            std::shared_ptr<detail::root_task_state<t_type>> completion_;
            std::shared_ptr<void> retirement_lease_;
            bool delivered_{false};

            root_completion(executor_type executor_value, std::shared_ptr<detail::root_task_state<t_type>> state_value,
                std::shared_ptr<void> lease_value) noexcept
                : executor_(std::move(executor_value)),
                  completion_(std::move(state_value)),
                  retirement_lease_(std::move(lease_value)) {}
            root_completion(const root_completion&) = delete;
            root_completion& operator=(const root_completion&) = delete;
            root_completion(root_completion&& other) noexcept
                : executor_(std::move(other.executor_)),
                  completion_(std::move(other.completion_)),
                  retirement_lease_(std::move(other.retirement_lease_)),
                  delivered_(std::exchange(other.delivered_, false)) {}
            root_completion& operator=(root_completion&&) = delete;
            ~root_completion() {
                if (delivered_) {
                    completion_->publish_completion();
                }
            }
            [[nodiscard]] executor_type get_executor() const noexcept {
                return executor_;
            }
            void operator()(detail::task_completion_result<t_type>&& result_value) noexcept {
                try {
                    if (result_value.index() == 1) {
                        completion_->stage_failure(std::get<1>(result_value));
                    } else if constexpr (std::is_void_v<t_type>) {
                        completion_->stage_value();
                    } else {
                        completion_->stage_value(std::move(std::get<0>(result_value)));
                    }
                } catch (...) {
                    completion_->stage_failure(std::current_exception());
                }
                delivered_ = true;
            }
        };
        auto posted = post([completion, bound_executor,
                               retirement_lease = std::move(retirement_lease),
                               launch = launch_guard(std::move(task_value), completion)]() mutable {
            try {
                detail::async_start_task(std::move(*launch.task_),
                    root_completion(bound_executor, completion, retirement_lease));
                launch.release();
            } catch (...) {
                launch.release();
                completion->complete_failure(std::current_exception());
            }
        });
        if (!posted.accepted()) {
            if (posted.status() == post_status::queue_full) {
                throw std::runtime_error("event loop root task queue is full");
            }
            throw std::runtime_error("event loop is stopping");
        }
        return root_task<t_type>(std::move(completion));
    }

    // Stop cleanup is asynchronous and joined before runtime retirement. The
    // callable owner remains address-stable until its returned task and
    // completion handler have both finished; resetting the registration only
    // unregisters cleanup that has not started.
    template <typename fn_type>
        requires std::invocable<fn_type&> &&
                 std::same_as<std::invoke_result_t<fn_type&>, task<void>> &&
                 std::move_constructible<std::decay_t<fn_type>>
    [[nodiscard]] event_loop_stop_registration on_stop(fn_type&& fn) const {
        return register_stop_callback(
            move_only_function<task<void>()>(std::forward<fn_type>(fn)));
    }

private:
    explicit event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept;
    [[nodiscard]] const worker_handle& dispatch_handle() const noexcept;
    [[nodiscard]] event_loop_stop_registration register_stop_callback(
        move_only_function<task<void>()> callback) const;
    [[nodiscard]] std::shared_ptr<void> acquire_root_lease() const;
    [[nodiscard]] bool defer_cleanup_task(move_only_function<void()> task) const;

    std::shared_ptr<detail::event_loop_state> state_;
    friend class event_loop_pool;
    friend class event_loop_attachment;
};

struct event_loop_attachment_options final {
    std::size_t queue_capacity_{1024};
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
    // identity installed. The attachment object must outlive this call.
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
