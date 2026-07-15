#pragma once

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
    struct Awaiter;

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

    void notify() noexcept;

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
        Awaiter* next{nullptr};
        std::coroutine_handle<> continuation{};

        [[nodiscard]] bool await_ready() noexcept {
            if (!signal.pending_) {
                return false;
            }
            signal.pending_ = false;
            return true;
        }

        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation = value;
            next = signal.waiters_;
            signal.waiters_ = this;
            return true;
        }

        void await_resume() const noexcept {}
    };

    Target target_;
    Awaiter* waiters_{nullptr};
    bool pending_{false};
};

inline void WorkerSignal::notify() noexcept {
    auto* waiter = std::exchange(waiters_, nullptr);
    if (waiter == nullptr) {
        pending_ = true;
        return;
    }

    while (waiter != nullptr) {
        auto* next = waiter->next;
        const auto continuation = std::exchange(waiter->continuation, {});
        waiter->next = nullptr;
        try {
            if (const auto* worker = std::get_if<WorkerHandle>(&target_)) {
                WorkerHandleAccess::defer(
                    *worker, [continuation] { continuation.resume(); });
            } else {
                asio::post(
                    std::get<asio::any_io_executor>(target_),
                    [continuation] { continuation.resume(); });
            }
        } catch (...) {
            std::terminate();
        }
        waiter = next;
    }
}

}
