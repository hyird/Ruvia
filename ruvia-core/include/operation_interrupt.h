#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include "operation_options.h"
#include "stop_token.h"
#include "task.h"
#include "worker_cancellation.h"
#include "worker_ref.h"
#include "worker_timer.h"

// Interruptible I/O operation primitives: single-operation lane leases,
// parent-linked operation options, and deadline/stop delivery to running
// worker-affine operations.
namespace ruvia {

// A failed claim leaves the lane untouched; the caller supplies its domain error.
class operation_lane_lease final {
public:
    explicit operation_lane_lease(bool& active) noexcept :
        active_(active ? nullptr : &active) {
        if (active_ != nullptr) {
            *active_ = true;
        }
    }

    operation_lane_lease(const operation_lane_lease&) = delete;
    operation_lane_lease& operator=(const operation_lane_lease&) = delete;
    operation_lane_lease(operation_lane_lease&& other) noexcept :
        active_(std::exchange(other.active_, nullptr)) {}
    operation_lane_lease& operator=(operation_lane_lease&&) = delete;

    ~operation_lane_lease() noexcept {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return active_ != nullptr;
    }

private:
    bool* active_;
};

[[nodiscard]] inline operation_lane_lease claim_operation_lane(bool& active, const char* in_progress_message) {
    operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error(in_progress_message);
    }
    return lease;
}

namespace detail {

template <typename result_type, typename start_type>
task<result_type> run_linked_operation(operation_options options, stop_token host_stop, start_type start) {
    const linked_operation_options linked(options, host_stop);
    co_return co_await start(linked.options());
}

}  // namespace detail

// Starts one operation of a host-bound handle: the handle's own options with
// the host's cancellation linked, never replaced by a user token. When the
// tokens are the same or either side cannot stop, `start` receives one
// directly, without a wrapper coroutine frame or allocation; otherwise a
// wrapper frame owns the link until the operation completes.
template <typename start_type>
[[nodiscard]] std::invoke_result_t<start_type&, const operation_options&> start_linked_operation(const operation_options& options, const stop_token& host_stop, start_type start) {
    if (!host_stop.stoppable() || options.stop_token_ == host_stop) {
        return start(options);
    }
    if (!options.stop_token_.stoppable()) {
        return start(operation_options{.timeout_ = options.timeout_, .stop_token_ = host_stop});
    }
    using result_type = typename std::invoke_result_t<start_type&, const operation_options&>::value_type;
    return detail::run_linked_operation<result_type>(options, host_stop, std::move(start));
}

// The user token when it can stop, otherwise the host's. Alone it decides
// whether a host-bound wait is bounded; as operation input it is complete only
// when make_operation_link() returned null.
[[nodiscard]] inline operation_options bound_operation_options(const operation_options& options, const stop_token& host_stop) {
    if (options.stop_token_.stoppable() || !host_stop.stoppable()) {
        return options;
    }
    return {.timeout_ = options.timeout_, .stop_token_ = host_stop};
}

// The heap-owned link of a long-lived host-bound resource (stream, transaction)
// that outlives the call creating it; null when one token alone cancels it.
[[nodiscard]] inline std::unique_ptr<const linked_operation_options> make_operation_link(const operation_options& options, const stop_token& host_stop) {
    if (!options.stop_token_.stoppable() || !host_stop.stoppable() || options.stop_token_ == host_stop) {
        return nullptr;
    }
    return std::make_unique<const linked_operation_options>(options, host_stop);
}

class operation_interrupt;

// Routes stop requests to the running operations of one I/O owner on its
// worker. The owner keeps it at a stable address until those operations have
// completed. The first stoppable operation creates the shared target; it is
// detached on destruction, so a stop delivered later reaches no owner.
class operation_interrupt_source final {
public:
    operation_interrupt_source() noexcept = default;
    operation_interrupt_source(const operation_interrupt_source&) = delete;
    operation_interrupt_source& operator=(const operation_interrupt_source&) = delete;
    ~operation_interrupt_source() {
        if (target_) {
            target_->detach(*this);
        }
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept;

private:
    friend class operation_interrupt;

    std::shared_ptr<worker_cancellation_target<operation_interrupt_source>> target_;
    operation_interrupt* running_{};
};

enum class operation_interrupt_cause : std::uint8_t {
    cancelled,
    timed_out,
};

// The deadline and stop token of one running operation. Both are delivered on
// the operation's worker, where the first records its cause and calls
// `interrupt(target)` once. A stop requested before construction throws
// operation_canceled before anything is armed.
class operation_interrupt final {
public:
    using interrupt_type = void (*)(void*) noexcept;

