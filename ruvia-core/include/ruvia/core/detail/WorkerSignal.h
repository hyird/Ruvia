#pragma once

#include <array>
#include <concepts>
#include <coroutine>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia::detail {

class WorkerSignal final {
public:
    explicit WorkerSignal(WorkerHandle worker)
        : target_(makeWorkerTarget(std::move(worker))) {}

    template <typename Executor>
        requires (!std::same_as<std::remove_cvref_t<Executor>, WorkerHandle>)
    explicit WorkerSignal(Executor&& executor)
        : target_(
              std::in_place_type<asio::any_io_executor>,
              std::forward<Executor>(executor)) {}

    template <typename Executor>
    WorkerSignal(const WorkerHandle* worker, Executor&& executor)
        : target_(makeTarget(worker, std::forward<Executor>(executor))) {}

    WorkerSignal(const WorkerSignal&) = delete;
    WorkerSignal& operator=(const WorkerSignal&) = delete;

    [[nodiscard]] Task<void> wait() {
        const auto* worker = std::get_if<WorkerHandle>(&target_);
        if (worker != nullptr && !worker->isCurrent()) {
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
                if (const auto* worker = std::get_if<WorkerHandle>(&target_)) {
                    WorkerHandleAccess::defer(
                        *worker, [waiter] { waiter.resume(); });
                } else {
                    asio::post(
                        std::get<asio::any_io_executor>(target_),
                        [waiter] { waiter.resume(); });
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
    using Target = std::variant<WorkerHandle, asio::any_io_executor>;

    [[nodiscard]] static Target makeWorkerTarget(WorkerHandle worker) {
        if (!worker.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
        return Target(
            std::in_place_type<WorkerHandle>, std::move(worker));
    }

    template <typename Executor>
    [[nodiscard]] static Target makeTarget(
        const WorkerHandle* worker,
        Executor&& executor) {
        if (worker != nullptr && worker->valid()) {
            return Target(std::in_place_type<WorkerHandle>, *worker);
        }
        return Target(
            std::in_place_type<asio::any_io_executor>,
            std::forward<Executor>(executor));
    }

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

    Target target_;
    std::array<std::coroutine_handle<>, 8> waiters_{};
    bool pending_{false};
};

}
