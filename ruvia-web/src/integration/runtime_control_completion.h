#pragma once

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia::detail {

// Receipts cover execution AND destruction of posted controls. Waiting never
// cancels early: borrowed command state cannot retire before every receipt.
class runtime_control_completion final {
    struct state final {
        explicit state(worker_handle caller)
            : caller_(std::move(caller)) {}
        ~state() {
            if (failure_ && !observed_) {
                report_unhandled_failure("runtime control", failure_);
            }
        }
        worker_handle caller_;
        std::mutex mutex_;
        std::exception_ptr failure_;
        std::coroutine_handle<> continuation_{};
        std::size_t completed_{};
        bool released_{};
        bool observed_{};
    };

public:
    template <typename function_type>
    class command;
    class receipt final {
    public:
        void complete(std::exception_ptr failure = {}) const noexcept {
            std::lock_guard lock(state_->mutex_);
            ++state_->completed_;
            if (!state_->failure_) {
                state_->failure_ = std::move(failure);
            }
        }
        template <typename function_type>
        [[nodiscard]] command<std::decay_t<function_type>> bind(function_type function) && {
            return command<std::decay_t<function_type>>(std::move(*this), std::move(function));
        }

    private:
        receipt(std::shared_ptr<state> state_value, std::shared_ptr<void> lifetime) noexcept
            : state_(std::move(state_value)),
              lifetime_(std::move(lifetime)) {}
        std::shared_ptr<state> state_;
        std::shared_ptr<void> lifetime_;
        friend class runtime_control_completion;
    };

    // Capture lists do not specify closure member declaration order. Keep the
    // receipt in a named first member so command payload destruction completes
    // before its final receipt can resume another worker.
    template <typename function_type>
    class command final {
    public:
        command(receipt completion, function_type function)
            : completion_(std::move(completion)),
              function_(std::move(function)) {}
        command(const command&) = delete;
        command& operator=(const command&) = delete;
        command(command&&) = default;
        command& operator=(command&&) = delete;
        template <typename... args_type>
            requires std::invocable<function_type&, const receipt&, args_type...>
        decltype(auto) operator()(args_type&&... args) {
            return std::invoke(function_, std::as_const(completion_), std::forward<args_type>(args)...);
        }

    private:
        receipt completion_;
        function_type function_;
    };

    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!state_->caller_.is_current()) {
                throw std::logic_error("runtime control completion must be awaited on its caller worker");
            }
            std::lock_guard lock(state_->mutex_);
            started_ = true;
            return state_->released_;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(state_->mutex_);
            if (state_->released_) {
                return false;
            }
            if (state_->continuation_) {
                std::terminate();
            }
            state_->continuation_ = continuation;
            return true;
        }
        void await_resume() noexcept {
            resumed_ = true;
        }

    private:
        explicit wait_operation(std::shared_ptr<state> value) noexcept
            : state_(std::move(value)) {}
        std::shared_ptr<state> state_;
        bool started_{};
        bool resumed_{};
        friend class runtime_control_completion;
    };
    explicit runtime_control_completion(worker_handle caller)
        : state_(std::make_shared<state>(std::move(caller))),
          lifetime_(state_.get(), [value = state_](void*) noexcept {
              std::coroutine_handle<> continuation;
              {
                  std::lock_guard lock(value->mutex_);
                  value->released_ = true;
                  continuation = std::exchange(value->continuation_, {});
              }
              if (continuation) {
                  value->caller_.defer_resume(continuation);
              }
          }) {}
    runtime_control_completion(const runtime_control_completion&) = delete;
    runtime_control_completion& operator=(const runtime_control_completion&) = delete;
    [[nodiscard]] receipt make_receipt() const {
        if (!lifetime_) {
            throw std::logic_error("runtime control completion is already waiting");
        }
        return receipt(state_, lifetime_);
    }
    template <typename function_type>
    [[nodiscard]] command<std::decay_t<function_type>> make_command(function_type function) const {
        return make_receipt().bind(std::move(function));
    }
    [[nodiscard]] wait_operation wait() & {
        if (!state_->caller_.is_current()) {
            throw std::logic_error("runtime control must wait on its caller worker");
        }
        if (waiting_) {
            throw std::logic_error("runtime control completion can only wait once");
        }
        waiting_ = true;
        lifetime_.reset();
        return wait_operation(state_);
    }
    wait_operation wait() && = delete;
    [[nodiscard]] std::size_t completed() const noexcept {
        std::lock_guard lock(state_->mutex_);
        return state_->completed_;
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        std::lock_guard lock(state_->mutex_);
        state_->observed_ = true;
        return state_->failure_;
    }

private:
    std::shared_ptr<state> state_;
    std::shared_ptr<void> lifetime_;
    bool waiting_{};
};

}  // namespace ruvia::detail
