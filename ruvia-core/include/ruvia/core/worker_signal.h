#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/worker_handle.h"

namespace ruvia {

// Intrusive, allocation-free wake primitive for one worker. Every operation
// which touches its waiter list is worker-affine; the worker_handle is therefore
// the dispatch target and the affinity capability, not an optional fast path.
class worker_signal final {
    enum class await_state_type : std::uint8_t {
        idle,
        linked,
        scheduled,
    };

public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        // Only a cold operation may move; a linked waiter must stay address-stable.
        wait_operation(wait_operation&& other) noexcept
            : signal_(std::exchange(other.signal_, nullptr)) {
            if (other.started_) {
                std::terminate();
            }
        }
        wait_operation& operator=(wait_operation&&) = delete;
        ~wait_operation() {
            if (state_ != await_state_type::idle) {
                std::terminate();
            }
            release_reservation();
        }

        [[nodiscard]] bool await_ready() {
            if (signal_ == nullptr || started_) {
                throw std::logic_error("worker signal wait cannot be awaited");
            }
            started_ = true;
            try {
                signal_->require_current_worker();
            } catch (...) {
                release_reservation();
                throw;
            }
            if (!signal_->pending_) {
                return false;
            }
            signal_->pending_ = false;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation_ = value;
            next_ = signal_->waiters_;
            state_ = await_state_type::linked;
            signal_->waiters_ = this;
            return true;
        }
        void await_resume() noexcept {
            release_reservation();
        }

    private:
        explicit wait_operation(worker_signal& signal) noexcept
            : signal_(&signal) {
            ++signal.reserved_waits_;
        }
        void release_reservation() noexcept {
            if (signal_ != nullptr) {
                --signal_->reserved_waits_;
                signal_ = nullptr;
            }
        }
        worker_signal* signal_;
        wait_operation* next_{nullptr};
        std::coroutine_handle<> continuation_{};
        await_state_type state_{await_state_type::idle};
        bool started_{false};
        friend class worker_signal;
    };

private:
    using awaiter_type = wait_operation;

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

    // A cold single-use awaiter reserves the signal until completion or destruction.
    // Creation and starting the wait both require the bound worker.
    [[nodiscard]] wait_operation wait() {
        require_current_worker();
        return wait_operation(*this);
    }

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

    void notify() noexcept;

private:
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
        // is terminal instead of silently stranding the continuation. On the
        // worker's own thread this small fixed callable is posted directly.
        detail::worker_handle_access::defer_or_terminate(
            worker_, [this, waiter, continuation] { resume_scheduled(waiter, continuation); });
        waiter = next_value;
    }
}

inline void worker_signal::resume_scheduled(
    awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept {
    if (!worker_.is_current() || waiter->state_ != await_state_type::scheduled ||
        waiter->continuation_ != continuation || scheduled_waiters_ == 0) {
        std::terminate();
    }
    --scheduled_waiters_;
    waiter->continuation_ = {};
    waiter->state_ = await_state_type::idle;
    continuation.resume();
}

}  // namespace ruvia
