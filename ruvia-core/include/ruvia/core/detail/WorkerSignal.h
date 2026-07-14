#pragma once

#include <coroutine>
#include <array>
#include <concepts>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <optional>

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia::detail {

class WorkerSignal final {
public:
    explicit WorkerSignal(WorkerHandle worker) : worker_(std::move(worker)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
    }

    template <typename Executor>
        requires (!std::same_as<std::remove_cvref_t<Executor>, WorkerHandle>)
    explicit WorkerSignal(Executor&& executor)
        : executor_(asio::any_io_executor(std::forward<Executor>(executor))) {}

    template <typename Executor>
    WorkerSignal(const WorkerHandle* worker, Executor&& executor)
        : worker_(worker == nullptr ? WorkerHandle{} : *worker) {
        if (!worker_.valid()) {
            executor_.emplace(std::forward<Executor>(executor));
        }
    }

    WorkerSignal(const WorkerSignal&) = delete;
    WorkerSignal& operator=(const WorkerSignal&) = delete;

    [[nodiscard]] Task<void> wait() {
        if (worker_.valid() && !worker_.isCurrent()) {
            throw std::logic_error("worker signal wait must run on its worker");
        }
        co_await Awaiter{*this};
    }

    void notify() noexcept {
        bool woke = false;
        for (auto& slot : waiters_) {
            if (!slot) {
                continue;
            }
            woke = true;
            const auto waiter = std::exchange(slot, {});
            try {
                if (worker_.valid()) {
                    WorkerHandleAccess::defer(worker_, [waiter] { waiter.resume(); });
                } else {
                    asio::post(*executor_, [waiter] { waiter.resume(); });
                }
            } catch (...) {
                std::terminate();
            }
        }
        if (!woke) {
            pending_ = true;
        }
    }

private:
    struct Awaiter final {
        WorkerSignal& signal;

        [[nodiscard]] bool await_ready() noexcept {
            if (!signal.pending_) {
                return false;
            }
            signal.pending_ = false;
            return true;
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            for (auto& slot : signal.waiters_) {
                if (!slot) {
                    slot = continuation;
                    return true;
                }
            }
            throw std::logic_error("worker signal waiter capacity exceeded");
        }

        void await_resume() const noexcept {}
    };

    WorkerHandle worker_;
    std::optional<asio::any_io_executor> executor_;
    std::array<std::coroutine_handle<>, 8> waiters_{};
    bool pending_{false};
};

}
