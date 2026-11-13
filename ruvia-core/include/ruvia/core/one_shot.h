#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia {

enum class one_shot_complete_status : std::uint8_t {
    completed,
    already_completed,
    in_progress,
    receiver_closed,
    worker_stopping,
};

template <typename t_type>
class one_shot_completion;

template <typename t_type>
using one_shot_complete_result =
    worker_delivery_result<t_type, one_shot_complete_status, one_shot_complete_status::completed>;

namespace detail {

struct one_shot_pending final {};
struct one_shot_completing final {};
struct one_shot_consumed final {};
struct one_shot_receiver_closed final {};
struct one_shot_worker_stopping final {};

struct one_shot_ready final {};

template <typename t_type>
struct one_shot_state final {
    explicit one_shot_state(worker_ref target)
        : worker_(std::move(target)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void worker_stopping() noexcept;

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] worker_wait_slot<std::monostate, one_shot_state>& wait_slot() noexcept {
        return wait_slot_;
    }
    [[nodiscard]] t_type take_ready_value() {
        struct ready_consumption final {
            std::optional<t_type>& value_;
            ~ready_consumption() {
                value_.reset();
            }
        } consumption{ready_value_};
        return std::move(*ready_value_);
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    using lifecycle_type = std::variant<one_shot_pending, one_shot_completing, one_shot_ready,
        one_shot_consumed, one_shot_receiver_closed, one_shot_worker_stopping>;
    lifecycle_type lifecycle_;
    // After publishing consumed/closed, the thread that claimed ready owns this
    // stable slot until its value has been moved or destroyed outside mutex_.
    std::optional<t_type> ready_value_;
    worker_wait_slot<std::monostate, one_shot_state<t_type>> wait_slot_;
};

template <typename t_type>
void one_shot_state<t_type>::worker_stopping() noexcept {
    std::lock_guard lock(mutex_);
    // Delivered, consumed and closed outcomes stay terminal; only an
    // unresolved exchange is ended by worker shutdown.
    if (std::holds_alternative<one_shot_pending>(lifecycle_) ||
        std::holds_alternative<one_shot_completing>(lifecycle_)) {
        lifecycle_.template emplace<one_shot_worker_stopping>();
    }
    wait_slot_.complete(worker_wait_status::worker_stopping);
}

}  // namespace detail

template <typename t_type>
class one_shot_completion final {
public:
    one_shot_completion() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    // Keep this completion handle alive through the call, including payload
    // moves and destruction. Concurrent attempts report in_progress while the
    // first value is being constructed.
    [[nodiscard]] one_shot_complete_result<t_type> complete(t_type value) const {
        // Payload code may synchronously rebind this handle while the call is active.
        const auto state = state_;
        if (state == nullptr) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(
                one_shot_complete_status::receiver_closed, std::move(value));
        }
        std::optional<one_shot_complete_status> rejection;
        {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_ready>(state->lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::already_completed;
            } else if (std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                rejection = one_shot_complete_status::in_progress;
            } else if (std::holds_alternative<detail::one_shot_worker_stopping>(state->lifecycle_)) {
                rejection = one_shot_complete_status::worker_stopping;
            } else if (std::holds_alternative<detail::one_shot_receiver_closed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::receiver_closed;
            } else if (!state->worker_.accepting()) {
                rejection = one_shot_complete_status::worker_stopping;
            } else {
                assert(std::holds_alternative<detail::one_shot_pending>(state->lifecycle_));
                state->lifecycle_.template emplace<detail::one_shot_completing>();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(*rejection, std::move(value));
        }
        try {
            state->ready_value_.emplace(std::move(value));
        } catch (...) {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                state->lifecycle_.template emplace<detail::one_shot_pending>();
            }
            throw;
        }
        {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_receiver_closed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::receiver_closed;
            } else if (std::holds_alternative<detail::one_shot_worker_stopping>(state->lifecycle_) ||
                       !state->worker_.accepting()) {
                state->lifecycle_.template emplace<detail::one_shot_worker_stopping>();
                state->wait_slot_.complete(worker_wait_status::worker_stopping);
                rejection = one_shot_complete_status::worker_stopping;
            } else if (state->wait_slot_.occupied()) {
                state->lifecycle_.template emplace<detail::one_shot_consumed>();
                state->wait_slot_.complete(worker_wait_result<std::monostate>::with_value(std::monostate{}));
            } else {
                state->lifecycle_.template emplace<detail::one_shot_ready>();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(*rejection, state->take_ready_value());
        }
        return detail::worker_delivery_access::accept<one_shot_complete_result<t_type>>();
    }

private:
    explicit one_shot_completion(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_ref);
};

