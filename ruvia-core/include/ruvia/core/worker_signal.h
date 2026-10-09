#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

// Intrusive, allocation-free wake primitive for one worker. Every operation
// which touches its waiter list is worker-affine; the worker_handle is therefore
// the dispatch target and the affinity capability, not an optional fast path.
class worker_signal final {
    struct awaiter_type;

public:
    explicit worker_signal(const worker_handle& worker_value)
        : worker_(require_worker(worker_value)) {}
    worker_signal(worker_handle&&) = delete;

    ~worker_signal() {
        if (waiters_ != nullptr || scheduled_waiters_ != 0 || reserved_waits_ != 0) {
            std::terminate();
        }
    }

    worker_signal(const worker_signal&) = delete;
    worker_signal& operator=(const worker_signal&) = delete;

    [[nodiscard]] task<void> wait() {
        require_current_worker();
        return wait_reserved(wait_reservation_type(*this));
    }

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

    void notify() noexcept;

private:
    enum class await_state_type : std::uint8_t {
        idle,
        linked,
        scheduled,
    };

    [[nodiscard]] static const worker_handle& require_worker(const worker_handle& worker_value) {
        if (!worker_value.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
        return worker_value;
    }

    void require_current_worker() const {
        if (!worker_.is_current()) {
            throw std::logic_error("worker signal operation must run on its worker");
        }
    }

    void resume_scheduled(awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept;

    class wait_reservation_type final {
    public:
        explicit wait_reservation_type(worker_signal& signal) noexcept
            : signal_(&signal) {
            ++signal_->reserved_waits_;
        }
        ~wait_reservation_type() {
            if (signal_ != nullptr) {
                --signal_->reserved_waits_;
            }
        }

        wait_reservation_type(const wait_reservation_type&) = delete;
        wait_reservation_type& operator=(const wait_reservation_type&) = delete;
        wait_reservation_type(wait_reservation_type&& other) noexcept
            : signal_(std::exchange(other.signal_, nullptr)) {}
        wait_reservation_type& operator=(wait_reservation_type&&) = delete;

        [[nodiscard]] worker_signal& signal() const noexcept {
            return *signal_;
        }

    private:
        worker_signal* signal_;
    };

    [[nodiscard]] static task<void> wait_reserved(wait_reservation_type reservation) {
        auto& signal = reservation.signal();
        // wait() returns a lazy task. Recheck affinity when that task actually
        // starts: a cold wait can otherwise be created on the owner worker and
        // later started on another worker, mutating the intrusive list there.
        signal.require_current_worker();
        co_await awaiter_type{signal};
    }

    struct awaiter_type final {
        explicit awaiter_type(worker_signal& owner_value) noexcept
            : signal_(owner_value) {}

        ~awaiter_type() {
            if (state_ != await_state_type::idle) {
                std::terminate();
            }
        }

        awaiter_type(const awaiter_type&) = delete;
        awaiter_type& operator=(const awaiter_type&) = delete;

        [[nodiscard]] bool await_ready() noexcept {
            if (!signal_.pending_) {
                return false;
            }
            signal_.pending_ = false;
            return true;
        }

        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation_ = value;
            next_ = signal_.waiters_;
            state_ = await_state_type::linked;
            signal_.waiters_ = this;
            return true;
        }

        void await_resume() const noexcept {}

        worker_signal& signal_;
        awaiter_type* next_{nullptr};
        std::coroutine_handle<> continuation_{};
        await_state_type state_{await_state_type::idle};
    };

    // The owning session/connection keeps its stable worker handle alive until
    // every signal waiter and scheduled resumption has joined.
    const worker_handle& worker_;
    awaiter_type* waiters_{nullptr};
    std::size_t scheduled_waiters_{0};
    std::size_t reserved_waits_{0};
    bool pending_{false};
};

inline void worker_signal::notify() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }

    auto* waiter = std::exchange(waiters_, nullptr);
    if (waiter == nullptr) {
        pending_ = true;
        return;
    }

    while (waiter != nullptr) {
        auto* next_value = waiter->next_;
        const auto continuation = waiter->continuation_;
        waiter->next_ = nullptr;
        waiter->state_ = await_state_type::scheduled;
        ++scheduled_waiters_;
        // A detached intrusive node has no recoverable owner. Dispatch failure
        // is terminal instead of silently stranding the continuation.
        detail::worker_handle_access::defer_or_terminate(
            worker_, [this, waiter, continuation] { resume_scheduled(waiter, continuation); });
        waiter = next_value;
    }
}

inline void worker_signal::resume_scheduled(
    awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept {
    if (!worker_.is_current() || waiter == nullptr || waiter->state_ != await_state_type::scheduled ||
        waiter->continuation_ != continuation || scheduled_waiters_ == 0) {
        std::terminate();
    }
    --scheduled_waiters_;
    waiter->continuation_ = {};
    waiter->state_ = await_state_type::idle;
    continuation.resume();
}

}  // namespace ruvia
