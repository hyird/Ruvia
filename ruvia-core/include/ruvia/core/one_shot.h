#pragma once

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/detail/worker/worker_wait_awaiter.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
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

struct one_shot_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

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
    explicit one_shot_state(worker_handle target)
        : worker_(std::move(target)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void worker_stopping() noexcept;

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_hook shutdown_hook_;
    worker_handle worker_;
    std::mutex mutex_;
    using lifecycle_type = std::variant<one_shot_pending, one_shot_completing, one_shot_ready,
        one_shot_consumed, one_shot_receiver_closed, one_shot_worker_stopping>;
    lifecycle_type lifecycle_;
    // After publishing consumed/closed, the thread that claimed ready owns this
    // stable slot until its value has been moved or destroyed outside mutex_.
    std::optional<t_type> ready_value_;
    worker_wait_slot<std::monostate, one_shot_state<t_type>> wait_slot_;
};

// The wait operation: single-use, movable only before it is awaited, and
// started on the receiver's worker by co_await. Dropping a cold one has no
// effect on the exchange.
template <typename t_type>
struct one_shot_awaiter final {
    using wait_type = worker_single_wait_awaiter<std::monostate, one_shot_state<t_type>>;

    one_shot_awaiter(std::shared_ptr<one_shot_state<t_type>> value,
        std::optional<std::chrono::steady_clock::duration> timeout_value, stop_token stop_token_value) noexcept
        : wait_(std::move(value), timeout_value, std::move(stop_token_value)) {}

    [[nodiscard]] bool await_ready() {
        wait_.start("one-shot wait must run on its bound worker");
        auto& owner_value = wait_.state();
        std::unique_lock lock(owner_value.mutex_);
        if (std::holds_alternative<one_shot_ready>(owner_value.lifecycle_)) {
            owner_value.lifecycle_.template emplace<one_shot_consumed>();
            (void)wait_.complete_result(worker_wait_result_access::value(std::monostate{}));
            return true;
        }
        if (std::holds_alternative<one_shot_worker_stopping>(owner_value.lifecycle_)) {
            (void)wait_.complete_status(worker_wait_status::worker_stopping);
            return true;
        }
        if (std::holds_alternative<one_shot_receiver_closed>(owner_value.lifecycle_) ||
            std::holds_alternative<one_shot_consumed>(owner_value.lifecycle_)) {
            (void)wait_.complete_status(worker_wait_status::closed);
            return true;
        }
        assert(std::holds_alternative<one_shot_pending>(owner_value.lifecycle_) ||
               std::holds_alternative<one_shot_completing>(owner_value.lifecycle_));
        return wait_.complete_or_publish("one-shot supports one pending receiver");
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        return wait_.suspend(handle);
    }

    [[nodiscard]] worker_wait_result<t_type> await_resume() {
        auto result = wait_.take_result();
        if (!result.has_value()) {
            return worker_wait_result_access::outcome<t_type>(result.status());
        }
        auto& value = wait_.state().ready_value_;
        struct ready_consumption final {
            std::optional<t_type>& value_;
            ~ready_consumption() {
                value_.reset();
            }
        } consumption{value};
        return worker_wait_result_access::value(std::move(*value));
    }

private:
    wait_type wait_;
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

    // Keep this completion handle alive through the call, including payload
    // moves and destruction. Concurrent attempts report in_progress while the
    // first value is being constructed.
    [[nodiscard]] one_shot_complete_result<t_type> complete(t_type value) const {
        auto* const state = state_.get();
        if (state == nullptr) {
            return one_shot_complete_result<t_type>::reject(
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
            return one_shot_complete_result<t_type>::reject(*rejection, std::move(value));
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
                state->wait_slot_.complete(detail::worker_wait_result_access::value(std::monostate{}));
            } else {
                state->lifecycle_.template emplace<detail::one_shot_ready>();
            }
        }
        if (rejection) {
            struct discard final {
                std::optional<t_type>& value_;
                ~discard() {
                    value_.reset();
                }
            } discard_value{state->ready_value_};
            return one_shot_complete_result<t_type>::reject(*rejection, std::move(*state->ready_value_));
        }
        return one_shot_complete_result<t_type>::accept();
    }

private:
    explicit one_shot_completion(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_handle, one_shot_options);
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

    // The receiver owns the receive side: destroying it closes every cold or
    // running wait. Require a named owner so storing an operation cannot
    // silently turn a temporary receiver into an already-closed operation.
    using wait_operation = detail::one_shot_awaiter<t_type>;

    [[nodiscard]] wait_operation wait(stop_token stop_token_value = {}) const& {
        return wait_operation(state_, std::nullopt, std::move(stop_token_value));
    }
    wait_operation wait(stop_token = {}) const&& = delete;

    template <typename rep_type, typename period_type>
    [[nodiscard]] wait_operation wait_for(
        std::chrono::duration<rep_type, period_type> duration, stop_token stop_token_value = {}) const& {
        return wait_operation(
            state_, ::ruvia::worker_timer_saturating_duration_cast(duration), std::move(stop_token_value));
    }
    template <typename rep_type, typename period_type>
    wait_operation wait_for(std::chrono::duration<rep_type, period_type>, stop_token = {}) const&& = delete;

    // The worker every wait must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_handle& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_handle& worker() const&& = delete;

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
    friend auto make_one_shot(worker_handle, one_shot_options);
};

template <typename t_type>
// The value parameter accepts both lvalue handles and rvalue handles before
// transferring the stable dispatcher endpoint into the one-shot state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_one_shot(worker_handle worker_value, one_shot_options options = {}) {
    auto state_value = detail::make_worker_bound_state<detail::one_shot_state<t_type>>(
        pmr_resource_or_default(options.resource_), std::move(worker_value));
    return std::pair(one_shot_completion<t_type>(state_value), one_shot_receiver<t_type>(state_value));
}

}  // namespace ruvia