template <typename t_type>
class one_shot_receiver final {
public:
    one_shot_receiver() = delete;
    one_shot_receiver(const one_shot_receiver&) = delete;
    one_shot_receiver& operator=(const one_shot_receiver&) = delete;
    one_shot_receiver(one_shot_receiver&&) noexcept = default;
    one_shot_receiver& operator=(one_shot_receiver&&) = delete;
    ~one_shot_receiver() {
        close();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    // The wait operation: single-use, movable only before it is awaited, and
    // started on the receiver's worker by co_await. Dropping a cold one has no
    // effect on the exchange.
    class wait_operation final {
    public:
        wait_operation(wait_operation&&) noexcept = default;
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        wait_operation& operator=(wait_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("one-shot wait must run on its bound worker");
            auto& owner_value = wait_.state();
            auto lock = owner_value.lock_state();
            if (std::holds_alternative<detail::one_shot_ready>(owner_value.lifecycle_)) {
                owner_value.lifecycle_.template emplace<detail::one_shot_consumed>();
                wait_.complete_ready(worker_wait_result<std::monostate>::with_value(std::monostate{}));
                return true;
            }
            if (std::holds_alternative<detail::one_shot_worker_stopping>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (std::holds_alternative<detail::one_shot_receiver_closed>(owner_value.lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::closed);
                return true;
            }
            assert(std::holds_alternative<detail::one_shot_pending>(owner_value.lifecycle_) ||
                   std::holds_alternative<detail::one_shot_completing>(owner_value.lifecycle_));
            return wait_.complete_or_publish("one-shot supports one pending receiver");
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<t_type> await_resume() {
            auto result = wait_.take_result();
            if (!result.has_value()) {
                return worker_wait_result<t_type>::with_status(result.status());
            }
            return worker_wait_result<t_type>::with_value(wait_.state().take_ready_value());
        }

    private:
        friend class one_shot_receiver;
        using state_type = detail::one_shot_state<t_type>;

        wait_operation(std::shared_ptr<state_type> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<std::monostate, state_type> wait_;
    };

    // The receiver owns the receive side: destroying it closes every cold or
    // running wait. Require a named owner so storing an operation cannot
    // silently turn a temporary receiver into an already-closed operation.
    // A relative options.timeout_ bounds the wait from its start; a deadline
    // that has passed reports timed_out unless a value is ready.
    [[nodiscard]] wait_operation wait(operation_options options = {}) const& {
        return wait_operation(state_, std::move(options));
    }
    wait_operation wait(operation_options = {}) const&& = delete;

    // The worker every wait must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_ref& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Closing discards an unconsumed value before returning and closes every
    // future wait. Its destruction may reenter this receiver or its completion.
    void close() const {
        if (!state_) {
            return;
        }
        const auto state = state_;
        bool discard = false;
        {
            std::lock_guard lock(state->mutex_);
            discard = std::holds_alternative<detail::one_shot_ready>(state->lifecycle_);
            if (!discard && !std::holds_alternative<detail::one_shot_pending>(state->lifecycle_) &&
                !std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                return;
            }
            state->lifecycle_.template emplace<detail::one_shot_receiver_closed>();
            state->wait_slot_.complete(worker_wait_status::closed);
        }
        if (discard) {
            state->ready_value_.reset();
        }
    }

private:
    explicit one_shot_receiver(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_ref);
};

template <typename t_type>
// The value parameter accepts both lvalue and rvalue worker refs before
// transferring the stable dispatcher endpoint into the one-shot state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_one_shot(worker_ref worker_value) {
    auto state_value = make_worker_wait_state<detail::one_shot_state<t_type>>(std::move(worker_value));
    one_shot_completion<t_type> completion(state_value);
    return std::pair(std::move(completion), one_shot_receiver<t_type>(std::move(state_value)));
}

}  // namespace ruvia