    operation_interrupt(operation_interrupt_source& source, const worker_ref& worker_value, const operation_options& options, void* target, interrupt_type interrupt, const char* operation_name) :
        source_(source),
        target_(target),
        interrupt_(interrupt),
        operation_name_(operation_name) {
        if (options.stop_token_.stop_requested()) {
            throw_cause(operation_interrupt_cause::cancelled, operation_name_);
        }
        if (options.stop_token_.stoppable() && !source_.target_) {
            source_.target_ = make_worker_cancellation_target(source_, worker_value);
        }
        if (options.timeout_.has_value()) {
            (void)timer_.schedule(worker_value, options.timeout_->deadline_from(std::chrono::steady_clock::now()), [this](worker_timer_status outcome) noexcept {
                if (outcome == worker_timer_status::expired) {
                    fire(operation_interrupt_cause::timed_out);
                }
            });
        }
        // Linked before arming: a stop requested meanwhile is delivered inline.
        next_ = source_.running_;
        source_.running_ = this;
        if (options.stop_token_.stoppable()) {
            try {
                cancellation_.emplace(source_.target_, id_);
                cancellation_->arm(options.stop_token_);
            } catch (...) {
                cancellation_.reset();
                unlink();
                throw;
            }
        }
    }

    operation_interrupt(const operation_interrupt&) = delete;
    operation_interrupt& operator=(const operation_interrupt&) = delete;

    ~operation_interrupt() {
        // Joins a stop callback before this operation leaves the source.
        cancellation_.reset();
        unlink();
    }

    [[nodiscard]] bool interrupted() const noexcept {
        return cause_.has_value();
    }

    [[nodiscard]] std::optional<operation_interrupt_cause> cause() const noexcept {
        return cause_;
    }

    // Throws std::system_error (timed_out or operation_canceled) once interrupted.
    void throw_if_interrupted() const {
        if (cause_.has_value()) {
            throw_cause(*cause_, operation_name_);
        }
    }

    [[noreturn]] static void throw_cause(operation_interrupt_cause cause, const char* operation_name) {
        throw std::system_error(std::make_error_code(cause == operation_interrupt_cause::timed_out ? std::errc::timed_out : std::errc::operation_canceled), operation_name);
    }

private:
    friend class operation_interrupt_source;

    void unlink() noexcept {
        for (auto** link = &source_.running_; *link != nullptr; link = &(*link)->next_) {
            if (*link == this) {
                *link = next_;
                return;
            }
        }
    }

    void fire(operation_interrupt_cause cause) noexcept {
        if (cause_.has_value()) {
            return;
        }
        cause_ = cause;
        interrupt_(target_);
    }

    operation_interrupt_source& source_;
    void* target_;
    interrupt_type interrupt_;
    const char* operation_name_;
    std::optional<operation_interrupt_cause> cause_;
    operation_interrupt* next_{};
    std::uint64_t id_{};
    worker_timer_registration timer_;
    // Declared last: its reset joins a pending stop callback first.
    std::optional<worker_cancellation_registration<worker_cancellation_target<operation_interrupt_source>>> cancellation_;
};

inline void operation_interrupt_source::cancel_operation_by_id(std::uint64_t id) noexcept {
    for (auto* running = running_; running != nullptr; running = running->next_) {
        if (id != 0 && running->id_ == id) {
            running->fire(operation_interrupt_cause::cancelled);
            return;
        }
    }
}

// One operation made of several interruptible steps: each step runs with the
// time left and the same stop token, and a step's interruption is recognized
// as this operation's own.
class operation_steps final {
public:
    explicit operation_steps(operation_options operation) noexcept :
        deadline_(operation.timeout_),
        stop_(std::move(operation.stop_token_)) {}

    // The time left and the same stop token for the next step; std::nullopt
    // once the deadline has passed.
    [[nodiscard]] std::optional<operation_options> try_next() const {
        if (deadline_.expired()) {
            return std::nullopt;
        }
        return operation_options{.timeout_ = deadline_.timeout(), .stop_token_ = stop_};
    }

    // Throws std::system_error (timed_out) once the deadline has passed.
    [[nodiscard]] operation_options next(const char* operation_name) const {
        auto step = try_next();
        if (!step.has_value()) {
            operation_interrupt::throw_cause(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }

    // Whether this operation's own deadline or stop token accounts for `cause`.
    [[nodiscard]] bool interrupted_by(operation_interrupt_cause cause) const noexcept {
        return cause == operation_interrupt_cause::timed_out ? deadline_.expired() : stop_.stop_requested();
    }

    // Whether `error` is a step interrupted by this operation's deadline or stop token.
    [[nodiscard]] bool interrupted(const std::system_error& error) const noexcept {
        return (error.code() == std::errc::timed_out && interrupted_by(operation_interrupt_cause::timed_out)) || (error.code() == std::errc::operation_canceled && interrupted_by(operation_interrupt_cause::cancelled));
    }

private:
    operation_deadline deadline_;
    stop_token stop_;
};

}  // namespace ruvia
